/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    MMHUB register offsets by IP version, from Linux's
    mmhub_2_0_0_offset.h and mmhub_4_1_0_offset.h.
*/

#define DEBUG 1
#include <aros/debug.h>

#include "amdgpu_intern.h"
#include "amdgpu_mmhub.h"

/* MMHUB 2.0.x-2.1.x: Navi 1x and Navi 2x (Linux mmhub_v2_0.c) */
static const struct AmdgpuMmhubRegs mmhub_v2_0_regs = {
    .linux_name = "mmhub_v2_0",
    .fb_location_base = 0x086c, .fb_location_top = 0x086d, .fb_offset = 0x0857,
    .agp_top = 0x086e, .agp_bot = 0x086f, .agp_base = 0x0870,
    .system_aperture_low = 0x0871, .system_aperture_high = 0x0872,
    .system_aperture_default_lsb = 0x0858, .system_aperture_default_msb = 0x0859,
    .mx_l1_tlb_cntl = 0x0873,
    .l2_cntl = 0x0680, .l2_cntl2 = 0x0681, .l2_cntl3 = 0x0682,
    .l2_cntl4 = 0x0698, .l2_cntl5 = 0x06a1,
    .l2_protection_fault_cntl2 = 0x0689,
    .l2_protection_fault_status_lo = 0x068c, .l2_protection_fault_status_hi = 0,
    .l2_protection_fault_addr_lo = 0x068d, .l2_protection_fault_addr_hi = 0x068e,
    .l2_protection_fault_default_addr_lo = 0x068f, .l2_protection_fault_default_addr_hi = 0x0690,
    .l2_context1_identity_aperture_low_lo = 0x0692, .l2_context1_identity_aperture_low_hi = 0x0693,
    .l2_context1_identity_aperture_high_lo = 0x0694, .l2_context1_identity_aperture_high_hi = 0x0695,
    .l2_context_identity_physical_offset_lo = 0x0696, .l2_context_identity_physical_offset_hi = 0x0697,
    .context0_cntl = 0x06c0,
    .context0_page_table_base_lo = 0x072b, .context0_page_table_base_hi = 0x072c,
    .context0_page_table_start_lo = 0x074b, .context0_page_table_start_hi = 0x074c,
    .context0_page_table_end_lo = 0x076b, .context0_page_table_end_hi = 0x076c,
    .invalidate_eng0_sem = 0x06d1, .invalidate_eng0_req = 0x06e3, .invalidate_eng0_ack = 0x06f5,
    .invalidate_eng0_addr_range_lo = 0x0707, .invalidate_eng0_addr_range_hi = 0x0708,
};

/* MMHUB 4.1.0: Navi 4x (Linux mmhub_v4_1_0.c) */
static const struct AmdgpuMmhubRegs mmhub_v4_1_0_regs = {
    .linux_name = "mmhub_v4_1_0",
    .fb_location_base = 0x0554, .fb_location_top = 0x0555, .fb_offset = 0x04c7,
    .agp_top = 0x0556, .agp_bot = 0x0557, .agp_base = 0x0558,
    .system_aperture_low = 0x0559, .system_aperture_high = 0x055a,
    .system_aperture_default_lsb = 0x04c8, .system_aperture_default_msb = 0x04c9,
    .mx_l1_tlb_cntl = 0x055b,
    .l2_cntl = 0x04e4, .l2_cntl2 = 0x04e5, .l2_cntl3 = 0x04e6,
    .l2_cntl4 = 0x04fd, .l2_cntl5 = 0x0503,
    .l2_protection_fault_cntl2 = 0x04ed,
    .l2_protection_fault_status_lo = 0x04f0, .l2_protection_fault_status_hi = 0x04f1,
    .l2_protection_fault_addr_lo = 0x04f2, .l2_protection_fault_addr_hi = 0x04f3,
    .l2_protection_fault_default_addr_lo = 0x04f4, .l2_protection_fault_default_addr_hi = 0x04f5,
    .l2_context1_identity_aperture_low_lo = 0x04f7, .l2_context1_identity_aperture_low_hi = 0x04f8,
    .l2_context1_identity_aperture_high_lo = 0x04f9, .l2_context1_identity_aperture_high_hi = 0x04fa,
    .l2_context_identity_physical_offset_lo = 0x04fb, .l2_context_identity_physical_offset_hi = 0x04fc,
    .context0_cntl = 0x0564,
    .context0_page_table_base_lo = 0x05cf, .context0_page_table_base_hi = 0x05d0,
    .context0_page_table_start_lo = 0x05ef, .context0_page_table_start_hi = 0x05f0,
    .context0_page_table_end_lo = 0x060f, .context0_page_table_end_hi = 0x0610,
    .invalidate_eng0_sem = 0x0575, .invalidate_eng0_req = 0x0587, .invalidate_eng0_ack = 0x0599,
    .invalidate_eng0_addr_range_lo = 0x05ab, .invalidate_eng0_addr_range_hi = 0x05ac,
};

const struct AmdgpuMmhubRegs *amdgpu_mmhub_regs(struct AmdgpuDevice *ad)
{
    const struct AmdgpuIpBase *mm = &ad->ip.mmhub;

    if (!mm->valid)
        return NULL;

    /* The same split as Linux gmc_v10_0_set_mmhub_funcs(): 2.3/2.4 are
       mmhub_v2_3, every other 2.x is mmhub_v2_0. */
    if (mm->major == 2 && !(mm->minor == 3 || mm->minor == 4))
        return &mmhub_v2_0_regs;
    if (mm->major == 4 && mm->minor == 1 && mm->revision == 0)
        return &mmhub_v4_1_0_regs;
    return NULL;
}
