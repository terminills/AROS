/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

/******************************************************************************

    NAME

        PrinterWatch

    SYNOPSIS

        (none; started from SYS:WBStartup)

    LOCATION

        SYS:WBStartup

    FUNCTION

        Watches the local network for printers (DNS-SD) and tells the user
        about each new one with a notification showing the printer's own
        picture. Double-clicking it sets the printer up: ipp.device for the
        connection, printspool.device in front of it, and printer.device's
        settings for a unit. The first network printer becomes printer 0,
        the default, unless that unit already prints somewhere real.

        When Gutenprint (from Contrib) is installed, the setup is handed to
        GUTENPRINT:GutenprintPrefs, which picks the printer's model;
        otherwise a printer that takes PostScript is set up with AROS's
        PostScript driver.

        It also keeps an eye on the network printers that are set up and
        tells the user when one runs out of paper or toner, jams or goes
        offline. While printspool.device sends a job it says the job has
        started, with the printer's ink or toner levels; while the printer
        works on it, it tells about a jam, an empty tray or supply, an open
        door; and it follows the job at the printer (IPP Get-Job-Attributes)
        to say when it is printed or why it was not. TellJobStart,
        TellJobDone and TellProblems set to FALSE in
        ENVARC:Sys/PrinterWatch.xml leave those out.

        A printer that is not set up is offered once each start, on at most
        three starts; the count is kept in ENVARC:Sys/PrinterWatch.xml. Send it APPLIBMT_Quit (or CTRL-C) to
        stop it.

******************************************************************************/

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/notify.h>
#include <dos/var.h>
#include <devices/timer.h>
#include <libraries/application.h>
#include <prefs/prefhdr.h>
#include <prefs/printertxt.h>
#include <prefs/printergfx.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/application.h>
#include <proto/bsdsocket.h>

#include <string.h>

#include "../findprinters/fp_mdns.h"
#include "../findprinters/fp_ipp.h"

const TEXT version[] = "$VER: PrinterWatch 1.0 (18.10.2026)";

#define MAX_FOUND       32
#define MAX_UNITS       10
#define FIRST_SCAN      20      /* seconds after start, for the network to come up */
#define FAST_SCAN       60      /* the first ten minutes */
#define SLOW_SCAN       300
#define STATUS_EVERY    300
#define PICTURE_DIR     "T:PrinterWatch"
#define GUTENPRINTPREFS "GUTENPRINT:GutenprintPrefs"
#define ANNOUNCED_KEY   "Announced"
#define MAX_OFFERS      3       /* starts on which a printer not set up is offered */
#define JOB_POLL        5       /* seconds between looks at the printer during a job */
#define JOB_FOLLOW      900     /* seconds to follow a sent job to its end */
#define JOBVAR          "PrintSpool/Job"

struct Library *ApplicationBase;
struct Library *SocketBase;

/* A printer seen on the network. */
struct Seen {
    struct MDNSPrinter s_P;
    struct PrinterInfo s_Info;
    BOOL s_HaveInfo;
    char s_Picture[64];                 /* its picture, a PNG, or "" */
    char s_Reasons[160];                /* last printer-state-reasons told about */
    ULONG s_LastStatus;                 /* seconds since start of the last status check */
    BOOL s_Offered;                     /* announced since this start */
};

static struct Seen seen[MAX_FOUND];
static LONG nseen;
static ULONG appid;
static PrefsObject *state;              /* ENVARC:Sys/PrinterWatch.xml */
static ULONG now;                       /* seconds since start */

/* The print job printspool.device is sending, and after it the job at the printer. */
static struct {
    BOOL j_Active;                      /* printing or waiting, or followed at the printer */
    char j_Job[64];
    char j_State[16];
    char j_Reason[32];
    char j_Device[64];
    LONG j_DevUnit;
    char j_Document[128];
    char j_Application[64];
    char j_URI[256];                    /* an ipp:// printer, or "" */
    char j_Printer[128];                /* what to call it */
    char j_Picture[64];
    ULONG j_JobID;                      /* the printer's job-id once it has the job */
    ULONG j_NextPoll, j_Until;
    char j_Told[200];                   /* the printer-state-reasons told about for this job */
    BOOL j_Started;                     /* "Printing" told, also after a wait first */
} job;

/* ------------------------------------------------------------------ small helpers */

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

static BOOL contains(const char *hay, const char *needle)
{
    LONG n = strlen(needle);

    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0)
            return TRUE;
    return FALSE;
}

static BOOL exists(const char *path)
{
    BPTR lock = Lock(path, SHARED_LOCK);

    if (lock)
        UnLock(lock);
    return lock != BNULL;
}

/* A printer is known by its UUID, or by its address if it has none. */
static const char *keyOf(struct Seen *s)
{
    return s->s_P.mp_UUID[0] ? s->s_P.mp_UUID : s->s_P.mp_URI;
}

/* What to call it: its model, or its DNS-SD name. */
static const char *nameOf(struct Seen *s)
{
    return s->s_HaveInfo && s->s_Info.pi_Model[0] ? s->s_Info.pi_Model : s->s_P.mp_Name;
}

static ULONG notify(const char *title, const char *text, const char *picture, const char *back, ULONG pri)
{
    return Notify(appid,
        APPNOTIFY_Title, (IPTR)title,
        APPNOTIFY_Text, (IPTR)text,
        picture && picture[0] ? APPNOTIFY_ImageFile : TAG_IGNORE, (IPTR)picture,
        APPNOTIFY_ImageVertAlign, 1,
        back ? APPNOTIFY_BackMsg : TAG_IGNORE, (IPTR)back,
        APPNOTIFY_CloseOnDC, TRUE,
        APPNOTIFY_Pri, pri,
        TAG_DONE);
}

/* ------------------------------------------------------------------ what is set up */

/* The value of KEY=value in an ENV:IPP/Unit<n> file. */
static BOOL unitValue(LONG unit, const char *key, char *out, LONG max)
{
    char path[32] = "ENV:IPP/Unit", line[300];
    LONG kl = strlen(key);
    BOOL found = FALSE;
    BPTR fh;

    appendNumber(path, unit);
    out[0] = 0;
    if (!(fh = Open(path, MODE_OLDFILE)))
        return FALSE;
    while (!found && FGets(fh, line, sizeof(line))) {
        LONG n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
            strncpy(out, line + kl + 1, max - 1);
            out[max - 1] = 0;
            found = TRUE;
        }
    }
    Close(fh);
    return found;
}

/* The ipp.device unit this printer is set up on, or -1. */
static LONG configuredUnit(struct Seen *s)
{
    char v[300];
    LONG u;

    for (u = 0; u < MAX_UNITS; u++) {
        if (s->s_P.mp_UUID[0] && unitValue(u, "UUID", v, sizeof(v)) && strcasecmp(v, s->s_P.mp_UUID) == 0)
            return u;
        if (unitValue(u, "URI", v, sizeof(v)) && strcasecmp(v, s->s_P.mp_URI) == 0)
            return u;
    }
    return -1;
}

static void prefsPath(char *path, const char *dir, LONG unit)
{
    strcpy(path, dir);
    strcat(path, "Sys/printer");
    if (unit)
        appendNumber(path, unit);
    strcat(path, ".prefs");
}

/* The output device printer.device unit's prefs name ("" when it has no prefs). */
static void prefsDevice(LONG unit, char *out, LONG max)
{
    char path[48];
    UBYTE buf[512];
    LONG len, i;
    BPTR fh;

    out[0] = 0;
    prefsPath(path, "ENV:", unit);
    if (!(fh = Open(path, MODE_OLDFILE)))
        return;
    len = Read(fh, buf, sizeof(buf));
    Close(fh);
    for (i = 12; i + 8 <= len; ) {
        LONG cl = (buf[i + 4] << 24) | (buf[i + 5] << 16) | (buf[i + 6] << 8) | buf[i + 7];
        if (memcmp(buf + i, "PUNT", 4) == 0 && cl >= (LONG)sizeof(struct PrinterUnitPrefs) && i + 8 + cl <= len) {
            struct PrinterUnitPrefs *pu = (struct PrinterUnitPrefs *)(buf + i + 8);
            strncpy(out, (char *)pu->pu_DeviceName, max - 1);
            out[max - 1] = 0;
            return;
        }
        i += 8 + cl + (cl & 1);
    }
}

/*
 * The unit to set a new printer up on: 0, the default printer, while no
 * network printer is set up and unit 0 prints nowhere real; else the first
 * unit with neither an ipp.device setting nor printer prefs.
 */
static LONG freeUnit(void)
{
    char v[300], dev[40];
    LONG u;
    BOOL network = FALSE;

    for (u = 0; u < MAX_UNITS; u++)
        if (unitValue(u, "URI", v, sizeof(v)))
            network = TRUE;
    prefsDevice(0, dev, sizeof(dev));
    if (!network && (!dev[0] || strcasecmp(dev, "printtofile") == 0))
        return 0;
    for (u = 1; u < MAX_UNITS; u++) {
        char path[48];
        prefsPath(path, "ENV:", u);
        if (!unitValue(u, "URI", v, sizeof(v)) && !exists(path))
            return u;
    }
    return -1;
}

/* ------------------------------------------------------------------ setting up */

static BOOL writeText(const char *path, const char *text)
{
    BPTR fh;
    LONG n = strlen(text);
    BOOL ok;

    if (!(fh = Open(path, MODE_NEWFILE)))
        return FALSE;
    ok = Write(fh, (APTR)text, n) == n;
    return Close(fh) && ok;
}

static void makeDir(const char *path)
{
    BPTR lock;

    if ((lock = Lock(path, SHARED_LOCK)) || (lock = CreateDir(path)))
        UnLock(lock);
}

static UBYTE *putChunk(UBYTE *p, const char *id, const void *data, ULONG len)
{
    memcpy(p, id, 4);
    p[4] = len >> 24; p[5] = len >> 16; p[6] = len >> 8; p[7] = len;
    memcpy(p + 8, data, len);
    p += 8 + len;
    if (len & 1)
        *p++ = 0;
    return p;
}

/* printer.device prefs for a unit, as Printer prefs writes them: the PostScript
   driver, printing to printspool.device <unit>. */
static BOOL writePrinterPrefs(const char *dir, LONG unit, BOOL a4)
{
    struct PrinterTxtPrefs txt;
    struct PrinterUnitPrefs pu;
    struct PrinterDeviceUnitPrefs dev;
    struct PrinterGfxPrefs gfx;
    UBYTE hdr[6] = { PHV_CURRENT, 0, 0, 0, 0, 0 }, out[512], *p = out + 12;
    char path[48], name[16] = "Unit ";
    BPTR fh;
    LONG len;
    BOOL ok;

    memset(&txt, 0, sizeof(txt));
    strcpy((char *)txt.pt_Driver, "PostScript");
    txt.pt_Port = PP_PARALLEL;
    txt.pt_PaperType = AROS_WORD2BE(PT_SINGLE);
    txt.pt_PaperSize = AROS_WORD2BE(a4 ? PS_EURO_A4 : PS_US_LETTER);
    txt.pt_PaperLength = AROS_WORD2BE(65);
    txt.pt_Pitch = AROS_WORD2BE(PP_PICA);
    txt.pt_Spacing = AROS_WORD2BE(PS_SIX_LPI);
    txt.pt_LeftMargin = AROS_WORD2BE(5);
    txt.pt_RightMargin = AROS_WORD2BE(75);
    txt.pt_Quality = AROS_WORD2BE(PQ_LETTER);

    memset(&pu, 0, sizeof(pu));
    pu.pu_UnitNum = AROS_LONG2BE(unit);
    strcpy((char *)pu.pu_DeviceName, "printspool");

    memset(&dev, 0, sizeof(dev));
    dev.pd_UnitNum = AROS_LONG2BE(unit);
    appendNumber(name, unit);
    strcpy((char *)dev.pd_UnitName, name);

    memset(&gfx, 0, sizeof(gfx));
    gfx.pg_Aspect = AROS_WORD2BE(PA_HORIZONTAL);
    gfx.pg_Shade = AROS_WORD2BE(PS_COLOR);
    gfx.pg_Image = AROS_WORD2BE(PI_POSITIVE);
    gfx.pg_Threshold = AROS_WORD2BE(256);
    gfx.pg_Dimensions = PD_IGNORE;
    gfx.pg_GraphicFlags = AROS_WORD2BE(PGFF_CENTER_IMAGE);
    gfx.pg_PrintDensity = PD_FLOYD;

    p = putChunk(p, "PRHD", hdr, sizeof(hdr));
    p = putChunk(p, "PTXT", &txt, sizeof(txt));
    p = putChunk(p, "PUNT", &pu, sizeof(pu));
    p = putChunk(p, "PDEV", &dev, sizeof(dev));
    p = putChunk(p, "PGFX", &gfx, sizeof(gfx));
    len = p - out;
    memcpy(out, "FORM", 4);
    out[4] = (len - 8) >> 24; out[5] = (len - 8) >> 16; out[6] = (len - 8) >> 8; out[7] = len - 8;
    memcpy(out + 8, "PREF", 4);

    strcpy(path, dir);
    strcat(path, "Sys");
    makeDir(path);
    prefsPath(path, dir, unit);
    if (!(fh = Open(path, MODE_NEWFILE)))
        return FALSE;
    ok = Write(fh, out, len) == len;
    return Close(fh) && ok;
}

/* ipp.device's unit, printspool.device in front of it, printer.device's prefs. */
static BOOL setupIn(const char *dir, LONG unit, struct Seen *s, BOOL a4)
{
    char path[48], text[2048], *t;

    strcpy(path, dir);
    strcat(path, "IPP");
    makeDir(path);
    strcat(path, "/Unit");
    appendNumber(path, unit);
    strcpy(text, "URI=");
    strcat(text, s->s_P.mp_URI);
    strcat(text, "\nNAME=");
    strcat(text, nameOf(s));
    strcat(text, "\nFORMAT=application/postscript\n");
    if (s->s_P.mp_UUID[0]) {
        strcat(text, "UUID=");
        strcat(text, s->s_P.mp_UUID);
        strcat(text, "\n");
    }
    if (s->s_P.mp_Formats[0] && strlen(s->s_P.mp_Formats) < 1000) {
        strcat(text, "FORMATS=");
        strcat(text, s->s_P.mp_Formats);
        strcat(text, "\n");
    }
    if (s->s_HaveInfo && s->s_Info.pi_Model[0]) {
        strcat(text, "MODEL=");
        strcat(text, s->s_Info.pi_Model);
        strcat(text, "\n");
    }
    if (s->s_Picture[0]) {
        /* The picture stays with the unit's settings. */
        char pic[48];
        BPTR in, out;
        strcpy(pic, path);
        strcat(pic, ".png");
        if ((in = Open(s->s_Picture, MODE_OLDFILE))) {
            if ((out = Open(pic, MODE_NEWFILE))) {
                UBYTE buf[1024];
                LONG n;
                while ((n = Read(in, buf, sizeof(buf))) > 0)
                    Write(out, buf, n);
                Close(out);
                t = text + strlen(text);
                strcpy(t, "PICTURE=");
                strcat(t, pic);
                strcat(t, "\n");
            }
            Close(in);
        }
    }
    if (!writeText(path, text))
        return FALSE;

    strcpy(path, dir);
    strcat(path, "PrintSpool");
    makeDir(path);
    strcat(path, "/Unit");
    appendNumber(path, unit);
    strcpy(text, "ipp.device ");
    appendNumber(text, unit);
    if (!writeText(path, text))
        return FALSE;

    return writePrinterPrefs(dir, unit, a4);
}

/* Hands the printer to Gutenprint's own setup, which picks the model. */
static LONG gutenprintSetup(LONG unit, struct Seen *s)
{
    char cmd[1700] = GUTENPRINTPREFS " SETUP \"";
    BPTR nil;
    LONG rc;

    strcat(cmd, s->s_P.mp_URI);
    if (s->s_P.mp_Formats[0] && !strchr(s->s_P.mp_Formats, '"') && strlen(s->s_P.mp_Formats) < 1300) {
        strcat(cmd, "\" FORMATS \"");
        strcat(cmd, s->s_P.mp_Formats);
    }
    strcat(cmd, "\" UNIT ");
    appendNumber(cmd, unit);
    strcat(cmd, " SAVE");
    if (!(nil = Open("NIL:", MODE_NEWFILE)))
        return -1;
    rc = SystemTags(cmd, SYS_Output, (IPTR)nil, SYS_Input, BNULL, TAG_DONE);
    Close(nil);
    return rc;
}

static void setUp(struct Seen *s)
{
    char text[300];
    LONG unit = configuredUnit(s);
    BOOL ps = contains(s->s_P.mp_Formats, "postscript");
    BOOL a4 = s->s_HaveInfo && contains(s->s_Info.pi_Paper, "a4");
    BOOL done = FALSE;

    if (unit >= 0) {
        strcpy(text, nameOf(s));
        strcat(text, " is already printer ");
        appendNumber(text, unit);
        strcat(text, ".");
        notify("Printer", text, s->s_Picture, NULL, 0);
        return;
    }
    if ((unit = freeUnit()) < 0) {
        notify("Printer not set up", "All ten printer units are in use; free one in Printer prefs.",
            s->s_Picture, NULL, 0);
        return;
    }

    if (exists(GUTENPRINTPREFS))
        done = gutenprintSetup(unit, s) == 0;
    if (!done && ps && s->s_P.mp_IPP) {
        done = setupIn("ENV:", unit, s, a4);
        if (done && !setupIn("ENVARC:", unit, s, a4)) {
            /* ENVARC: on a read-only disk: works until a reboot. */
        }
    }

    if (done) {
        strcpy(text, nameOf(s));
        strcat(text, unit == 0 ? " is printer 0, the default printer." : " is set up as printer ");
        if (unit) {
            appendNumber(text, unit);
            strcat(text, ".");
        }
        notify("Printer ready", text, s->s_Picture, NULL, 0);
    } else {
        strcpy(text, nameOf(s));
        strcat(text, ps ? " could not be set up; try Printer prefs."
                        : " takes no PostScript: install Gutenprint from Contrib to print to it.");
        notify("Printer not set up", text, s->s_Picture, NULL, 0);
    }
}

/* ------------------------------------------------------------------ watching */

/* How many starts a printer has been offered on (ENVARC:Sys/PrinterWatch.xml). */
static LONG offers(struct Seen *s)
{
    return DictGetIntegerForKey(DictGetObjectForKey(state, ANNOUNCED_KEY), keyOf(s), 0);
}

/* Offered once each start, on at most MAX_OFFERS starts, so a pop-up missed is
   offered again next time but an unwanted printer stops being offered. */
static BOOL announced(struct Seen *s)
{
    return s->s_Offered || offers(s) >= MAX_OFFERS;
}

static void markAnnounced(struct Seen *s)
{
    PrefsObject *list = DictGetObjectForKey(state, ANNOUNCED_KEY), *count;

    s->s_Offered = TRUE;
    if (!state)
        return;
    if (!list) {
        if (!(list = PrefsDictionary(NULL, NULL, ALPO_Alloc, 0, TAG_DONE)))
            return;
        if (!DictSetObjectForKey(state, list, ANNOUNCED_KEY)) {
            PrefsDictionary(list, NULL, ALPO_Release, 0, TAG_DONE);
            return;
        }
    }
    if ((count = PrefsNumber(NULL, NULL, ALPONUM_AllocSetLong, offers(s) + 1, TAG_DONE))
        && !DictSetObjectForKey(list, count, keyOf(s)))
        PrefsNumber(count, NULL, ALPO_Release, 0, TAG_DONE);
    SetApplicationAttrs(appid, APPATTR_FlushPrefs, 0, TAG_DONE);
}

static void fetchInfo(struct Seen *s, LONG index)
{
    const char *from;
    LONG status;

    if (!s->s_P.mp_IPP)
        return;
    s->s_HaveInfo = IPP_GetInfo(s->s_P.mp_URI, &s->s_Info);
    if (s->s_HaveInfo && !s->s_Picture[0]) {
        char path[64] = PICTURE_DIR "/";
        makeDir(PICTURE_DIR);
        appendNumber(path, index);
        strcat(path, ".png");
        if (IPP_SavePicture(&s->s_Info, path, &from, &status))
            strcpy(s->s_Picture, path);
    }
}

static void announce(struct Seen *s, LONG index)
{
    char text[400], back[16] = "setup:", *host, *end;

    fetchInfo(s, index);
    strcpy(text, nameOf(s));
    /* "at 192.168.0.63" from ipp://192.168.0.63:631/ipp/print */
    if ((host = strstr(s->s_P.mp_URI, "://"))) {
        host += 3;
        strcat(text, " at ");
        end = text + strlen(text);
        while (*host && *host != ':' && *host != '/' && end < text + sizeof(text) - 64)
            *end++ = *host++;
        *end = 0;
    }
    strcat(text, " is on the network. Double-click to set it up.");
    appendNumber(back, index);
    if (notify("Printer found", text, s->s_Picture, back, 0) == APPNOTIFY_OK_MSGQUEUED)
        markAnnounced(s);
}

/* printer-state-reasons worth interrupting the user for, in words. */
static const char *reasonText(const char *reason)
{
    static const struct { const char *key, *text; } words[] = {
        { "media-empty", "is out of paper" },
        { "media-needed", "needs paper" },
        { "media-jam", "has a paper jam" },
        { "toner-empty", "is out of toner" },
        { "toner-low", "is low on toner" },
        { "marker-supply-empty", "is out of ink or toner" },
        { "marker-supply-low", "is low on ink or toner" },
        { "door-open", "has a door open" },
        { "cover-open", "has its cover open" },
        { "output-area-full", "has a full output tray" },
        { "offline", "is offline" },
        { NULL, NULL }
    };
    LONG i;

    for (i = 0; words[i].key; i++)
        if (strncmp(reason, words[i].key, strlen(words[i].key)) == 0)
            return words[i].text;
    return NULL;
}

/* Tells about reasons that were not there at the last look. */
static void checkStatus(struct Seen *s, LONG index)
{
    char reasons[160], one[64], text[300];
    const char *r, *say;
    LONG n;

    fetchInfo(s, index);
    s->s_LastStatus = now;
    if (!s->s_HaveInfo)
        return;
    strcpy(reasons, s->s_Info.pi_Reasons);
    for (r = reasons; *r; r += n + (r[n] == ',')) {
        for (n = 0; r[n] && r[n] != ','; n++)
            ;
        if (n >= (LONG)sizeof(one))
            continue;
        memcpy(one, r, n);
        one[n] = 0;
        if (!(say = reasonText(one)) || contains(s->s_Reasons, one))
            continue;
        strcpy(text, nameOf(s));
        strcat(text, " ");
        strcat(text, say);
        strcat(text, ".");
        notify("Printer", text, s->s_Picture, NULL, contains(one, "empty") || contains(one, "jam") ? 9 : 0);
    }
    strcpy(s->s_Reasons, reasons);
}

static void scan(void)
{
    static struct MDNSPrinter found[MAX_FOUND];
    LONG n, i, j;

    if (!(SocketBase = OpenLibrary("bsdsocket.library", 4)))
        return;
    n = MDNS_FindPrinters(3000, found, MAX_FOUND);
    for (i = 0; i < n; i++) {
        struct Seen *s = NULL;
        for (j = 0; j < nseen; j++)
            if (strcmp(seen[j].s_P.mp_Name, found[i].mp_Name) == 0)
                s = &seen[j];
        if (!s) {
            if (nseen == MAX_FOUND)
                continue;
            s = &seen[j = nseen++];
            memset(s, 0, sizeof(*s));
        }
        s->s_P = found[i];
        if (configuredUnit(s) >= 0) {
            /* during a job, the job's own watch tells about problems */
            if (s->s_P.mp_IPP && (!s->s_LastStatus || now - s->s_LastStatus >= STATUS_EVERY)
                && !(job.j_Active && strcasecmp(job.j_URI, s->s_P.mp_URI) == 0))
                checkStatus(s, j);
        } else if (!announced(s))
            announce(s, j);
    }
    CloseLibrary(SocketBase);
    SocketBase = NULL;
}

/* ------------------------------------------------------------------ print jobs */

static BOOL tell(const char *what)
{
    return DictGetBoolForKey(state, what, TRUE);
}

/* A Key=value-lines variable: GetVar() stops at the first line unless it is
   read as binary. */
static BOOL getLines(const char *var, char *buf, LONG size)
{
    LONG n;

    memset(buf, 0, size);
    n = GetVar(var, buf, size - 1, GVF_GLOBAL_ONLY | GVF_BINARY_VAR);
    return n > 0;
}

static void keyValue(const char *text, const char *key, char *out, LONG max)
{
    LONG kl = strlen(key);
    const char *p = text;

    out[0] = 0;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            LONG n = 0;
            p += kl + 1;
            while (p[n] && p[n] != '\n' && n < max - 1) {
                out[n] = p[n];
                n++;
            }
            out[n] = 0;
            return;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
}

/* The printer's supplies as notification gauges: "<level>;#RRGGBB;<name>" lines. */
static void supplyGauges(struct PrinterInfo *pi, char *out, LONG max)
{
    LONG i, n = 0;

    out[0] = 0;
    for (i = 0; i < pi->pi_NumSupplies && n < APPNOTIFY_GAUGES_MAX; i++) {
        char item[96];
        LONG low = pi->pi_Low[i] >= 0 ? pi->pi_Low[i] : 15;

        if (pi->pi_Level[i] < 0 || contains(pi->pi_Supply[i], "waste"))
            continue;
        item[0] = 0;
        appendNumber(item, pi->pi_Level[i]);
        strcat(item, ";");
        strcat(item, pi->pi_Colour[i][0] == '#' && strlen(pi->pi_Colour[i]) == 7 ? pi->pi_Colour[i] : "#808080");
        strcat(item, ";");
        strncat(item, pi->pi_Supply[i], 40);
        if (pi->pi_Level[i] <= low)
            strcat(item, " (low)");
        if ((LONG)(strlen(out) + strlen(item) + 2) >= max)
            break;
        if (out[0])
            strcat(out, "\n");
        strcat(out, item);
        n++;
    }
}

static const char *documentName(void)
{
    return job.j_Document[0] ? job.j_Document : job.j_Application[0] ? job.j_Application : "Your document";
}

/* The printer of a job going to ipp.device unit n: its address, name and picture. */
static void jobPrinter(void)
{
    char v[300];

    job.j_URI[0] = job.j_Picture[0] = 0;
    strcpy(job.j_Printer, "the printer");
    if (strcasecmp(job.j_Device, "ipp.device") != 0)
        return;
    if (unitValue(job.j_DevUnit, "URI", v, sizeof(v)) && strncmp(v, "ipp://", 6) == 0)
        strcpy(job.j_URI, v);
    if (unitValue(job.j_DevUnit, "NAME", v, sizeof(v)) && v[0])
        strncpy(job.j_Printer, v, sizeof(job.j_Printer) - 1);
    if (unitValue(job.j_DevUnit, "PICTURE", v, sizeof(v)) && exists(v) && strlen(v) < sizeof(job.j_Picture))
        strcpy(job.j_Picture, v);
}

/* Paper, jams, supplies and doors, while a job is under way: each problem once a job. */
static void jobProblems(void)
{
    struct PrinterInfo pi;
    char one[64], text[300];
    const char *r, *say;
    LONG n;

    if (!job.j_URI[0] || !IPP_GetInfo(job.j_URI, &pi))
        return;
    for (r = pi.pi_Reasons; *r; r += n + (r[n] == ',')) {
        for (n = 0; r[n] && r[n] != ','; n++)
            ;
        if (n >= (LONG)sizeof(one))
            continue;
        memcpy(one, r, n);
        one[n] = 0;
        if (!(say = reasonText(one)) || contains(job.j_Told, one))
            continue;
        if (strlen(job.j_Told) + n + 2 < sizeof(job.j_Told)) {
            strcat(job.j_Told, one);
            strcat(job.j_Told, ",");
        }
        if (!tell("TellProblems"))
            continue;
        strcpy(text, job.j_Printer);
        strcat(text, " ");
        strcat(text, say);
        strcat(text, contains(one, "low") ? "." : ": printing waits until it is seen to.");
        notify("Printer", text, job.j_Picture, NULL, contains(one, "low") ? 0 : 9);
    }
}

/* "Printing", with the printer's ink or toner levels as bars. */
static void jobStarted(void)
{
    struct PrinterInfo pi;
    char text[300], gauges[512] = "";

    if (!tell("TellJobStart"))
        return;
    strcpy(text, "Printing ");
    strncat(text, documentName(), 60);
    strcat(text, " on ");
    strncat(text, job.j_Printer, 60);
    strcat(text, ".");
    if (job.j_URI[0] && IPP_GetInfo(job.j_URI, &pi))
        supplyGauges(&pi, gauges, sizeof(gauges));
    Notify(appid,
        APPNOTIFY_Title, (IPTR)"Printing",
        APPNOTIFY_Text, (IPTR)text,
        job.j_Picture[0] ? APPNOTIFY_ImageFile : TAG_IGNORE, (IPTR)job.j_Picture,
        gauges[0] ? APPNOTIFY_Gauges : TAG_IGNORE, (IPTR)gauges,
        TAG_DONE);
}

static void jobFinished(LONG how, const char *reasons)
{
    char text[300];

    job.j_Active = FALSE;
    if (!tell("TellJobDone"))
        return;
    strncpy(text, documentName(), 80);
    text[80] = 0;
    if (how == IPP_JOB_COMPLETED || how == IPP_JOB_GONE)
        strcat(text, " is printed.");
    else if (how == -2) {
        strcat(text, " went to ");
        strncat(text, job.j_Printer, 60);
        strcat(text, ".");
    } else {
        strcat(text, how == IPP_JOB_CANCELED ? " was cancelled at the printer." : " was not printed");
        if (how != IPP_JOB_CANCELED) {
            if (reasons && reasons[0] && strcmp(reasons, "none") != 0) {
                strcat(text, " (");
                strncat(text, reasons, 80);
                strcat(text, ")");
            }
            strcat(text, ".");
        }
    }
    notify(how == IPP_JOB_COMPLETED || how == IPP_JOB_GONE || how == -2 ? "Printed" : "Not printed",
        text, job.j_Picture, NULL, how == IPP_JOB_ABORTED ? 9 : 0);
}

/* PrintSpool/Job changed: the spooler started, stalled, finished or cancelled a job. */
static void jobChanged(void)
{
    char text[600], name[64], st[16], reason[32];
    BOOL fresh;

    if (!getLines(JOBVAR, text, sizeof(text)))
        return;
    keyValue(text, "Job", name, sizeof(name));
    keyValue(text, "State", st, sizeof(st));
    keyValue(text, "Reason", reason, sizeof(reason));
    if (!name[0])
        return;
    fresh = strcmp(name, job.j_Job) != 0;
    if (!fresh && strcmp(st, job.j_State) == 0 && strcmp(reason, job.j_Reason) == 0)
        return;
    if (fresh) {
        char unit[12];
        memset(&job, 0, sizeof(job));
        strcpy(job.j_Job, name);
        keyValue(text, "Device", job.j_Device, sizeof(job.j_Device));
        keyValue(text, "DeviceUnit", unit, sizeof(unit));
        job.j_DevUnit = unit[0] - '0';
        keyValue(text, "Document", job.j_Document, sizeof(job.j_Document));
        keyValue(text, "Application", job.j_Application, sizeof(job.j_Application));
        jobPrinter();
    }
    strcpy(job.j_State, st);
    strcpy(job.j_Reason, reason);

    if (strcmp(st, "printing") == 0) {
        if (!job.j_Started) {
            job.j_Started = TRUE;
            jobStarted();
        }
        job.j_Active = TRUE;
        job.j_NextPoll = now + JOB_POLL;
    } else if (strcmp(st, "waiting") == 0) {
        job.j_Active = TRUE;
        job.j_NextPoll = now;
        if (strcmp(reason, "unreachable") == 0 && tell("TellProblems")) {
            strcpy(text, job.j_Printer);
            strcat(text, " does not answer; ");
            strncat(text, documentName(), 60);
            strcat(text, " waits until it does.");
            notify("Printer", text, job.j_Picture, NULL, 0);
        }
    } else if (strcmp(st, "cancelled") == 0) {
        job.j_Active = FALSE;
        if (tell("TellJobDone")) {
            strncpy(text, documentName(), 80);
            text[80] = 0;
            strcat(text, " was cancelled.");
            notify("Printing cancelled", text, job.j_Picture, NULL, 0);
        }
    } else if (strcmp(st, "done") == 0) {
        char var[24] = "IPP/Unit", jt[300], id[16];
        if (job.j_URI[0]) {
            appendNumber(var, job.j_DevUnit);
            strcat(var, ".Job");
            if (getLines(var, jt, sizeof(jt))) {
                const char *c;
                keyValue(jt, "JobID", id, sizeof(id));
                for (c = id; *c >= '0' && *c <= '9'; c++)
                    job.j_JobID = job.j_JobID * 10 + (*c - '0');
            }
        }
        if (job.j_JobID) {
            /* Sent: follow it at the printer until it is printed. */
            job.j_Active = TRUE;
            job.j_NextPoll = now + 2;
            job.j_Until = now + JOB_FOLLOW;
        } else
            jobFinished(-2, NULL);
    }
}

/* Each second while a job is under way. */
static void jobTick(void)
{
    char reasons[160];
    LONG st;

    if (!job.j_Active || now < job.j_NextPoll)
        return;
    job.j_NextPoll = now + JOB_POLL;
    if (!(SocketBase = OpenLibrary("bsdsocket.library", 4)))
        return;
    jobProblems();
    if (job.j_JobID) {
        st = IPP_GetJobState(job.j_URI, job.j_JobID, reasons, sizeof(reasons));
        if (st >= IPP_JOB_CANCELED)
            jobFinished(st, reasons);
        else if (now >= job.j_Until)
            job.j_Active = FALSE;
    }
    CloseLibrary(SocketBase);
    SocketBase = NULL;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    struct MsgPort *appport = NULL, *timerport = NULL;
    struct timerequest *tr = NULL;
    struct NotifyRequest nr;
    BYTE jobsig = -1;
    BOOL notifying = FALSE;
    ULONG nextscan = FIRST_SCAN;
    BOOL run = TRUE;
    int rc = RETURN_FAIL;

    if (!(ApplicationBase = OpenLibrary("application.library", 1)))
        return RETURN_FAIL;
    appid = RegisterApplication("PrinterWatch",
        REGAPP_URLIdentifier, (IPTR)"aros.org",
        REGAPP_Description, (IPTR)"Finds network printers and sets them up",
        REGAPP_UniqueApplication, TRUE,
        REGAPP_NoIcon, TRUE,
        REGAPP_CustomPrefsFileName, (IPTR)"Sys/PrinterWatch.xml",
        REGAPP_SavePrefs, TRUE,
        TAG_DONE);
    if (!appid)
        goto out;
    GetApplicationAttrs(appid, APPATTR_Port, (IPTR)&appport, APPATTR_MainPrefsDict, (IPTR)&state, TAG_DONE);

    if (!(timerport = CreateMsgPort())
        || !(tr = (struct timerequest *)CreateIORequest(timerport, sizeof(*tr)))
        || OpenDevice(TIMERNAME, UNIT_VBLANK, &tr->tr_node, 0)) {
        if (tr)
            DeleteIORequest(&tr->tr_node);
        tr = NULL;
        goto out;
    }

    /* printspool.device rewrites ENV:PrintSpool/Job at each change of a job. */
    makeDir("ENV:PrintSpool");
    if ((jobsig = AllocSignal(-1)) >= 0) {
        memset(&nr, 0, sizeof(nr));
        nr.nr_Name = "ENV:" JOBVAR;
        nr.nr_Flags = NRF_SEND_SIGNAL;
        nr.nr_stuff.nr_Signal.nr_Task = FindTask(NULL);
        nr.nr_stuff.nr_Signal.nr_SignalNum = jobsig;
        notifying = StartNotify(&nr);
    }

    rc = RETURN_OK;
    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = 1;
    tr->tr_time.tv_micro = 0;
    SendIO(&tr->tr_node);

    while (run) {
        ULONG sigs = Wait((1UL << timerport->mp_SigBit) | (1UL << appport->mp_SigBit) | SIGBREAKF_CTRL_C
                          | (notifying ? 1UL << jobsig : 0));
        struct ApplicationMsg *am;

        if (sigs & SIGBREAKF_CTRL_C)
            run = FALSE;
        if (notifying && (sigs & (1UL << jobsig))) {
            /* a job start asks the printer for its supplies */
            if ((SocketBase = OpenLibrary("bsdsocket.library", 4))) {
                jobChanged();
                CloseLibrary(SocketBase);
                SocketBase = NULL;
            } else
                jobChanged();
        }
        while ((am = (struct ApplicationMsg *)GetMsg(appport))) {
            if (am->type == APPLIBMT_Quit || am->type == APPLIBMT_ForceQuit)
                run = FALSE;
            else if (am->type == APPLIBMT_CustomMsg) {
                const char *m = ((struct ApplicationCustomMsg *)am)->customMsg;
                LONG i = 0;
                if (m && strncmp(m, "setup:", 6) == 0) {
                    for (m += 6; *m >= '0' && *m <= '9'; m++)
                        i = i * 10 + (*m - '0');
                    if (i < nseen) {
                        ReplyMsg(&am->msg);
                        am = NULL;
                        if ((SocketBase = OpenLibrary("bsdsocket.library", 4))) {
                            setUp(&seen[i]);
                            CloseLibrary(SocketBase);
                            SocketBase = NULL;
                        }
                    }
                }
            }
            if (am)
                ReplyMsg(&am->msg);
        }
        if (CheckIO(&tr->tr_node)) {
            WaitIO(&tr->tr_node);
            now++;
            jobTick();
            if (now >= nextscan) {
                scan();
                nextscan = now + (now < 600 ? FAST_SCAN : SLOW_SCAN);
            }
            tr->tr_node.io_Command = TR_ADDREQUEST;
            tr->tr_time.tv_secs = 1;
            tr->tr_time.tv_micro = 0;
            SendIO(&tr->tr_node);
        }
    }

out:
    if (notifying)
        EndNotify(&nr);
    if (jobsig >= 0)
        FreeSignal(jobsig);
    if (tr) {
        AbortIO(&tr->tr_node);
        WaitIO(&tr->tr_node);
        CloseDevice(&tr->tr_node);
        DeleteIORequest(&tr->tr_node);
    }
    if (timerport)
        DeleteMsgPort(timerport);
    if (appid)
        UnregisterApplication(appid, NULL);
    CloseLibrary(ApplicationBase);
    return rc;
}
