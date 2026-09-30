/*
 * amdgpu.hidd — library init: probe, identify, run the tier-1 ladder,
 * report. Deliberately observe-only at this tier: nothing is claimed,
 * no display modes are registered, and the radeonsi monolith keeps
 * driving any AMD GPU. This driver earns ownership tier by tier on
 * real hardware.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <aros/symbolsets.h>
#include <proto/oop.h>
#include <hidd/pci.h>

#include "amdgpu_intern.h"
#include "amdgpu_gfx_intern.h"
#include "amdgpu_bo.h"
#include "amdgpu_gart.h"
#include "amdgpu_cs.h"
#include "amdgpu_query_internal.h"

struct AmdgpuBase amdgpuBase;

static int Amdgpu_Init(APTR libbase)
{
    struct AmdgpuHiddBase *AmdgpuHiddBase = (struct AmdgpuHiddBase *)libbase;
    struct AmdgpuDevice *ad;

    D(bug("[amdgpu] base driver (clean bring-up, tier 1) — "
          "ACD-amdgpu-driver-stack-split\n"));

    amdgpuBase.pciDeviceAB = OOP_ObtainAttrBase(IID_Hidd_PCIDevice);
    amdgpuBase.pciDriverAB = OOP_ObtainAttrBase(IID_Hidd_PCIDriver);
    if (!amdgpuBase.pciDeviceAB) {
        D(bug("[amdgpu] no PCIDevice attrbase — PCI hidd not present?\n"));
        return TRUE;    /* library loads; there is just nothing to do */
    }

    /* The device ladder maps firmware/rings before the display is published. */
    amdgpu_gart_service_init();
    if (amdgpu_pci_probe(&amdgpuBase)) {
        ForeachNode(&amdgpuBase.devices, ad) {
            amdgpu_device_init(&amdgpuBase, ad);
            amdgpu_render_info_init(ad);
        }

        D(bug("[amdgpu] tier-1 ladder complete for %lu device(s); "
              "observe-only, claiming nothing\n",
              (unsigned long)amdgpuBase.device_count));
    }

    /*
     * Boot-mode display: adopt the firmware/GOP framebuffer as a real AROS
     * gfx-HIDD display (bootloader.resource VesaInfo — the surface the DCN is
     * already scanning out). Registers a DDRV_BootMode driver so the desktop
     * paints on the AMD GPU, and is auto-superseded when a native-mode driver
     * arrives. Runs after the wake loop; the gfx class ptr (gsd.gfxclass) was
     * populated by genmodule's class setup before this init.
     */
    AmdgpuGfx_InitDisplay(AmdgpuHiddBase);
    amdgpu_bo_service_init(&AmdgpuHiddBase->gsd.data);
    amdgpu_cs_service_init(&AmdgpuHiddBase->gsd.data);

    return TRUE;
}

static int Amdgpu_Expunge(APTR libbase)
{
    if (amdgpuBase.pciDeviceAB)
        OOP_ReleaseAttrBase(IID_Hidd_PCIDevice);
    if (amdgpuBase.pciDriverAB)
        OOP_ReleaseAttrBase(IID_Hidd_PCIDriver);
    return TRUE;
}

ADD2INITLIB(Amdgpu_Init, 0)
ADD2EXPUNGELIB(Amdgpu_Expunge, 0)
