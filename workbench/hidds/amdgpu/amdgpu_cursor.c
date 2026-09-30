/*
 * amdgpu.hidd — DCN hardware cursor (DCN 4.1 / Navi 44).
 *
 * WHY A HARDWARE CURSOR IS THE RIGHT RUNG HERE
 * --------------------------------------------
 * The SDMA benchmark showed the copy engine losing badly to the CPU at small
 * sizes — 19-24x slower at 4 KiB in both runs. A 64x64 ARGB cursor is 16 KB,
 * squarely inside that losing range, so drawing the pointer via a GPU blit would
 * be a pessimisation.
 *
 * A hardware cursor is not a blit at all. DCN composites it in the scanout path,
 * so a pointer move costs ONE register write instead of a CPU blit plus a damage
 * rectangle plus a restore of what was underneath. On a software-blit driver the
 * mouse pointer is the most constant load there is, so this is the cheapest real
 * win available, and it needs no rings, no firmware and no command submission —
 * it is independent of everything still missing on the CP/3D side.
 *
 * REGISTERS (dcn_4_1_0_offset.h; every one of these is BASE_IDX 2, i.e.
 * dmu_seg[2], the same segment amdgpu_dcn.c already uses for HUBP/OTG):
 *   CURSOR0_0_CURSOR_SURFACE_ADDRESS / _HIGH   where the image lives (MC address)
 *   CURSOR0_0_CURSOR_SIZE                      width<<16 | height
 *   CURSOR0_0_CURSOR_CONTROL                   enable, mode, pitch, lines/chunk
 *   CURSOR0_0_CURSOR_POSITION                  x<<15 | y   <-- x is at bit 15
 *   CURSOR0_0_CURSOR_HOT_SPOT                  x<<16 | y
 *   CM_CUR0_CURSOR0_CONTROL                    the DPP-side enable + mode
 *
 * Field values follow hubp2_cursor_set_attributes() and
 * hubp2_get_lines_per_chunk(): for a 64-wide un-premultiplied ARGB cursor the
 * pitch enum is CURSOR_PITCH_64_PIXELS (0) and lines-per-chunk is
 * CURSOR_LINE_PER_CHUNK_8 (3). CURSOR_X_POSITION sits at bit 15, NOT 16 — the
 * one field here that reads like a typo and is not.
 *
 * SCOPE: this puts a static, visibly-shaped cursor on the live scanout and reads
 * every register back. It is not yet wired to the HIDD gfx cursor methods
 * (moHidd_Gfx_SetCursorShape / SetCursorPos / SetCursorVisible) — that is the
 * next step and needs this proven first. Verification is deliberately something
 * a human can see on the DUT's HDMI output, not just a log line.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <exec/memory.h>

#include "amdgpu_intern.h"

/* dmu_seg[2]-relative, verified against dcn_4_1_0_offset.h */
#define regCURSOR_CONTROL              0x0679
#define regCURSOR_SURFACE_ADDRESS      0x067a
#define regCURSOR_SURFACE_ADDRESS_HIGH 0x067b
#define regCURSOR_SIZE                 0x067c
#define regCURSOR_POSITION             0x067d
#define regCURSOR_HOT_SPOT             0x067e
#define regCURSOR_DST_OFFSET           0x0680
#define regCM_CUR0_CURSOR0_CONTROL     0x0cf1
#define regCM_CUR0_CUR0_MATRIX_MODE    0x0cf6
/* DSCL (scaler). DCN4 composites the cursor AFTER the scaler, in RECOUT space:
 * dcn401_hwseq.c "DCN4 moved cursor composition after Scaler ... valid
 * coordinates are only in range from 0,0 - recout width, recout height". */
#define regDSCL_SCL_MODE               0x0d08
#define regDSCL_RECOUT_START           0x0d1e
#define regDSCL_RECOUT_SIZE            0x0d1f
/* THE ONE I MISSED. hubp32_cursor_set_attributes() — which is what DCN 4.x
 * actually uses, not the dcn20 version I first copied — programs CURSOR_SETTINGS
 * with CURSOR0_CHUNK_HDL_ADJUST = 3, described in Linux as "used to shift the
 * cursor chunk request deadline". Without it the cursor fetch is never scheduled,
 * which presents exactly as: every register accepts its value and nothing is
 * composited. Note the block prefix differs from the other cursor registers —
 * HUBPREQ0, not CURSOR0_0. */
#define regHUBPREQ_CURSOR_SETTINGS     0x0653
#define CUR_SET_DST_Y_OFFSET_SHIFT     0
#define CUR_SET_CHUNK_HDL_ADJUST_SHIFT 8

/* USE_MALL_FOR_CURSOR: hubp32 sets it when cursor_size > 16384. A 64x64 ARGB
 * cursor is exactly 16384, so this must be 0 — and it is worth writing rather
 * than assuming, since we inherit whatever the GOP left behind. */
#define regHUBP_DCHUBP_MALL_CONFIG     0x05f7
#define MALL_USE_FOR_CURSOR_SHIFT      2

#define regCURSOR_MEM_PWR_CTRL         0x067f
#define regCURSOR_MEM_PWR_STATUS       0x0681

/* dmu_seg[3]-relative DCN composition state. These are read-only diagnostics:
 * the cursor registers can stick while the MPC/DPP update remains pending. */
#define regMPCC0_MPCC_TOP_SEL          0x0000
#define regMPCC0_MPCC_BOT_SEL          0x0001
#define regMPCC0_MPCC_CONTROL          0x0003
#define regMPCC0_MPCC_STATUS           0x000e
#define regMPC_OUT0_MUX                0x02f2
#define regCUR_VUPDATE_LOCK_SET0       0x02c5

#define CM_CUR0_UPDATE_PENDING_MASK    0x00010000UL

/* CURSOR_MEM_PWR_CTRL fields. The cursor ROB (the on-chip buffer DCN fetches the
 * cursor image into) has its own power control. We adopt a GOP-initialised
 * scanout and the GOP never used a hardware cursor, so this memory can plausibly
 * be sitting power-gated — in which case enabling the cursor programs perfectly
 * and composites nothing, which is exactly the symptom. */
#define CROB_MEM_PWR_FORCE_SHIFT       0
#define CROB_MEM_PWR_DIS_SHIFT         2
#define CROB_MEM_PWR_LS_MODE_SHIFT     4

/* CURSOR_CONTROL fields (dcn_4_1_0_sh_mask.h) */
#define CUR_CTL_ENABLE_SHIFT           0
#define CUR_CTL_REQ_MODE_SHIFT         2    /* DCN4x: MUST be 1; 0 unsupported */
#define CUR_CTL_MODE_SHIFT             8
#define CUR_CTL_PITCH_SHIFT            16
#define CUR_CTL_LINES_PER_CHUNK_SHIFT  24
/* CURSOR_SIZE / POSITION / HOT_SPOT */
#define CUR_SIZE_HEIGHT_SHIFT          0
#define CUR_SIZE_WIDTH_SHIFT           16
#define CUR_POS_Y_SHIFT                0
#define CUR_POS_X_SHIFT                15    /* 15, not 16 */
#define CUR_HOT_Y_SHIFT                0
#define CUR_HOT_X_SHIFT                16
/* CM_CUR0_CURSOR0_CONTROL */
#define CM_CUR0_ENABLE_SHIFT           0
#define CM_CUR0_MODE_SHIFT             4

/* dc_hw_types.h enum dc_cursor_color_format */
#define CURSOR_MODE_UN_PRE_MULT_ALPHA  3
/* inc/hw/hubp.h */
#define CURSOR_PITCH_64_PIXELS         0
#define CURSOR_LINE_PER_CHUNK_8        3

/*
 * PIN TEST. When 1, the driver keeps its own 64x64 opaque test block at a fixed
 * position and IGNORES AROS's shape/position updates. This partitions a problem
 * that register auditing cannot: AROS adopts the cursor within milliseconds and
 * replaces the big block with a 16x16 pointer, so the unmissable image has never
 * actually been on screen long enough to judge.
 *   block visible  -> the DCN cursor path WORKS; the bug is in AROS integration
 *                     (shape upload, hotspot, position, or visibility churn).
 *   block absent   -> the hardware path is broken despite every register and the
 *                     surface being provably correct.
 * Set back to 0 once that question is answered.
 */
#define AMDGPU_CURSOR_PIN_TEST 0

/*
 * FB-SOURCE TEST. Points CURSOR_SURFACE_ADDRESS at the live framebuffer instead
 * of our own surface. The framebuffer is memory the display engine provably
 * reads every single frame -- the desktop is drawn from it -- so this separates
 * two things no register dump can:
 *   cursor shows a 64x64 copy of the desktop's top-left corner at (320,240)
 *       -> the cursor engine fetches and composites correctly, and the fault is
 *          in OUR surface memory (location, domain, or visibility to the
 *          display client) despite the CPU proving it writable.
 *   still nothing
 *       -> the cursor engine is not compositing AT ALL regardless of what it is
 *          pointed at, and the fault is downstream of the fetch.
 * Requires PIN_TEST so AROS cannot move or reshape it.
 */
#define AMDGPU_CURSOR_FB_SOURCE_TEST 0

#define AMDGPU_CURSOR_DIM      64
#define AMDGPU_CURSOR_BYTES    (AMDGPU_CURSOR_DIM * AMDGPU_CURSOR_DIM * 4)
/*
 * The cursor surface is placed immediately AFTER the visible framebuffer, and
 * written through the framebuffer's own CPU pointer.
 *
 * Everything else about this sprite verifies — registers read back, the fetch is
 * scheduled, the image is opaque, AROS drives position and visibility — and it
 * still does not composite. The one assumption never validated is that the GPU
 * can see our image at the address we hand it: the previous surface lived at a
 * private MC offset (720 MiB) written through the driver's own VRAM aperture,
 * and both the write and the read-back used that SAME aperture, so they would
 * agree even if the GPU's view of that address were different memory. This
 * file's sibling warns about exactly that: an earlier cut mis-picked the small
 * 256 MB BAR and "that BAR maps a DIFFERENT VRAM region".
 *
 * The VRAM handoff, by contrast, PROVED one mapping: CPU 0x70_00800000 <-> MC
 * 0x80_00800000, because the GPU is scanning out of it right now through that
 * aperture. Placing the cursor just past the visible framebuffer and writing it
 * via that same CPU pointer removes the addressing question entirely rather than
 * testing it again.
 */
static UQUAD cursor_mc;          /* set by amdgpu_cursor_init() */
static UBYTE *cursor_cpu;
#define AMDGPU_CURSOR_MC_BASE  0x8000000000ULL

static ULONG crd(struct AmdgpuDevice *ad, ULONG off)
{
    return amdgpu_rreg32(ad, ad->dmu_seg[2] + off);
}

static void cwr(struct AmdgpuDevice *ad, ULONG off, ULONG val)
{
    amdgpu_wreg32(ad, ad->dmu_seg[2] + off, val);
}

static void amdgpu_cursor_composite_snapshot(struct AmdgpuDevice *ad,
                                              const char *where)
{
    ULONG cm, hubctl, settings;

    if (!ad || !ad->dmu_seg[2])
        return;

    cm = crd(ad, regCM_CUR0_CURSOR0_CONTROL);
    hubctl = crd(ad, regCURSOR_CONTROL);
    settings = crd(ad, regHUBPREQ_CURSOR_SETTINGS);

    if (!ad->dmu_seg[3]) {
        bug("[amdgpu-cursor] DCN composite @%s: CM_CUR0=%08lx pending=%lu "
            "HUBP_CURSOR_CONTROL=%08lx SETTINGS=%08lx; MPCC/MPC base absent\n",
            where, (unsigned long)cm,
            (unsigned long)((cm & CM_CUR0_UPDATE_PENDING_MASK) != 0),
            (unsigned long)hubctl, (unsigned long)settings);
        return;
    }

    bug("[amdgpu-cursor] DCN composite @%s: CM_CUR0=%08lx pending=%lu "
        "HUBP_CURSOR_CONTROL=%08lx SETTINGS=%08lx "
        "MPCC_TOP=%08lx BOT=%08lx CTRL=%08lx STATUS=%08lx "
        "MPC_OUT_MUX=%08lx CUR_VUPDATE_LOCK=%08lx\n",
        where, (unsigned long)cm,
        (unsigned long)((cm & CM_CUR0_UPDATE_PENDING_MASK) != 0),
        (unsigned long)hubctl, (unsigned long)settings,
        (unsigned long)amdgpu_rreg32(ad, ad->dmu_seg[3] + regMPCC0_MPCC_TOP_SEL),
        (unsigned long)amdgpu_rreg32(ad, ad->dmu_seg[3] + regMPCC0_MPCC_BOT_SEL),
        (unsigned long)amdgpu_rreg32(ad, ad->dmu_seg[3] + regMPCC0_MPCC_CONTROL),
        (unsigned long)amdgpu_rreg32(ad, ad->dmu_seg[3] + regMPCC0_MPCC_STATUS),
        (unsigned long)amdgpu_rreg32(ad, ad->dmu_seg[3] + regMPC_OUT0_MUX),
        (unsigned long)amdgpu_rreg32(ad, ad->dmu_seg[3] + regCUR_VUPDATE_LOCK_SET0));
}

/* A solid opaque white 64x64 block with a black border.
 *
 * The first attempt drew a thin arrow, which was the wrong choice for a FIRST
 * test: it is only a couple of hundred opaque pixels and easy to miss, and if
 * the pitch or colour format is subtly wrong the result is a thin smear that
 * looks like nothing rather than like a broken cursor. A solid block is
 * unmissable if it works, and obviously mangled if the format is wrong. Shape
 * fidelity can come back once the path is proven. */
static void cursor_build_image(struct AmdgpuDevice *ad)
{
    ULONG y, x, px;

    for (y = 0; y < AMDGPU_CURSOR_DIM; y++) {
        for (x = 0; x < AMDGPU_CURSOR_DIM; x++) {
            BOOL border = (x < 2) || (y < 2) ||
                          (x >= AMDGPU_CURSOR_DIM - 2) || (y >= AMDGPU_CURSOR_DIM - 2);
            px = border ? 0xFF000000UL     /* opaque black frame */
                        : 0xFFFFFFFFUL;    /* opaque white body  */
            ((ULONG *)cursor_cpu)[y * AMDGPU_CURSOR_DIM + x] = px;
        }
    }
}

/*
 * Public primitives for the Hidd_Display cursor methods.
 *
 * These exist because the standalone sprite this file started as was the wrong
 * thing entirely: it programmed DCN directly with a test image, so it was a
 * hardware sprite AROS knew nothing about, while Wanderer carried on drawing its
 * own software pointer. terminills put it plainly — that is the GOP's mouse, not
 * the Workbench one. A hardware cursor only becomes THE cursor when the display
 * class implements SetCursorShape/Pos/Visible and advertises DIPF_IS_SPRITES so
 * AROS stops drawing a software one.
 */
void amdgpu_cursor_set_pos(struct AmdgpuDevice *ad, LONG x, LONG y)
{
    if (!ad || !ad->dmu_seg[2])
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    /* Steady-state truth: the init dump only proves init. Report what the
     * hardware actually holds once AROS has settled, including whether the
     * surface still contains opaque pixels. */
    {
        static ULONG n = 0;
        if (++n <= 3 || (n & 63) == 0) {
            volatile ULONG *p = (volatile ULONG *)cursor_cpu;
            ULONG i, opaque = 0, anyalpha = 0, amin = 0xFF, amax = 0, s0 = 0, s1 = 0;
            /*
             * Count BOTH ways and report the alpha range.
             *
             * This counter previously tested (p[i] >> 24) == 0xFF -- fully
             * opaque only -- while the shape-copy check next to it tested
             * (p[i] & 0xFF000000) -- any non-zero alpha. So a shape of
             * partially transparent pixels read as "78 copied" and then
             * "0/4096 present", which looks exactly like the surface being
             * wiped and is not. The same diagnostic has sent me chasing a
             * phantom corruption bug once already.
             *
             * The alpha RANGE is the datum that actually matters here: DCN's
             * cursor blend mode is chosen per alpha convention, so knowing
             * whether these pixels are 0xFF, 0x9F or premultiplied decides
             * whether the sprite can be visible at all.
             */
            if (p) for (i = 0; i < AMDGPU_CURSOR_DIM * AMDGPU_CURSOR_DIM; i++) {
                ULONG a = p[i] >> 24;
                if (a == 0xFF) opaque++;
                if (a) {
                    anyalpha++;
                    if (a < amin) amin = a;
                    if (a > amax) amax = a;
                    if (!s0) s0 = p[i]; else if (!s1) s1 = p[i];
                }
            }
            bug("[amdgpu-cursor] STEADY #%lu: alpha!=0=%lu alpha==FF=%lu range=%02lx..%02lx "
                "samples=%08lx,%08lx | CONTROL=%08lx en=%lu POS=%08lx ADDR=%08lx:%08lx "
                "CM_CUR0=%08lx\n",
                (unsigned long)n, (unsigned long)anyalpha, (unsigned long)opaque,
                (unsigned long)(anyalpha ? amin : 0), (unsigned long)amax,
                (unsigned long)s0, (unsigned long)s1,
                (unsigned long)crd(ad, regCURSOR_CONTROL),
                (unsigned long)(crd(ad, regCURSOR_CONTROL) & 1UL),
                (unsigned long)crd(ad, regCURSOR_POSITION),
                (unsigned long)crd(ad, regCURSOR_SURFACE_ADDRESS_HIGH),
                (unsigned long)crd(ad, regCURSOR_SURFACE_ADDRESS),
                (unsigned long)crd(ad, regCM_CUR0_CURSOR0_CONTROL));
        }
    }

#if AMDGPU_CURSOR_PIN_TEST
    return;                      /* hold the test block at its init position */
#endif
    cwr(ad, regCURSOR_POSITION,
        ((ULONG)x << CUR_POS_X_SHIFT) | ((ULONG)y << CUR_POS_Y_SHIFT));
}

void amdgpu_cursor_set_visible(struct AmdgpuDevice *ad, BOOL visible)
{
    ULONG ctl;

    if (!ad || !ad->dmu_seg[2])
        return;
    ctl = crd(ad, regCURSOR_CONTROL);
    if (visible) ctl |=  (1UL << CUR_CTL_ENABLE_SHIFT);
    else         ctl &= ~(1UL << CUR_CTL_ENABLE_SHIFT);
    cwr(ad, regCURSOR_CONTROL, ctl);
}

/* Upload an ARGB32 image (already <= 64x64) and size the sprite to it. */
void amdgpu_cursor_set_shape(struct AmdgpuDevice *ad, const ULONG *argb,
                             ULONG w, ULONG h, WORD hot_x, WORD hot_y)
{
    ULONG y, x, zero = 0;

    if (!ad || !ad->dmu_seg[2] || !argb)
        return;
#if AMDGPU_CURSOR_PIN_TEST
    {
        static int told = 0;
        if (!told) { told = 1;
            bug("[amdgpu-cursor] PIN TEST: ignoring AROS shape upload (%lux%lu) — "
                "keeping the 64x64 test block\n", (unsigned long)w, (unsigned long)h); }
        return;
    }
#endif
    if (w > AMDGPU_CURSOR_DIM) w = AMDGPU_CURSOR_DIM;
    if (h > AMDGPU_CURSOR_DIM) h = AMDGPU_CURSOR_DIM;

    for (y = 0; y < AMDGPU_CURSOR_DIM; y++)
        for (x = 0; x < AMDGPU_CURSOR_DIM; x++) {
            ULONG px = (x < w && y < h) ? argb[y * w + x] : zero;
            if (cursor_cpu)
                ((ULONG *)cursor_cpu)[y * AMDGPU_CURSOR_DIM + x] = px;
        }

    /*
     * The caller measures its OWN buffer and reports 78 opaque pixels, yet the
     * surface reads back 0/4096. Exactly two things can cause that and they
     * need different fixes, so distinguish them here rather than guess again:
     *   cursor_cpu == NULL  -> the copy above was skipped entirely (init never
     *                          established the mapping).
     *   src != 0, dst == 0  -> the writes are not landing in that memory, which
     *                          is the unmapped-aperture failure all over again.
     * Count both sides of the same copy, once.
     */
    {
        static int once = 0;
        if (!once) {
            ULONG i, src = 0, dst = 0;
            once = 1;
            for (i = 0; i < w * h; i++)
                if (argb[i] & 0xFF000000UL) src++;
            if (cursor_cpu) {
                volatile ULONG *p = (volatile ULONG *)cursor_cpu;
                for (i = 0; i < AMDGPU_CURSOR_DIM * AMDGPU_CURSOR_DIM; i++)
                    if ((p[i] & 0xFF000000UL)) dst++;
            }
            bug("[amdgpu-cursor] SHAPE COPY: cursor_cpu=%p src_opaque=%lu/%lu "
                "dst_alpha=%lu/4096 %s\n",
                cursor_cpu, (unsigned long)src, (unsigned long)(w * h),
                (unsigned long)dst,
                !cursor_cpu ? "*** cursor_cpu NULL — copy skipped ***"
                            : (dst ? "copy OK" : "*** WRITES NOT LANDING ***"));
        }
    }

    cwr(ad, regCURSOR_SIZE,
        ((ULONG)AMDGPU_CURSOR_DIM << CUR_SIZE_WIDTH_SHIFT)
      | ((ULONG)AMDGPU_CURSOR_DIM << CUR_SIZE_HEIGHT_SHIFT));
    if (hot_x < 0) hot_x = 0;
    if (hot_y < 0) hot_y = 0;
    if (hot_x >= AMDGPU_CURSOR_DIM) hot_x = AMDGPU_CURSOR_DIM - 1;
    if (hot_y >= AMDGPU_CURSOR_DIM) hot_y = AMDGPU_CURSOR_DIM - 1;
    cwr(ad, regCURSOR_HOT_SPOT,
        ((ULONG)hot_x << CUR_HOT_X_SHIFT) | ((ULONG)hot_y << CUR_HOT_Y_SHIFT));
}

BOOL amdgpu_cursor_init(struct AmdgpuDevice *ad, APTR fb_cpu, UQUAD fb_mc, ULONG fbsize,
                        ULONG scr_w, ULONG scr_h)
{
    UQUAD mc;
    ULONG pad = (fbsize + 0xFFFFUL) & ~0xFFFFUL;   /* 64 KiB past the visible fb */

    if (!fb_cpu || !fb_mc) {
        bug("[amdgpu-cursor] no proven framebuffer mapping — cursor unavailable\n");
        return TRUE;
    }
    /*
     * The surface must land INSIDE the aperture the handoff mapped, which is
     * the visible fb plus AMDGPU_FB_AUX_RESERVE. This check exists because the
     * mapping used to end exactly at the fb: the cursor sat past it, every
     * pixel write was discarded, and the failure was invisible -- all cursor
     * registers still read back correct. Fail loudly rather than draw nothing.
     */
    if ((UQUAD)pad + AMDGPU_CURSOR_BYTES > (UQUAD)fbsize + AMDGPU_FB_AUX_RESERVE) {
        bug("[amdgpu-cursor] surface would fall OUTSIDE the mapped aperture: "
            "pad 0x%lx + %lu > fbsize 0x%lx + reserve 0x%lx — cursor disabled\n",
            (unsigned long)pad, (unsigned long)AMDGPU_CURSOR_BYTES,
            (unsigned long)fbsize, (unsigned long)AMDGPU_FB_AUX_RESERVE);
        return TRUE;
    }
    cursor_cpu = (UBYTE *)fb_cpu + pad;
    cursor_mc  = fb_mc + pad;
    mc = cursor_mc;
    bug("[amdgpu-cursor] surface placed just past the scanout: cpu %p MC 0x%08lx%08lx "
        "(fb cpu %p MC 0x%08lx%08lx + 0x%lx)\n",
        cursor_cpu, (unsigned long)(mc >> 32), (unsigned long)(ULONG)mc,
        fb_cpu, (unsigned long)(fb_mc >> 32), (unsigned long)(ULONG)fb_mc,
        (unsigned long)pad);
    ULONG ctl, size, pos, rb_ctl, rb_addr, rb_size, rb_pos;
    ULONG x = 320, y = 240;      /* somewhere obvious inside a 1024x768 mode */

    if (!ad || !ad->rmmio || !ad->dmu_seg[2]) {
        D(bug("[amdgpu-cursor] no DMU seg2 — cursor unavailable\n"));
        return TRUE;             /* never fatal to boot */
    }

    /* Read the cursor ROB power state BEFORE touching anything, then take it out
     * of any gated/light-sleep state. Order matters: powering the buffer after
     * enabling the fetcher would race the first composited frame. */
    {
        ULONG pwr_before = crd(ad, regCURSOR_MEM_PWR_STATUS);
        ULONG ctl_before = crd(ad, regCURSOR_MEM_PWR_CTRL);

        cwr(ad, regCURSOR_MEM_PWR_CTRL,
            (1UL << CROB_MEM_PWR_DIS_SHIFT));      /* disable power gating, no force, LS off */

        bug("[amdgpu-cursor] CROB mem power: STATUS %08lx -> %08lx  CTRL %08lx -> %08lx\n",
            (unsigned long)pwr_before, (unsigned long)crd(ad, regCURSOR_MEM_PWR_STATUS),
            (unsigned long)ctl_before, (unsigned long)crd(ad, regCURSOR_MEM_PWR_CTRL));
    }

    /*
     * PIPE SURVEY. The surface is proven writable and correct, every cursor
     * register reads back right, and nothing composites. The remaining
     * assumption never tested is that pipe 0 is the pipe driving the panel:
     * we program CURSOR0_0_* / CM_CUR0_* (DPP0) because the handoff assumed
     * HUBP0. If GOP set the display up on another instance, our writes land in
     * real registers on a DARK pipe -- which reads back perfectly and shows
     * nothing. Dump all four pipes so one boot settles it instead of one
     * hypothesis per 35-minute cycle.
     *
     * Offsets from dcn_4_1_0_offset.h, all BASE_IDX 2 (dmu_seg[2]).
     */
    {
        static const ULONG surf[4]   = { 0x060a, 0x06e6, 0x07c2, 0x089e };
        static const ULONG surfhi[4] = { 0x060b, 0x06e7, 0x07c3, 0x089f };
        static const ULONG curctl[4] = { 0x0679, 0x0755, 0x0831, 0x090d };
        static const ULONG dppctl[4] = { 0x0cc5, 0x0e30, 0x0f9b, 0x1106 };
        static const ULONG otgctl[4] = { 0x1b43, 0x1bc3, 0x1c43, 0x1cc3 };
        ULONG i;

        bug("[amdgpu-cursor] PIPE SURVEY (our fb MC 0x%08lx%08lx, cursor MC 0x%08lx%08lx):\n",
            (unsigned long)(fb_mc >> 32), (unsigned long)(ULONG)fb_mc,
            (unsigned long)(mc >> 32), (unsigned long)(ULONG)mc);
        for (i = 0; i < 4; i++) {
            ULONG sa = crd(ad, surf[i]), sah = crd(ad, surfhi[i]);
            ULONG cc = crd(ad, curctl[i]), dc = crd(ad, dppctl[i]);
            ULONG oc = crd(ad, otgctl[i]);
            bug("[amdgpu-cursor]   pipe%lu: SURF=%08lx:%08lx %s | CUR_CTL=%08lx en=%lu "
                "| DPP_CTL=%08lx clk_en=%lu | OTG_CTL=%08lx master_en=%lu\n",
                (unsigned long)i, (unsigned long)sah, (unsigned long)sa,
                (sah == (ULONG)(fb_mc >> 32) && sa == (ULONG)(fb_mc & 0xFFFFFFFFUL))
                    ? "<== OUR FRAMEBUFFER" : "",
                (unsigned long)cc, (unsigned long)(cc & 1UL),
                (unsigned long)dc, (unsigned long)((dc >> 4) & 1UL),
                (unsigned long)oc, (unsigned long)(oc & 1UL));
        }
    }

    /*
     * RECOUT is the cursor's destination rectangle on DCN4. We adopt a
     * GOP-configured pipe and have never touched the scaler, so if GOP left
     * recout at zero the cursor has nowhere to land -- invisible from every
     * cursor register, all of which read back correct.
     */
    {
        ULONG scl = crd(ad, regDSCL_SCL_MODE);
        ULONG rst = crd(ad, regDSCL_RECOUT_START);
        ULONG rsz = crd(ad, regDSCL_RECOUT_SIZE);
        ULONG rw  = rsz & 0xFFFFUL, rh = (rsz >> 16) & 0xFFFFUL;

        bug("[amdgpu-cursor] DSCL: SCL_MODE=%08lx RECOUT_START=%08lx (x=%lu y=%lu) "
            "RECOUT_SIZE=%08lx (w=%lu h=%lu) screen=%lux%lu\n",
            (unsigned long)scl, (unsigned long)rst,
            (unsigned long)(rst & 0xFFFFUL), (unsigned long)((rst >> 16) & 0xFFFFUL),
            (unsigned long)rsz, (unsigned long)rw, (unsigned long)rh,
            (unsigned long)scr_w, (unsigned long)scr_h);

        if (scr_w && scr_h && (rw < scr_w || rh < scr_h)) {
            cwr(ad, regDSCL_RECOUT_START, 0);
            cwr(ad, regDSCL_RECOUT_SIZE, (scr_w & 0xFFFFUL) | ((scr_h & 0xFFFFUL) << 16));
            bug("[amdgpu-cursor] DSCL: recout %lux%lu smaller than screen — programmed "
                "START=0 SIZE=%08lx; readback START=%08lx SIZE=%08lx\n",
                (unsigned long)rw, (unsigned long)rh,
                (unsigned long)((scr_w & 0xFFFFUL) | ((scr_h & 0xFFFFUL) << 16)),
                (unsigned long)crd(ad, regDSCL_RECOUT_START),
                (unsigned long)crd(ad, regDSCL_RECOUT_SIZE));
        } else {
            bug("[amdgpu-cursor] DSCL: recout already covers the screen — NOT the gap\n");
        }
    }

    cursor_build_image(ad);

    /*
     * PROVE the surface writes land. Until 5cd76c4c5b this region sat outside
     * the MapPCI window and every write was silently discarded while every
     * cursor register still read back correct -- the failure was undetectable
     * from the register state alone. Read the image back through the same
     * mapping and, separately, round-trip a sentinel. A surface that cannot
     * hold data explains an invisible cursor better than any register can.
     */
    {
        volatile ULONG *p = (volatile ULONG *)cursor_cpu;
        ULONG i, nonzero = 0, opaque = 0;
        ULONG sent_rb, saved = p[0];

        for (i = 0; i < AMDGPU_CURSOR_DIM * AMDGPU_CURSOR_DIM; i++) {
            ULONG v = p[i];
            if (v) nonzero++;
            if ((v >> 24) == 0xFF) opaque++;
        }

        p[0] = 0xDEADBEEFUL;
        sent_rb = p[0];
        p[0] = saved;

        bug("[amdgpu-cursor] SURFACE READBACK: %lu/%lu nonzero, %lu opaque(alpha=ff); "
            "sentinel wrote=deadbeef read=%08lx %s\n",
            (unsigned long)nonzero,
            (unsigned long)(AMDGPU_CURSOR_DIM * AMDGPU_CURSOR_DIM),
            (unsigned long)opaque, (unsigned long)sent_rb,
            (sent_rb == 0xDEADBEEFUL)
                ? "*** SURFACE IS WRITABLE ***"
                : "*** SURFACE DOES NOT HOLD WRITES — cursor cannot work ***");
    }

    /* Point the hardware at the image before enabling anything, so the first
     * composited frame cannot pick up whatever was previously at that address. */
#if AMDGPU_CURSOR_FB_SOURCE_TEST
    bug("[amdgpu-cursor] FB-SOURCE TEST: aiming the cursor at the LIVE FRAMEBUFFER "
        "MC 0x%08lx%08lx instead of our surface 0x%08lx%08lx — expect a 64x64 copy "
        "of the desktop's top-left corner at (320,240)\n",
        (unsigned long)(fb_mc >> 32), (unsigned long)(ULONG)fb_mc,
        (unsigned long)(mc >> 32), (unsigned long)(ULONG)mc);
    cwr(ad, regCURSOR_SURFACE_ADDRESS_HIGH, (ULONG)(fb_mc >> 32));
    cwr(ad, regCURSOR_SURFACE_ADDRESS,      (ULONG)(fb_mc & 0xFFFFFFFFUL));
#else
    cwr(ad, regCURSOR_SURFACE_ADDRESS_HIGH, (ULONG)(mc >> 32));
    cwr(ad, regCURSOR_SURFACE_ADDRESS,      (ULONG)(mc & 0xFFFFFFFFUL));
#endif

    size = ((ULONG)AMDGPU_CURSOR_DIM << CUR_SIZE_WIDTH_SHIFT)
         | ((ULONG)AMDGPU_CURSOR_DIM << CUR_SIZE_HEIGHT_SHIFT);
    cwr(ad, regCURSOR_SIZE, size);

    cwr(ad, regCURSOR_HOT_SPOT,   0);
    cwr(ad, regCURSOR_DST_OFFSET, 0);

    /* Schedule the cursor fetch. Missing this is why the first two attempts
     * programmed cleanly and displayed nothing. */
    cwr(ad, regHUBPREQ_CURSOR_SETTINGS,
        (0UL << CUR_SET_DST_Y_OFFSET_SHIFT) | (3UL << CUR_SET_CHUNK_HDL_ADJUST_SHIFT));
    {
        ULONG mall = crd(ad, regHUBP_DCHUBP_MALL_CONFIG);
        cwr(ad, regHUBP_DCHUBP_MALL_CONFIG, mall & ~(1UL << MALL_USE_FOR_CURSOR_SHIFT));
    }

    pos = ((ULONG)x << CUR_POS_X_SHIFT) | ((ULONG)y << CUR_POS_Y_SHIFT);
    cwr(ad, regCURSOR_POSITION, pos);

    /*
     * Force the cursor colour-space converter to BYPASS. We never programmed
     * CUR0_MATRIX_MODE, so it holds whatever GOP/VBIOS left; if it is in
     * SET_A/SET_B the cursor's pixels are run through stale coefficients that
     * can transform them to nothing -- which looks exactly like the failure we
     * have: every cursor register reads back correct and nothing appears.
     * Linux dpp401_program_cursor_csc() writes the whole register to 0 with
     * CUR0_MATRIX_MODE=CUR_MATRIX_BYPASS(0) for every RGB colour space.
     * Log the prior value so we learn whether it was actually the cause.
     */
    {
        ULONG mm_before = crd(ad, regCM_CUR0_CUR0_MATRIX_MODE);
        cwr(ad, regCM_CUR0_CUR0_MATRIX_MODE, 0);
        bug("[amdgpu-cursor]   CUR0_MATRIX_MODE before=%08lx -> wrote 0 (BYPASS) read=%08lx"
            " (mode=%lu current=%lu)\n",
            (unsigned long)mm_before,
            (unsigned long)crd(ad, regCM_CUR0_CUR0_MATRIX_MODE),
            (unsigned long)(mm_before & 3UL),
            (unsigned long)((mm_before >> 2) & 3UL));
    }

    /* DPP side first, then the HUBP enable last: the pixel path should be ready
     * to describe the cursor before the fetcher starts requesting it. */
    cwr(ad, regCM_CUR0_CURSOR0_CONTROL,
        (1UL << CM_CUR0_ENABLE_SHIFT)
      | ((ULONG)CURSOR_MODE_UN_PRE_MULT_ALPHA << CM_CUR0_MODE_SHIFT));

    ctl = (1UL << CUR_CTL_ENABLE_SHIFT)
        | (1UL << CUR_CTL_REQ_MODE_SHIFT)
        | ((ULONG)CURSOR_MODE_UN_PRE_MULT_ALPHA << CUR_CTL_MODE_SHIFT)
        | ((ULONG)CURSOR_PITCH_64_PIXELS        << CUR_CTL_PITCH_SHIFT)
        | ((ULONG)CURSOR_LINE_PER_CHUNK_8       << CUR_CTL_LINES_PER_CHUNK_SHIFT);
    cwr(ad, regCURSOR_CONTROL, ctl);

    /*
     * A GPU-as-reader verification belongs here and is deliberately NOT
     * reinstated yet. The first attempt was written backwards: it passed the
     * probe buffer as the SOURCE and the cursor surface as the DESTINATION, so
     * it copied zeros INTO the cursor and then "verified" a buffer the GPU had
     * never written. Its verdict ("GPU sees different memory") was meaningless.
     * amdgpu_sdma_blit_rect() only moves sysmem -> VRAM; reading VRAM back needs
     * the opposite direction, which that helper does not expose. Left out rather
     * than left in as a test that cannot fail correctly.
     */
    rb_ctl  = crd(ad, regCURSOR_CONTROL);
    bug("[amdgpu-cursor]   CM_CUR0 wrote=%08lx read=%08lx  ADDR_HIGH read=%08lx  "
        "HOT read=%08lx  DST_OFF read=%08lx  CROB_STATUS=%08lx\n",
        (unsigned long)((1UL << CM_CUR0_ENABLE_SHIFT)
                      | ((ULONG)CURSOR_MODE_UN_PRE_MULT_ALPHA << CM_CUR0_MODE_SHIFT)),
        (unsigned long)crd(ad, regCM_CUR0_CURSOR0_CONTROL),
        (unsigned long)crd(ad, regCURSOR_SURFACE_ADDRESS_HIGH),
        (unsigned long)crd(ad, regCURSOR_HOT_SPOT),
        (unsigned long)crd(ad, regCURSOR_DST_OFFSET),
        (unsigned long)crd(ad, regCURSOR_MEM_PWR_STATUS));
    bug("[amdgpu-cursor]   CURSOR_SETTINGS wrote=%08lx read=%08lx  MALL_CONFIG read=%08lx\n",
        (unsigned long)(3UL << CUR_SET_CHUNK_HDL_ADJUST_SHIFT),
        (unsigned long)crd(ad, regHUBPREQ_CURSOR_SETTINGS),
        (unsigned long)crd(ad, regHUBP_DCHUBP_MALL_CONFIG));
    rb_addr = crd(ad, regCURSOR_SURFACE_ADDRESS);
    rb_size = crd(ad, regCURSOR_SIZE);
    rb_pos  = crd(ad, regCURSOR_POSITION);

    bug("[amdgpu-cursor] %ux%u ARGB @MC 0x%08lx%08lx pos (%lu,%lu)\n",
        AMDGPU_CURSOR_DIM, AMDGPU_CURSOR_DIM,
        (unsigned long)(mc >> 32), (unsigned long)(ULONG)mc,
        (unsigned long)x, (unsigned long)y);
    bug("[amdgpu-cursor]   CONTROL wrote=%08lx read=%08lx  ADDR read=%08lx  "
        "SIZE wrote=%08lx read=%08lx  POS wrote=%08lx read=%08lx\n",
        (unsigned long)ctl, (unsigned long)rb_ctl, (unsigned long)rb_addr,
        (unsigned long)size, (unsigned long)rb_size,
        (unsigned long)pos, (unsigned long)rb_pos);
    amdgpu_cursor_composite_snapshot(ad, "init");

    if (rb_ctl == 0xFFFFFFFFUL || rb_size != size || rb_pos != pos) {
        bug("[amdgpu-cursor] registers did not stick — cursor NOT enabled\n");
        return TRUE;
    }
    bug("[amdgpu-cursor] *** HARDWARE CURSOR ENABLED — a 64x64 WHITE BLOCK with a black "
        "border should be visible at (%lu,%lu), composited by DCN with zero blits ***\n",
        (unsigned long)x, (unsigned long)y);
    return TRUE;
}
