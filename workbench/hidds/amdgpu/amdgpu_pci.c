/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

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

static ULONG amdgpu_pci_readl(OOP_Object *pciDevice, UWORD reg)
{
    struct pHidd_PCIDevice_ReadConfigLong msg = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice,
                               moHidd_PCIDevice_ReadConfigLong),
        .reg = reg,
    };

    return (ULONG)OOP_DoMethod(pciDevice, (OOP_Msg)&msg);
}

/* Resizable BAR control: bits 2:0 = BAR index, bits 13:8 = size as 1MB << n */
static IPTR amdgpu_rebar_size(OOP_Object *pciDevice, UBYTE bar)
{
    UWORD pos = 0x100;
    int guard = 48;

    while (pos >= 0x100 && guard--) {
        ULONG hdr = amdgpu_pci_readl(pciDevice, pos);

        if (hdr == 0 || hdr == 0xffffffff)
            break;
        if ((hdr & 0xffff) == 0x0015) {
            ULONG ctrl = amdgpu_pci_readl(pciDevice, pos + 8);
            ULONG nbars = (ctrl >> 5) & 7;
            ULONG i;

            for (i = 0; i < nbars; i++) {
                ctrl = amdgpu_pci_readl(pciDevice, pos + 8 + i * 8);
                if ((ctrl & 7) == bar)
                    return (IPTR)1 << (((ctrl >> 8) & 0x3f) + 20);
            }
            break;
        }
        pos = (hdr >> 20) & 0xffc;
    }
    return 0;
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
        if (i == 0 && pbase && !psize) {
            psize = amdgpu_rebar_size(ad->pciDevice, 0);
            bug("[amdgpu]   BAR0 size from Resizable BAR capability: %lu MB\n",
                (unsigned long)(psize >> 20));
            if (!psize) {
                ad->vram_bar_phys = pbase;
                continue;
            }
        }
        if (!pbase || !psize)
            continue;

        D(bug("[amdgpu]   BAR%d: 0x%p, %lu KB\n", i, (APTR)pbase,
              (unsigned long)(psize >> 10)));

        /* The BAR layout by index, as Linux amdgpu_device_init() uses it:
         * BAR0 is the VRAM aperture; from Sea Islands (CIK) on, BAR2 is the
         * doorbell aperture and BAR5 the register aperture; Southern Islands
         * (SI) has no doorbells and its registers are in BAR2. */
        if (i == 0) {
            ad->vram_bar_phys = pbase;
            ad->vram_bar_size = psize;
        } else if (i == 2 && ad->gfx_level == AMDGPU_GFX6) {
            ad->rmmio_phys = pbase;
            ad->rmmio_size = psize;
        } else if (i == 2) {
            ad->doorbell_phys = pbase;
            ad->doorbell_size = psize;
        } else if (i == 5 && ad->gfx_level != AMDGPU_GFX6) {
            ad->rmmio_phys = pbase;
            ad->rmmio_size = psize;
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
        ad->gfx_level = AMDGPU_GFX_UNKNOWN;   /* from the chip table or IP discovery */
        ad->tested    = FALSE;
        ad->name      = NULL;
        D(bug("[amdgpu] found %04lx:%04lx rev %02lx — no marketing name in amdgpu_ids[]\n",
              (unsigned long)vendorid, (unsigned long)deviceid,
              (unsigned long)revision));
    }
    amdgpu_identify_chip(ad);
    if (!ad->name)
        ad->name = "AMD GPU (via IP discovery)";
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
