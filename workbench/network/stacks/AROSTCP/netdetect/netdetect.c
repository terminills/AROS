/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Find the Ethernet adapter and write an AROSTCP interfaces line
          for it.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/sana2.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>

#include <oop/oop.h>
#include <hidd/pci.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/oop.h>

#include <string.h>

static int NameEquals(CONST_STRPTR a, CONST_STRPTR b)
{
    while (*a != '\0' && *b != '\0')
    {
        TEXT ca = *a++, cb = *b++;

        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return 1;
    }

    return (*a == *b) ? 0 : 1;
}

const TEXT version[] = "$VER: netdetect 1.0 (2.10.2026)";

#define NETWORKS_DIR    "DEVS:Networks"
#define DEFAULT_IFACES  "ENV:AROSTCP/db/interfaces"
#define DEFAULT_IFNAME  "eth0"

#define ARG_TEMPLATE    "INTERFACES/K,IFNAME/K,DEVICE/K,QUIET/S"

enum
{
    ARG_INTERFACES,
    ARG_IFNAME,
    ARG_DEVICE,
    ARG_QUIET,
    ARG_COUNT
};

static BOOL CopyToBuff(APTR to, APTR from, ULONG len)
{
    CopyMem(from, to, len);
    return TRUE;
}

static BOOL CopyFromBuff(APTR to, APTR from, ULONG len)
{
    CopyMem(from, to, len);
    return TRUE;
}

static struct TagItem buffer_management[] =
{
    { S2_CopyToBuff,   (IPTR)CopyToBuff   },
    { S2_CopyFromBuff, (IPTR)CopyFromBuff },
    { TAG_END,         0                  }
};

/* Not usable as a DHCP Ethernet interface at boot */
static CONST_STRPTR skip_devices[] =
{
    "ppp.device",
    "atheros5000.device",
    "prism2.device",
    "realtek8180.device",
    NULL
};

static BOOL IsSkipped(CONST_STRPTR name)
{
    ULONG i;

    if (strstr(name, "_debug") != NULL)
        return TRUE;

    for (i = 0; skip_devices[i] != NULL; i++)
    {
        if (NameEquals(name, skip_devices[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

/* TRUE if <path> opens exclusively as Ethernet and reports a station address */
static BOOL ProbeDeviceAt(CONST_STRPTR path)
{
    struct MsgPort *port;
    struct IOSana2Req *req;
    struct Sana2DeviceQuery query;
    BOOL usable = FALSE;

    port = CreateMsgPort();
    if (port == NULL)
        return FALSE;

    req = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
    if (req != NULL)
    {
        req->ios2_BufferManagement = (APTR)buffer_management;

        /* Exclusive, so a device another program owns is left alone */
        if (OpenDevice(path, 0, (struct IORequest *)req, SANA2OPF_MINE) == 0)
        {
            req->ios2_Req.io_Command = S2_DEVICEQUERY;
            req->ios2_StatData = &query;
            memset(&query, 0, sizeof(query));
            query.SizeAvailable = sizeof(query);
            query.DevQueryFormat = 0;
            DoIO((struct IORequest *)req);

            if (req->ios2_Req.io_Error == 0
                && query.HardwareType == S2WireType_Ethernet)
            {
                req->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
                req->ios2_StatData = NULL;
                DoIO((struct IORequest *)req);

                if (req->ios2_Req.io_Error == 0)
                    usable = TRUE;
            }

            CloseDevice((struct IORequest *)req);
        }

        DeleteIORequest((struct IORequest *)req);
    }

    DeleteMsgPort(port);

    return usable;
}

static BOOL ProbeDevice(CONST_STRPTR name)
{
    TEXT path[256];

    strlcpy(path, NETWORKS_DIR "/", sizeof(path));
    strlcat(path, name, sizeof(path));
    return ProbeDeviceAt(path);
}

/* SANA-II devices added by the USB network classes; tried only if loaded */

static CONST_STRPTR usb_devices[] =
{
    "usbrndis.device",
    "usbcdceth.device",
    "usbasixeth.device",
    "usblan78xx.device",
    "usbmoschipeth.device",
    "usbpegasus.device",
    NULL
};

static BOOL FindDeviceByUSB(STRPTR found, ULONG size, BOOL quiet)
{
    ULONG i;

    for (i = 0; usb_devices[i] != NULL; i++)
    {
        BOOL present;

        Forbid();
        present = FindName(&SysBase->DeviceList, usb_devices[i]) != NULL;
        Permit();
        if (!present)
            continue;

        if (ProbeDeviceAt(usb_devices[i]))
        {
            strlcpy(found, usb_devices[i], size);
            if (!quiet)
                Printf("netdetect: USB adapter on %s\n", (IPTR)usb_devices[i]);
            return TRUE;
        }
    }
    if (!quiet)
        PutStr("netdetect: no USB network adapter\n");
    return FALSE;
}

/* dhclient supports a single interface, so exactly one line is written */
static BOOL WriteInterfaces(CONST_STRPTR file, CONST_STRPTR ifname,
    CONST_STRPTR device, BOOL resident)
{
    BPTR fh;
    BOOL ok = FALSE;

    fh = Open(file, MODE_NEWFILE);
    if (fh != BNULL)
    {
        if (resident)
            ok = FPrintf(fh, "%s DEV=%s UNIT=0 IP=DHCP UP\n",
                         ifname, device) >= 0;
        else
            ok = FPrintf(fh, "%s DEV=%s/%s UNIT=0 IP=DHCP UP\n",
                         ifname, NETWORKS_DIR, device) >= 0;
        Close(fh);
    }

    return ok;
}

/* PCI network controllers and the driver that handles each */

struct NicMatch
{
    UWORD vendor;
    UWORD product;      /* 0 = any network-class device from this vendor */
    CONST_STRPTR driver;
};

static const struct NicMatch nic_table[] =
{
    { 0x10ec, 0x8139, "rtl8139.device"     },  /* Realtek 8139 (QEMU rtl8139) */
    { 0x10ec, 0x8136, "rtl816x.device"     },  /* Realtek 8101E/8102E */
    { 0x10ec, 0x8167, "rtl816x.device"     },
    { 0x10ec, 0x8168, "rtl816x.device"     },  /* Realtek 8111/8168 */
    { 0x10ec, 0x8169, "rtl816x.device"     },
    { 0x1022, 0x2000, "pcnet32.device"     },  /* AMD PCnet (QEMU pcnet) */
    { 0x1106, 0x3106, "rhine.device"       },  /* VIA Rhine */
    { 0x1039, 0x0900, "sis900.device"      },  /* SiS 900 */
    { 0x14e4, 0x4401, "broadcom4400.device"},  /* Broadcom 440x */
    { 0x14e4, 0x4400, "broadcom4400.device"},
    { 0x10b7, 0x0000, "etherlink3.device"  },  /* 3Com */
    { 0x10de, 0x0000, "nvidianet.device"   },  /* nForce */
    { 0x8086, 0x1229, "intelpro100.device" },  /* Intel PRO/100 */
    { 0x8086, 0x0000, "e1000.device"       },  /* Intel gigabit (QEMU e1000) */
    { 0x0000, 0x0000, NULL                 }
};

struct PciScan
{
    OOP_AttrBase   devattr;
    CONST_STRPTR   driver;      /* set when a match is found */
    UWORD          vendor;
    UWORD          product;
};

AROS_UFH3(void, NicEnumerator,
    AROS_UFHA(struct Hook *,  hook,      A0),
    AROS_UFHA(OOP_Object *,   pciDevice, A2),
    AROS_UFHA(APTR,           message,   A1))
{
    AROS_USERFUNC_INIT

    struct PciScan *scan = (struct PciScan *)hook->h_Data;
    IPTR vendor = 0, product = 0;
    int i;

    if (scan->driver == NULL)
    {
        OOP_GetAttr(pciDevice, scan->devattr + aoHidd_PCIDevice_VendorID, &vendor);
        OOP_GetAttr(pciDevice, scan->devattr + aoHidd_PCIDevice_ProductID, &product);

        for (i = 0; nic_table[i].driver != NULL; i++)
        {
            if (nic_table[i].vendor != (UWORD)vendor)
                continue;
            if (nic_table[i].product != 0 && nic_table[i].product != (UWORD)product)
                continue;

            scan->driver  = nic_table[i].driver;
            scan->vendor  = (UWORD)vendor;
            scan->product = (UWORD)product;
            break;
        }
    }

    AROS_USERFUNC_EXIT
}

static BOOL FindDeviceByPCI(STRPTR found, ULONG size, BOOL quiet)
{
    struct Library *OOPBase;
    OOP_Object *pci;
    struct PciScan scan = { 0, NULL, 0, 0 };
    BOOL got = FALSE;

    OOPBase = OpenLibrary("oop.library", 0);
    if (OOPBase == NULL)
    {
        if (!quiet)
            PutStr("netdetect: no oop.library\n");
        return FALSE;
    }

    scan.devattr = OOP_ObtainAttrBase(IID_Hidd_PCIDevice);
    if (scan.devattr != 0)
    {
        pci = OOP_NewObject(NULL, CLID_Hidd_PCI, NULL);
        if (pci != NULL)
        {
            struct Hook FindHook;
            /* Network controllers only */
            struct TagItem reqs[] =
            {
                { tHidd_PCI_Class, 0x02 },
                { TAG_DONE,        0    }
            };

            memset(&FindHook, 0, sizeof(FindHook));
            FindHook.h_Entry = (IPTR (*)())NicEnumerator;
            FindHook.h_Data  = &scan;

            HIDD_PCI_EnumDevices(pci, &FindHook, reqs);
            OOP_DisposeObject(pci);
        }
        else if (!quiet)
            PutStr("netdetect: no PCI subsystem\n");

        OOP_ReleaseAttrBase(IID_Hidd_PCIDevice);
    }

    if (scan.driver != NULL)
    {
        TEXT path[256];

        strlcpy(path, NETWORKS_DIR, sizeof(path));
        strlcat(path, "/", sizeof(path));
        strlcat(path, scan.driver, sizeof(path));

        if (!IsSkipped(scan.driver))
        {
            BPTR lock = Lock(path, SHARED_LOCK);
            if (lock != BNULL)
            {
                UnLock(lock);
                strlcpy(found, scan.driver, size);
                got = TRUE;
                if (!quiet)
                {
                    Printf("netdetect: PCI %04lx:%04lx -> %s\n",
                           (IPTR)scan.vendor, (IPTR)scan.product,
                           (IPTR)scan.driver);
                }
            }
            else if (!quiet)
            {
                Printf("netdetect: PCI %04lx:%04lx wants %s, which is not installed\n",
                       (IPTR)scan.vendor, (IPTR)scan.product, (IPTR)scan.driver);
            }
        }
    }
    else if (!quiet)
        PutStr("netdetect: no known network controller on PCI\n");

    CloseLibrary(OOPBase);

    return got;
}

int main(void)
{
    struct RDArgs *rdargs;
    IPTR args[ARG_COUNT] = { 0 };
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwindowptr;
    TEXT device[128];
    CONST_STRPTR file, ifname;
    BOOL quiet;
    BOOL resident = FALSE;
    int rc = RETURN_WARN;

    rdargs = ReadArgs(ARG_TEMPLATE, args, NULL);
    if (rdargs == NULL)
    {
        PrintFault(IoErr(), "netdetect");
        return RETURN_FAIL;
    }

    file = args[ARG_INTERFACES] ? (CONST_STRPTR)args[ARG_INTERFACES]
                                : (CONST_STRPTR)DEFAULT_IFACES;
    ifname = args[ARG_IFNAME] ? (CONST_STRPTR)args[ARG_IFNAME]
                              : (CONST_STRPTR)DEFAULT_IFNAME;
    quiet = args[ARG_QUIET] != 0;

    /* No requesters during the boot sequence */
    oldwindowptr = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;

    if (args[ARG_DEVICE] != 0)
    {
        if (ProbeDevice((CONST_STRPTR)args[ARG_DEVICE]))
        {
            strlcpy(device, (CONST_STRPTR)args[ARG_DEVICE], sizeof(device));
            rc = RETURN_OK;
        }
    }
    else if (FindDeviceByUSB(device, sizeof(device), quiet))
    {
        /* A plugged-in USB adapter is preferred over the onboard port */
        resident = TRUE;
        rc = RETURN_OK;
    }
    else if (FindDeviceByPCI(device, sizeof(device), quiet))
    {
        rc = RETURN_OK;
    }

    me->pr_WindowPtr = oldwindowptr;

    if (rc == RETURN_OK)
    {
        if (!WriteInterfaces(file, ifname, device, resident))
        {
            if (!quiet)
            {
                IPTR data[1];

                data[0] = (IPTR)file;
                VPrintf("netdetect: cannot write %s\n", (RAWARG)data);
            }
            rc = RETURN_FAIL;
        }
        else if (!quiet)
        {
            IPTR data[2];

            data[0] = (IPTR)device;
            data[1] = (IPTR)ifname;
            VPrintf("netdetect: %s -> %s\n", (RAWARG)data);
        }
    }
    else if (!quiet)
    {
        PutStr("netdetect: no usable Ethernet device found\n");
    }

    FreeArgs(rdargs);

    return rc;
}
