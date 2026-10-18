/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    FindPrinters: asks an IPP printer about itself with Get-Printer-Attributes
    (RFC 8011 section 4.2.5), encoded as RFC 8010 describes.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "fp_http.h"
#include "fp_ipp.h"

#define TAG_OPERATION_ATTRIBUTES    0x01
#define TAG_END_OF_ATTRIBUTES       0x03
#define TAG_INTEGER                 0x21
#define TAG_ENUM                    0x23
#define TAG_URI                     0x45
#define TAG_KEYWORD                 0x44
#define TAG_CHARSET                 0x47
#define TAG_NATURAL_LANGUAGE        0x48

#define IPP_OP_GET_PRINTER_ATTRIBUTES 0x000b
#define REPLY_MAX                   65536

static const char *wanted[] = {
    "printer-make-and-model", "printer-icons", "printer-state", "printer-state-reasons",
    "printer-state-message", "media-ready",
    "marker-names", "marker-levels", "marker-colors", NULL
};

static UBYTE *put(UBYTE *p, UBYTE tag, const char *name, const char *value)
{
    UWORD nl = strlen(name), vl = strlen(value);

    *p++ = tag;
    *p++ = nl >> 8;
    *p++ = nl;
    memcpy(p, name, nl);
    p += nl;
    *p++ = vl >> 8;
    *p++ = vl;
    memcpy(p, value, vl);
    return p + vl;
}

static void text(char *to, LONG max, const UBYTE *v, LONG len)
{
    if (len >= max)
        len = max - 1;
    memcpy(to, v, len);
    to[len] = 0;
}

/* Appends one value of a 1setOf attribute, comma separated. */
static void addValue(char *to, LONG max, const UBYTE *v, LONG len)
{
    LONG have = strlen(to);

    if (have && have + 1 < max)
        strcat(to, ",");
    have = strlen(to);
    text(to + have, max - have, v, len);
}

BOOL IPP_GetInfo(const char *uri, struct PrinterInfo *pi)
{
    UBYTE req[1024], *p = req, *buf, *body;
    LONG status, bodylen, i, names = 0, levels = 0, colours = 0;
    const char *attr = "";
    char url[300];

    memset(pi, 0, sizeof(*pi));
    for (i = 0; i < FP_SUPPLIES; i++)
        pi->pi_Level[i] = -1;
    if (strlen(uri) > 256)
        return FALSE;

    *p++ = 1;
    *p++ = 1;
    *p++ = IPP_OP_GET_PRINTER_ATTRIBUTES >> 8;
    *p++ = IPP_OP_GET_PRINTER_ATTRIBUTES & 0xff;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 1;
    *p++ = TAG_OPERATION_ATTRIBUTES;
    p = put(p, TAG_CHARSET, "attributes-charset", "utf-8");
    p = put(p, TAG_NATURAL_LANGUAGE, "attributes-natural-language", "en");
    p = put(p, TAG_URI, "printer-uri", uri);
    for (i = 0; wanted[i]; i++)
        p = put(p, TAG_KEYWORD, i == 0 ? "requested-attributes" : "", wanted[i]);
    *p++ = TAG_END_OF_ATTRIBUTES;

    if (!(buf = AllocVec(REPLY_MAX, MEMF_ANY)))
        return FALSE;
    strcpy(url, uri);
    status = HTTP_Request("POST", url, "application/ipp", req, p - req, buf, REPLY_MAX, &body, &bodylen, NULL, 0);
    if (status != 200 || bodylen < 9 || ((body[2] << 8) | body[3]) >= 0x0100) {
        FreeVec(buf);
        return FALSE;
    }

    /* Attribute groups: delimiter tags below 0x10; an empty name continues the last attribute. */
    for (i = 8; i < bodylen;) {
        UBYTE tag = body[i++];
        LONG nl, vl;
        const UBYTE *name, *v;

        if (tag == TAG_END_OF_ATTRIBUTES)
            break;
        if (tag < 0x10)
            continue;
        if (i + 2 > bodylen)
            break;
        nl = (body[i] << 8) | body[i + 1];
        name = body + i + 2;
        i += 2 + nl;
        if (i + 2 > bodylen)
            break;
        vl = (body[i] << 8) | body[i + 1];
        v = body + i + 2;
        i += 2 + vl;
        if (i > bodylen)
            break;
        if (nl > 0) {
            static char current[48];
            text(current, sizeof(current), name, nl);
            attr = current;
        }
        if (strcmp(attr, "printer-make-and-model") == 0)
            text(pi->pi_Model, sizeof(pi->pi_Model), v, vl);
        else if (strcmp(attr, "printer-icons") == 0 && pi->pi_NumIcons < FP_ICONS)
            text(pi->pi_Icons[pi->pi_NumIcons++], sizeof(pi->pi_Icons[0]), v, vl);
        else if (strcmp(attr, "printer-state") == 0 && vl == 4) {
            LONG s = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
            strcpy(pi->pi_State, s == 3 ? "idle" : s == 4 ? "processing" : s == 5 ? "stopped" : "unknown");
        } else if (strcmp(attr, "media-ready") == 0)
            addValue(pi->pi_Paper, sizeof(pi->pi_Paper), v, vl);
        else if (strcmp(attr, "printer-state-reasons") == 0)
            addValue(pi->pi_Reasons, sizeof(pi->pi_Reasons), v, vl);
        else if (strcmp(attr, "printer-state-message") == 0)
            text(pi->pi_Message, sizeof(pi->pi_Message), v, vl);
        else if (strcmp(attr, "marker-names") == 0 && names < FP_SUPPLIES)
            text(pi->pi_Supply[names++], sizeof(pi->pi_Supply[0]), v, vl);
        else if (strcmp(attr, "marker-levels") == 0 && levels < FP_SUPPLIES && vl == 4)
            pi->pi_Level[levels++] = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
        else if (strcmp(attr, "marker-colors") == 0 && colours < FP_SUPPLIES)
            text(pi->pi_Colour[colours++], sizeof(pi->pi_Colour[0]), v, vl);
    }
    pi->pi_NumSupplies = names;
    FreeVec(buf);
    return TRUE;
}

BOOL IPP_SavePicture(const struct PrinterInfo *pi, const char *path, const char **from, LONG *status)
{
    static const char *png = "\x89PNG";
    UBYTE *buf, *body;
    LONG bodylen, i, pick = -1;
    BPTR fh;
    BOOL ok = FALSE;

    *from = NULL;
    *status = 0;
    if (pi->pi_NumIcons == 0)
        return FALSE;
    for (i = 0; i < pi->pi_NumIcons; i++)
        if (strstr(pi->pi_Icons[i], "128"))
            pick = i;
    if (pick < 0)
        pick = pi->pi_NumIcons > 1 ? 1 : 0;
    *from = pi->pi_Icons[pick];
    if (!(buf = AllocVec(512 * 1024, MEMF_ANY)))
        return FALSE;
    *status = HTTP_Get(pi->pi_Icons[pick], buf, 512 * 1024, &body, &bodylen);
    if (*status == 200 && bodylen > 8 && memcmp(body, png, 4) == 0 && (fh = Open(path, MODE_NEWFILE))) {
        ok = Write(fh, body, bodylen) == bodylen;
        Close(fh);
    }
    FreeVec(buf);
    return ok;
}
