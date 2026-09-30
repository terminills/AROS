/*
 * amdgpu.hidd — PCI discovery.
 *
 * Enumerates AMD display-class devices via pci.hidd and records BAR
 * geometry. BAR roles on GCN+/RDNA parts follow a stable pattern:
 * the largest prefetchable BAR is the VRAM aperture, the small
 * (256KB–1MB) memory BAR is the register aperture, and on RDNA the
 * 2–8MB BAR is the doorbell aperture. We classify by size/type
 * rather than hardcoding indices, which survives ReBAR.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/oop.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <hidd/pci.h>
#include <hardware/pci.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>

#include "amdgpu_intern.h"

struct AmdgpuEnumData {
    struct AmdgpuBase *base;
};

static UWORD amdgpu_pci_command(OOP_Object *pciDevice)
{
    struct pHidd_PCIDevice_ReadConfigWord msg = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice,
                               moHidd_PCIDevice_ReadConfigWord),
        .reg = PCICS_COMMAND,
    };

    return (UWORD)OOP_DoMethod(pciDevice, (OOP_Msg)&msg);
}

static BOOL amdgpu_pci_enable(struct AmdgpuBase *base,
                              OOP_Object *pciDevice,
                              UWORD vendorid, UWORD deviceid)
{
    struct TagItem enable[] = {
        { base->pciDeviceAB + aoHidd_PCIDevice_isMEM, TRUE },
        { base->pciDeviceAB + aoHidd_PCIDevice_isMaster, TRUE },
        { TAG_DONE, 0 },
    };
    IPTR mem = FALSE, master = FALSE;
    UWORD before = amdgpu_pci_command(pciDevice);
    UWORD after;

    OOP_SetAttrs(pciDevice, enable);
    after = amdgpu_pci_command(pciDevice);
    OOP_GetAttr(pciDevice, base->pciDeviceAB + aoHidd_PCIDevice_isMEM,
                &mem);
    OOP_GetAttr(pciDevice, base->pciDeviceAB + aoHidd_PCIDevice_isMaster,
                &master);

    D(bug("[amdgpu:pci] %04lx:%04lx command 0x%04lx -> 0x%04lx "
          "MEM=%lu MASTER=%lu\n",
          (unsigned long)vendorid, (unsigned long)deviceid,
          (unsigned long)before, (unsigned long)after,
          (unsigned long)mem, (unsigned long)master));

    if (!mem || !master ||
        !(after & PCICMF_MEMDECODE) || !(after & PCICMF_BUSMASTER)) {
        bug("[amdgpu:pci] %04lx:%04lx PCI enable FAILED; refusing DMA bring-up\n",
            (unsigned long)vendorid, (unsigned long)deviceid);
        return FALSE;
    }

    return TRUE;
}

static void amdgpu_record_bars(struct AmdgpuBase *base,
                               struct AmdgpuDevice *ad)
{
    static const ULONG base_attr[] = {
        aoHidd_PCIDevice_Base0, aoHidd_PCIDevice_Base1,
        aoHidd_PCIDevice_Base2, aoHidd_PCIDevice_Base3,
        aoHidd_PCIDevice_Base4, aoHidd_PCIDevice_Base5,
    };
    static const ULONG size_attr[] = {
        aoHidd_PCIDevice_Size0, aoHidd_PCIDevice_Size1,
        aoHidd_PCIDevice_Size2, aoHidd_PCIDevice_Size3,
        aoHidd_PCIDevice_Size4, aoHidd_PCIDevice_Size5,
    };
    int i;

    for (i = 0; i < 6; i++) {
        IPTR pbase = 0, psize = 0;

        OOP_GetAttr(ad->pciDevice, base->pciDeviceAB + base_attr[i], &pbase);
        OOP_GetAttr(ad->pciDevice, base->pciDeviceAB + size_attr[i], &psize);
        if (!pbase || !psize)
            continue;

        D(bug("[amdgpu]   BAR%d: 0x%p, %lu KB\n", i, (APTR)pbase,
              (unsigned long)(psize >> 10)));

        /* BAR2 is the doorbell aperture on RDNA (Linux pci_resource_start(pdev,2)),
         * regardless of size — on Navi44 it is 256 MB, far outside the old 2-8 MB
         * heuristic, so classify it by index. We only need a small window. */
        if (i == 2 && !ad->doorbell_phys) {
            ad->doorbell_phys = pbase;
            ad->doorbell_size = psize;
            continue;
        }

        if (psize >= (256 << 10) && psize <= (1 << 20)) {
            /* register aperture */
            if (!ad->rmmio_phys) {
                ad->rmmio_phys = pbase;
                ad->rmmio_size = psize;
            }
        } else if (psize >= (2 << 20) && psize <= (8 << 20)) {
            /* doorbell aperture (RDNA) */
            if (!ad->doorbell_phys) {
                ad->doorbell_phys = pbase;
                ad->doorbell_size = psize;
            }
        } else if (psize > ad->vram_bar_size) {
            /* largest BAR = VRAM aperture */
            ad->vram_bar_phys = pbase;
            ad->vram_bar_size = psize;
        }
    }
}

static AROS_UFH3(void, amdgpu_enum_cb,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(OOP_Object *, pciDevice, A2),
    AROS_UFHA(APTR, message, A1))
{
    AROS_USERFUNC_INIT

    struct AmdgpuEnumData *ed = hook->h_Data;
    struct AmdgpuBase *base = ed->base;
    const struct AmdgpuPciId *id;
    struct AmdgpuDevice *ad;
    IPTR vendorid = 0, deviceid = 0, revision = 0, driver = 0;

    OOP_GetAttr(pciDevice, base->pciDeviceAB + aoHidd_PCIDevice_VendorID,
                &vendorid);
    OOP_GetAttr(pciDevice, base->pciDeviceAB + aoHidd_PCIDevice_ProductID,
                &deviceid);
    OOP_GetAttr(pciDevice, base->pciDeviceAB + aoHidd_PCIDevice_RevisionID,
                &revision);
    OOP_GetAttr(pciDevice, base->pciDeviceAB + aoHidd_PCIDevice_Driver,
                &driver);

    /* Match Linux's pci_enable_device() + pci_set_master() contract before
     * touching MMIO or handing any system-memory address to the GPU.  UEFI
     * usually leaves both bits set on the DUT, but the driver must not depend
     * on firmware state. */
    if (!amdgpu_pci_enable(base, pciDevice, (UWORD)vendorid,
                           (UWORD)deviceid))
        return;

    id = amdgpu_identify((UWORD)vendorid, (UWORD)deviceid);

    /* Match ANY AMD (0x1002) display-class device — this mirrors Linux amdgpu's
     * CHIP_IP_DISCOVERY wildcard: modern AMD GPUs (RDNA3/RDNA4+) are NOT in a
     * static PCI-ID list upstream; identity (GFX/DCN/SDMA/VCN IP versions) is
     * read at runtime from the IP-discovery table in VRAM. So amdgpu_ids[] is
     * now only an optional NAME/quirk hint, NOT a bring-up gate: a table miss
     * still enters the tier-1 ladder and resolves via IP discovery, so current
     * AND future cards work with zero table edits. The ladder is observe-only
     * (maps registers, reads IDs, claims nothing), so this is safe on an
     * unvalidated part. */
    ad = AllocMem(sizeof(*ad), MEMF_CLEAR | MEMF_ANY);
    if (!ad)
        return;

    ad->pciDevice = pciDevice;
    ad->pciDriver = (OOP_Object *)driver;
    ad->vendorid  = (UWORD)vendorid;
    ad->deviceid  = (UWORD)deviceid;
    ad->revision  = (UBYTE)revision;
    if (id) {
        ad->gfx_level = id->gfx_level;
        ad->tested    = id->tested;
        ad->name      = id->name;
        D(bug("[amdgpu] found %04lx:%04lx rev %02lx — %s [%s]%s\n",
              (unsigned long)vendorid, (unsigned long)deviceid,
              (unsigned long)revision, id->name,
              amdgpu_gfx_level_name(id->gfx_level),
              id->tested ? "" : " (not lab-validated yet)"));
    } else {
        ad->gfx_level = AMDGPU_GFX_UNKNOWN;   /* resolved by IP discovery */
        ad->tested    = FALSE;
        ad->name      = "AMD GPU (via IP discovery)";
        D(bug("[amdgpu] found %04lx:%04lx rev %02lx — not in amdgpu_ids[]; "
              "identifying via IP discovery (no table entry needed)\n",
              (unsigned long)vendorid, (unsigned long)deviceid,
              (unsigned long)revision));
    }
    amdgpu_record_bars(base, ad);

    AddTail((struct List *)&base->devices, (struct Node *)ad);
    base->device_count++;

    AROS_USERFUNC_EXIT
}

BOOL amdgpu_pci_probe(struct AmdgpuBase *base)
{
    OOP_Object *pci;
    struct Hook hook;
    struct AmdgpuEnumData ed = { base };

    NEWLIST(&base->devices);
    base->device_count = 0;

    pci = OOP_NewObject(NULL, CLID_Hidd_PCI, NULL);
    if (!pci) {
        D(bug("[amdgpu] no PCI subsystem\n"));
        return FALSE;
    }

    hook.h_Entry = (HOOKFUNC)amdgpu_enum_cb;
    hook.h_Data  = &ed;

    struct TagItem reqs[] = {
        { tHidd_PCI_VendorID, 0x1002 },
        { tHidd_PCI_Class,    0x03 },       /* display controller */
        { TAG_DONE,           0 }
    };

    struct pHidd_PCI_EnumDevices enumMsg = {
        .mID = OOP_GetMethodID(IID_Hidd_PCI, moHidd_PCI_EnumDevices),
        .callback = &hook,
        .requirements = reqs,
    };
    OOP_DoMethod(pci, (OOP_Msg)&enumMsg);
    OOP_DisposeObject(pci);

    D(bug("[amdgpu] probe complete: %lu supported device(s)\n",
          (unsigned long)base->device_count));
    return base->device_count > 0;
}
