/*
 * amdgpu.hidd — Navi44 NBIF initialization.
 *
 * Mirrors Linux nbif_v6_3_1_init_registers(). The target register lives beyond
 * Navi44's 512 KiB direct MMIO BAR, so Linux reaches it through the PF0 RSMU
 * index/data aperture. Keep that access local until a general AROS register-
 * access layer needs the same direct/indirect split.
 */

#define DEBUG 1
#include <aros/debug.h>

#include "amdgpu_intern.h"
#include "asic_reg/nbif_6_3_1_excerpt.h"

static ULONG nbif_indirect_rreg32(struct AmdgpuDevice *ad, ULONG index_dw,
                                  ULONG data_dw, ULONG byte_address)
{
    amdgpu_wreg32(ad, index_dw, byte_address);
    (void)amdgpu_rreg32(ad, index_dw);
    return amdgpu_rreg32(ad, data_dw);
}

static void nbif_indirect_wreg32(struct AmdgpuDevice *ad, ULONG index_dw,
                                 ULONG data_dw, ULONG byte_address, ULONG value)
{
    amdgpu_wreg32(ad, index_dw, byte_address);
    (void)amdgpu_rreg32(ad, index_dw);
    amdgpu_wreg32(ad, data_dw, value);
    (void)amdgpu_rreg32(ad, data_dw);
}

BOOL amdgpu_nbio_hw_init(struct AmdgpuDevice *ad)
{
    ULONG index_dw, data_dw, strap_addr;
    ULONG strap_before, strap_value, strap_after;

    if (ad->gfx_level != AMDGPU_GFX12)
        return TRUE;

    if (!ad->ip.nbio.valid || ad->ip.nbio.major != 6 ||
        ad->ip.nbio.minor != 3 || ad->ip.nbio.revision != 1) {
        D(bug("[amdgpu-nbio] unsupported NBIF version; skipping init\n"));
        return TRUE;
    }

    if (!ad->nbio_seg[1] || !ad->nbio_seg[5]) {
        D(bug("[amdgpu-nbio] NBIF BASE_IDX-1/5 unavailable; skipping init\n"));
        return TRUE;
    }

    index_dw = ad->nbio_seg[1] + NBIF_6_3_1_PF0_RSMU_INDEX_DW;
    data_dw = ad->nbio_seg[1] + NBIF_6_3_1_PF0_RSMU_DATA_DW;

    /* Clear NO_SOFT_RESET on the DEV0 functions so a subsequent soft/warm reset
     * actually cycles them. GFX/display/compute is FUNCTION 0, so EPF0 is the
     * strap that matters; the audio engine is F1, the USB-C/other is F2. The
     * original code cleared only F2 — which on 1002:7590 reads 0 already (a
     * no-op) — while EPF0 stayed set (0xb0132180, bit7=1), exempting GFX from
     * every reset (measured via GfxResetProbe nbifdump). Clear F0 and F2 both;
     * leave F1 (audio) untouched. This is a correctness fix, not a GFX-power
     * fix: clearing the strap makes GFX reset-ELIGIBLE but does not by itself
     * power the write-blocked GC domain (mode1+F0-clear still leaves
     * GRBM_STATUS=0 — GFX power is an SMU/PSP-IMU sequence, handled elsewhere). */
    {
        static const struct { ULONG dw; ULONG mask; const char *tag; } straps[] = {
            { NBIF_6_3_1_RCC_DEV0_EPF0_STRAP2_DW, NBIF_6_3_1_STRAP_NO_SOFT_RESET_DEV0_F0, "EPF0" },
            { NBIF_6_3_1_RCC_DEV0_EPF2_STRAP2_DW, NBIF_6_3_1_STRAP_NO_SOFT_RESET_DEV0_F2, "EPF2" },
        };
        int i;
        for (i = 0; i < (int)(sizeof(straps) / sizeof(straps[0])); i++) {
            strap_addr = (ad->nbio_seg[5] + straps[i].dw) << 2;
            strap_before = nbif_indirect_rreg32(ad, index_dw, data_dw, strap_addr);
            strap_value = strap_before & ~straps[i].mask;
            if (strap_value != strap_before)
                nbif_indirect_wreg32(ad, index_dw, data_dw, strap_addr, strap_value);
            strap_after = nbif_indirect_rreg32(ad, index_dw, data_dw, strap_addr);
            D(bug("[amdgpu-nbio] %s_STRAP2 byte=0x%08lx before=0x%08lx requested=0x%08lx after=0x%08lx\n",
                  straps[i].tag, (unsigned long)strap_addr, (unsigned long)strap_before,
                  (unsigned long)strap_value, (unsigned long)strap_after));
        }
    }

    D(bug("[amdgpu-nbio] NBIF 6.3.1 BASE_IDX-1=0x%08lx BASE_IDX-5=0x%08lx RSMU index/data=0x%lx/0x%lx\n",
          (unsigned long)ad->nbio_seg[1], (unsigned long)ad->nbio_seg[5],
          (unsigned long)index_dw, (unsigned long)data_dw));

    /* Doorbell aperture enable (Linux nbif_v6_3_1_hw_init -> enable_doorbell_aperture).
     * RCC_DOORBELL_APER_EN lets the CPU write the doorbell BAR. BASE_IDX 2 = nbio_seg[2].
     *
     * The SELFRING GPA aperture is DELIBERATELY NOT programmed here. The earlier claim
     * that it was required ("without it BAR2 master-aborts to 0xffffffff") rested on a
     * REFUTED premise: a doorbell BAR2 read returns 0xffffffff BY DESIGN (write-only
     * aperture) — proven on WORKING Linux via mmap(sysfs resource2), which reads the
     * SAME 0xffffffff even after a write. The full Linux amdgpu register-write trace on
     * this exact card (1002:7590) shows Linux NEVER writes the selfring registers
     * (0xf3/f4/f5). Worse, our selfring base == the doorbell BAR base (0x7400000000), so
     * enabling it makes the BIF claim CPU accesses to that base into the GPA path — a
     * candidate for why CPU doorbell writes LAND in the BAR but never reach the on-die
     * doorbell monitor (the MES/KIQ never wakes). Match Linux: enable the aperture only. */
    if (ad->doorbell_phys && ad->nbio_seg[2]) {
        ULONG seg  = ad->nbio_seg[2];
        ULONG apen = amdgpu_rreg32(ad, seg + NBIF_6_3_1_RCC_DOORBELL_APER_EN_DW);
        amdgpu_wreg32(ad, seg + NBIF_6_3_1_RCC_DOORBELL_APER_EN_DW,
                      apen | NBIF_6_3_1_RCC_BIF_DOORBELL_APER_EN);
        D(bug("[amdgpu-nbio] doorbell aperture EN (selfring NOT programmed, matching Linux)\n"));
    }

    return TRUE;
}
