/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

/******************************************************************************

    NAME

        FindPrinters

    SYNOPSIS

        UNIT/K/N, NAME/K, FORMAT/K, WAIT/K/N, SAVE/S, TO/K, URI/K

    LOCATION

        C:

    FUNCTION

        Finds the network printers on the local link (DNS-SD over multicast
        DNS, RFC 6762 and 6763) and lists them: IPP printers (_ipp._tcp) and
        those that take a raw job on a socket (_pdl-datastream._tcp).

        Each IPP printer is asked about itself (Get-Printer-Attributes): its
        make and model, state, the paper it has loaded, its supplies and the
        pictures it has of itself (printer-icons).

        With UNIT, sets ipp.device's unit to a printer found: the one whose
        name contains NAME, or the only one there is. The unit's settings go
        to ENV:IPP/Unit<n>, and with SAVE also to ENVARC:. To print
        through it in the background, set printspool.device in front of it:
        SetEnv SAVE PrintSpool/Unit0 "ipp.device 0".

    INPUTS

        UNIT    -- ipp.device unit to set up (0-9).
        NAME    -- part of the printer's name, when there are several.
        FORMAT  -- document format to send, e.g. application/postscript;
                   the default lets the printer tell.
        WAIT    -- seconds to listen for answers (default 3).
        SAVE    -- keep the unit's settings over a reboot.
        URI     -- ask this one IPP printer (ipp://host[:port]/path) instead
                   of looking for printers: for a status display.
        TO      -- also write what was found to this file for programs, one
                   block of Key=value lines per printer, with the printers'
                   pictures beside it as <file>.<n>.png.

    EXAMPLE

        FindPrinters
        FindPrinters UNIT 0 NAME Dell FORMAT application/postscript SAVE

******************************************************************************/

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>

#include <string.h>

#include "fp_http.h"
#include "fp_ipp.h"
#include "fp_mdns.h"

const TEXT version[] = "$VER: FindPrinters 1.0 (16.10.2026)";

#define TEMPLATE "UNIT/K/N,NAME/K,FORMAT/K,WAIT/K/N,SAVE/S,TO/K,URI/K"
enum { ARG_UNIT, ARG_NAME, ARG_FORMAT, ARG_WAIT, ARG_SAVE, ARG_TO, ARG_URI, ARG_COUNT };

#define MAX_PRINTERS    32

struct Printer {
    char p_Name[NAME_MAX];              /* the instance's first label */
    char p_URI[NAME_MAX];
    char p_Formats[TXT_MAX];
    char p_UUID[64];
    BOOL p_IPP;
    BOOL p_HaveInfo;
    struct PrinterInfo p_Info;
};

static struct Printer printers[MAX_PRINTERS];
static LONG nprinters;
struct Library *SocketBase;

static void appendNumber(char *s, ULONG v)
{
    char d[12];
    LONG i = 0;

    do {
        d[i++] = '0' + v % 10;
        v /= 10;
    } while (v);
    s += strlen(s);
    while (i)
        *s++ = d[--i];
    *s = 0;
}

/* ------------------------------------------------------------------ settings */

/* The printer's picture of itself, as a PNG file. */
static BOOL savePicture(struct Printer *p, const char *path)
{
    const char *from = NULL;
    LONG status = 0;

    if (!p->p_HaveInfo)
        return FALSE;
    if (IPP_SavePicture(&p->p_Info, path, &from, &status))
        return TRUE;
    if (from)
        Printf("   picture: not read from %s (%s %ld)\n", from,
            status < 0 ? (STRPTR)"no answer" : (STRPTR)"HTTP", (long)(status < 0 ? HTTP_LastError : status));
    return FALSE;
}

static void showInfo(struct Printer *p)
{
    struct PrinterInfo *pi = &p->p_Info;
    LONG i;

    if (!p->p_HaveInfo)
        return;
    if (pi->pi_Model[0])
        Printf("   model: %s\n", pi->pi_Model);
    if (pi->pi_State[0])
        Printf("   state: %s\n", pi->pi_State);
    if (pi->pi_Reasons[0] && strcmp(pi->pi_Reasons, "none") != 0)
        Printf("   attention: %s\n", pi->pi_Reasons);
    if (pi->pi_Message[0])
        Printf("   message: %s\n", pi->pi_Message);
    if (pi->pi_Paper[0])
        Printf("   paper: %s\n", pi->pi_Paper);
    for (i = 0; i < pi->pi_NumSupplies; i++)
        if (pi->pi_Level[i] >= 0)
            Printf("   %s: %ld%%\n", pi->pi_Supply[i], (long)pi->pi_Level[i]);
}

/* The TO file: a block of Key=value lines per printer, blank line between. */
static BOOL writeList(const char *to)
{
    char pic[300];
    BPTR fh;
    LONG i, j;

    if (!(fh = Open(to, MODE_NEWFILE)))
        return FALSE;
    for (i = 0; i < nprinters; i++) {
        struct Printer *p = &printers[i];
        struct PrinterInfo *pi = &p->p_Info;
        if (i)
            FPuts(fh, "\n");
        FPrintf(fh, "Printer=%s\nURI=%s\n", p->p_Name, p->p_URI);
        if (p->p_Formats[0]) FPrintf(fh, "Formats=%s\n", p->p_Formats);
        if (p->p_UUID[0]) FPrintf(fh, "UUID=%s\n", p->p_UUID);
        if (p->p_HaveInfo) {
            if (pi->pi_Model[0]) FPrintf(fh, "Model=%s\n", pi->pi_Model);
            if (pi->pi_State[0]) FPrintf(fh, "State=%s\n", pi->pi_State);
            if (pi->pi_Reasons[0]) FPrintf(fh, "Reasons=%s\n", pi->pi_Reasons);
            if (pi->pi_Message[0]) FPrintf(fh, "Message=%s\n", pi->pi_Message);
            if (pi->pi_Paper[0]) FPrintf(fh, "Paper=%s\n", pi->pi_Paper);
            for (j = 0; j < pi->pi_NumSupplies; j++)
                FPrintf(fh, "Supply=%ld;%s;%s\n", (long)pi->pi_Level[j], pi->pi_Colour[j], pi->pi_Supply[j]);
        }
        strcpy(pic, to);
        strcat(pic, ".");
        appendNumber(pic, i + 1);
        strcat(pic, ".png");
        if (savePicture(p, pic))
            FPrintf(fh, "Picture=%s\n", pic);
    }
    Close(fh);
    return TRUE;
}

static BOOL writeUnit(const char *dir, LONG unit, struct Printer *p, const char *format)
{
    char path[48], line[NAME_MAX + 16];
    BPTR fh, lock;

    strcpy(path, dir);
    strcat(path, "IPP");
    if ((lock = Lock(path, SHARED_LOCK)))
        UnLock(lock);
    else if ((lock = CreateDir(path)))
        UnLock(lock);
    strcat(path, "/Unit");
    appendNumber(path, unit);
    if (!(fh = Open(path, MODE_NEWFILE)))
        return FALSE;
    strcpy(line, "URI="); strcat(line, p->p_URI); strcat(line, "\n"); FPuts(fh, line);
    strcpy(line, "NAME="); strcat(line, p->p_Name); strcat(line, "\n"); FPuts(fh, line);
    if (format && *format) {
        strcpy(line, "FORMAT="); strcat(line, format); strcat(line, "\n"); FPuts(fh, line);
    }
    if (p->p_UUID[0]) {
        strcpy(line, "UUID="); strcat(line, p->p_UUID); strcat(line, "\n"); FPuts(fh, line);
    }
    if (p->p_Formats[0]) {
        FPuts(fh, "FORMATS="); FPuts(fh, p->p_Formats); FPuts(fh, "\n");
    }
    if (p->p_HaveInfo && p->p_Info.pi_Model[0])
        FPrintf(fh, "MODEL=%s\n", p->p_Info.pi_Model);
    strcat(path, ".png");
    if (savePicture(p, path))
        FPrintf(fh, "PICTURE=%s\n", path);
    Close(fh);
    return TRUE;
}

static BOOL contains(const char *hay, const char *needle)
{
    LONG n = strlen(needle);

    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0)
            return TRUE;
    return FALSE;
}

int main(void)
{
    IPTR args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    LONG wait = 3, i, rc = RETURN_OK;

    if (!(rda = ReadArgs(TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), "FindPrinters");
        return RETURN_FAIL;
    }
    if (args[ARG_WAIT])
        wait = *(LONG *)args[ARG_WAIT];
    if (!(SocketBase = OpenLibrary("bsdsocket.library", 4))) {
        PutStr("FindPrinters: the network is not running\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    {
        if (args[ARG_URI]) {
            /* One printer, asked directly. */
            struct Printer *p = &printers[0];
            memset(p, 0, sizeof(*p));
            strncpy(p->p_URI, (const char *)args[ARG_URI], sizeof(p->p_URI) - 1);
            strncpy(p->p_Name, p->p_URI, sizeof(p->p_Name) - 1);
            p->p_IPP = TRUE;
            nprinters = 1;
        } else {
            static struct MDNSPrinter found[MAX_PRINTERS];
            LONG n = MDNS_FindPrinters(wait * 1000, found, MAX_PRINTERS);
            for (i = 0; i < n; i++) {
                struct Printer *p = &printers[nprinters++];
                memset(p, 0, sizeof(*p));
                strcpy(p->p_Name, found[i].mp_Name);
                strcpy(p->p_URI, found[i].mp_URI);
                strcpy(p->p_Formats, found[i].mp_Formats);
                strcpy(p->p_UUID, found[i].mp_UUID);
                p->p_IPP = found[i].mp_IPP;
            }
        }

        if (nprinters == 0)
            PutStr("No network printers found.\n");
        for (i = 0; i < nprinters; i++) {
            if (printers[i].p_IPP)
                printers[i].p_HaveInfo = IPP_GetInfo(printers[i].p_URI, &printers[i].p_Info);
            if (args[ARG_URI] && printers[i].p_HaveInfo && printers[i].p_Info.pi_Model[0])
                strncpy(printers[i].p_Name, printers[i].p_Info.pi_Model, sizeof(printers[i].p_Name) - 1);
            if (args[ARG_URI] && !printers[i].p_HaveInfo) {
                Printf("FindPrinters: %s does not answer\n", printers[i].p_URI);
                rc = RETURN_WARN;
            }
            Printf("%ld  %s\n   %s\n", (long)i + 1, printers[i].p_Name, printers[i].p_URI);
            if (printers[i].p_Formats[0])
                Printf("   formats: %s\n", printers[i].p_Formats);
            showInfo(&printers[i]);
        }
        if (args[ARG_TO] && !writeList((const char *)args[ARG_TO])) {
            PrintFault(IoErr(), "FindPrinters");
            rc = RETURN_ERROR;
        }

        if (args[ARG_UNIT]) {
            LONG unit = *(LONG *)args[ARG_UNIT];
            const char *name = (const char *)args[ARG_NAME];
            struct Printer *pick = NULL;
            LONG matches = 0;
            for (i = 0; i < nprinters; i++)
                if (!name || contains(printers[i].p_Name, name)) {
                    if (!pick || (printers[i].p_IPP && !pick->p_IPP))
                        pick = &printers[i];
                    matches++;
                }
            if (unit < 0 || unit > 9) {
                PutStr("FindPrinters: UNIT is 0 to 9\n");
                rc = RETURN_ERROR;
            } else if (!pick) {
                PutStr("FindPrinters: no printer to set up\n");
                rc = RETURN_WARN;
            } else if (!name && nprinters > 1 && matches > 1) {
                PutStr("FindPrinters: several printers; name one with NAME\n");
                rc = RETURN_WARN;
            } else if (!writeUnit("ENV:", unit, pick, (const char *)args[ARG_FORMAT])
                       || (args[ARG_SAVE] && !writeUnit("ENVARC:", unit, pick, (const char *)args[ARG_FORMAT]))) {
                PrintFault(IoErr(), "FindPrinters");
                rc = RETURN_ERROR;
            } else
                Printf("ipp.device unit %ld: %s\n", (long)unit, pick->p_Name);
        }
    }
    CloseLibrary(SocketBase);
    FreeArgs(rda);
    return rc;
}
