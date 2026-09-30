#ifndef AROS_NBIF_6_3_1_EXCERPT_H
#define AROS_NBIF_6_3_1_EXCERPT_H

/* Minimal Navi44 NBIF 6.3.1 register subset from Linux amdgpu:
 *   nbif_6_3_1_offset.h
 *   nbif_6_3_1_sh_mask.h
 *
 * Offsets and discovery bases are DWORD units. The indirect aperture accepts
 * a BYTE address for registers beyond the direct MMIO BAR.
 */

#define NBIF_6_3_1_PF0_RSMU_INDEX_DW                    0x00000000UL
#define NBIF_6_3_1_PF0_RSMU_DATA_DW                     0x00000001UL

/* Per-endpoint-function STRAP2 registers are strided 0x80 dwords apart:
 *   EPF0 = 0xd002, EPF1 = 0xd082, EPF2 = 0xd102.
 * Confirmed empirically on 1002:7590 (GfxResetProbe nbifdump): EPF0 = 0xb0132180,
 * EPF1 = 0x00132080, EPF2 = 0x03132000 (shared 0x132 field, F2 anchor known).
 * NO_SOFT_RESET (bit7) exempts a function from soft/warm reset when SET; the
 * driver CLEARS it to make the function reset-eligible. GFX/display/compute is
 * function 0, so EPF0 is the strap that gates whether a reset cycles GFX. */
#define NBIF_6_3_1_RCC_DEV0_EPF0_STRAP2_DW              0x0000d002UL
#define NBIF_6_3_1_RCC_DEV0_EPF1_STRAP2_DW              0x0000d082UL
#define NBIF_6_3_1_RCC_DEV0_EPF2_STRAP2_DW              0x0000d102UL
#define NBIF_6_3_1_STRAP_NO_SOFT_RESET_DEV0_F0          0x00000080UL
#define NBIF_6_3_1_STRAP_NO_SOFT_RESET_DEV0_F1          0x00000080UL
#define NBIF_6_3_1_STRAP_NO_SOFT_RESET_DEV0_F2          0x00000080UL

/* Doorbell apertures (nbif_6_3_1_offset.h). All BASE_IDX 2 -> nbio_seg[2], within
 * the direct 512 KiB MMIO BAR, so plain amdgpu_wreg32 (no RSMU indirect). Linux
 * nbif_v6_3_1_hw_init programs BOTH: RCC_DOORBELL_APER_EN lets the CPU write the
 * doorbell BAR; the SELFRING GPA aperture routes doorbell-BAR accesses to the
 * on-die doorbell RAM. Without the selfring aperture BAR2 master-aborts to
 * 0xffffffff even with APER_EN set, the base correct, and MEM_SPACE enabled. */
#define NBIF_6_3_1_RCC_DOORBELL_APER_EN_DW              0x000000c0UL
#define NBIF_6_3_1_RCC_BIF_DOORBELL_APER_EN             0x00000001UL   /* bit0 */
#define NBIF_6_3_1_DOORBELL_SELFRING_GPA_APER_BASE_HIGH_DW  0x000000f3UL
#define NBIF_6_3_1_DOORBELL_SELFRING_GPA_APER_BASE_LOW_DW   0x000000f4UL
#define NBIF_6_3_1_DOORBELL_SELFRING_GPA_APER_CNTL_DW       0x000000f5UL
/* CNTL fields: EN bit0, MODE bit1, SIZE bit8. Linux writes EN|MODE, SIZE=0. */
#define NBIF_6_3_1_SELFRING_GPA_APER_CNTL_ENABLE        0x00000003UL

#endif /* AROS_NBIF_6_3_1_EXCERPT_H */
