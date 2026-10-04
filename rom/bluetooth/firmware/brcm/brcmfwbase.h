#ifndef BRCMFWBASE_H
#define BRCMFWBASE_H
/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <exec/libraries.h>
#include <dos/dosextens.h>
#include <libraries/bluetooth.h>

struct BrcmFwBase
{
    struct Library          bfw_Lib;
    struct Library         *bfw_BluetoothBase;
    struct DosLibrary      *bfw_DOSBase;
    struct BtFirmwareLoader bfw_Loader;
};

#define BT_MANUFACTURER_BROADCOM    15
#define BRCM_FW_DIR                 "DEVS:Firmware/brcm/"

#define HC_OP_RESET                 0x0c03
#define HC_OP_BRCM_DOWNLOAD_MINIDRV 0xfc2e

#endif /* BRCMFWBASE_H */
