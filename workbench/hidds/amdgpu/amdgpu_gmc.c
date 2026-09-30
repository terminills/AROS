/*
 * amdgpu.hidd — GMC (graphics memory controller) observe.
 *
 * Read-only: report the memory-controller aperture map the firmware programmed —
 * the VRAM MC window, the AGP aperture, and the system (GTT) aperture. This is the
 * foundation a later tier needs to place buffers by MC address (the PSP firmware
 * staging already relies on the VRAM base derived here). Registers live in the
 * MMHUB block (BASE_IDX 0 = the discovered mmhub base), at the offsets of its
 * IP version (amdgpu_mmhub.c). No writes.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>

#include "amdgpu_intern.h"
#include "amdgpu_vram.h"
#include "amdgpu_mmhub.h"

BOOL amdgpu_gmc_hw_init(struct AmdgpuDevice *ad)
{
    const struct AmdgpuMmhubRegs *r = amdgpu_mmhub_regs(ad);
    ULONG mb, fb_base, fb_top, fb_off, agp_base, agp_top, agp_bot, sa_lo, sa_hi;
    UQUAD vram_start, vram_end;

    if (!ad->rmmio || !ad->ip.mmhub.valid) {
        D(bug("[amdgpu-gmc] no MMHUB block — skipping memory-map read\n"));
        return TRUE;
    }
    if (!r) {
        bug("[amdgpu-gmc] MMHUB %u.%u.%u has no register map yet — skipping memory-map read\n",
            ad->ip.mmhub.major, ad->ip.mmhub.minor, ad->ip.mmhub.revision);
        return TRUE;
    }
    mb = ad->ip.mmhub.base;

    fb_base  = amdgpu_rreg32(ad, mb + r->fb_location_base);
    fb_top   = amdgpu_rreg32(ad, mb + r->fb_location_top);
    fb_off   = amdgpu_rreg32(ad, mb + r->fb_offset);
    agp_base = amdgpu_rreg32(ad, mb + r->agp_base);
    agp_top  = amdgpu_rreg32(ad, mb + r->agp_top);
    agp_bot  = amdgpu_rreg32(ad, mb + r->agp_bot);
    sa_lo    = amdgpu_rreg32(ad, mb + r->system_aperture_low);
    sa_hi    = amdgpu_rreg32(ad, mb + r->system_aperture_high);

    /* FB base/top are the MC address in 24-bit (16 MiB) units. */
    vram_start = ((UQUAD)(fb_base & 0x00FFFFFFUL)) << 24;
    vram_end   = (((UQUAD)(fb_top & 0x00FFFFFFUL)) << 24) | 0x00FFFFFFUL;
    amdgpu_vram_record_gmc(ad, fb_base, fb_top);

    D(bug("[amdgpu-gmc] VRAM MC window [0x%08lx%08lx .. 0x%08lx%08lx]  fb_offset=0x%08lx\n",
          (unsigned long)(vram_start >> 32), (unsigned long)(vram_start & 0xFFFFFFFFUL),
          (unsigned long)(vram_end >> 32), (unsigned long)(vram_end & 0xFFFFFFFFUL),
          (unsigned long)fb_off));
    D(bug("[amdgpu-gmc] AGP base=0x%08lx top=0x%08lx bot=0x%08lx\n",
          (unsigned long)agp_base, (unsigned long)agp_top, (unsigned long)agp_bot));
    D(bug("[amdgpu-gmc] system(GTT) aperture low=0x%08lx high=0x%08lx\n",
          (unsigned long)sa_lo, (unsigned long)sa_hi));

    return TRUE;
}
