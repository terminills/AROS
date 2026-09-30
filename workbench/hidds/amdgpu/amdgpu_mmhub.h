/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    MMHUB register offsets by IP version.
*/

#ifndef AMDGPU_MMHUB_H
#define AMDGPU_MMHUB_H

#include <exec/types.h>

/*
 * Dword offsets from the discovered MMHUB base (all BASE_IDX 0). Only the
 * registers this driver programs are listed. Field layouts of these
 * registers match across the versions below, so the values written do not
 * depend on the version; only where they go does.
 */
struct AmdgpuMmhubRegs {
    const char *linux_name;         /* the Linux file that drives it */
    UWORD fb_location_base, fb_location_top, fb_offset;
    UWORD agp_top, agp_bot, agp_base;
    UWORD system_aperture_low, system_aperture_high;
    UWORD system_aperture_default_lsb, system_aperture_default_msb;
    UWORD mx_l1_tlb_cntl;
    UWORD l2_cntl, l2_cntl2, l2_cntl3, l2_cntl4, l2_cntl5;
    UWORD l2_protection_fault_cntl2;
    UWORD l2_protection_fault_status_lo, l2_protection_fault_status_hi; /* hi 0 = one register */
    UWORD l2_protection_fault_addr_lo, l2_protection_fault_addr_hi;
    UWORD l2_protection_fault_default_addr_lo, l2_protection_fault_default_addr_hi;
    UWORD l2_context1_identity_aperture_low_lo, l2_context1_identity_aperture_low_hi;
    UWORD l2_context1_identity_aperture_high_lo, l2_context1_identity_aperture_high_hi;
    UWORD l2_context_identity_physical_offset_lo, l2_context_identity_physical_offset_hi;
    UWORD context0_cntl;
    UWORD context0_page_table_base_lo, context0_page_table_base_hi;
    UWORD context0_page_table_start_lo, context0_page_table_start_hi;
    UWORD context0_page_table_end_lo, context0_page_table_end_hi;
    UWORD invalidate_eng0_sem, invalidate_eng0_req, invalidate_eng0_ack;
    UWORD invalidate_eng0_addr_range_lo, invalidate_eng0_addr_range_hi;
};

struct AmdgpuDevice;

/* The register map for this GPU's MMHUB, or NULL when its version has
   none yet (the caller must then leave the MMHUB alone). */
const struct AmdgpuMmhubRegs *amdgpu_mmhub_regs(struct AmdgpuDevice *ad);

#endif /* AMDGPU_MMHUB_H */
