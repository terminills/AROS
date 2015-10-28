/*
    Copyright � 2015, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Virtual USB host controller
    Lang: English
*/

#ifdef DEBUG
#undef DEBUG
#endif
#define DEBUG 1

#include <aros/debug.h>
#include <aros/macros.h>
#include <aros/asmcall.h>
#include <aros/symbolsets.h>

#include <proto/exec.h>
#include <proto/stdc.h>
#include <proto/arossupport.h>

#include <devices/usb.h>
#include <devices/usb_hub.h>
#include <devices/newstyle.h>
#include <devices/usbhardware.h>

#include "vusbhci_device.h"
#include "libusb-bridge/libusb.h"

#include LC_LIBDEFS_FILE

struct VUSBHCIUnit *VUSBHCI_AddNewUnit(ULONG unitnum);
struct VUSBHCIPort *VUSBHCI_AddNewPort(struct VUSBHCIUnit *unit, ULONG portnum);

static int GM_UNIQUENAME(Init)(LIBBASETYPEPTR VUSBHCIBase) {
    mybug(-1,("[VUSBHCI] Init: Entering function\n"));

    struct VUSBHCIUnit *unit;

    NEWLIST(&VUSBHCIBase->unit_list);
    VUSBHCIBase->unit_count = 0;

    if(!libusb_bridge_init()) {
        return FALSE;
    }

    //libusb_bridge_cleanup();

    unit = VUSBHCI_AddNewUnit(VUSBHCIBase->unit_count);
    if(unit == NULL) {
        mybug(-1, ("[VUSBHCI] Init: Failed to create new USB unit!\n"));

        /*
            Free previous units if any exists
        */

        ForeachNode(&VUSBHCIBase->unit_list, unit) {
            mybug(-1,("[VUSBHCI] Init: Removing unit structure %s at %p\n", unit->node.ln_Name, unit));
            REMOVE(unit);
            FreeVec(unit);
        }
        return FALSE;
    } else {
        AddTail(&VUSBHCIBase->unit_list,(struct Node *)unit);
        VUSBHCIBase->unit_count++;
    }

    D(ForeachNode(&VUSBHCIBase->unit_list, unit) {
        mybug(-1, ("[VUSBHCI] Init: Created unit %d at %p %s\n", unit->number, unit, unit->name));
        struct VUSBHCIPort *port;
        ForeachNode(&unit->roothub.port_list, port) {
            mybug(-1, ("                      port %d at %p %s\n", port->number, port, port->name));
        }
        mybug(-1,("\n"));
    });

    return TRUE;
}

static int GM_UNIQUENAME(Open)(LIBBASETYPEPTR VUSBHCIBase, struct IOUsbHWReq *ioreq, ULONG unitnum, ULONG flags) {
    mybug(-1, ("[VUSBHCI] Open: Entering function\n"));
    mybug(-1, ("[VUSBHCI] Open: Unit %d\n", unitnum));

    struct VUSBHCIUnit *unit;

    /* Default to open failure. */
    ioreq->iouh_Req.io_Error = IOERR_OPENFAIL;
    ioreq->iouh_Req.io_Unit = NULL;

    /*
        Number of units eg. virtual usb controllers.
        Host controller is divided into individual units if it has both usb2.0 and usb3.0 ports
    */
    if(unitnum < VUSBHCIBase->unit_count) {

        if(ioreq->iouh_Req.io_Message.mn_Length < sizeof(struct IOUsbHWReq)) {
            mybug(-1, ("[VUSBHCI] Open: Invalid MN_LENGTH!\n"));
            ioreq->iouh_Req.io_Error = IOERR_BADLENGTH;
        }

        ioreq->iouh_Req.io_Unit = NULL;

        ForeachNode(&VUSBHCIBase->unit_list, unit) {
            mybug(-1, ("[VUSBHCI] Open: Opening unit number %d\n", unitnum));
            if(unit->number == unitnum) {
                mybug(-1, ("        Found unit from node list %s %p\n\n", unit->name, unit));
                if(unit->allocated) {
                    ioreq->iouh_Req.io_Error = IOERR_UNITBUSY;
                    ioreq->iouh_Req.io_Unit = NULL;
                    mybug(-1, ("        Found unit from node list %s %p -> already in use!\n\n", unit->name, unit));
                    break;
                }
                unit->allocated = TRUE;
                ioreq->iouh_Req.io_Unit = (struct Unit *) unit;
                break;
            }
        }

        if(ioreq->iouh_Req.io_Unit != NULL) {

            /* Opened ok! */
            ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
            ioreq->iouh_Req.io_Error				   = 0;

            return TRUE;
        } else {
            return FALSE;
        }
    }

    return FALSE;
}

static int GM_UNIQUENAME(Close)(LIBBASETYPEPTR VUSBHCIBase, struct IOUsbHWReq *ioreq) {
    mybug(-1, ("[VUSBHCI] Close: Entering function\n"));

    struct VUSBHCIUnit *unit;

    /*
        First check to see if the unit is valid before bashing the memory
        It might be useless to raise any error condition, as no one checks them.
    */
    ForeachNode(&VUSBHCIBase->unit_list, unit) {
        mybug(-1, ("[VUSBHCI] Close: Closing unit %p\n", ioreq->iouh_Req.io_Unit));
        if(ioreq->iouh_Req.io_Unit == (struct Unit *)unit) {
            mybug(-1, ("        Found unit from node list %s %p\n\n", unit->name, unit));
            unit->allocated = FALSE;
            ioreq->iouh_Req.io_Unit   = (APTR) -1;
            ioreq->iouh_Req.io_Device = (APTR) -1;

            return TRUE;
        }
    }

    ioreq->iouh_Req.io_Error = IOERR_BADADDRESS;
    ioreq->iouh_Req.io_Unit   = (APTR) -1;
    ioreq->iouh_Req.io_Device = (APTR) -1;

    mybug(-1, ("        Bad unit structure in ioreq, nothing done!\n\n"));
    return FALSE;
}

ADD2INITLIB(GM_UNIQUENAME(Init), 0)
ADD2OPENDEV(GM_UNIQUENAME(Open), 0)
ADD2CLOSEDEV(GM_UNIQUENAME(Close), 0)

AROS_LH1(void, BeginIO, AROS_LHA(struct IOUsbHWReq *, ioreq, A1), struct VUSBHCIBase *, VUSBHCIBase, 5, VUSBHCI) {
    AROS_LIBFUNC_INIT
    mybug(-1, ("[VUSBHCI] BeginIO: Entering function\n"));

    WORD ret = RC_OK;

    ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ioreq->iouh_Req.io_Error				   = UHIOERR_NO_ERROR;

    struct VUSBHCIUnit *unit = (struct VUSBHCIUnit *) ioreq->iouh_Req.io_Unit;

    if(unit != NULL) {

        switch (ioreq->iouh_Req.io_Command) {
            case CMD_RESET:
                mybug_unit(-1, ("CMD_RESET\n"));
                break;
            case CMD_FLUSH:
                mybug_unit(-1, ("CMD_FLUSH\n"));
                break;
            case UHCMD_QUERYDEVICE:
                mybug_unit(-1, ("UHCMD_QUERYDEVICE\n"));
                ret = cmdQueryDevice(ioreq);
                break;
            case UHCMD_USBRESET:
                mybug_unit(-1, ("UHCMD_USBRESET\n"));
                ret = cmdUsbReset(ioreq);
                break;
            case UHCMD_USBRESUME:
                mybug_unit(-1, ("UHCMD_USBRESUME\n"));
                break;
            case UHCMD_USBSUSPEND:
                mybug_unit(-1, ("UHCMD_USBSUSPEND\n"));
                break;
            case UHCMD_USBOPER:
                mybug_unit(-1, ("UHCMD_USBOPER\n"));
                //ret = cmdUsbOper(ioreq);
                break;
            case UHCMD_CONTROLXFER:
                mybug_unit(-1, ("UHCMD_CONTROLXFER unit %p %s\n", unit, unit->name));
                ret = cmdControlXFer(ioreq);
                break;
            case UHCMD_BULKXFER:
                mybug_unit(-1, ("UHCMD_BULKXFER\n"));
                break;
            case UHCMD_INTXFER:
                mybug_unit(-1, ("UHCMD_INTXFER unit %p %s\n", unit, unit->name));
                ret = cmdIntXFer(ioreq);
                break;
            case UHCMD_ISOXFER:
                mybug_unit(-1, ("UHCMD_ISOXFER\n"));
                ret = cmdISOXFer(ioreq);
                break;

            /* Poseidon doesn't actually check this, ever... */
            case NSCMD_DEVICEQUERY:
                mybug_unit(-1, ("NSCMD_DEVICEQUERY\n"));

                static const UWORD NSDSupported[] = {
                    CMD_FLUSH, CMD_RESET,
                    UHCMD_QUERYDEVICE,
                    UHCMD_USBRESET,
                    UHCMD_USBRESUME,
                    UHCMD_USBSUSPEND,
                    UHCMD_USBOPER,
                    UHCMD_CONTROLXFER ,
                    UHCMD_ISOXFER,
                    UHCMD_INTXFER,
                    UHCMD_BULKXFER,
                    NSCMD_DEVICEQUERY,
                    0
                };

                struct NSDeviceQueryResult *nsdq = (struct NSDeviceQueryResult *)((struct IOStdReq *)(ioreq))->io_Data;
                nsdq->DevQueryFormat    = 0;
                nsdq->SizeAvailable     = sizeof(struct NSDeviceQueryResult);
                nsdq->DeviceType        = NSDEVTYPE_USBHARDWARE;
                nsdq->DeviceSubType     = 0;
                nsdq->SupportedCommands = (UWORD *)NSDSupported;
                ret = RC_OK;
                break;
            default:
                mybug_unit(-1, ("IOERR_NOCMD\n"));
                ret = IOERR_NOCMD;
                break;
        }
    } else {
        /* We have aborted the request as unit is invalid */
        ret = IOERR_ABORTED;
    }

    if(ret != RC_DONTREPLY) {
        /* Set error codes */
        if (ret != RC_OK) {
            ioreq->iouh_Req.io_Error = ret & 0xff;
        }
        /* Terminate the iorequest */
        ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
        /* If not quick I/O, reply the message */
        if(!(ioreq->iouh_Req.io_Flags & IOF_QUICK)) {
            ReplyMsg(&ioreq->iouh_Req.io_Message);
        }
    }

    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, AbortIO, AROS_LHA(struct IOUsbHWReq *, ioreq, A1), struct VUSBHCIBase *, VUSBHCIBase, 6, VUSBHCI) {
    AROS_LIBFUNC_INIT
    mybug(-1, ("[VUSBHCI] AbortIO: Entering function\n"));

    if(ioreq->iouh_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE) {
        if(cmdAbortIO(ioreq)) {
            return(0);
        }
    }

    return(-1);
    AROS_LIBFUNC_EXIT
}

struct VUSBHCIUnit *VUSBHCI_AddNewUnit(ULONG unitnum) {

    struct VUSBHCIUnit *unit;
    struct VUSBHCIPort *port;

    unit = AllocVec(sizeof(struct VUSBHCIUnit), MEMF_ANY|MEMF_CLEAR);
    if(unit == NULL) {
        mybug(-1, ("[VUSBHCI] VUSBHCI_AddNewUnit: Failed to create new unit structure\n"));
        return NULL;
    } else {
        unit->node.ln_Type = NT_USER;
        unit->number = unitnum;
        unit->node.ln_Name = (STRPTR)&unit->name;
        unit->state = UHSF_SUSPENDED;
        unit->allocated = FALSE;

        NEWLIST(&unit->roothub.port_list);
        NEWLIST(&unit->roothub.io_queue);

        /* This is our root hub device descriptor */
        unit->roothub.devdesc.bLength                       = sizeof(struct UsbStdDevDesc);
        unit->roothub.devdesc.bDescriptorType               = UDT_DEVICE;
        unit->roothub.devdesc.bcdUSB                        = AROS_WORD2LE(0x0200);
        unit->roothub.devdesc.bDeviceClass                  = HUB_CLASSCODE;
        unit->roothub.devdesc.bDeviceSubClass               = 0;
        unit->roothub.devdesc.bDeviceProtocol               = 0;
        unit->roothub.devdesc.bMaxPacketSize0               = 8; // Valid values are 8, 9(SuperSpeed), 16, 32, 64
        unit->roothub.devdesc.idVendor                      = AROS_WORD2LE(0x0000);
        unit->roothub.devdesc.idProduct                     = AROS_WORD2LE(0x0000);
        unit->roothub.devdesc.bcdDevice                     = AROS_WORD2LE(0x0200);
        unit->roothub.devdesc.iManufacturer                 = 1;
        unit->roothub.devdesc.iProduct                      = 2;
        unit->roothub.devdesc.iSerialNumber                 = 0;
        unit->roothub.devdesc.bNumConfigurations            = 1;

        /* This is our root hub config descriptor */
        unit->roothub.config.cfgdesc.bLength                = sizeof(struct UsbStdCfgDesc);
        unit->roothub.config.cfgdesc.bLength                = sizeof(struct UsbStdCfgDesc);
        unit->roothub.config.cfgdesc.bDescriptorType        = UDT_CONFIGURATION;
        unit->roothub.config.cfgdesc.wTotalLength           = AROS_WORD2LE(sizeof(struct RHConfig));
        unit->roothub.config.cfgdesc.bNumInterfaces         = 1;
        unit->roothub.config.cfgdesc.bConfigurationValue    = 1;
        unit->roothub.config.cfgdesc.iConfiguration         = 3;
        unit->roothub.config.cfgdesc.bmAttributes           = (USCAF_ONE|USCAF_SELF_POWERED);
        unit->roothub.config.cfgdesc.bMaxPower              = 0;

        unit->roothub.config.ifdesc.bLength                 = sizeof(struct UsbStdIfDesc);
        unit->roothub.config.ifdesc.bDescriptorType         = UDT_INTERFACE;
        unit->roothub.config.ifdesc.bInterfaceNumber        = 0;
        unit->roothub.config.ifdesc.bAlternateSetting       = 0;
        unit->roothub.config.ifdesc.bNumEndpoints           = 1;
        unit->roothub.config.ifdesc.bInterfaceClass         = HUB_CLASSCODE;
        unit->roothub.config.ifdesc.bInterfaceSubClass      = 0;
        unit->roothub.config.ifdesc.bInterfaceProtocol      = 0;
        unit->roothub.config.ifdesc.iInterface              = 4;

        unit->roothub.config.epdesc.bLength                 = sizeof(struct UsbStdEPDesc);
        unit->roothub.config.epdesc.bDescriptorType         = UDT_ENDPOINT;
        unit->roothub.config.epdesc.bEndpointAddress        = (URTF_IN|1);
        unit->roothub.config.epdesc.bmAttributes            = USEAF_INTERRUPT;
        unit->roothub.config.epdesc.wMaxPacketSize          = AROS_WORD2LE(8);
        unit->roothub.config.epdesc.bInterval               = 12;

        /* This is our root hub hub descriptor */
        unit->roothub.hubdesc.bLength             = sizeof(struct UsbHubDesc);
        unit->roothub.hubdesc.bDescriptorType     = UDT_HUB;
        unit->roothub.hubdesc.bNbrPorts           = (UBYTE) unit->roothub.port_count;   //Set below after port creation
        unit->roothub.hubdesc.wHubCharacteristics = AROS_WORD2LE(UHCF_INDIVID_POWER|UHCF_INDIVID_OVP);
        unit->roothub.hubdesc.bPwrOn2PwrGood      = 0;
        unit->roothub.hubdesc.bHubContrCurrent    = 1;
        unit->roothub.hubdesc.DeviceRemovable     = 1;
        unit->roothub.hubdesc.PortPwrCtrlMask     = 0;

        sprintf(unit->name, "VUSBHCI_USB%x%x[%d]", (AROS_LE2WORD(unit->roothub.devdesc.bcdUSB)>>8)&0xf, (AROS_LE2WORD(unit->roothub.devdesc.bcdUSB)>>4)&0xf, unit->number);

        port = VUSBHCI_AddNewPort(unit, 0);
        if(port == NULL) {
            mybug(-1, ("[VUSBHCI] VUSBHCI_AddNewUnit: Failed to create new port structure\n"));

            /*
                Free previous ports if any exists and delete this unit
            */

            ForeachNode(&unit->roothub.port_list, port) {
                mybug(-1, ("[VUSBHCI] VUSBHCI_AddNewUnit: Removing port structure %s at %p\n", port->node.ln_Name, port));
                REMOVE(port);
                FreeVec(port);
            }
            FreeVec(unit);
            return NULL;
        } else {
            AddTail(&unit->roothub.port_list,(struct Node *)port);
            unit->roothub.port_count++;
        }

        D( mybug(0, ("[VUSBHCI] VUSBHCI_AddNewUnit:\n"));
        mybug(0, ("        Created new unit numbered %d at %p\n",unit->number, unit));
        mybug(0, ("        Unit node name %s\n", unit->node.ln_Name));

        switch(unit->state) {
            case UHSF_SUSPENDED:
                mybug(0, ("        Unit state: UHSF_SUSPENDED\n"));
                break;
            case UHSF_OPERATIONAL:
                mybug(0, ("        Unit state: UHSF_OPERATIONAL\n"));
                break;
            default:
                mybug(0, ("        Unit state: %lx (Error?)\n", unit->state));
                break;
        } );

        unit->roothub.hubdesc.bNbrPorts = (UBYTE) unit->roothub.port_count;

        return unit;
    }
}

struct VUSBHCIPort *VUSBHCI_AddNewPort(struct VUSBHCIUnit *unit, ULONG portnum) {
    struct VUSBHCIPort *port;

    port = AllocVec(sizeof(struct VUSBHCIPort), MEMF_ANY|MEMF_CLEAR);
    if(port == NULL) {
        mybug(-1, ("[VUSBHCI] VUSBHCI_AddNewPort: Failed to create new port structure\n"));
        return NULL;
    } else {
        port->node.ln_Type = NT_USER;
        /* Poseidon treats port number 0 as roothub */
        port->number = portnum+1;

        sprintf(port->name, "VUSBHCI_USB%x%x[%d:%d]", (AROS_LE2WORD(unit->roothub.devdesc.bcdUSB)>>8)&0xf, (AROS_LE2WORD(unit->roothub.devdesc.bcdUSB)>>4)&0xf, unit->number, port->number);
        port->node.ln_Name = (STRPTR)&port->name;
    }

    mybug(0, ("[VUSBHCI] VUSBHCI_AddNewPort:\n"));
    mybug(0, ("        Created new port numbered %d at %p\n",port->number, port));
    mybug(0, ("        Port node name %s\n", port->node.ln_Name));

    return port;
}

