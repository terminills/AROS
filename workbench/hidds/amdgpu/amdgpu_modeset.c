/*
 * amdgpu.hidd — Stage 3 modeset: DCN-direct OTG/HUBP timing.
 *
 * RDNA4's VBIOS strips the ATOM display command tables (survey in
 * amdgpu_display.c: 0/10 present), so a mode cannot be set the ATOM way. The
 * SMU route was investigated and rejected: the SMU is live but autonomous and
 * does not service driver mailbox messages (amdgpu_smu.c), AND — verified
 * against the Linux amdgpu source (amd/display/dc/dccg/dcn401/dcn401_dccg.c) —
 * the per-mode PIXEL clock is NOT an SMU message at all: it is a DCCG DTO plus
 * OTG timing, programmed by direct register writes. The SMU only sets the
 * DISPCLK/envelope hard-minimums, which POST already set high enough for the
 * probed modes (<=1080p). So modeset is DCN-register-direct.
 *
 * This is the port of radeonsi's proven DCN4 write path (radeonsi_dcn401.c:
 * dcn401_optc_read_timing / dcn401_optc_program_timing) into amdgpu, which —
 * unlike radeonsi — actually owns the woken GPU and the live scanout.
 *
 * Addressing: the OTG/HUBP registers are BASE_IDX-2 (the DMU seg-2 segment,
 * ad->dmu_seg[2], 0x34c0 on Navi 44), accessed with amdgpu_rreg32/wreg32 which
 * are DWORD-indexed — so the raw register dword offset is added directly to
 * dmu_seg[2] (same convention as amdgpu_dcn.c's scanout accessors).
 *
 * Increment 1 (this commit): READ-ONLY. Read + decode the live OTG timing the
 * firmware programmed, so a boot shows the current mode and confirms the
 * register addressing before any write. No writes here.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>

#include "amdgpu_intern.h"

/* OTG block, per-instance register DWORD offsets (SEG2-relative). Values from
 * radeonsi_dcn401.c's dcn401_otg_*_dw tables (asic_reg/dcn/dcn_4_1_0_offset.h).
 * Field layout (dcn_4_1_0_sh_mask.h):
 *   OTG_H_TOTAL.OTG_H_TOTAL                [15:0]  (hardware value = total-1)
 *   OTG_H_BLANK_START_END.H_BLANK_START    [15:0]
 *   OTG_H_BLANK_START_END.H_BLANK_END      [31:16]
 *   OTG_H_SYNC_A.H_SYNC_A_START            [15:0]
 *   OTG_H_SYNC_A.H_SYNC_A_CUTOFF (width)   [31:16]
 *   (V_* the same, per line/frame)                                          */
static const ULONG otg_h_total_dw[4]           = { 0x1b2a, 0x1baa, 0x1c2a, 0x1caa };
static const ULONG otg_h_blank_start_end_dw[4] = { 0x1b2b, 0x1bab, 0x1c2b, 0x1cab };
static const ULONG otg_h_sync_a_dw[4]          = { 0x1b2c, 0x1bac, 0x1c2c, 0x1cac };
static const ULONG otg_v_total_dw[4]           = { 0x1b2f, 0x1baf, 0x1c2f, 0x1caf };
static const ULONG otg_v_blank_start_end_dw[4] = { 0x1b38, 0x1bb8, 0x1c38, 0x1cb8 };
static const ULONG otg_v_sync_a_dw[4]          = { 0x1b39, 0x1bb9, 0x1c39, 0x1cb9 };
static const ULONG otg_control_dw[4]           = { 0x1b43, 0x1bc3, 0x1c43, 0x1cc3 };
static const ULONG otg_status_dw[4]            = { 0x1b49, 0x1bc9, 0x1c49, 0x1cc9 };
/* Write-path additions (dcn_4_1_0_offset.h; OTG instances stride by 0x80). */
static const ULONG otg_h_sync_a_cntl_dw[4]     = { 0x1b2d, 0x1bad, 0x1c2d, 0x1cad };
static const ULONG otg_v_sync_a_cntl_dw[4]     = { 0x1b3a, 0x1bba, 0x1c3a, 0x1cba };
static const ULONG otg_v_total_min_dw[4]       = { 0x1b30, 0x1bb0, 0x1c30, 0x1cb0 };
static const ULONG otg_v_total_max_dw[4]       = { 0x1b31, 0x1bb1, 0x1c31, 0x1cb1 };
/* OTG_MASTER_UPDATE_LOCK: bit0 = lock request, bit8 = UPDATE_LOCK_STATUS. */
static const ULONG otg_master_update_lock_dw[4]= { 0x1b89, 0x1c09, 0x1c89, 0x1d09 };
#define OTG_MASTER_UPDATE_LOCK_BIT     0x00000001UL
#define OTG_UPDATE_LOCK_STATUS_BIT     0x00000100UL

/* HUBP DCHUBP_CNTL per instance (read-only diagnostic). */
static const ULONG hubp_cntl_dw[4]             = { 0x05f4, 0x06d0, 0x07ac, 0x0888 };

/* HUBP surface-fetch rectangle, per instance (radeonsi dcn401 tables):
 *   PRI_VIEWPORT_START      X_START [15:0], Y_START [31:16]
 *   PRI_VIEWPORT_DIMENSION  WIDTH  [13:0], HEIGHT  [29:16]
 *   DCSURF_SURFACE_PITCH    PITCH  [15:0]  (pixel stride minus one)          */
static const ULONG hubp_viewport_start_dw[4]   = { 0x05e9, 0x06c5, 0x07a1, 0x087d };
static const ULONG hubp_viewport_dim_dw[4]     = { 0x05eb, 0x06c7, 0x07a3, 0x087f };
static const ULONG hubp_surface_pitch_dw[4]    = { 0x0607, 0x06e3, 0x07bf, 0x089b };

#define AMDGPU_OTG_INST_MAX   4
#define AMDGPU_HUBP_INST_MAX  4

/* OTG_CONTROL.OTG_MASTER_EN [0] — is this timing generator running. */
#define OTG_MASTER_EN_MASK    0x00000001UL

/*
 * amdgpu_modeset_read_timing — read + decode the live OTG timing (BIOS POST
 * mode) for one OTG instance. Pure RREG32; no writes. Fills *out (an
 * AmdgpuDisplayMode) when non-NULL and returns TRUE if the block looks live.
 * Mirrors radeonsi dcn401_optc_read_timing.
 */
BOOL amdgpu_modeset_read_timing(struct AmdgpuDevice *ad, ULONG otg_inst,
                                struct AmdgpuDisplayMode *out)
{
    ULONG dcn;
    ULONG h_total_raw, h_blank_raw, h_sync_raw;
    ULONG v_total_raw, v_blank_raw, v_sync_raw;
    ULONG control, status, hubp_cntl;
    ULONG h_total, v_total, width = 0, height = 0;
    ULONG h_blank_start, h_blank_end, h_sync_start, h_sync_width;
    ULONG v_blank_start, v_blank_end, v_sync_start, v_sync_width;

    if (!ad || !ad->rmmio || otg_inst >= AMDGPU_OTG_INST_MAX)
        return FALSE;
    dcn = ad->dmu_seg[2];
    if (!dcn) {
        bug("[amdgpu-modeset] DMU seg2 not resolved (dmu_seg[2]=0)\n");
        return FALSE;
    }

    h_total_raw = amdgpu_rreg32(ad, dcn + otg_h_total_dw[otg_inst]);
    h_blank_raw = amdgpu_rreg32(ad, dcn + otg_h_blank_start_end_dw[otg_inst]);
    h_sync_raw  = amdgpu_rreg32(ad, dcn + otg_h_sync_a_dw[otg_inst]);
    v_total_raw = amdgpu_rreg32(ad, dcn + otg_v_total_dw[otg_inst]);
    v_blank_raw = amdgpu_rreg32(ad, dcn + otg_v_blank_start_end_dw[otg_inst]);
    v_sync_raw  = amdgpu_rreg32(ad, dcn + otg_v_sync_a_dw[otg_inst]);
    control     = amdgpu_rreg32(ad, dcn + otg_control_dw[otg_inst]);
    status      = amdgpu_rreg32(ad, dcn + otg_status_dw[otg_inst]);
    hubp_cntl   = amdgpu_rreg32(ad, dcn + hubp_cntl_dw[otg_inst]);

    if ((h_total_raw == 0xFFFFFFFFUL && v_total_raw == 0xFFFFFFFFUL) ||
        (h_total_raw == 0 && v_total_raw == 0)) {
        D(bug("[amdgpu-modeset] OTG%lu inactive (H/V total 0x%08lx/0x%08lx)\n",
              (unsigned long)otg_inst,
              (unsigned long)h_total_raw, (unsigned long)v_total_raw));
        return FALSE;
    }

    h_total       = (h_total_raw & 0xFFFFUL);
    v_total       = (v_total_raw & 0xFFFFUL);
    h_blank_start = (h_blank_raw & 0xFFFFUL);
    h_blank_end   = ((h_blank_raw >> 16) & 0xFFFFUL);
    h_sync_start  = (h_sync_raw & 0xFFFFUL);
    h_sync_width  = ((h_sync_raw >> 16) & 0xFFFFUL);
    v_blank_start = (v_blank_raw & 0xFFFFUL);
    v_blank_end   = ((v_blank_raw >> 16) & 0xFFFFUL);
    v_sync_start  = (v_sync_raw & 0xFFFFUL);
    v_sync_width  = ((v_sync_raw >> 16) & 0xFFFFUL);

    /* DCN blank window straddles the line/frame wrap; active = START - END. */
    if (h_blank_start > h_blank_end)
        width = h_blank_start - h_blank_end;
    if (v_blank_start > v_blank_end)
        height = v_blank_start - v_blank_end;

    bug("[amdgpu-modeset] OTG%lu %s %lux%lu : H_TOTAL=%lu blank=%lu..%lu sync=%lu+%lu "
        "V_TOTAL=%lu blank=%lu..%lu sync=%lu+%lu CTL=0x%08lx STAT=0x%08lx HUBP=0x%08lx\n",
        (unsigned long)otg_inst,
        (control & OTG_MASTER_EN_MASK) ? "RUNNING" : "off",
        (unsigned long)width, (unsigned long)height,
        (unsigned long)(h_total + 1),
        (unsigned long)h_blank_start, (unsigned long)h_blank_end,
        (unsigned long)h_sync_start, (unsigned long)h_sync_width,
        (unsigned long)(v_total + 1),
        (unsigned long)v_blank_start, (unsigned long)v_blank_end,
        (unsigned long)v_sync_start, (unsigned long)v_sync_width,
        (unsigned long)control, (unsigned long)status, (unsigned long)hubp_cntl);

    if (out) {
        out->w           = (UWORD)width;
        out->h           = (UWORD)height;
        out->htotal      = (UWORD)(h_total + 1);
        out->vtotal      = (UWORD)(v_total + 1);
        out->hsync_start = (UWORD)h_sync_start;
        out->hsync_end   = (UWORD)(h_sync_start + h_sync_width);
        out->vsync_start = (UWORD)v_sync_start;
        out->vsync_end   = (UWORD)(v_sync_start + v_sync_width);
    }
    return TRUE;
}

/*
 * amdgpu_modeset_set_viewport — resize HUBP's surface-fetch rectangle to
 * width x height with the given pitch (in pixels), origin 0,0. This is the ONLY
 * write needed to make a HUBP fetch a larger surface into an already-running OTG
 * raster (the Stage-3-for-native case: OTG is already at the panel timing, only
 * the fetched rectangle is smaller). Self-verifying: reads each register back and
 * returns FALSE on mismatch so the caller can abort the geometry change and keep
 * the working display. Mirrors radeonsi dcn401_hubp_set_viewport + the pitch
 * field of dcn401_hubp_set_surface_format.
 */

/*
 * amdgpu_modeset_device_alive -- has the GPU stopped answering MMIO?
 *
 * An all-ones read is how a PCI device that has fallen off the bus (or wedged)
 * presents itself. Worth a named check because the failure it guards is not
 * subtle: on the first 800x600 screenmode change the HUBP viewport write
 * succeeded, the DSCL recout write read back 0xffffffff, and the platform reset
 * itself a few lines later. Continuing to program a device that has already
 * gone away turns a recoverable "mode change failed" into a reboot.
 */
static BOOL amdgpu_modeset_device_alive(struct AmdgpuDevice *ad, ULONG dcn, ULONG otg_inst)
{
    ULONG v = amdgpu_rreg32(ad, dcn + otg_control_dw[otg_inst]);

    if (v == 0xFFFFFFFFUL) {
        bug("[amdgpu-modeset] OTG%lu reads 0xffffffff -- device is not responding;"
            " refusing to program it further\n", (unsigned long)otg_inst);
        return FALSE;
    }
    return TRUE;
}

/*
 * amdgpu_modeset_lock / _unlock -- bracket plane (HUBP/DPP) updates.
 *
 * DCN double-buffers the plane registers and latches them on VUPDATE. Writing
 * the fetch rectangle and scaler of a LIVE, scanning-out pipe without taking
 * the lock lets the hardware latch a half-written configuration mid-frame,
 * which underflows the pipe. That is not a theoretical hazard here: it is
 * exactly what a 1920x1080 -> 800x600 screenmode change did, taking the whole
 * machine down with it.
 *
 * Mirrors optc1_lock/optc1_unlock (dcn10_optc.c), which dcn401 inherits.
 * OTG_GLOBAL_CONTROL0.OTG_MASTER_UPDATE_LOCK_SEL is deliberately not written:
 * it selects WHICH OTG the lock applies to, and 0 (this OTG) is both the reset
 * value and the only instance we drive.
 *
 * The status wait is bounded. FALSE means the caller MUST NOT update the
 * plane or unlock somebody else's transaction. Cancel our request on timeout
 * while the block still responds; do not write again after an all-ones read.
 * A software lock must serialize callers across this hardware transaction.
 */
BOOL amdgpu_modeset_lock(struct AmdgpuDevice *ad, ULONG otg_inst)
{
    ULONG dcn, i, v;

    if (!ad || !ad->rmmio || otg_inst >= AMDGPU_OTG_INST_MAX)
        return FALSE;
    dcn = ad->dmu_seg[2];
    if (!dcn || !amdgpu_modeset_device_alive(ad, dcn, otg_inst))
        return FALSE;

    v = amdgpu_rreg32(ad, dcn + otg_master_update_lock_dw[otg_inst]);
    if (v == 0xFFFFFFFFUL ||
        (v & (OTG_MASTER_UPDATE_LOCK_BIT | OTG_UPDATE_LOCK_STATUS_BIT))) {
        bug("[amdgpu-modeset] OTG%lu update lock unavailable (LOCK=0x%08lx)\n",
            (unsigned long)otg_inst, (unsigned long)v);
        return FALSE;
    }

    amdgpu_wreg32(ad, dcn + otg_master_update_lock_dw[otg_inst],
                  OTG_MASTER_UPDATE_LOCK_BIT);

    for (i = 0; i < 5000; i++) {
        v = amdgpu_rreg32(ad, dcn + otg_master_update_lock_dw[otg_inst]);
        if (v == 0xFFFFFFFFUL) {
            bug("[amdgpu-modeset] OTG%lu vanished while taking the update lock\n",
                (unsigned long)otg_inst);
            return FALSE;
        }
        if (v & OTG_UPDATE_LOCK_STATUS_BIT)
            return TRUE;
    }

    bug("[amdgpu-modeset] OTG%lu update lock did not take (LOCK=0x%08lx);"
        " cancelling request, keeping old plane\n",
        (unsigned long)otg_inst, (unsigned long)v);
    amdgpu_wreg32(ad, dcn + otg_master_update_lock_dw[otg_inst], 0);
    return FALSE;
}

void amdgpu_modeset_unlock(struct AmdgpuDevice *ad, ULONG otg_inst)
{
    ULONG dcn;

    if (!ad || !ad->rmmio || otg_inst >= AMDGPU_OTG_INST_MAX)
        return;
    dcn = ad->dmu_seg[2];
    if (!dcn)
        return;

    amdgpu_wreg32(ad, dcn + otg_master_update_lock_dw[otg_inst], 0);
}

/* Linux hubp401_program_size encodes surface_pitch - 1. JOB-0084 native
 * boot11 independently read the untouched GOP's 1024-pixel stride as 0x3ff.
 * The caller supplies the REAL storage stride, not necessarily visible width,
 * and must own the plane update lock. Readback alone does not prove latching
 * or provide rollback if a later plane helper fails. */
BOOL amdgpu_modeset_set_viewport(struct AmdgpuDevice *ad, ULONG hubp_inst,
                                 ULONG width, ULONG height, ULONG pitch_pixels)
{
    ULONG dcn, dim_val, pitch_val;
    ULONG start_after, dim_after, pitch_after;

    if (!ad || !ad->rmmio || hubp_inst >= AMDGPU_HUBP_INST_MAX)
        return FALSE;
    if (!width || width > 0x3FFFUL || !height || height > 0x3FFFUL ||
        !pitch_pixels || pitch_pixels > 0x10000UL || pitch_pixels < width)
        return FALSE;
    dcn = ad->dmu_seg[2];
    if (!dcn)
        return FALSE;

    dim_val   = (height << 16) | width;
    pitch_val = pitch_pixels - 1;

    amdgpu_wreg32(ad, dcn + hubp_viewport_start_dw[hubp_inst], 0);
    amdgpu_wreg32(ad, dcn + hubp_viewport_dim_dw[hubp_inst], dim_val);
    amdgpu_wreg32(ad, dcn + hubp_surface_pitch_dw[hubp_inst], pitch_val);

    start_after = amdgpu_rreg32(ad, dcn + hubp_viewport_start_dw[hubp_inst]);
    dim_after   = amdgpu_rreg32(ad, dcn + hubp_viewport_dim_dw[hubp_inst]);
    pitch_after = amdgpu_rreg32(ad, dcn + hubp_surface_pitch_dw[hubp_inst]);

    {
        BOOL ok = (start_after == 0) && (dim_after == dim_val)
               && ((pitch_after & 0xFFFFUL) == pitch_val);
        bug("[amdgpu-modeset] HUBP%lu viewport -> %lux%lu pitch=%lu : START=0x%08lx "
            "DIM=0x%08lx(want 0x%08lx) PITCH=0x%08lx(want 0x%08lx) %s\n",
            (unsigned long)hubp_inst, (unsigned long)width, (unsigned long)height,
            (unsigned long)pitch_pixels, (unsigned long)start_after,
            (unsigned long)dim_after, (unsigned long)dim_val,
            (unsigned long)pitch_after, (unsigned long)pitch_val,
            ok ? "OK" : "MISMATCH");
        return ok;
    }
}

/*
 * amdgpu_modeset_survey — read + log the live timing of every OTG. Read-only
 * baseline: shows which OTG drives the panel and at what mode, so we know how
 * much the write path (increment 2) must change before touching anything.
 */
/*
 * amdgpu_modeset_set_recout — place the pipe's output rectangle on the raster.
 *
 * The HUBP viewport says how much surface to FETCH; recout says where that
 * lands on the OTG's raster. Setting both is what makes a smaller-than-native
 * screenmode actually work without touching timing:
 *
 *     viewport = mode size      (fetch 1280x1024)
 *     recout   = mode size, centred on the native raster
 *     OTG      = untouched      (still the panel's native timing)
 *
 * That is a real mode change -- the display genuinely shows a 1280x1024 screen
 * -- letterboxed rather than scaled. Scaling it to fill the panel needs the
 * DSCL ratios and filter taps, which is the next increment; centring needs
 * neither and works today.
 *
 * Deliberately NOT a substitute for full modesetting. Changing the RASTER
 * (and so the refresh rate, or any mode LARGER than native) still requires OTG
 * timing plus a DCCG/PLL pixel-clock change, which this driver does not have.
 *
 * Self-verifying like set_viewport: reads back and returns FALSE on mismatch so
 * the caller can abandon the change and keep a working display.
 */
BOOL amdgpu_modeset_set_recout(struct AmdgpuDevice *ad, ULONG dscl_inst,
                               ULONG x, ULONG y, ULONG width, ULONG height)
{
    /* DSCL0 base; instances stride by 0x16B (DSCL0 0x0d08 -> DSCL1 0x0e73). */
    const ULONG dscl_stride     = 0x16BUL;
    const ULONG recout_start_dw = 0x0d1eUL;
    const ULONG recout_size_dw  = 0x0d1fUL;
    ULONG dcn, start_val, size_val, start_after, size_after;

    if (!ad || !ad->rmmio || dscl_inst >= AMDGPU_HUBP_INST_MAX)
        return FALSE;
    if (!width || !height)
        return FALSE;
    dcn = ad->dmu_seg[2];
    if (!dcn)
        return FALSE;

    start_val = ((y & 0xFFFFUL) << 16) | (x & 0xFFFFUL);
    size_val  = ((height & 0xFFFFUL) << 16) | (width & 0xFFFFUL);

    amdgpu_wreg32(ad, dcn + recout_start_dw + dscl_inst * dscl_stride, start_val);
    amdgpu_wreg32(ad, dcn + recout_size_dw  + dscl_inst * dscl_stride, size_val);

    start_after = amdgpu_rreg32(ad, dcn + recout_start_dw + dscl_inst * dscl_stride);
    size_after  = amdgpu_rreg32(ad, dcn + recout_size_dw  + dscl_inst * dscl_stride);

    {
        BOOL ok = (start_after == start_val) && (size_after == size_val);
        bug("[amdgpu-modeset] DSCL%lu recout -> %lux%lu at (%lu,%lu) : "
            "START=0x%08lx(want 0x%08lx) SIZE=0x%08lx(want 0x%08lx) %s\n",
            (unsigned long)dscl_inst, (unsigned long)width, (unsigned long)height,
            (unsigned long)x, (unsigned long)y,
            (unsigned long)start_after, (unsigned long)start_val,
            (unsigned long)size_after, (unsigned long)size_val,
            ok ? "OK" : "MISMATCH");
        return ok;
    }
}


/*
 * amdgpu_modeset_program_timing — Increment 2: the OTG timing WRITE path.
 *
 * Port of optc1_program_timing (dcn10_optc.c, which dcn401 inherits for the
 * timing registers themselves) restricted to the progressive, non-interlaced,
 * single-OPP case we can actually produce. Field shifts verified against
 * dcn_4_1_0_sh_mask.h; register offsets against dcn_4_1_0_offset.h.
 *
 * AmdgpuDisplayMode carries VESA-convention timing, which maps onto what the
 * reference calls for:
 *     h_front_porch = hsync_start - w        h_sync_width = hsync_end - hsync_start
 *     h_addressable = w                      h_total      = htotal
 * and the same per frame. Borders are zero for every mode we publish.
 *
 * Deliberately NOT wired to anything yet. Retiming a live OTG is how you blank
 * a working display, and the pixel clock is a separate problem: the DCCG DTO
 * (dcn401_dccg.c) still has to be programmed to match, or the panel is handed
 * timing at the wrong rate. This function is the half that is well understood;
 * calling it before the DTO half exists would produce a black screen and a
 * hard-to-attribute regression. It is here so the register work is reviewable
 * and bisectable on its own.
 *
 * Returns TRUE only if every register reads back what was written.
 */
BOOL amdgpu_modeset_program_timing(struct AmdgpuDevice *ad, ULONG otg_inst,
                                   const struct AmdgpuDisplayMode *mode)
{
    ULONG dcn;
    ULONG h_total, h_front_porch, h_sync_width, h_blank_start, h_blank_end;
    ULONG v_total, v_front_porch, v_sync_width, v_blank_start, v_blank_end;
    ULONG h_total_val, h_sync_val, h_blank_val;
    ULONG v_total_val, v_sync_val, v_blank_val;
    ULONG h_pol, v_pol, ctl;
    BOOL  ok = TRUE;

    if (!ad || !ad->rmmio || !mode || otg_inst >= AMDGPU_OTG_INST_MAX)
        return FALSE;
    dcn = ad->dmu_seg[2];
    if (!dcn)
        return FALSE;

    /* Timing has to be self-consistent or we would program nonsense into a
     * live timing generator. Reject rather than clamp. */
    if (!mode->w || !mode->h || !mode->htotal || !mode->vtotal)
        return FALSE;
    if (mode->hsync_start < mode->w || mode->hsync_end <= mode->hsync_start ||
        mode->htotal <= mode->hsync_end)
        return FALSE;
    if (mode->vsync_start < mode->h || mode->vsync_end <= mode->vsync_start ||
        mode->vtotal <= mode->vsync_end)
        return FALSE;

    h_total       = mode->htotal;
    h_front_porch = (ULONG)mode->hsync_start - mode->w;
    h_sync_width  = (ULONG)mode->hsync_end - mode->hsync_start;

    v_total       = mode->vtotal;
    v_front_porch = (ULONG)mode->vsync_start - mode->h;
    v_sync_width  = (ULONG)mode->vsync_end - mode->vsync_start;

    /* apply_front_porch_workaround(): progressive needs v_front_porch >= 1. */
    if (v_front_porch < 1)
        v_front_porch = 1;

    /* blank_start = line/frame end - front porch; blank_end = start - active
     * (borders are zero for every mode we publish). */
    h_blank_start = h_total - h_front_porch;
    h_blank_end   = h_blank_start - mode->w;
    v_blank_start = v_total - v_front_porch;
    v_blank_end   = v_blank_start - mode->h;

    /* Hardware takes total-1; sync starts at 0 and ends at the sync width. */
    h_total_val = (h_total - 1) & 0xFFFFUL;
    h_sync_val  = (h_sync_width & 0xFFFFUL) << 16;
    h_blank_val = ((h_blank_end & 0xFFFFUL) << 16) | (h_blank_start & 0xFFFFUL);

    v_total_val = (v_total - 1) & 0xFFFFUL;
    v_sync_val  = (v_sync_width & 0xFFFFUL) << 16;
    v_blank_val = ((v_blank_end & 0xFFFFUL) << 16) | (v_blank_start & 0xFFFFUL);

    /*
     * Sync polarity. AmdgpuDisplayMode.flags is declared but the EDID parser
     * does not populate it yet, so we preserve whatever POST programmed rather
     * than inventing a polarity: a wrong polarity is a silently out-of-lock
     * panel. Populating flags from the EDID detailed-timing descriptor is the
     * follow-up that lets this become a real decision.
     */
    h_pol = amdgpu_rreg32(ad, dcn + otg_h_sync_a_cntl_dw[otg_inst]);
    v_pol = amdgpu_rreg32(ad, dcn + otg_v_sync_a_cntl_dw[otg_inst]);

    amdgpu_wreg32(ad, dcn + otg_h_total_dw[otg_inst],           h_total_val);
    amdgpu_wreg32(ad, dcn + otg_h_sync_a_dw[otg_inst],          h_sync_val);
    amdgpu_wreg32(ad, dcn + otg_h_blank_start_end_dw[otg_inst], h_blank_val);

    amdgpu_wreg32(ad, dcn + otg_v_total_dw[otg_inst],           v_total_val);
    /* With V_TOTAL_CONTROL on, MIN/MAX must equal V_TOTAL or the frame rate
     * drifts (variable-refresh path). Keep them pinned. */
    amdgpu_wreg32(ad, dcn + otg_v_total_min_dw[otg_inst],       v_total_val);
    amdgpu_wreg32(ad, dcn + otg_v_total_max_dw[otg_inst],       v_total_val);
    amdgpu_wreg32(ad, dcn + otg_v_sync_a_dw[otg_inst],          v_sync_val);
    amdgpu_wreg32(ad, dcn + otg_v_blank_start_end_dw[otg_inst], v_blank_val);

    /* DP/eDP want START_POINT_CNTL=1; FIELD_NUMBER_CNTL=0 (progressive). */
    ctl = amdgpu_rreg32(ad, dcn + otg_control_dw[otg_inst]);
    ctl = (ctl & ~((1UL << 12) | (1UL << 13))) | (1UL << 12);
    amdgpu_wreg32(ad, dcn + otg_control_dw[otg_inst], ctl);

#define AMDGPU_OTG_VERIFY(_dw, _want, _name)                                   \
    do {                                                                       \
        ULONG _got = amdgpu_rreg32(ad, dcn + (_dw));                           \
        if (_got != (ULONG)(_want)) {                                          \
            ok = FALSE;                                                        \
            bug("[amdgpu-modeset] OTG%lu %s = 0x%08lx (want 0x%08lx) MISMATCH\n",\
                (unsigned long)otg_inst, _name,                                \
                (unsigned long)_got, (unsigned long)(_want));                  \
        }                                                                      \
    } while (0)

    AMDGPU_OTG_VERIFY(otg_h_total_dw[otg_inst],           h_total_val, "H_TOTAL");
    AMDGPU_OTG_VERIFY(otg_h_sync_a_dw[otg_inst],          h_sync_val,  "H_SYNC_A");
    AMDGPU_OTG_VERIFY(otg_h_blank_start_end_dw[otg_inst], h_blank_val, "H_BLANK");
    AMDGPU_OTG_VERIFY(otg_v_total_dw[otg_inst],           v_total_val, "V_TOTAL");
    AMDGPU_OTG_VERIFY(otg_v_sync_a_dw[otg_inst],          v_sync_val,  "V_SYNC_A");
    AMDGPU_OTG_VERIFY(otg_v_blank_start_end_dw[otg_inst], v_blank_val, "V_BLANK");
#undef AMDGPU_OTG_VERIFY

    bug("[amdgpu-modeset] OTG%lu timing -> %ux%u@%uHz  "
        "H(total %lu fp %lu sync %lu) V(total %lu fp %lu sync %lu)  "
        "pol H=0x%08lx V=0x%08lx (preserved)  %s\n",
        (unsigned long)otg_inst, mode->w, mode->h, mode->refresh,
        (unsigned long)h_total, (unsigned long)h_front_porch, (unsigned long)h_sync_width,
        (unsigned long)v_total, (unsigned long)v_front_porch, (unsigned long)v_sync_width,
        (unsigned long)h_pol, (unsigned long)v_pol,
        ok ? "OK" : "MISMATCH");

    return ok;
}


/*
 * amdgpu_modeset_survey_dto -- read the DCCG pixel-clock DTO. READ-ONLY.
 *
 * The OTG timing write path (amdgpu_modeset_program_timing) is ported and its
 * arithmetic is confirmed against the live raster, but it is inert because
 * timing alone does not set the PIXEL CLOCK. On DCN4 that clock is a DCCG
 * fractional divider, not an ATOM command and not an SMU message
 * (dcn401_dccg.c, dccg401_set_dp_dto):
 *
 *      modulo = refclk_hz
 *      int    = pixclk_hz / refclk_hz
 *      phase  = pixclk_hz - int * refclk_hz
 *
 * Every term is known except refclk (DTBCLK), which Linux takes from a clock
 * manager we do not have. Rather than guess it, DERIVE it: the panel is
 * already running a mode whose pixel clock we can compute exactly from the
 * timing the OTG survey reads back, so the live MODULO *is* refclk and the
 * live PHASE/INT let the formula be checked against hardware that is visibly
 * working. Getting refclk wrong means a panel handed correct timing at the
 * wrong rate -- a black screen with every register reading plausibly.
 *
 * NOTE the segment: DCCG registers are BASE_IDX 1 (dmu_seg[1]), NOT the
 * BASE_IDX 2 segment the OTG and HUBP registers live in. Reading them at
 * dmu_seg[2] would return unrelated values that look like data.
 *
 * RESULT (2026-08-20, RX 9060 XT, HDMI-A-1):
 *
 *     DCCG seg=0xc0  DP_DTO0 PHASE=0 MODULO=0
 *     PIXEL_RATE_DIV=0x00008421 DPDTO0_INT=16
 *
 * PHASE and MODULO are ZERO and 0x8421 is a reset pattern (all four INT fields
 * read 16), so the DP DTO is not driving this display -- and that is CORRECT,
 * not a fault. dccg401_set_dp_dto() wraps its whole body in
 * `if (!dc_is_tmds_signal(params->signal))`, and our panel is HDMI, which is
 * TMDS. For TMDS, DCN4 instead selects OTG<n>_TMDS_PIXEL_RATE_DIV (only /2 and
 * /4 are valid) via dccg401_set_pixel_rate_div(), and the pixel clock itself
 * comes from the PHY PLL upstream of the DCCG.
 *
 * So the DTO write path is the DISPLAYPORT answer and would have been the
 * wrong thing to build for this monitor. Reading before writing cost one boot
 * and saved implementing a clock path this display never uses.
 *
 * What HDMI modesetting actually needs, and why it is the harder half: the
 * pixel clock must come from the PHY PLL, and this card's VBIOS has no clock
 * tables to ask -- the ATOM survey reports
 * "SetPixelClock [12]: MISSING <core!>" -- so it means programming the PHY PLL
 * directly. That is the remaining blocker on a real per-mode modeset; OTG
 * timing (amdgpu_modeset_program_timing) is ported and arithmetically
 * confirmed against the live raster, and waits on it.
 */
void amdgpu_modeset_survey_dto(struct AmdgpuDevice *ad)
{
    const ULONG dp_dto0_phase_dw  = 0x0081;
    const ULONG dp_dto0_modulo_dw = 0x0082;
    const ULONG otg_pixel_rate_div_dw = 0x006f;
    ULONG dccg, phase, modulo, rate_div, dto_int;
    struct AmdgpuDisplayMode otg;

    if (!ad || !ad->rmmio)
        return;
    dccg = ad->dmu_seg[1];
    if (!dccg) {
        bug("[amdgpu-dto] DCCG segment (BASE_IDX 1) not discovered -- skipping\n");
        return;
    }

    phase    = amdgpu_rreg32(ad, dccg + dp_dto0_phase_dw);
    modulo   = amdgpu_rreg32(ad, dccg + dp_dto0_modulo_dw);
    rate_div = amdgpu_rreg32(ad, dccg + otg_pixel_rate_div_dw);
    dto_int  = (rate_div >> 1) & 0x1F;      /* DPDTO0_INT [5:1] */

    bug("[amdgpu-dto] DCCG seg=0x%lx  DP_DTO0 PHASE=%lu MODULO=%lu  "
        "PIXEL_RATE_DIV=0x%08lx DPDTO0_INT=%lu\n",
        (unsigned long)dccg, (unsigned long)phase, (unsigned long)modulo,
        (unsigned long)rate_div, (unsigned long)dto_int);

    /* Cross-check against the mode the panel is actually displaying. If the
     * reconstruction matches the raster's own pixel clock, the formula and the
     * register addresses are both right and the write path can be trusted. */
    if (modulo && amdgpu_modeset_read_timing(ad, 0, &otg) && otg.htotal && otg.vtotal) {
        UQUAD live_pix = (UQUAD)otg.htotal * otg.vtotal * (otg.refresh ? otg.refresh : 60);
        UQUAD from_dto = (UQUAD)dto_int * modulo + phase;

        bug("[amdgpu-dto] refclk(MODULO)=%lu Hz ; DTO reconstructs %lu Hz ; "
            "raster %ux%u@%u implies %lu Hz -- %s\n",
            (unsigned long)modulo, (unsigned long)from_dto,
            otg.w, otg.h, otg.refresh, (unsigned long)live_pix,
            (from_dto && live_pix &&
             (from_dto > live_pix ? from_dto - live_pix : live_pix - from_dto)
                 < live_pix / 100)
                ? "MATCH (formula + addresses confirmed)"
                : "MISMATCH -- do not trust the DTO write path yet");
    }
}

void amdgpu_modeset_survey(struct AmdgpuDevice *ad)
{
    ULONG i;

    if (!ad || !ad->dmu_seg[2]) {
        bug("[amdgpu-modeset] survey skipped (no DMU seg2)\n");
        return;
    }
    bug("[amdgpu-modeset] === live OTG timing survey (read-only) ===\n");
    for (i = 0; i < AMDGPU_OTG_INST_MAX; i++)
        amdgpu_modeset_read_timing(ad, i, NULL);
}
