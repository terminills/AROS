/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Broadcom patchram firmware loader (brcmbt.fwl)
*/

/*
 * Broadcom USB radios (BCM20702, BCM4335/4354/4356, ...) run a ROM firmware
 * until the host downloads a patch (.hcd). The .hcd file is a list of HCI
 * vendor commands; it is replayed after Download_Minidriver and ends with
 * Launch_RAM. Patches are read from DEVS:Firmware/brcm/<chip>.hcd or the
 * Linux-style <chip>-<vid>-<pid>.hcd.
 */

#include <aros/libcall.h>
#include <aros/symbolsets.h>

#include <exec/exec.h>
#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bluetooth.h>

#include <string.h>

#include "brcmfwbase.h"

#include LC_LIBDEFS_FILE

struct BrcmChip
{
    UWORD        bc_Subver;
    CONST_STRPTR bc_Name;
};

/* LMP subversion -> chip name, as in Linux btbcm's USB table */
static const struct BrcmChip brcm_chips[] =
{
    { 0x2105, "BCM20703A1" },
    { 0x2118, "BCM20702A0" },
    { 0x2126, "BCM4335A0"  },
    { 0x220e, "BCM20702A1" },
    { 0x230f, "BCM4356A2"  },
    { 0x4106, "BCM4335B0"  },
    { 0x410e, "BCM20702B0" },
    { 0x6109, "BCM4335C0"  },
    { 0x610c, "BCM4354"    },
    { 0x6607, "BCM4350C5"  },
    { 0, NULL }
};

static CONST_STRPTR brcmChipName(UWORD subver)
{
    const struct BrcmChip *c;

    for(c = brcm_chips; c->bc_Name; c++) {
        if(c->bc_Subver == subver) {
            return(c->bc_Name);
        }
    }
    return(NULL);
}

static UBYTE *brcmLoadFile(struct DosLibrary *DOSBase, CONST_STRPTR path, ULONG *len)
{
    BPTR fh;
    LONG size;
    UBYTE *buf = NULL;

    if(!(fh = Open(path, MODE_OLDFILE))) {
        return(NULL);
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if((size > 0) && (buf = AllocVec(size, MEMF_ANY))) {
        if(Read(fh, buf, size) != size) {
            FreeVec(buf);
            buf = NULL;
        } else {
            *len = (ULONG) size;
        }
    }
    Close(fh);
    return(buf);
}

static BOOL brcmNameEq(CONST_STRPTR a, CONST_STRPTR b, ULONG n)
{
    while(n--) {
        UBYTE ca = *a++, cb = *b++;

        if(ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if(cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if(ca != cb) {
            return(FALSE);
        }
    }
    return(TRUE);
}

/* <chip>.hcd, else the first <chip>-*.hcd in the firmware drawer */
static UBYTE *brcmFindPatch(struct DosLibrary *DOSBase, CONST_STRPTR chip,
                            STRPTR path, ULONG pathlen, ULONG *len)
{
    struct FileInfoBlock *fib;
    UBYTE *buf;
    BPTR dir;
    ULONG clen = strlen(chip);

    strlcpy(path, BRCM_FW_DIR, pathlen);
    strlcat(path, chip, pathlen);
    strlcat(path, ".hcd", pathlen);
    if((buf = brcmLoadFile(DOSBase, path, len))) {
        return(buf);
    }

    if(!(dir = Lock(BRCM_FW_DIR, SHARED_LOCK))) {
        return(NULL);
    }
    if((fib = AllocDosObject(DOS_FIB, NULL))) {
        if(Examine(dir, fib)) {
            while(!buf && ExNext(dir, fib)) {
                ULONG nlen = strlen(fib->fib_FileName);

                if((fib->fib_DirEntryType < 0) && (nlen > clen + 4) &&
                   brcmNameEq(fib->fib_FileName, chip, clen) && (fib->fib_FileName[clen] == '-') &&
                   brcmNameEq(&fib->fib_FileName[nlen - 4], ".hcd", 4)) {
                    strlcpy(path, BRCM_FW_DIR, pathlen);
                    strlcat(path, fib->fib_FileName, pathlen);
                    buf = brcmLoadFile(DOSBase, path, len);
                }
            }
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(dir);
    return(buf);
}

static ULONG brcmMatch(struct BtFirmwareLoader *self, struct BtFirmwareContext *ctx)
{
    (void) self;
    if(ctx->fwc_Manufacturer != BT_MANUFACTURER_BROADCOM) {
        return(0);
    }
    return(brcmChipName(ctx->fwc_LMPSubversion) ? 100 : 0);
}

static LONG brcmLoad(struct BtFirmwareLoader *self, struct BtFirmwareContext *ctx)
{
    struct BrcmFwBase *bfw = self->fwl_UserData;
    struct DosLibrary *DOSBase = bfw->bfw_DOSBase;
    CONST_STRPTR chip = brcmChipName(ctx->fwc_LMPSubversion);
    UBYTE *fw;
    ULONG fwlen = 0, off = 0, records = 0;
    UBYTE status = 0;
    char path[128];
    LONG res = -1;

    if(!chip) {
        return(-1);
    }
    if(!(fw = brcmFindPatch(DOSBase, chip, path, sizeof(path), &fwlen))) {
        ctx->fwc_Log(ctx, RETURN_WARN, "Broadcom %s: no patch in %s - running the ROM firmware.",
                     chip, BRCM_FW_DIR);
        return(0);
    }

    if(ctx->fwc_HciCommand(ctx, HC_OP_RESET, NULL, 0, &status, NULL, NULL, 0)) {
        ctx->fwc_Log(ctx, RETURN_FAIL, "Broadcom %s: reset failed (status 0x%02lx).", chip, (ULONG) status);
        goto done;
    }
    if(ctx->fwc_HciCommand(ctx, HC_OP_BRCM_DOWNLOAD_MINIDRV, NULL, 0, &status, NULL, NULL, 0)) {
        ctx->fwc_Log(ctx, RETURN_FAIL, "Broadcom %s: Download_Minidriver failed (status 0x%02lx).",
                     chip, (ULONG) status);
        goto done;
    }
    Delay(3);   /* 50 ms for the minidriver to start */

    while(off + 3 <= fwlen) {
        UWORD opcode = fw[off] | (fw[off + 1] << 8);
        UBYTE plen = fw[off + 2];

        if(off + 3 + plen > fwlen) {
            ctx->fwc_Log(ctx, RETURN_FAIL, "Broadcom %s: '%s' is truncated at offset %ld.",
                         chip, path, off);
            goto done;
        }
        if(ctx->fwc_HciCommand(ctx, opcode, &fw[off + 3], plen, &status, NULL, NULL, 0)) {
            ctx->fwc_Log(ctx, RETURN_FAIL, "Broadcom %s: patch command 0x%04lx failed (status 0x%02lx).",
                         chip, (ULONG) opcode, (ULONG) status);
            goto done;
        }
        off += 3 + plen;
        records++;
    }
    Delay(13);  /* 250 ms for the patched firmware to launch */

    if(ctx->fwc_HciCommand(ctx, HC_OP_RESET, NULL, 0, &status, NULL, NULL, 0)) {
        ctx->fwc_Log(ctx, RETURN_FAIL, "Broadcom %s: reset after the patch failed.", chip);
        goto done;
    }
    ctx->fwc_Log(ctx, RETURN_OK, "Broadcom %s: patched from %s (%ld commands, %ld bytes).",
                 chip, path, records, fwlen);
    res = 0;

done:
    FreeVec(fw);
    return(res);
}

static int GM_UNIQUENAME(libInit)(LIBBASETYPEPTR bfw)
{
    struct Library *BluetoothBase;

    bfw->bfw_DOSBase = (struct DosLibrary *) OpenLibrary("dos.library", 36);
    if(!bfw->bfw_DOSBase) {
        return FALSE;
    }
    if(!(BluetoothBase = OpenLibrary("bluetooth.library", 1))) {
        CloseLibrary((struct Library *) bfw->bfw_DOSBase);
        return FALSE;
    }
    bfw->bfw_BluetoothBase = BluetoothBase;

    bfw->bfw_Loader.fwl_Name     = "Broadcom (patchram)";
    bfw->bfw_Loader.fwl_Match    = brcmMatch;
    bfw->bfw_Loader.fwl_Load     = brcmLoad;
    bfw->bfw_Loader.fwl_UserData = bfw;
    btAddFirmwareLoader(&bfw->bfw_Loader);
    return TRUE;
}

static int GM_UNIQUENAME(libExpunge)(LIBBASETYPEPTR bfw)
{
    struct Library *BluetoothBase = bfw->bfw_BluetoothBase;

    if(BluetoothBase) {
        btRemFirmwareLoader(&bfw->bfw_Loader);
        CloseLibrary(BluetoothBase);
    }
    if(bfw->bfw_DOSBase) {
        CloseLibrary((struct Library *) bfw->bfw_DOSBase);
    }
    return TRUE;
}

ADD2INITLIB(GM_UNIQUENAME(libInit), 0)
ADD2EXPUNGELIB(GM_UNIQUENAME(libExpunge), 0)
