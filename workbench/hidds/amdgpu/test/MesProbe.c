/* MesProbe - inspect Navi44 MES firmware headers and live MES queue state. */
#define DEBUG 1
#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/oop.h>
#include <hidd/pci.h>
#include "amdgpu_intern.h"

struct AmdgpuBase amdgpuBase;

static ULONG le32(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) |
           ((ULONG)p[3] << 24);
}

static ULONG cfg_r32(OOP_Object *dev, UWORD reg)
{
    struct pHidd_PCIDevice_ReadConfigLong m = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice,
                               moHidd_PCIDevice_ReadConfigLong),
        .reg = reg,
    };
    return (ULONG)OOP_DoMethod(dev, (OOP_Msg)&m);
}

static void inspect_blob(CONST_STRPTR name)
{
    BPTR fh;
    UBYTE h[80];
    LONG got;

    fh = Open(name, MODE_OLDFILE);
    if (!fh) {
        bug("[MesProbe] missing %s\n", name);
        return;
    }
    got = Read(fh, h, sizeof(h));
    Close(fh);
    if (got < 64) {
        bug("[MesProbe] %s short: %ld bytes\n", name, got);
        return;
    }
    bug("[MesProbe] %s: len>=%ld version=0x%08lx start=0x%08lx%08lx "
        "ucode_size=%lu ucode_off=%lu data_size=%lu data_off=%lu\n",
        name, got, (unsigned long)le32(h + 0),
        (unsigned long)le32(h + 60), (unsigned long)le32(h + 56),
        (unsigned long)le32(h + 36), (unsigned long)le32(h + 40),
        (unsigned long)le32(h + 48), (unsigned long)le32(h + 52));
}

int main(void)
{
    struct AmdgpuDevice *ad;

    bug("[MesProbe] firmware/header comparison begins\n");
    inspect_blob("DEVS:Firmware/amdgpu/gc_12_0_0_uni_mes.bin");
    inspect_blob("DEVS:Firmware/amdgpu/gc_12_0_0_mes.bin");
    inspect_blob("DEVS:Firmware/amdgpu/gc_12_0_0_mes1.bin");

    amdgpuBase.pciDeviceAB = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDevice);
    amdgpuBase.pciDriverAB = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDriver);
    if (!amdgpuBase.pciDeviceAB || !amdgpu_pci_probe(&amdgpuBase)) {
        bug("[MesProbe] AMD GPU probe failed\n");
        return 20;
    }
    ForeachNode(&amdgpuBase.devices, ad) {
        if (!amdgpu_mmio_map(&amdgpuBase, ad)) {
            bug("[MesProbe] MMIO map failed\n");
            continue;
        }
        amdgpu_discovery_init(ad, amdgpu_get_vram_mb(ad));
        if (!ad->gc_seg[1]) {
            bug("[MesProbe] GC segment unavailable\n");
            continue;
        }
        bug("[MesProbe] device %04lx:%04lx GC0=0x%05lx GC1=0x%05lx "
            "MES_CNTL=0x%08lx MES_PC=0x%08lx\n",
            (unsigned long)ad->vendorid, (unsigned long)ad->deviceid,
            (unsigned long)ad->gc_seg[0], (unsigned long)ad->gc_seg[1],
            (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[1] + 0x2807),
            (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[1] + 0x2813));
        bug("[MesProbe] CP_STAT=0x%08lx GFX_HQD_ACTIVE=0x%08lx "
            "GFX_HQD_BASE=0x%08lx\n",
            (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[0] + 0x0f40),
            (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[0] + 0x1e80),
            (unsigned long)amdgpu_rreg32(ad, ad->gc_seg[0] + 0x1e86));

        bug("[MesProbe] PCI COMMAND=0x%08lx BARs:",
            (unsigned long)(cfg_r32(ad->pciDevice, 0x04) & 0xffff));
        {
            int i;
            for (i = 0; i < 6; i++)
                bug(" %d=0x%08lx", i,
                    (unsigned long)cfg_r32(ad->pciDevice,
                                           (UWORD)(0x10 + i * 4)));
            bug("\n");
        }
        bug("[MesProbe] classified VRAM BAR=0x%p size=0x%08lx "
            "doorbell BAR=0x%p size=0x%08lx\n",
            (APTR)ad->vram_bar_phys, (unsigned long)ad->vram_bar_size,
            (APTR)ad->doorbell_phys, (unsigned long)ad->doorbell_size);

        /* This is deliberately an isolated probe of the suspected MES wedge.
         * Do not submit a queue: first establish whether BAR2 is decoded and
         * whether NBIO accepts the aperture-enable bit. */
        if (ad->doorbell_phys && ad->nbio_seg[2]) {
            ULONG dbsz = ad->doorbell_size && ad->doorbell_size < 0x100000
                       ? (ULONG)ad->doorbell_size : 0x100000;
            volatile ULONG *db = (volatile ULONG *)HIDD_PCIDriver_MapPCI(
                ad->pciDriver, (APTR)ad->doorbell_phys, dbsz);
            ULONG ap = ad->nbio_seg[2] + 0x00c0;
            ULONG ap_before = amdgpu_rreg32(ad, ap);
            ULONG db_before = db ? db[0] : 0xffffffffUL;
            ULONG ap_after;
            ULONG db_after;

            bug("[MesProbe] doorbell phys=0x%08lx size=0x%08lx cpu=%p "
                "BAR2[0] before=0x%08lx\n",
                (unsigned long)ad->doorbell_phys,
                (unsigned long)ad->doorbell_size, (APTR)db,
                (unsigned long)db_before);
            bug("[MesProbe] RCC_DOORBELL_APER_EN @0x%05lx before=0x%08lx\n",
                (unsigned long)ap, (unsigned long)ap_before);

            if (db) {
                amdgpu_wreg32(ad, ap, ap_before | 1UL);
                ap_after = amdgpu_rreg32(ad, ap);
                db_after = db[0];
                bug("[MesProbe] RCC_DOORBELL_APER_EN after=0x%08lx "
                    "BAR2[0] after=0x%08lx\n",
                    (unsigned long)ap_after, (unsigned long)db_after);
                bug("[MesProbe] verdict aperture=%s bar2_decode=%s\n",
                    (ap_after & 1) ? "LATCHED" : "NOT_LATCHED",
                    (db_after != 0xffffffffUL) ? "CHANGED" : "ALL_ONES");
            }
        } else {
            bug("[MesProbe] doorbell probe unavailable: phys=0x%08lx "
                "nbio_seg2=0x%05lx\n",
                (unsigned long)ad->doorbell_phys,
                (unsigned long)ad->nbio_seg[2]);
        }
    }
    return 0;
}
