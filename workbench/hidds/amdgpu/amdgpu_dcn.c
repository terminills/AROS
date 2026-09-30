/*
 * amdgpu.hidd — DCN (display controller) observe.
 *
 * Read-only: report the live scanout the firmware (UEFI GOP / VBIOS) already set
 * up — where in VRAM the GPU is currently displaying from, and at what geometry.
 * That is exactly the framebuffer we would adopt to put an AROS display on this
 * GPU without a full modeset.
 *
 * The DCN scanout registers use the DMU block's BASE_IDX-2 segment (dmu_seg[2],
 * captured by the discovery parse). Offsets are from Linux dcn_4_1_0_offset.h and
 * all carry BASE_IDX 2, so with dmu_seg[2] inside the 512 KB register BAR they are
 * directly readable. No writes — the ladder stays observe-only.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>

#include "amdgpu_intern.h"

/* dword offsets within DMU seg[2] (dcn_4_1_0_offset.h, BASE_IDX 2) */
#define DCN_HUBP0_SURFACE_CONFIG     0x05e5   /* DCSURF_SURFACE_CONFIG               */
#define DCN_SURF_PIXEL_FORMAT_SHIFT  0        /* SURFACE_PIXEL_FORMAT, mask 0x7F     */
#define DCN_SURF_PIXEL_FORMAT_MASK   0x7FUL
/* Hardware values, from dcn10_hubp.c's format switch (the mapping is stable
 * across DCN generations; the software enum in dc_hw_types.h is NOT the
 * register value and must not be used directly). */
#define DCN_SURF_FMT_ARGB1555        1
#define DCN_SURF_FMT_RGB565          3
#define DCN_SURF_FMT_ARGB8888        8
#define DCN_SURF_FMT_ARGB2101010     10

#define DCN_HUBP0_SURFACE_ADDR       0x060a   /* DCSURF_PRIMARY_SURFACE_ADDRESS      */
#define DCN_HUBP0_SURFACE_ADDR_HIGH  0x060b   /* ..._HIGH                            */
#define DCN_HUBP0_VIEWPORT_DIM       0x05eb   /* DCSURF_PRI_VIEWPORT_DIMENSION       */

/* HUBP0 pixel-channel CROSSBAR (HUBPRET0_HUBPRET_CONTROL, dcn_4_1_0). This --
 * NOT the surface-format code -- selects which memory channel feeds each display
 * channel. ARGB8888 and ABGR8888 share format code 8 and differ only here
 * (dcn35_hubp.c hubp35_program_pixel_format: ARGB green/blue/red = 1/2/3, ABGR
 * swaps red/blue to 2/3). The desktop colour-swap is this being left at whatever
 * the GOP programmed while the driver draws in a fixed (BGR032) memory order.
 * Each field is 2 bits. */
#define DCN_HUBPRET0_CONTROL         0x066d
#define DCN_CROSSBAR_Y_G_SHIFT       18       /* green source, mask 0x000C0000 */
#define DCN_CROSSBAR_CB_B_SHIFT      20       /* blue  source, mask 0x00300000 */
#define DCN_CROSSBAR_CR_R_SHIFT      22       /* red   source, mask 0x00C00000 */
#define DCN_CROSSBAR_SRC_MASK        0x3UL

/* DPP (CNVC) format crossbar -- a SECOND channel router after HUBP's, programmed
 * separately (dcn30_dpp.c dpp3_cnv_setup: R/G/B = 0/1/2 canonical). regCNVC_CFG0_
 * FORMAT_CONTROL, dcn_4_1_0, pipe 0. Each field 2 bits. */
#define DCN_CNVC_CFG0_FORMAT_CONTROL 0x0cd0
#define DCN_FMT_CROSSBAR_R_SHIFT     24       /* mask 0x03000000 */
#define DCN_FMT_CROSSBAR_G_SHIFT     26       /* mask 0x0C000000 */
#define DCN_FMT_CROSSBAR_B_SHIFT     28       /* mask 0x30000000 */
#define DCN_OTG0_H_TOTAL             0x1b2a
#define DCN_OTG0_V_TOTAL             0x1b2f
#define DCN_OTG0_CONTROL             0x1b43   /* bit0 = OTG_MASTER_EN */
#define OTG_MASTER_EN                0x00000001UL

/* HPD (hot-plug detect) per connector — regHPD<n>_DC_HPD_INT_STATUS, stride 8. */
#define DCN_HPD0_INT_STATUS          0x175a
#define DCN_HPD_STRIDE               0x0008
#define DCN_MAX_CONNECTORS           6
#define DC_HPD_SENSE                 0x00000002UL   /* bit 1: immediate sense       */
#define DC_HPD_SENSE_DELAYED         0x00000010UL   /* bit 4: debounced = attached  */

BOOL amdgpu_dcn_hw_init(struct AmdgpuDevice *ad)
{
    ULONG dcn, lo, hi, vp, ht, vt, oc;
    UQUAD fb;

    if (!ad->rmmio || !ad->ip.dmu.valid) {
        D(bug("[amdgpu-dcn] no display (DMU) block discovered\n"));
        return TRUE;
    }
    dcn = ad->dmu_seg[2];        /* BASE_IDX 2 segment (0x34c0 on Navi 44) */
    if (!dcn) {
        D(bug("[amdgpu-dcn] DMU seg2 not resolved (dmu_seg[2]=0)\n"));
        return TRUE;
    }

    lo = amdgpu_rreg32(ad, dcn + DCN_HUBP0_SURFACE_ADDR);
    hi = amdgpu_rreg32(ad, dcn + DCN_HUBP0_SURFACE_ADDR_HIGH);
    vp = amdgpu_rreg32(ad, dcn + DCN_HUBP0_VIEWPORT_DIM);
    ht = amdgpu_rreg32(ad, dcn + DCN_OTG0_H_TOTAL);
    vt = amdgpu_rreg32(ad, dcn + DCN_OTG0_V_TOTAL);
    oc = amdgpu_rreg32(ad, dcn + DCN_OTG0_CONTROL);
    fb = ((UQUAD)hi << 32) | lo;

    D(bug("[amdgpu-dcn] DMU v%u.%u seg2=0x%05lx (via BAR5), HUBP0/OTG0 pipe 0:\n",
          ad->ip.dmu.major, ad->ip.dmu.minor, (unsigned long)dcn));
    D(bug("[amdgpu-dcn]   PRIMARY_SURFACE_ADDR = 0x%08lx%08lx (MC)\n",
          (unsigned long)hi, (unsigned long)lo));
    D(bug("[amdgpu-dcn]   VIEWPORT = %lu x %lu   OTG H_TOTAL=%lu V_TOTAL=%lu\n",
          (unsigned long)(vp & 0xFFFF), (unsigned long)((vp >> 16) & 0x1FFF),
          (unsigned long)(ht & 0xFFFF), (unsigned long)(vt & 0xFFFF)));
    D(bug("[amdgpu-dcn]   OTG0_CONTROL=0x%08lx -> pipe 0 OTG %s\n", (unsigned long)oc,
          (oc & OTG_MASTER_EN) ? "MASTER-ENABLED (actively driving timing)" : "disabled"));

    if (fb != 0)
        D(bug("[amdgpu-dcn] *** LIVE SCANOUT — pipe 0 displaying VRAM MC 0x%08lx%08lx "
              "at %lux%lu; this is the framebuffer to adopt for AROS ***\n",
              (unsigned long)hi, (unsigned long)lo,
              (unsigned long)(vp & 0xFFFF), (unsigned long)((vp >> 16) & 0x1FFF)));
    else
        D(bug("[amdgpu-dcn] HUBP0 surface addr 0 — pipe 0 idle (display may be on "
              "another pipe/output; the KVM shows no HDMI signal either)\n"));

    /* Enumerate display connectors via HPD sense — which physical outputs have a
     * monitor attached. Feeds AROS's multi-display framework (Kalamatee) and
     * pinpoints the KVM-dark output: if the KVM's connector reads 'empty' the
     * capture isn't on this GPU's active output. Connector TYPE (DP/HDMI) needs
     * the VBIOS connector-object table (ATOM) — a later pass; HPD gives the
     * attach state now, all via DMU seg2 in BAR5. */
    {
        UBYTE c, attached = 0;
        D(bug("[amdgpu-dcn] connector map (HPD sense, %u slots):\n",
              (unsigned)DCN_MAX_CONNECTORS));
        for (c = 0; c < DCN_MAX_CONNECTORS; c++) {
            ULONG hpd = amdgpu_rreg32(ad,
                          dcn + DCN_HPD0_INT_STATUS + c * DCN_HPD_STRIDE);
            BOOL conn = (hpd & (DC_HPD_SENSE_DELAYED | DC_HPD_SENSE)) != 0;
            if (conn)
                attached++;
            D(bug("[amdgpu-dcn]   connector %u: HPD_INT_STATUS=0x%08lx -> %s\n",
                  (unsigned)c, (unsigned long)hpd,
                  conn ? "CONNECTED (monitor attached)" : "empty"));
        }
        D(bug("[amdgpu-dcn] %u connector(s) with a monitor attached\n",
              (unsigned)attached));
    }

    /* Stage 3 increment 1: log the live OTG timing the firmware programmed. This
     * baseline tells us the current mode (and whether OTG is already at the
     * panel's native timing) before the modeset write path touches anything. */
    amdgpu_modeset_survey(ad);
    amdgpu_modeset_survey_dto(ad);

    return TRUE;
}

/*
 * Reusable scanout accessors for the display path (Stage 0 VRAM handoff).
 * amdgpu_dcn_get_scanout: read HUBP0's current primary-surface MC address, the
 * active viewport, and whether OTG0 is master-enabled (actively driving timing).
 * amdgpu_dcn_set_scanout: repoint HUBP0 at a new MC surface base — the flip that
 * hands the displayed surface to an amdgpu-owned VRAM buffer. All register
 * knowledge stays here; the gfx code calls through these.
 */
BOOL amdgpu_dcn_get_scanout(struct AmdgpuDevice *ad, UQUAD *mc, ULONG *w, ULONG *h, BOOL *master)
{
    ULONG dcn, lo, hi, vp, oc;

    if (!ad->rmmio || !ad->ip.dmu.valid)
        return FALSE;
    dcn = ad->dmu_seg[2];
    if (!dcn)
        return FALSE;

    lo = amdgpu_rreg32(ad, dcn + DCN_HUBP0_SURFACE_ADDR);
    hi = amdgpu_rreg32(ad, dcn + DCN_HUBP0_SURFACE_ADDR_HIGH);
    vp = amdgpu_rreg32(ad, dcn + DCN_HUBP0_VIEWPORT_DIM);
    oc = amdgpu_rreg32(ad, dcn + DCN_OTG0_CONTROL);

    if (mc)     *mc = ((UQUAD)hi << 32) | lo;
    if (w)      *w  = vp & 0xFFFF;
    if (h)      *h  = (vp >> 16) & 0x1FFF;
    if (master) *master = (oc & OTG_MASTER_EN) != 0;

    /* AUDIT P0-1 item 1: report the CURRENT scanout pixel format + channel
     * crossbar the GOP left, read-only. The desktop colour-swap comes from this
     * crossbar not matching the (BGR032) memory order the driver draws in. Values
     * are the raw 2-bit source selects; ARGB8888 canonical is G/B/R = 1/2/3. */
    {
        ULONG cfg  = amdgpu_rreg32(ad, dcn + DCN_HUBP0_SURFACE_CONFIG);
        ULONG xbar = amdgpu_rreg32(ad, dcn + DCN_HUBPRET0_CONTROL);
        ULONG fmt  = amdgpu_rreg32(ad, dcn + DCN_CNVC_CFG0_FORMAT_CONTROL);
        bug("[amdgpu-dcn] SCANOUT pixfmt=%lu HUBP-XBAR G=%lu B=%lu R=%lu | "
            "DPP-XBAR R=%lu G=%lu B=%lu (HUBPRET=0x%08lx FMT_CTRL=0x%08lx)\n",
            (unsigned long)(cfg & DCN_SURF_PIXEL_FORMAT_MASK),
            (unsigned long)((xbar >> DCN_CROSSBAR_Y_G_SHIFT) & DCN_CROSSBAR_SRC_MASK),
            (unsigned long)((xbar >> DCN_CROSSBAR_CB_B_SHIFT) & DCN_CROSSBAR_SRC_MASK),
            (unsigned long)((xbar >> DCN_CROSSBAR_CR_R_SHIFT) & DCN_CROSSBAR_SRC_MASK),
            (unsigned long)((fmt >> DCN_FMT_CROSSBAR_R_SHIFT) & DCN_CROSSBAR_SRC_MASK),
            (unsigned long)((fmt >> DCN_FMT_CROSSBAR_G_SHIFT) & DCN_CROSSBAR_SRC_MASK),
            (unsigned long)((fmt >> DCN_FMT_CROSSBAR_B_SHIFT) & DCN_CROSSBAR_SRC_MASK),
            (unsigned long)xbar, (unsigned long)fmt);
    }
    return TRUE;
}

/*
 * Set the scanout's PIXEL FORMAT.
 *
 * This is what a real driver does for a depth change: the display engine can
 * scan out RGB565 natively, so a 16-bit screenmode should reprogram the surface
 * format rather than have the refresh path convert every frame in software.
 * Converting is a workaround; changing the scanout is the actual mechanism, and
 * it is what every other driver does.
 *
 * Format only -- not timing. Resolution changes still need the OTG/DCCG work
 * this driver does not have yet, but depth does not require it: the raster is
 * unchanged, only the bytes-per-pixel the fetcher reads.
 *
 * Returns FALSE for a bpp with no mapping, so the caller can refuse the mode
 * instead of leaving the scanout misconfigured.
 */
BOOL amdgpu_dcn_set_scanout_format(struct AmdgpuDevice *ad, ULONG bytesperpixel)
{
    ULONG want, cfg, got;

    if (!ad || !ad->dmu_seg[2])
        return FALSE;

    switch (bytesperpixel)
    {
        case 2:  want = DCN_SURF_FMT_RGB565;   break;
        case 4:  want = DCN_SURF_FMT_ARGB8888; break;
        default:
            bug("[amdgpu-dcn] scanout format: no mapping for %lu bytes/pixel\n",
                (unsigned long)bytesperpixel);
            return FALSE;
    }

    cfg = amdgpu_rreg32(ad, ad->dmu_seg[2] + DCN_HUBP0_SURFACE_CONFIG);
    got = cfg & DCN_SURF_PIXEL_FORMAT_MASK;
    if (got == want)
        return TRUE;                      /* already right, do not disturb the pipe */

    cfg = (cfg & ~DCN_SURF_PIXEL_FORMAT_MASK)
        | ((want << DCN_SURF_PIXEL_FORMAT_SHIFT) & DCN_SURF_PIXEL_FORMAT_MASK);
    amdgpu_wreg32(ad, ad->dmu_seg[2] + DCN_HUBP0_SURFACE_CONFIG, cfg);

    got = amdgpu_rreg32(ad, ad->dmu_seg[2] + DCN_HUBP0_SURFACE_CONFIG)
        & DCN_SURF_PIXEL_FORMAT_MASK;
    bug("[amdgpu-dcn] scanout format -> %lu bpp (SURFACE_PIXEL_FORMAT wrote %lu read %lu) %s\n",
        (unsigned long)bytesperpixel, (unsigned long)want, (unsigned long)got,
        (got == want) ? "OK" : "*** REFUSED ***");

    return (got == want);
}

void amdgpu_dcn_set_scanout(struct AmdgpuDevice *ad, UQUAD mc)
{
    ULONG dcn;

    if (!ad->rmmio || !ad->ip.dmu.valid)
        return;
    dcn = ad->dmu_seg[2];
    if (!dcn)
        return;

    /* HIGH first, then LOW — the LOW write latches the new scanout base
     * (radeonsi dcn401 dcn401_hubp_set_surface_address ordering). */
    amdgpu_wreg32(ad, dcn + DCN_HUBP0_SURFACE_ADDR_HIGH, (ULONG)(mc >> 32));
    amdgpu_wreg32(ad, dcn + DCN_HUBP0_SURFACE_ADDR,      (ULONG)(mc & 0xFFFFFFFFUL));

    /*
     * Read it back. The surface address is DOUBLE-BUFFERED like the rest of the
     * plane state, so a write that is not bracketed by the OTG update lock can
     * be accepted and never latched -- the register reports the new value while
     * the pipe keeps fetching the old surface.
     *
     * That distinction is not academic: a boot that logged this retarget as
     * done still displayed a stale frame, and without a read-back there was no
     * way to tell "the address did not take" from "the address took and the
     * contents are wrong". Two very different bugs that look identical on
     * screen.
     */
    {
        ULONG hi = amdgpu_rreg32(ad, dcn + DCN_HUBP0_SURFACE_ADDR_HIGH);
        ULONG lo = amdgpu_rreg32(ad, dcn + DCN_HUBP0_SURFACE_ADDR);
        UQUAD got = ((UQUAD)hi << 32) | lo;

        bug("[amdgpu-dcn] scanout addr -> 0x%08x%08x (read 0x%08x%08x) %s\n",
            (ULONG)(mc >> 32), (ULONG)mc, hi, lo,
            (got == mc) ? "OK" : "*** NOT LATCHED ***");
    }
}
