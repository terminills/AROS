/*
The contents of this file are subject to the AROS Public License Version 1.1 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at
http://www.aros.org/license.html

Software distributed under the License is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
ANY KIND, either express or implied. See the License for the specific language governing rights and
limitations under the License.

(C) Copyright 2026 The AROS Dev Team.

All Rights Reserved.

Glue binding the driver to the hdaudio.hidd base class: a private
subclass implements the moHidd_HDA_HWInit/HWExit overrides that make a
PCI-attached controller work, and the driver finds, owns and wraps the
PCI functions it wants to drive.
*/

#include <aros/debug.h>

#include <exec/types.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <oop/oop.h>
#include <aros/asmcall.h>

#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/dos.h>

#include <dos/dos.h>

#include <hidd/hidd.h>
#include <hidd/pci.h>
#include <hidd/hda.h>
#include <hardware/pci.h>

#include <exec/rawfmt.h>
#include <stdarg.h>

#include "hda_hidd.h"
#include "misc.h"

#define MAX_CONTROLLERS 8

struct Library *OOPBase;
OOP_AttrBase __IHidd_HDA;
OOP_AttrBase __IHidd_PCIDevice;

static struct Library *HDAHiddBase;
static OOP_Class *hdapciClass;

/******************************************************************************
** Init log *******************************************************************
******************************************************************************/

static BPTR hdaLogFile = BNULL;

static void hda_log_line(CONST_STRPTR prefix, CONST_STRPTR line)
{
    LONG len;

    if (!hdaLogFile)
        return;

    for (len = 0; prefix[len]; len++);
    Write(hdaLogFile, (APTR)prefix, len);
    for (len = 0; line[len]; len++);
    Write(hdaLogFile, (APTR)line, len);
    Write(hdaLogFile, "\n", 1);
}

BOOL hda_log_open(void)
{
    if (hdaLogFile)
        return TRUE;

    hdaLogFile = Open("SYS:hdaudio.log", MODE_READWRITE);
    if (!hdaLogFile)
        return FALSE;

    Seek(hdaLogFile, 0, OFFSET_END);
    hda_log_line("[hdaudio] ", "---- " DRIVER " init ----");

    return TRUE;
}

void hda_log_close(void)
{
    if (hdaLogFile)
    {
        Close(hdaLogFile);
        hdaLogFile = BNULL;
    }
}

void hda_log(CONST_STRPTR fmt, ...)
{
    char line[256];
    va_list ap;

    if (!hdaLogFile)
        return;

    va_start(ap, fmt);
    VNewRawDoFmt(fmt, RAWFMTFUNC_STRING, line, ap);
    va_end(ap);

    hda_log_line("[hdaudio] ", line);
}

/* The controller hidd's init steps arrive here, one line each */
AROS_UFH3(static IPTR, HDALogHookFunc,
          AROS_UFHA(struct Hook *, hook, A0),
          AROS_UFHA(APTR, object, A2),
          AROS_UFHA(CONST_STRPTR, line, A1))
{
    AROS_USERFUNC_INIT

    hda_log_line("[hda]     ", line);

    return 0;

    AROS_USERFUNC_EXIT
}

static struct Hook hdaLogHook =
{
    .h_Entry = (HOOKFUNC)HDALogHookFunc
};

/******************************************************************************
** The PCI attachment subclass ************************************************
******************************************************************************/

struct HDAPCIData
{
    OOP_Object *pciDevice;
    OOP_Object *pciDriver;
    BOOL        irqAdded;
};

/* Controller fixes, mainly cache snoop control */
static void HDAPCI_Quirks(OOP_Object *dev, UWORD vendor, UWORD product)
{
    static const UWORD intel_no_snoop[] =
    {
        0x2668, 0x27d8, 0x269a, 0x284b, 0x293e, 0x293f, 0x3a3e, 0x3a6e, 0
    };
    ULONG data;
    UWORD i;

    /*
     * Traffic Class Select: CORB, RIRB and stream data on TC0
     * (Intel HD Audio specification, PCI configuration register 0x44).
     * Not an HDA register on ATI/AMD controllers.
     */
    if (vendor != 0x1002 && vendor != 0x1022)
    {
        hda_log("quirk: TCSEL (0x44 bits 0-2) cleared");
        data = HIDD_PCIDevice_ReadConfigByte(dev, 0x44);
        HIDD_PCIDevice_WriteConfigByte(dev, 0x44, data & ~0x07);
    }

    if (vendor == 0x8086)
    {
        BOOL snoop = TRUE;

        for (i = 0; intel_no_snoop[i]; i++)
        {
            if (intel_no_snoop[i] == product)
                snoop = FALSE;
        }

        if (snoop)
        {
            D(bug("[" DRIVER "] Intel controller, enabling snoop\n"));
            hda_log("quirk: Intel snoop (0x78 bit 11 cleared)");
            data = HIDD_PCIDevice_ReadConfigWord(dev, 0x78);
            HIDD_PCIDevice_WriteConfigWord(dev, 0x78, data & ~0x800);
        }
    }

    if ((vendor == 0x1002 && (product == 0x437B || product == 0x4383)) ||
        (vendor == 0x1022 && (product == 0x780D || product == 0x1457 ||
                              product == 0x1487 || product == 0x15E3)))
    {
        D(bug("[" DRIVER "] ATI SB/AMD controller, enabling snoop\n"));
        hda_log("quirk: ATI/AMD snoop (0x42 bits 0-2 = 2)");
        data = HIDD_PCIDevice_ReadConfigByte(dev, 0x42);
        HIDD_PCIDevice_WriteConfigByte(dev, 0x42, (data & ~0x07) | 0x02);
    }

    if (vendor == 0x10DE)
    {
        D(bug("[" DRIVER "] NVidia controller, enabling snoop\n"));
        hda_log("quirk: NVidia snoop (0x4c-0x4e)");
        data = HIDD_PCIDevice_ReadConfigByte(dev, 0x4E);
        HIDD_PCIDevice_WriteConfigByte(dev, 0x4E, data | 0x0F);
        data = HIDD_PCIDevice_ReadConfigByte(dev, 0x4D);
        HIDD_PCIDevice_WriteConfigByte(dev, 0x4D, data | 0x01);
        data = HIDD_PCIDevice_ReadConfigByte(dev, 0x4C);
        HIDD_PCIDevice_WriteConfigByte(dev, 0x4C, data | 0x01);
    }
}

/*
 * Bring the function to D0 before touching it. A D3hot->D0 transition
 * may soft-reset the function (PMCSR bit 3 clear), which loses the
 * BARs the firmware assigned; those are written back.
 */
static void HDAPCI_PowerUp(OOP_Object *dev, UWORD pmcap)
{
    UWORD pmcsr;
    ULONG bar0lo, bar0hi;

    if (!pmcap)
        return;

    pmcsr = HIDD_PCIDevice_ReadConfigWord(dev, pmcap + 4);
    if ((pmcsr & 0x3) == 0)
        return;

    bar0lo = HIDD_PCIDevice_ReadConfigLong(dev, 0x10);
    bar0hi = HIDD_PCIDevice_ReadConfigLong(dev, 0x14);

    HIDD_PCIDevice_WriteConfigWord(dev, pmcap + 4, pmcsr & ~0x3);
    /* PCI PM 1.2 5.6.1: 10 ms recovery after leaving D3hot */
    udelay(10000);

    hda_log("power: PMCSR %04x -> %04x, BAR0 %08x:%08x -> %08x:%08x%s",
            pmcsr, HIDD_PCIDevice_ReadConfigWord(dev, pmcap + 4),
            bar0hi, bar0lo, HIDD_PCIDevice_ReadConfigLong(dev, 0x14),
            HIDD_PCIDevice_ReadConfigLong(dev, 0x10),
            (pmcsr & 0x8) ? "" : " (no NO_SOFT_RESET)");

    if (HIDD_PCIDevice_ReadConfigLong(dev, 0x10) != bar0lo)
    {
        HIDD_PCIDevice_WriteConfigLong(dev, 0x10, bar0lo);
        if ((bar0lo & 0x6) == 0x4)
            HIDD_PCIDevice_WriteConfigLong(dev, 0x14, bar0hi);
        hda_log("power: BAR0 written back");
    }
}

static OOP_Object *HDAPCI__Root__New(OOP_Class *cl, OOP_Object *o,
        struct pRoot_New *msg)
{
    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (o)
    {
        struct HDAPCIData *hpd = OOP_INST_DATA(cl, o);
        IPTR val = 0;

        /* The base class stores our PCI device as the device data */
        OOP_GetAttr(o, aHidd_HDA_DeviceData, &val);
        hpd->pciDevice = (OOP_Object *)val;
        if (hpd->pciDevice)
            OOP_GetAttr(hpd->pciDevice, aHidd_PCIDevice_Driver,
                        (IPTR *)&hpd->pciDriver);

        if (!hpd->pciDevice || !hpd->pciDriver || !HIDD_HDA_Setup(o))
        {
            OOP_MethodID dispose_mid = OOP_GetMethodID(IID_Root,
                                                       moRoot_Dispose);

            OOP_CoerceMethod(cl, o, (OOP_Msg)&dispose_mid);
            o = NULL;
        }
    }

    return o;
}

static APTR HDAPCI__Hidd_HDA__HWInit(OOP_Class *cl, OOP_Object *o,
        struct pHidd_HDA_HWInit *msg)
{
    struct HDAPCIData *hpd = OOP_INST_DATA(cl, o);
    OOP_Object *dev = hpd->pciDevice;
    IPTR mmio = 0, val = 0, barsize = 0, bus = 0, devnr = 0, sub = 0;
    IPTR revision = 0, intline = 0, pmcap = 0;
    UWORD vendor, product, command;

    OOP_GetAttr(dev, aHidd_PCIDevice_VendorID, &val);
    vendor = val;
    OOP_GetAttr(dev, aHidd_PCIDevice_ProductID, &val);
    product = val;
    OOP_GetAttr(dev, aHidd_PCIDevice_Bus, &bus);
    OOP_GetAttr(dev, aHidd_PCIDevice_Dev, &devnr);
    OOP_GetAttr(dev, aHidd_PCIDevice_Sub, &sub);
    OOP_GetAttr(dev, aHidd_PCIDevice_RevisionID, &revision);
    OOP_GetAttr(dev, aHidd_PCIDevice_INTLine, &intline);
    OOP_GetAttr(dev, aHidd_PCIDevice_CapabilityPowerManagement, &pmcap);
    OOP_GetAttr(dev, aHidd_PCIDevice_Base0, &mmio);
    OOP_GetAttr(dev, aHidd_PCIDevice_Size0, &barsize);

    D(bug("[" DRIVER "] PCI HDA controller %04x:%04x\n", vendor, product));

    command = HIDD_PCIDevice_ReadConfigWord(dev, PCICS_COMMAND);

    hda_log("controller %02x:%02x.%u %04x:%04x rev %02x: BAR0 %p size %p, "
            "IRQ %u, command %04x, PMCSR %04x (cap at %02x)",
            (ULONG)bus, (ULONG)devnr, (ULONG)sub, vendor, product,
            (ULONG)revision, (APTR)mmio, (APTR)barsize, (ULONG)intline,
            command,
            pmcap ? HIDD_PCIDevice_ReadConfigWord(dev, pmcap + 4) : 0,
            (ULONG)pmcap);
    hda_log("config before quirks: 0x42 %02x 0x44 %02x 0x78 %04x",
            HIDD_PCIDevice_ReadConfigByte(dev, 0x42),
            HIDD_PCIDevice_ReadConfigByte(dev, 0x44),
            HIDD_PCIDevice_ReadConfigWord(dev, 0x78));

    HDAPCI_PowerUp(dev, (UWORD)pmcap);

    command |= PCICMF_IODECODE | PCICMF_MEMDECODE | PCICMF_BUSMASTER;
    HIDD_PCIDevice_WriteConfigWord(dev, PCICS_COMMAND, command);

    HDAPCI_Quirks(dev, vendor, product);

    hda_log("config after quirks:  0x42 %02x 0x44 %02x 0x78 %04x, "
            "command %04x",
            HIDD_PCIDevice_ReadConfigByte(dev, 0x42),
            HIDD_PCIDevice_ReadConfigByte(dev, 0x44),
            HIDD_PCIDevice_ReadConfigWord(dev, 0x78),
            HIDD_PCIDevice_ReadConfigWord(dev, PCICS_COMMAND));

    if (!mmio)
    {
        D(bug("[" DRIVER "] No BAR0!\n"));
        hda_log("no BAR0");
        return NULL;
    }

    /* A BAR outside the identity mapped window has no CPU mapping yet */
    mmio = (IPTR)HIDD_PCIDriver_MapPCI(hpd->pciDriver, (APTR)mmio, (ULONG)barsize);
    if (!mmio)
    {
        D(bug("[" DRIVER "] Failed to map BAR0!\n"));
        hda_log("BAR0 could not be mapped");
        return NULL;
    }

    if (!HIDD_PCIDriver_AddInterrupt(hpd->pciDriver, dev, msg->controllerInt))
    {
        D(bug("[" DRIVER "] Failed to add interrupt handler\n"));
        hda_log("interrupt handler for IRQ %u refused", (ULONG)intline);
        return NULL;
    }
    hpd->irqAdded = TRUE;

    return (APTR)mmio;
}

static VOID HDAPCI__Hidd_HDA__HWExit(OOP_Class *cl, OOP_Object *o,
        struct pHidd_HDA_HWExit *msg)
{
    struct HDAPCIData *hpd = OOP_INST_DATA(cl, o);
    OOP_Object *dev = hpd->pciDevice;
    UWORD command;

    if (hpd->irqAdded)
    {
        HIDD_PCIDriver_RemoveInterrupt(hpd->pciDriver, dev,
                                       msg->controllerInt);
        hpd->irqAdded = FALSE;
    }

    command = HIDD_PCIDevice_ReadConfigWord(dev, PCICS_COMMAND);
    command &= ~(PCICMF_IODECODE | PCICMF_MEMDECODE | PCICMF_BUSMASTER);
    HIDD_PCIDevice_WriteConfigWord(dev, PCICS_COMMAND, command);
}

static BOOL create_pci_subclass(void)
{
    OOP_AttrBase MetaAttrBase = OOP_ObtainAttrBase(IID_Meta);

    struct OOP_MethodDescr root_descr[] =
    {
        {(OOP_MethodFunc)HDAPCI__Root__New, moRoot_New},
        {NULL, 0}
    };

    struct OOP_MethodDescr hda_descr[] =
    {
        {(OOP_MethodFunc)HDAPCI__Hidd_HDA__HWInit, moHidd_HDA_HWInit},
        {(OOP_MethodFunc)HDAPCI__Hidd_HDA__HWExit, moHidd_HDA_HWExit},
        {NULL, 0}
    };

    struct OOP_InterfaceDescr ifdescr[] =
    {
        {root_descr, IID_Root,     1},
        {hda_descr,  IID_Hidd_HDA, 2},
        {NULL, NULL}
    };

    struct TagItem tags[] =
    {
        {aMeta_SuperID,        (IPTR)CLID_Hidd_HDA},
        {aMeta_InterfaceDescr, (IPTR)ifdescr},
        {aMeta_InstSize,       (IPTR)sizeof(struct HDAPCIData)},
        {TAG_DONE,             0}
    };

    if (!MetaAttrBase)
        return FALSE;

    hdapciClass = OOP_NewObject(NULL, CLID_HiddMeta, tags);

    OOP_ReleaseAttrBase(IID_Meta);

    return hdapciClass != NULL;
}

/******************************************************************************
** Controller discovery *******************************************************
******************************************************************************/

struct pci_enum_ctx
{
    BOOL displayAudio;
    UWORD vendor;
    OOP_Object *devices[MAX_CONTROLLERS];
    ULONG count;
};

/*
    The audio function of a display adapter: a function other than 0
    whose function 0 is a display device.
*/
static BOOL device_is_display_audio(OOP_Object *dev)
{
    OOP_Object *driver = NULL;
    IPTR sub = 0, bus = 0, devnr = 0;
    UWORD classword;

    OOP_GetAttr(dev, aHidd_PCIDevice_Sub, &sub);
    if (sub == 0)
        return FALSE;

    OOP_GetAttr(dev, aHidd_PCIDevice_Driver, (IPTR *)&driver);
    if (!driver)
        return FALSE;

    OOP_GetAttr(dev, aHidd_PCIDevice_Bus, &bus);
    OOP_GetAttr(dev, aHidd_PCIDevice_Dev, &devnr);

    classword = HIDD_PCIDriver_ReadConfigWord(driver, dev, bus, devnr, 0,
                                              0x0A);

    return ((classword >> 8) & 0xFF) == 0x03;
}

AROS_UFH3(static void, PCIEnumerator,
          AROS_UFHA(struct Hook *, hook, A0),
          AROS_UFHA(OOP_Object *, device, A2),
          AROS_UFHA(APTR, msg, A1))
{
    AROS_USERFUNC_INIT

    struct pci_enum_ctx *ctx = (struct pci_enum_ctx *)hook->h_Data;
    IPTR val = 0, vendor = 0, product = 0, bus = 0, devnr = 0, sub = 0;
    CONST_STRPTR owner;
    BOOL displayAudio;

    if (ctx->count >= MAX_CONTROLLERS)
        return;

    OOP_GetAttr(device, aHidd_PCIDevice_VendorID, &vendor);
    OOP_GetAttr(device, aHidd_PCIDevice_ProductID, &product);
    OOP_GetAttr(device, aHidd_PCIDevice_Bus, &bus);
    OOP_GetAttr(device, aHidd_PCIDevice_Dev, &devnr);
    OOP_GetAttr(device, aHidd_PCIDevice_Sub, &sub);

    if (ctx->vendor)
    {
        OOP_GetAttr(device, aHidd_PCIDevice_VendorID, &val);
        if ((UWORD)val != ctx->vendor)
            return;
    }

    displayAudio = device_is_display_audio(device);

    if (displayAudio != ctx->displayAudio)
    {
        hda_log("found %02x:%02x.%u %04x:%04x: %s, not for this driver",
                (ULONG)bus, (ULONG)devnr, (ULONG)sub, (ULONG)vendor,
                (ULONG)product,
                displayAudio ? "display audio" : "not display audio");
        return;
    }

    /* Bind: claim the PCI function for this driver */
    owner = HIDD_PCIDevice_Obtain(device, DRIVER);
    if (owner != NULL)
    {
        D(bug("[" DRIVER "] device @ 0x%p already owned\n", device));
        hda_log("found %02x:%02x.%u %04x:%04x: already owned by %s",
                (ULONG)bus, (ULONG)devnr, (ULONG)sub, (ULONG)vendor,
                (ULONG)product, owner);
        return;
    }

    hda_log("found %02x:%02x.%u %04x:%04x: claimed", (ULONG)bus,
            (ULONG)devnr, (ULONG)sub, (ULONG)vendor, (ULONG)product);

    ctx->devices[ctx->count++] = device;

    AROS_USERFUNC_EXIT
}

ULONG ahi_hda_obtain_controllers(BOOL displayAudio, UWORD vendor,
                                 APTR *controllers, ULONG max)
{
    struct pci_enum_ctx ctx = { displayAudio, vendor, {NULL}, 0 };

    struct Hook enumHook =
    {
        .h_Entry = (HOOKFUNC)PCIEnumerator,
        .h_Data  = &ctx
    };

    struct TagItem requirements[] =
    {
        { tHidd_PCI_Class,    0x04 }, /* Multimedia */
        { tHidd_PCI_SubClass, 0x03 }, /* HD Audio */
        { TAG_DONE,           0    }
    };

    OOP_Object *pci;
    ULONG i, count = 0;

    if (!hdapciClass)
        return 0;

    pci = OOP_NewObject(NULL, CLID_Hidd_PCI, NULL);
    if (!pci)
        return 0;

    HIDD_PCI_EnumDevices(pci, &enumHook, requirements);

    OOP_DisposeObject(pci);

    for (i = 0; i < ctx.count; i++)
    {
        struct TagItem tags[] =
        {
            { aHidd_HDA_DeviceData, (IPTR)ctx.devices[i]                  },
            { aHidd_HDA_LogHook,    hdaLogFile ? (IPTR)&hdaLogHook : 0    },
            { TAG_DONE,             0                                     }
        };
        OOP_Object *ctrl;

        if (count < max &&
            (ctrl = OOP_NewObject(hdapciClass, NULL, tags)) != NULL)
        {
            hda_log("controller object %p created", ctrl);
            controllers[count++] = ctrl;
        }
        else
        {
            hda_log("controller bring-up failed, device released");
            HIDD_PCIDevice_Release(ctx.devices[i]);
        }
    }

    return count;
}

void ahi_hda_release_controller(APTR controller)
{
    IPTR val = 0;
    OOP_Object *dev;

    if (!controller)
        return;

    OOP_GetAttr((OOP_Object *)controller, aHidd_HDA_DeviceData, &val);
    dev = (OOP_Object *)val;

    OOP_DisposeObject((OOP_Object *)controller);

    if (dev)
        HIDD_PCIDevice_Release(dev);
}

/******************************************************************************
** Library setup and call-throughs ********************************************
******************************************************************************/

BOOL ahi_hda_init(struct DriverBase *AHIsubBase)
{
    OOPBase = OpenLibrary(AROSOOP_NAME, 0);
    if (!OOPBase)
        return FALSE;

    HDAHiddBase = OpenLibrary("DRIVERS:hdaudio.hidd", 0);
    if (!HDAHiddBase)
    {
        D(bug("[" DRIVER "] Cannot open hdaudio.hidd\n"));
        return FALSE;
    }

    __IHidd_HDA = OOP_ObtainAttrBase(IID_Hidd_HDA);
    __IHidd_PCIDevice = OOP_ObtainAttrBase(IID_Hidd_PCIDevice);
    if (!__IHidd_HDA || !__IHidd_PCIDevice)
        return FALSE;

    if (!create_pci_subclass())
    {
        D(bug("[" DRIVER "] Cannot create PCI attachment subclass\n"));
        return FALSE;
    }

    return TRUE;
}

void ahi_hda_exit(void)
{
    if (hdapciClass)
    {
        OOP_DisposeObject((OOP_Object *)hdapciClass);
        hdapciClass = NULL;
    }

    if (__IHidd_HDA)
    {
        OOP_ReleaseAttrBase(IID_Hidd_HDA);
        __IHidd_HDA = 0;
    }

    if (__IHidd_PCIDevice)
    {
        OOP_ReleaseAttrBase(IID_Hidd_PCIDevice);
        __IHidd_PCIDevice = 0;
    }

    if (HDAHiddBase)
    {
        CloseLibrary(HDAHiddBase);
        HDAHiddBase = NULL;
    }

    if (OOPBase)
    {
        CloseLibrary(OOPBase);
        OOPBase = NULL;
    }
}

IPTR ahi_hda_get_attr(APTR controller, ULONG attr)
{
    IPTR val = 0;

    OOP_GetAttr((OOP_Object *)controller, attr, &val);

    return val;
}

ULONG hda_command(APTR controller, ULONG cmd)
{
    return HIDD_HDA_SendCommand((OOP_Object *)controller, cmd);
}

APTR hda_stream_alloc(APTR controller, ULONG direction,
                      struct Interrupt *streamInt)
{
    return HIDD_HDA_AllocStream((OOP_Object *)controller, direction,
                                streamInt);
}

BOOL hda_stream_setup(APTR controller, APTR stream, UWORD format,
                      ULONG bufferSize, ULONG bufferCount,
                      struct HDA_StreamInfo *info)
{
    return HIDD_HDA_SetupStream((OOP_Object *)controller, stream, format,
                                bufferSize, bufferCount, info);
}

void hda_stream_start(APTR controller, APTR stream)
{
    HIDD_HDA_StartStream((OOP_Object *)controller, stream);
}

void hda_stream_stop(APTR controller, APTR stream)
{
    HIDD_HDA_StopStream((OOP_Object *)controller, stream);
}

void hda_stream_free(APTR controller, APTR stream)
{
    HIDD_HDA_FreeStream((OOP_Object *)controller, stream);
}

void hda_stream_sync(APTR controller, APTR stream, ULONG index)
{
    HIDD_HDA_SyncStreamBuffer((OOP_Object *)controller, stream, index);
}
