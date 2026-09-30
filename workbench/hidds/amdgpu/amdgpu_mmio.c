/*
 * amdgpu.hidd — register aperture mapping and MMIO access.
 *
 * VRAM size readout per generation follows the kernel's nbio
 * get_memsize() callbacks: NBIO 2.3 for Navi 1x/2x (the V620 path,
 * verified against nbio_v2_3.c + navi10_ip_offset.h — see
 * asic_reg/nbio_2_3_excerpt.h). GFX11/12 use different NBIO
 * revisions and, properly, the IP-discovery table — implemented when
 * the 9060 XT is on the bench rather than guessed at now.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/oop.h>
#include <proto/exec.h>
#include <hidd/pci.h>

#include "amdgpu_intern.h"
#include "asic_reg/nbio_2_3_excerpt.h"
#include "asic_reg/discovery_excerpt.h"

BOOL amdgpu_mmio_map(struct AmdgpuBase *base, struct AmdgpuDevice *ad)
{
    if (ad->rmmio)
        return TRUE;
    if (!ad->rmmio_phys || !ad->rmmio_size) {
        D(bug("[amdgpu] %s: no register BAR found\n", ad->name));
        return FALSE;
    }
    if (!ad->pciDriver) {
        D(bug("[amdgpu] %s: no PCI driver object\n", ad->name));
        return FALSE;
    }

    ad->rmmio = HIDD_PCIDriver_MapPCI(ad->pciDriver,
                                      (APTR)ad->rmmio_phys,
                                      ad->rmmio_size);
    if (!ad->rmmio) {
        D(bug("[amdgpu] %s: MapPCI failed for register BAR\n", ad->name));
        return FALSE;
    }
    D(bug("[amdgpu] %s: registers mapped 0x%p -> 0x%p (%lu KB)\n",
          ad->name, (APTR)ad->rmmio_phys, (APTR)ad->rmmio,
          (unsigned long)(ad->rmmio_size >> 10)));

    /* First point at which register/VRAM aperture access becomes possible, and
     * the idempotent early-return above guarantees this runs exactly once per
     * device — so it is the correct place to arm the MM_INDEX aperture lock
     * (codex TRIG-0686) before discovery does its first amdgpu_vram_read(). */
    InitSemaphore(&ad->aperture_sem);
    return TRUE;
}

ULONG amdgpu_rreg32(struct AmdgpuDevice *ad, ULONG dword_offset)
{
    if (!ad->rmmio || (dword_offset << 2) >= ad->rmmio_size)
        return 0;
    return ad->rmmio[dword_offset];
}

void amdgpu_wreg32(struct AmdgpuDevice *ad, ULONG dword_offset, ULONG val)
{
    if (!ad->rmmio || (dword_offset << 2) >= ad->rmmio_size)
        return;
    ad->rmmio[dword_offset] = val;
}

ULONG amdgpu_get_vram_mb(struct AmdgpuDevice *ad)
{
    switch (ad->gfx_level) {
    case AMDGPU_GFX10_1:
    case AMDGPU_GFX10_3:
        /* nbio_v2_3_get_memsize(): NBIO seg2 + RCC_CONFIG_MEMSIZE.
           Resolves to the same absolute dword the kernel reads pre-
           discovery (0xde3); value is VRAM MB. */
        return amdgpu_rreg32(ad, NBIO_2_3_RCC_CONFIG_MEMSIZE_DW);

    case AMDGPU_GFX11:
    case AMDGPU_GFX12:
        /* Same memsize register offset holds pre-discovery on RDNA3/4;
           if discovery gave us the real NBIO base, prefer a base-
           relative read, else fall back to the absolute offset. The
           caller also passes a discovery-independent boot value. */
        if (ad->ip.nbio.valid)
            return amdgpu_rreg32(ad,
                ad->ip.nbio.base + AMDGPU_mmRCC_CONFIG_MEMSIZE_REL);
        return amdgpu_rreg32(ad, AMDGPU_mmRCC_CONFIG_MEMSIZE);

    case AMDGPU_GFX8:
    case AMDGPU_GFX9:
        /* GCN memsize path — wired when Polaris/Vega silicon is on the
           bench (RX 480s unverified/delayed). Honest 0. */
        return 0;

    default:
        return 0;
    }
}
