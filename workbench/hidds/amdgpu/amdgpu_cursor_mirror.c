/* DCN4.1 native pointer acceptance stage, JOB-0084.
 * References: Linux f377d0025eb0 hubp32_cursor_set_attributes,
 * hubp401_cursor_set_position, dpp401_set_cursor_attributes/position and
 * mpc1_cursor_lock; dcn_4_1_0_offset.h/sh_mask.h. ADT Nouveau extracts BGRA32
 * on little endian. This does not advertise HW sprites or replace CursorFB.
 * The legacy amdgpu_cursor.c experiments remain dormant and are not called.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <devices/timer.h>
#include <exec/memory.h>
#include "amdgpu_intern.h"
#include "amdgpu_gfx_intern.h"

#define CUR_CONTROL 0x0679
#define CUR_ADDRESS 0x067a
#define CUR_ADDRESS_HIGH 0x067b
#define CUR_SIZE 0x067c
#define CUR_POSITION 0x067d
#define CUR_HOTSPOT 0x067e
#define CUR_STEREO 0x067f
#define CUR_DST_OFFSET 0x0680
#define CUR_POWER 0x0681
#define CUR_POWER_STATUS 0x0682
#define CUR_CM 0x0cf1
#define CUR_SCALE_G 0x0cf4
#define CUR_SCALE_RB 0x0cf5
#define CUR_MATRIX 0x0cf6
#define CUR_SETTINGS 0x0653
#define CUR_MALL 0x05f7
#define CUR_LOCK 0x02c5 /* dmu_seg[3], OPP0 */
#define FRAME_COUNT 0x1b4d
#define REF_FREQ_TO_PIX_FREQ 0x0654
#define REFCYC_PER_HTOTAL 0x063d
#define VRATIO_PREFETCH 0x063f
#define CUR_TTU 0x0627
#define CUR_TTU_PREFETCH 0x0628
#define HUB_GLOBAL_TIMER 0x0527
#define OTG_H_TOTAL 0x1b2a
#define OTG_V_TOTAL 0x1b2f
#define HUBPREQ_DEBUG_DB 0x05fc

/* Linux hubp32_cursor_set_attributes/hubp401_init/position own enable,
 * request mode, magnification, pixel mode, pitch and lines-per-chunk only.
 * dpp401 attributes/position own enable, expansion, ROM and pixel mode.
 * Preserve other fields as REG_UPDATE does: in particular CM bit2 is pixel
 * inversion, bit7 alpha modulation, and bit16 is read-only update pending.
 * None of these is an alternative cursor-enable bit to set speculatively.
 */
#define CUR_CONTROL_OWNED 0x1f030715UL
#define CUR_CM_OWNED      0x0000007bUL

/* Temporary cold37 discriminator: only the private x+64 hardware mirror gets
 * opaque white pixels. The real AROS/prefs bitmap in cursor->argb and the
 * superclass software pointer are untouched. Preserve dimensions/pitch/format
 * and every register setting from v8 so cold37 tested image contents alone.
 * Cold38 keeps this image unchanged while testing masked control updates.
 * Remove this calibration before handing pointer ownership to hardware.
 */
static ULONG mirror_pixel(const struct AmdgpuCursorMirror *cursor,
                           ULONG row, ULONG col)
{
    return row < cursor->height && col < cursor->width ? 0xffffffffUL : 0;
}

static void describe_source_image(const struct AmdgpuCursorMirror *cursor)
{
    ULONG row, col, zero = 0, partial = 0, opaque = 0;
    ULONG first = 0, min_alpha = 255, max_alpha = 0;
    if (cursor->shape_checks >= 16) return;
    for (row = 0; row < cursor->height; ++row)
        for (col = 0; col < cursor->width; ++col) {
            ULONG pixel = cursor->argb[row * AMDGPU_MIRROR_DIM + col];
            ULONG alpha = pixel >> 24;
            if (!alpha) ++zero;
            else {
                if (!partial && !opaque) first = pixel;
                if (alpha == 255) ++opaque;
                else ++partial;
                if (alpha < min_alpha) min_alpha = alpha;
                if (alpha > max_alpha) max_alpha = alpha;
            }
        }
    if (!partial && !opaque) min_alpha = 0;
    bug("[amdgpu-cursor] mirror-v9 source shape=%lux%lu alpha zero=%lu partial=%lu opaque=%lu nonzero_range=%lu..%lu first=%08lx; software source unchanged, hardware calibration=ffffffff\n",
        (unsigned long)cursor->width, (unsigned long)cursor->height,
        (unsigned long)zero, (unsigned long)partial, (unsigned long)opaque,
        (unsigned long)min_alpha, (unsigned long)max_alpha,
        (unsigned long)first);
}

static ULONG rd(struct AmdgpuDevice *ad, ULONG offset)
{
    return amdgpu_rreg32(ad, ad->dmu_seg[2] + offset);
}
static void wr(struct AmdgpuDevice *ad, ULONG offset, ULONG value)
{
    amdgpu_wreg32(ad, ad->dmu_seg[2] + offset, value);
}
/* hubp401_program_deadline puts DLG in mission mode before any deadlines;
 * hubp32_init uses the same exact value. The GOP framebuffer remaining visible
 * does not establish that its inherited cursor request scheduler is configured.
 * This is a documented direct operating-mode register, NOT a debug selector.
 * Only reached on the already-owned, validated unity pipe below. No clocks,
 * surface timing, routing, viewport or request-memory selector is changed.
 * Retain before/after evidence even if firmware already supplied mission mode:
 * an unchanged 0x100 explicitly rejects this missing-initialization hypothesis.
 */
static BOOL cursor_dlg_mode(struct AmdgpuDevice *ad,
                            struct AmdgpuCursorMirror *cursor)
{
    ULONG before, after;
    if (cursor->dlg_mode_checked) return TRUE;
    before = rd(ad, HUBPREQ_DEBUG_DB);
    if (before == 0xffffffffUL) return FALSE;
    if (before != 0x00000100UL) wr(ad, HUBPREQ_DEBUG_DB, 0x00000100UL);
    after = rd(ad, HUBPREQ_DEBUG_DB);
    bug("[amdgpu-cursor] mirror-v7 DLG mission mode before=%08lx after=%08lx changed=%lu; visibility unproven\n",
        (unsigned long)before, (unsigned long)after,
        (unsigned long)(before != 0x00000100UL));
    if (after != 0x00000100UL) return FALSE;
    cursor->dlg_mode_checked = TRUE;
    return TRUE;
}

/* Direct sequential observations of request progression and shared DLG state.
 * REG0 exposes the request scheduler VTG count; REG2 includes cursor urgency,
 * self-refresh and pipe recovery/flush state. No W1C acknowledgement, indirect
 * selector, global-clock change or sampling loop is used. A snapshot cannot
 * prove cursor fetch, but separates no scheduling from composition suspects.
 */
static void cursor_request_snapshot(struct AmdgpuDevice *ad)
{
    bug("[amdgpu-cursor] mirror-v7 request debug=%08lx status=%08lx/%08lx/%08lx pstate=%08lx blank=%08lx/%08lx after=%08lx prefetch=%08lx globalttu=%08lx qos=%08lx frame=%08lx\n",
        (unsigned long)rd(ad, HUBPREQ_DEBUG_DB),
        (unsigned long)rd(ad, 0x0661), (unsigned long)rd(ad, 0x0662),
        (unsigned long)rd(ad, 0x0663), (unsigned long)rd(ad, 0x0660),
        (unsigned long)rd(ad, 0x063b), (unsigned long)rd(ad, 0x063c),
        (unsigned long)rd(ad, 0x063e), (unsigned long)rd(ad, 0x0641),
        (unsigned long)rd(ad, 0x0622), (unsigned long)rd(ad, 0x0621),
        (unsigned long)rd(ad, FRAME_COUNT));
}
static void cursor_lock(struct AmdgpuDevice *ad, BOOL lock)
{
    amdgpu_wreg32(ad, ad->dmu_seg[3] + CUR_LOCK, lock ? 1 : 0);
}
static UQUAD ticks(struct AmdgpuCursorMirror *cursor)
{
    struct Device *TimerBase = cursor->timer;
    struct EClockVal t;
    ReadEClock(&t);
    return ((UQUAD)t.ev_hi << 32) | t.ev_lo;
}
/* Five of cold35's six post-frame samples observed VBLANK1. Compare DLG status
 * against the independent OTG raster during three active-line windows instead
 * of inferring "no fetch" from blank. Linux optc1_get_position and DCN4.1
 * STATUS_POSITION define the low15-bit vertical counter used here.
 *
 * Collect before logging: UART output between reads would destroy the phase
 * relationship. Only the first six shapes are sampled. No register writes,
 * selector changes, clock changes or tests of a replacement loaded in RAM.
 * A100ms wall deadline and finite poll count bound the observation even if
 * either counter stops. Missing windows are diagnostic, not a cursor failure;
 * unreadable MMIO does stop further cursor writes via the caller.
 */
static BOOL cursor_phase_snapshot(struct AmdgpuDevice *ad,
                                   struct AmdgpuCursorMirror *cursor)
{
    struct CursorPhase {
        ULONG before, req0, req1, req2, after, frame;
    } samples[3];
    ULONG blank, end, height, target, raw, line, count = 0, polls = 0, i;
    UQUAD start, limit;
    BOOL readable = TRUE;

    blank = rd(ad, 0x1b38); /* OTG_V_BLANK_START_END */
    if (blank == 0xffffffffUL) return FALSE;
    end = (blank >> 16) & 0x7fffUL;
    height = blank & 0x7fffUL;
    if (height <= end || height > cursor->clock_vtotal || height - end < 128)
        return TRUE;
    height -= end;
    start = ticks(cursor);
    limit = ((UQUAD)cursor->frequency + 9) / 10;
    while (count < 3 && polls++ < 2000000) {
        if (ticks(cursor) - start >= limit) break;
        raw = rd(ad, 0x1b4a); /* OTG_STATUS_POSITION */
        if (raw == 0xffffffffUL) { readable = FALSE; break; }
        line = raw & 0x7fffUL;
        target = end + height * (count + 1) / 4;
        if (line >= target && line < target + 8) {
            struct CursorPhase *s = &samples[count];
            s->before = raw;
            s->req0 = rd(ad, 0x0661);
            s->req1 = rd(ad, 0x0662);
            s->req2 = rd(ad, 0x0663);
            s->after = rd(ad, 0x1b4a);
            s->frame = rd(ad, FRAME_COUNT);
            if (s->req0 == 0xffffffffUL || s->req1 == 0xffffffffUL ||
                s->req2 == 0xffffffffUL || s->after == 0xffffffffUL ||
                s->frame == 0xffffffffUL) { readable = FALSE; break; }
            /* Reject a delayed read that left the requested active window. */
            line = s->after & 0x7fffUL;
            if (line >= target && line < target + 8) ++count;
        }
        __asm__ __volatile__("pause");
    }
    for (i = 0; i < count; ++i) {
        struct CursorPhase *s = &samples[i];
        bug("[amdgpu-cursor] mirror-v8 active shape=%lu phase=%lu OTG=%08lx/%08lx status=%08lx/%08lx/%08lx frame=%08lx; fetch not proven\n",
            (unsigned long)cursor->shape_checks, (unsigned long)(i + 1),
            (unsigned long)s->before, (unsigned long)s->after,
            (unsigned long)s->req0, (unsigned long)s->req1,
            (unsigned long)s->req2, (unsigned long)s->frame);
    }
    if (count != 3)
        bug("[amdgpu-cursor] mirror-v8 active sampling incomplete=%lu/3 readable=%ld polls=%lu; no timing changes\n",
            (unsigned long)count, (long)readable, (unsigned long)polls);
    return readable;
}
static void hide(struct AmdgpuDevice *ad)
{
    cursor_lock(ad, TRUE);
    wr(ad, CUR_CONTROL, rd(ad, CUR_CONTROL) & ~1UL);
    wr(ad, CUR_CM, rd(ad, CUR_CM) & ~1UL);
    cursor_lock(ad, FALSE);
}
/* Driver-owned, permanently retained scratch; no framebuffer/pool lock is held
 * here. Display -> SDMA is the same ordering used by guarded bitmap transfers.
 * Compare the actual GPU-addressed bytes, including transparent pitch padding,
 * rather than treating the CPU stores or the address-register echo as evidence.
 * This proves SDMA can read the image, NOT that DCN fetched or composed it.
 * The existing unretired-DMA policy retains caller ownership until reboot.
 */
static BOOL verify_shape(struct AmdgpuDevice *ad, struct AmdgpuCursorMirror *cursor)
{
    ULONG row, col, bad = 0, first = 0xffffffffUL;
    ULONG expected_hash = 2166136261UL, actual_hash = 2166136261UL;
    if (cursor->shape_checks >= 16) return TRUE;
    ++cursor->shape_checks;
    if (!amdgpu_sdma_download_rect(ad, cursor->mc, AMDGPU_MIRROR_DIM * 4,
            cursor->readback, AMDGPU_MIRROR_DIM * 4,
            AMDGPU_MIRROR_DIM * 4, AMDGPU_MIRROR_DIM, AMDGPU_XFER_RAW)) {
        bug("[amdgpu-cursor] mirror-v6 image readback unavailable; software retained\n");
        return FALSE;
    }
    for (row = 0; row < AMDGPU_MIRROR_DIM; ++row)
        for (col = 0; col < AMDGPU_MIRROR_DIM; ++col) {
            ULONG i = row * AMDGPU_MIRROR_DIM + col;
            ULONG want = mirror_pixel(cursor, row, col);
            ULONG got = cursor->readback[i];
            expected_hash = (expected_hash ^ want) * 16777619UL;
            actual_hash = (actual_hash ^ got) * 16777619UL;
            if (got != want) {
                if (!bad) first = i;
                ++bad;
            }
        }
    bug("[amdgpu-cursor] mirror-v6 SDMA image check=%lu mc=%08lx:%08lx pixels=4096 bad=%lu first=%08lx hash=%08lx/%08lx (memory proof, not DCN fetch)\n",
        (unsigned long)cursor->shape_checks,
        (unsigned long)(cursor->mc >> 32), (unsigned long)(ULONG)cursor->mc,
        (unsigned long)bad, (unsigned long)first,
        (unsigned long)actual_hash, (unsigned long)expected_hash);
    return bad == 0;
}
static void observe_visibility(struct AmdgpuDevice *ad,
                                struct AmdgpuCursorMirror *cursor,
                                const char *reason)
{
    /* Bound serial overhead and retain the actual final enable bits after both
     * hide and show. A mouse move that leaves enable unchanged is not logged. */
    if (cursor->visibility_reports++ >= 16) return;
    bug("[amdgpu-cursor] mirror-v6 visibility %s control=%08lx cm=%08lx position=%08lx frame=%08lx\n",
        reason, (unsigned long)rd(ad, CUR_CONTROL),
        (unsigned long)rd(ad, CUR_CM), (unsigned long)rd(ad, CUR_POSITION),
        (unsigned long)rd(ad, FRAME_COUNT));
}
/* Fixed direct read-only configuration/status observations. No debug selectors,
 * W1C acknowledgements or routing changes. Called only for a published shape,
 * never on every mouse move. A sequential snapshot is not fetch/pixel proof. */
static void cursor_route_snapshot(struct AmdgpuDevice *ad)
{
    ULONG i, base;
    bug("[amdgpu-cursor] mirror-v5 route seg2=%05lx seg3=%05lx HUBP=%08lx clock=%08lx vmpg=%08lx vm=%08lx return=%08lx DPP=%08lx scale=%08lx/%08lx matrix=%08lx\n",
        (unsigned long)ad->dmu_seg[2], (unsigned long)ad->dmu_seg[3],
        (unsigned long)rd(ad, 0x05f4), (unsigned long)rd(ad, 0x05f5),
        (unsigned long)rd(ad, 0x05f6), (unsigned long)rd(ad, 0x063a),
        (unsigned long)rd(ad, 0x066d), (unsigned long)rd(ad, 0x0cc5),
        (unsigned long)rd(ad, CUR_SCALE_G), (unsigned long)rd(ad, CUR_SCALE_RB),
        (unsigned long)rd(ad, CUR_MATRIX));
    for (i = 0; i < 4; ++i) {
        base = ad->dmu_seg[3] + i * 0x15;
        bug("[amdgpu-cursor] mirror-v5 MPCC%lu top=%08lx bottom=%08lx opp=%08lx control=%08lx status=%08lx OUT%lu=%08lx\n",
            (unsigned long)i,
            (unsigned long)amdgpu_rreg32(ad, base),
            (unsigned long)amdgpu_rreg32(ad, base + 1),
            (unsigned long)amdgpu_rreg32(ad, base + 2),
            (unsigned long)amdgpu_rreg32(ad, base + 3),
            (unsigned long)amdgpu_rreg32(ad, base + 0x0e),
            (unsigned long)i,
            (unsigned long)amdgpu_rreg32(ad, ad->dmu_seg[3] + 0x02f2 + i * 4));
    }
}
static UWORD rom16(const UBYTE *rom, ULONG off)
{
    return rom[off] | ((UWORD)rom[off + 1] << 8);
}
static BOOL rom_range(struct AmdgpuDevice *ad, ULONG off, ULONG size)
{
    return off && off < ad->rom_size && size <= ad->rom_size - off;
}
/* Read the already-copied ROM only. No ATOM execution or PCI ROM mapping.
 * atomfirmware v2.1 master entry27 is DCE info; versions4.1..4.5 share the
 * reference-clock field at+12, in10kHz. Linux bios_parser2/dccg401/hubbub2
 * obtains HUBBUB's clock from this crystal and the live global timer divider.
 */
static ULONG cursor_ref_khz(struct AmdgpuDevice *ad, ULONG timer)
{
    const UBYTE *rom = ad->rom_base;
    ULONG header, master, dce, size, ref;
    if (!rom || ad->rom_size < 0x4a || rom[0] != 0x55 || rom[1] != 0xaa)
        return 0;
    header = rom16(rom, 0x48);
    if (!rom_range(ad, header, 0x22) || rom16(rom, header) < 0x22 ||
        rom[header + 4] != 'A' || rom[header + 5] != 'T' ||
        rom[header + 6] != 'O' || rom[header + 7] != 'M') return 0;
    master = rom16(rom, header + 0x20);
    if (!rom_range(ad, master, 60) || rom16(rom, master) < 60 ||
        rom[master + 2] != 2 || rom[master + 3] != 1) return 0;
    dce = rom16(rom, master + 4 + 27 * 2);
    if (!rom_range(ad, dce, 14)) return 0;
    size = rom16(rom, dce);
    if (size < 14 || !rom_range(ad, dce, size) || rom[dce + 2] != 4 ||
        rom[dce + 3] < 1 || rom[dce + 3] > 5) return 0;
    ref = (ULONG)rom16(rom, dce + 12) * 10;
    bug("[amdgpu-cursor] mirror-v4 DCE=%lu.%lu crystal=%lukHz global_timer=%08lx\n",
        (unsigned long)rom[dce + 2], (unsigned long)rom[dce + 3],
        (unsigned long)ref, (unsigned long)timer);
    if (!(timer & 0x1000) || (timer & 15) > 2) return 0;
    if ((timer & 15) == 2) ref /= 2;
    return ref >= 40000 && ref <= 60000 ? ref : 0;
}
/* Two independent two-frame intervals, with a preceding boundary to avoid
 * measuring a partial frame. No logs inside the bounded sampling loop.
 * Inherited HUBP deadlines were inconsistent on cold29; do not use either
 * one to infer the actual pixel clock. No OTG/clock registers are changed.
 */
static int measure_clock(struct AmdgpuDevice *ad, struct AmdgpuCursorMirror *cursor,
                         ULONG htotal, ULONG vtotal, ULONG timer)
{
    ULONG ref = cursor_ref_khz(ad, timer), raw, first, last, advances = 0;
    ULONG polls = 0, pixel[2], delta, after_h, after_v, after_timer, after_control;
    UQUAD begin, now, stamp[3] = {0, 0, 0}, limit, measured[2];
    if (!ref) return 0;
    raw = rd(ad, FRAME_COUNT);
    if (raw == 0xffffffffUL) return -1;
    first = last = raw & 0xffffffUL;
    begin = ticks(cursor);
    limit = ((UQUAD)cursor->frequency + 3) / 4;
    while (advances < 5) {
        raw = rd(ad, FRAME_COUNT);
        if (raw == 0xffffffffUL) return -1;
        now = ticks(cursor);
        raw &= 0xffffffUL;
        if (raw != last) {
            if (((raw - last) & 0xffffffUL) != 1) return 0;
            advances = (raw - first) & 0xffffffUL;
            if (advances == 1) stamp[0] = now;
            if (advances == 3) stamp[1] = now;
            if (advances == 5) stamp[2] = now;
            last = raw;
        }
        if (now - begin >= limit || ++polls >= 4000000) return 0;
        __asm__ __volatile__("pause");
    }
    if (!stamp[0] || stamp[1] <= stamp[0] || stamp[2] <= stamp[1]) return 0;
    measured[0] = (UQUAD)htotal * vtotal * 2 * cursor->frequency /
                   (stamp[1] - stamp[0]);
    measured[1] = (UQUAD)htotal * vtotal * 2 * cursor->frequency /
                   (stamp[2] - stamp[1]);
    if (measured[0] < 25000000 || measured[0] > 300000000 ||
        measured[1] < 25000000 || measured[1] > 300000000) return 0;
    pixel[0] = (ULONG)measured[0]; pixel[1] = (ULONG)measured[1];
    delta = pixel[0] > pixel[1] ? pixel[0] - pixel[1] : pixel[1] - pixel[0];
    if (delta > pixel[0] / 100) return 0;
    after_h = rd(ad, OTG_H_TOTAL); after_v = rd(ad, OTG_V_TOTAL);
    after_timer = rd(ad, HUB_GLOBAL_TIMER); after_control = rd(ad, 0x1b43);
    if (after_h == 0xffffffffUL || after_v == 0xffffffffUL ||
        after_timer == 0xffffffffUL || after_control == 0xffffffffUL) return -1;
    if (after_h != htotal - 1 || after_v != vtotal - 1 ||
        after_timer != timer || !(after_control & 1)) return 0;
    cursor->measured_ratio = (ULONG)(((UQUAD)ref * 1000 << 19) /
                                     (((UQUAD)pixel[0] + pixel[1]) / 2));
    if (!cursor->measured_ratio || cursor->measured_ratio > 0x001fffffUL)
        return 0;
    cursor->clock_htotal = htotal;
    cursor->clock_vtotal = vtotal;
    cursor->clock_timer = timer;
    bug("[amdgpu-cursor] mirror-v4 measured ref=%lukHz pixel=%lu/%luHz totals=%lux%lu ratioQ19=%lu; cursor-only deadlines\n",
        (unsigned long)ref, (unsigned long)pixel[0], (unsigned long)pixel[1],
        (unsigned long)htotal, (unsigned long)vtotal,
        (unsigned long)cursor->measured_ratio);
    return 1;
}
/* Linux dml2_0 CalculatePixelDeliveryTimes / rq_dlg_calc: with RGB unity
 * scaling and VRatioPrefetch <= 1, a 256-byte cursor request is delivered in
 * width / pixel_clock / requests reference-clock cycles. The measured clock
 * ratio is Q19; cursor TTU is Q10. The ratio now comes from measured OTG
 * timing and firmware/HUBBUB clock metadata, not stale inherited deadlines.
 * Scaled / DPP-clock-limited paths need a complete DML clock model and are
 * deliberately not enabled by this single-pipe acceptance adapter.
 * Returns -1 on unreadable MMIO (caller must not issue further writes).
 */
static int cursor_schedule(struct AmdgpuDevice *ad,
                           struct AmdgpuCursorMirror *cursor,
                           ULONG recout, ULONG *ratio, ULONG *delivery)
{
    ULONG raw_ratio = rd(ad, REF_FREQ_TO_PIX_FREQ);
    ULONG raw_htotal = rd(ad, OTG_H_TOTAL);
    ULONG raw_vtotal = rd(ad, OTG_V_TOTAL), timer = rd(ad, HUB_GLOBAL_TIMER);
    ULONG raw_cycles = rd(ad, REFCYC_PER_HTOTAL);
    ULONG raw_prefetch = rd(ad, VRATIO_PREFETCH);
    ULONG viewport = rd(ad, 0x05eb), scaler = rd(ad, 0x0d08);
    ULONG htotal, vtotal, requests;
    int measured;
    if (raw_ratio == 0xffffffffUL || raw_htotal == 0xffffffffUL ||
        raw_cycles == 0xffffffffUL || raw_prefetch == 0xffffffffUL ||
        viewport == 0xffffffffUL || scaler == 0xffffffffUL ||
        raw_vtotal == 0xffffffffUL || timer == 0xffffffffUL)
        return -1;
    *ratio = 0; *delivery = 0;
    htotal = (raw_htotal & 0x7fffUL) + 1;
    vtotal = (raw_vtotal & 0x7fffUL) + 1;
    if ((scaler & 7) != 0 ||
        (viewport & 0x3fff3fffUL) != (recout & 0x3fff3fffUL) ||
        (raw_prefetch & 0x003fffffUL) > (1UL << 19)) {
        if (!cursor->timing_warned) {
            bug("[amdgpu-cursor] mirror-v4 scheduling withheld inherited_ratio=%08lx htotal=%lu cycles=%08lx prefetch=%08lx scaler=%08lx viewport=%08lx recout=%08lx; software retained\n",
                (unsigned long)raw_ratio, (unsigned long)htotal,
                (unsigned long)raw_cycles,
                (unsigned long)raw_prefetch, (unsigned long)scaler,
                (unsigned long)viewport, (unsigned long)recout);
            cursor->timing_warned = TRUE;
        }
        return 0;
    }
    if (!cursor_dlg_mode(ad, cursor)) {
        cursor->failed = TRUE;
        bug("[amdgpu-cursor] mirror-v7 DLG mode unavailable/rejected; software retained, no further cursor writes\n");
        return -1;
    }
    if (!cursor->measured_ratio || cursor->clock_htotal != htotal ||
        cursor->clock_vtotal != vtotal || cursor->clock_timer != timer) {
        if (cursor->started) hide(ad);
        measured = measure_clock(ad, cursor, htotal, vtotal, timer);
        if (measured != 1) {
            cursor->failed = TRUE;
            bug("[amdgpu-cursor] mirror-v4 clock validation failed=%ld totals=%lux%lu timer=%08lx; software retained\n",
                (long)measured, (unsigned long)htotal, (unsigned long)vtotal,
                (unsigned long)timer);
            return measured;
        }
        bug("[amdgpu-cursor] mirror-v4 inherited ratio=%08lx cycles=%08lx not used for cursor timing\n",
            (unsigned long)raw_ratio, (unsigned long)raw_cycles);
    }
    *ratio = cursor->measured_ratio;
    requests = (cursor->width * 32 + 2047) / 2048;
    *delivery = requests ? (ULONG)(((UQUAD)cursor->width * *ratio) /
                                    ((UQUAD)requests * 512)) : 0;
    if (!*delivery || *delivery > 0x007fffffUL) return 0;
    cursor->timing_warned = FALSE;
    return 1;
}
/* Shape memory is permanently reserved while the pinned driver is loaded.
 * After disabling fetch, allow two real frames before overwriting it. Failure
 * retains the allocation, disables this probe and leaves CursorFB untouched. */
static BOOL retire_shape(struct AmdgpuDevice *ad, struct AmdgpuCursorMirror *cursor)
{
    ULONG raw = rd(ad, FRAME_COUNT), frame, polls = 0;
    UQUAD start = ticks(cursor), limit = ((UQUAD)cursor->frequency + 4) / 5;
    if (raw == 0xffffffffUL) return FALSE;
    frame = raw & 0x00ffffffUL;
    while (TRUE) {
        raw = rd(ad, FRAME_COUNT);
        if (raw == 0xffffffffUL) return FALSE;
        if (((raw - frame) & 0x00ffffffUL) >= 2) return TRUE;
        if (ticks(cursor) - start >= limit || ++polls >= 2000000) return FALSE;
        __asm__ __volatile__("pause");
    }
}

void amdgpu_cursor_mirror_prepare(struct AmdgpuGfx_staticdata *xsd)
{
    struct AmdgpuDevice *ad = xsd->device;
    struct AmdgpuCursorMirror *cursor;
    struct Device *TimerBase;
    struct EClockVal t;
    /* Same exact silicon/pipe0 boundary as the proven native plane adapter. */
    if (!ad || ad->vendorid != 0x1002 || ad->deviceid != 0x7590 ||
        ad->revision != 0xc0 || !ad->rmmio || !ad->ip.dmu.valid ||
        !ad->dmu_seg[2] || !ad->dmu_seg[3] || !xsd->data.vram_pool_size ||
        ((UQUAD)ad->dmu_seg[2] + FRAME_COUNT + 1) * 4 > ad->rmmio_size ||
        ((UQUAD)ad->dmu_seg[3] + 0x02fe + 1) * 4 > ad->rmmio_size)
        return;
    cursor = AllocVec(sizeof(*cursor), MEMF_ANY | MEMF_CLEAR);
    if (!cursor) return;
    cursor->port = CreateMsgPort();
    if (cursor->port)
        cursor->request = (struct timerequest *)CreateIORequest(cursor->port,
                                                               sizeof(*cursor->request));
    if (!cursor->request || OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                                       (struct IORequest *)cursor->request, 0)) goto fail;
    cursor->timer = cursor->request->tr_node.io_Device;
    TimerBase = cursor->timer;
    cursor->frequency = ReadEClock(&t);
    if (!cursor->frequency || !amdgpu_vram_alloc(&xsd->data, AMDGPU_MIRROR_BYTES,
                                                256, &cursor->cpu, &cursor->mc)) goto fail;
    xsd->cursor_mirror = cursor;
    bug("[amdgpu-cursor] mirror-v10 prepared cpu=%p mc=%08lx:%08lx; masked control publication, unchanged opaque-white private calibration; real AROS software pointer retained, hardware at x+64 only after AMD screen selection\n",
        cursor->cpu, (unsigned long)(cursor->mc >> 32), (unsigned long)(ULONG)cursor->mc);
    return;
fail:
    if (cursor->timer) CloseDevice((struct IORequest *)cursor->request);
    if (cursor->request) DeleteIORequest((struct IORequest *)cursor->request);
    if (cursor->port) DeleteMsgPort(cursor->port);
    FreeVec(cursor);
}

/* Caller holds display_lock. Never called under framebufferlock/pool_lock. */
void amdgpu_cursor_mirror_update(struct AmdgpuGfx_staticdata *xsd, BOOL visible)
{
    struct AmdgpuCursorMirror *cursor = xsd->cursor_mirror;
    struct AmdgpuDevice *ad = xsd->device;
    struct HWData *hw = &xsd->data;
    LONG x, y, hotx = 0, hoty = 0;
    ULONG recout, origin, rw, rh, control, ratio, delivery, dst_offset, prior_enable;
    ULONG before_control, before_cm, cm;
    int schedule;
    BOOL published_shape = FALSE;
    UQUAD live;
    if (!cursor || cursor->failed) return;
    if (!visible || !cursor->valid || !xsd->visible || hw->plane_faulted ||
        xsd->cursor_update_depth) {
        if (cursor->started) {
            prior_enable = rd(ad, CUR_CONTROL) & 1;
            hide(ad);
            if (prior_enable) observe_visibility(ad, cursor, "hidden/request-state");
        }
        return;
    }
    live = ((UQUAD)rd(ad, 0x060b) << 32) | rd(ad, 0x060a);
    if (live != hw->scanout_active_mc || live != hw->fb_mc ||
        !(rd(ad, 0x1b43) & 1)) {
        if (cursor->started) {
            prior_enable = rd(ad, CUR_CONTROL) & 1;
            hide(ad);
            if (prior_enable) observe_visibility(ad, cursor, "hidden/scanout-state");
        }
        return;
    }
    origin = rd(ad, 0x0d1e);
    recout = rd(ad, 0x0d1f);
    rw = recout & 0xffff; rh = recout >> 16;
    x = cursor->x + cursor->xoffset + 64 - (LONG)(origin & 0xffff);
    y = cursor->y + cursor->yoffset - (LONG)(origin >> 16);
    if (!rw || !rh || x >= (LONG)rw || y >= (LONG)rh ||
        x <= -(LONG)cursor->width || y <= -(LONG)cursor->height) {
        if (cursor->started) {
            prior_enable = rd(ad, CUR_CONTROL) & 1;
            hide(ad);
            if (prior_enable) observe_visibility(ad, cursor, "hidden/offscreen");
        }
        return;
    }
    if (x < 0) { hotx = -x; x = 0; }
    if (y < 0) { hoty = -y; y = 0; }
    schedule = cursor_schedule(ad, cursor, recout, &ratio, &delivery);
    if (schedule < 0) {
        cursor->failed = TRUE;
        bug("[amdgpu-cursor] mirror-v4 MMIO unreadable; retained storage, no further cursor writes\n");
        return;
    }
    /* hubp401_set_cursor_position: reference cycles before the visible X.
     * The unity viewport/scaler gate above removes both scaling divisors. */
    dst_offset = (ULONG)(((UQUAD)(x > hotx ? x - hotx : 0) * ratio) >> 19);
    if (!schedule || dst_offset > 0x3fffUL) {
        if (cursor->started) hide(ad);
        return;
    }
    if (cursor->dirty || !cursor->started) {
        ULONG row, col, alpha = 0, power, power_after;
        if (cursor->started) {
            hide(ad);
            if (!retire_shape(ad, cursor)) {
                cursor->failed = TRUE;
                bug("[amdgpu-cursor] mirror-v4 retirement TIMEOUT; retained storage, software cursor unchanged\n");
                return;
            }
        }
        /* DCN4.1 cursor memory control is 0681; 067f is stereo control.
         * Preserve LS/reserved fields, clear FORCE and disable ROB gating.
         * Reject an unreadable/rejected control before publishing an image.
         * Status is logged after real frame boundaries below; its encoding
         * alone is not treated as proof that pixels were fetched/composed. */
        power = rd(ad, CUR_POWER);
        if (power == 0xffffffffUL) {
            cursor->failed = TRUE;
            return;
        }
        wr(ad, CUR_POWER, (power & ~7UL) | 4);
        power_after = rd(ad, CUR_POWER);
        bug("[amdgpu-cursor] mirror-v4 memory power@0681=%08lx->%08lx status@0682=%08lx stereo@067f=%08lx (not written)\n",
            (unsigned long)power, (unsigned long)power_after,
            (unsigned long)rd(ad, CUR_POWER_STATUS),
            (unsigned long)rd(ad, CUR_STEREO));
        if (power_after == 0xffffffffUL || (power_after & 7) != 4) {
            cursor->failed = TRUE;
            bug("[amdgpu-cursor] mirror-v4 memory power readback rejected; software retained\n");
            return;
        }
        describe_source_image(cursor);
        for (row = 0; row < AMDGPU_MIRROR_DIM; ++row)
            for (col = 0; col < AMDGPU_MIRROR_DIM; ++col) {
                ULONG pixel = mirror_pixel(cursor, row, col);
                ((volatile ULONG *)cursor->cpu)[row * AMDGPU_MIRROR_DIM + col] = pixel;
                if (pixel >> 24) ++alpha;
            }
        __asm__ __volatile__("sfence" ::: "memory");
        if (!verify_shape(ad, cursor)) {
            cursor->failed = TRUE;
            return;
        }
        bug("[amdgpu-cursor] mirror-v5 inherited scale=%08lx/%08lx; setting FP16 unity/zero bias (not a visibility claim)\n",
            (unsigned long)rd(ad, CUR_SCALE_G), (unsigned long)rd(ad, CUR_SCALE_RB));
        cursor_lock(ad, TRUE);
        wr(ad, CUR_ADDRESS_HIGH, (ULONG)(cursor->mc >> 32));
        wr(ad, CUR_ADDRESS, (ULONG)cursor->mc);
        wr(ad, CUR_SIZE, (cursor->width << 16) | cursor->height);
        wr(ad, CUR_SETTINGS, 3UL << 8);
        wr(ad, CUR_MALL, rd(ad, CUR_MALL) & ~4UL);
        wr(ad, CUR_MATRIX, 0); /* RGB CSC bypass */
        /* dcn10_set_cursor_sdr_white_level -> dpp401 optional attributes:
         * FP16 1.0 = 0x3c00, bias = 0, for both G and R/B. GOP need not have
         * initialized the separate cursor color path used by DCN4.1. */
        wr(ad, CUR_SCALE_G, 0x00003c00UL);
        wr(ad, CUR_SCALE_RB, 0x00003c00UL);
        cursor_lock(ad, FALSE);
        if (rd(ad, CUR_SCALE_G) != 0x00003c00UL ||
            rd(ad, CUR_SCALE_RB) != 0x00003c00UL) {
            hide(ad);
            cursor->failed = TRUE;
            bug("[amdgpu-cursor] mirror-v5 unity scale rejected; software retained\n");
            return;
        }
        cursor->dirty = FALSE;
        cursor->started = TRUE;
        published_shape = TRUE;
        bug("[amdgpu-cursor] mirror-v4 shape=%lux%lu alpha=%lu offset=%ld,%ld TTU=%08lx/%08lx MPC=%08lx lock=%08lx\n",
            (unsigned long)cursor->width, (unsigned long)cursor->height,
            (unsigned long)alpha, (long)cursor->xoffset, (long)cursor->yoffset,
            (unsigned long)rd(ad, 0x0627), (unsigned long)rd(ad, 0x0628),
            (unsigned long)amdgpu_rreg32(ad, ad->dmu_seg[3] + 0x02f2),
            (unsigned long)amdgpu_rreg32(ad, ad->dmu_seg[3] + CUR_LOCK));
    }
    if (cursor->ratio_q19 != ratio || cursor->delivery_q10 != delivery) {
        ULONG ttu, prefetch;
        cursor_lock(ad, TRUE);
        ttu = (rd(ad, CUR_TTU) & ~0x1f7fffffUL) | delivery | (8UL << 24);
        prefetch = (rd(ad, CUR_TTU_PREFETCH) & ~0x007fffffUL) | delivery;
        wr(ad, CUR_TTU, ttu);
        wr(ad, CUR_TTU_PREFETCH, prefetch);
        cursor_lock(ad, FALSE);
        ttu = rd(ad, CUR_TTU);
        prefetch = rd(ad, CUR_TTU_PREFETCH);
        if ((ttu & 0x1f7fffffUL) != (delivery | (8UL << 24)) ||
            (prefetch & 0x007fffffUL) != delivery) {
            hide(ad);
            cursor->failed = TRUE;
            bug("[amdgpu-cursor] mirror-v4 TTU readback rejected %08lx/%08lx; software retained\n",
                (unsigned long)ttu, (unsigned long)prefetch);
            return;
        }
        cursor->ratio_q19 = ratio;
        cursor->delivery_q10 = delivery;
        bug("[amdgpu-cursor] mirror-v4 scheduling ratioQ19=%lu requestQ10=%lu TTU=%08lx/%08lx; live unity timing, QoS8\n",
            (unsigned long)ratio, (unsigned long)delivery,
            (unsigned long)ttu, (unsigned long)prefetch);
    }
    before_control = rd(ad, CUR_CONTROL);
    before_cm = rd(ad, CUR_CM);
    if (before_control == 0xffffffffUL || before_cm == 0xffffffffUL) {
        cursor->failed = TRUE;
        bug("[amdgpu-cursor] mirror-v10 unreadable controls; no further cursor writes\n");
        return;
    }
    control = (before_control & ~CUR_CONTROL_OWNED) | 1 | 4 | (3UL << 8) |
              ((cursor->width <= 32 ? 4UL : 3UL) << 24);
    cm = (before_cm & ~CUR_CM_OWNED) | 1 | (3UL << 4);
    prior_enable = before_control & 1;
    cursor_lock(ad, TRUE);
    wr(ad, CUR_POSITION, ((ULONG)x << 15) | (ULONG)y);
    wr(ad, CUR_HOTSPOT, ((ULONG)hotx << 16) | (ULONG)hoty);
    wr(ad, CUR_DST_OFFSET, (rd(ad, CUR_DST_OFFSET) & ~0x3fffUL) | dst_offset);
    wr(ad, CUR_CM, cm);
    wr(ad, CUR_CONTROL, control);
    cursor_lock(ad, FALSE);
    if (published_shape && cursor->reports < 8)
        bug("[amdgpu-cursor] mirror-v10 masked control=%08lx->%08lx cm=%08lx->%08lx preserved=%08lx/%08lx; register contract, visibility unproven\n",
            (unsigned long)before_control, (unsigned long)control,
            (unsigned long)before_cm, (unsigned long)cm,
            (unsigned long)(before_control & ~CUR_CONTROL_OWNED),
            (unsigned long)(before_cm & ~CUR_CM_OWNED & ~0x00010000UL));
    if (!prior_enable) observe_visibility(ad, cursor, "enabled");
    if (published_shape) {
        /* Observe pending state after two actual frames, not immediately
         * after programming. No logging inside the bounded frame wait. */
        BOOL advanced = retire_shape(ad, cursor);
        if (cursor->shape_checks <= 8) cursor_request_snapshot(ad);
        if (cursor->shape_checks <= 6 && !cursor_phase_snapshot(ad, cursor)) {
            cursor->failed = TRUE;
            return; /* MMIO unreadable: no further cursor writes. */
        }
        if (cursor->reports < 8) cursor_route_snapshot(ad);
        bug("[amdgpu-cursor] mirror-v4 post-frame advanced=%ld control=%08lx cm=%08lx power=%08lx state=%08lx address=%08lx:%08lx size=%08lx position=%08lx hotspot=%08lx\n",
            (long)advanced, (unsigned long)rd(ad, CUR_CONTROL),
            (unsigned long)rd(ad, CUR_CM), (unsigned long)rd(ad, CUR_POWER),
            (unsigned long)rd(ad, CUR_POWER_STATUS),
            (unsigned long)rd(ad, CUR_ADDRESS_HIGH),
            (unsigned long)rd(ad, CUR_ADDRESS), (unsigned long)rd(ad, CUR_SIZE),
            (unsigned long)rd(ad, CUR_POSITION), (unsigned long)rd(ad, CUR_HOTSPOT));
    }
    if (++cursor->reports <= 4)
        bug("[amdgpu-cursor] mirror-v4 position=%ld,%ld hot=%ld,%ld control=%08lx cm=%08lx; require separate HDMI copy at x+64\n",
            (long)x, (long)y, (long)hotx, (long)hoty,
            (unsigned long)rd(ad, CUR_CONTROL), (unsigned long)rd(ad, CUR_CM));
}
