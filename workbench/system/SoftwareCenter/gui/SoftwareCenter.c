/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: SoftwareCenter — Zune package manager / updater front-end for
          libpkg (DESIGN.md §1.2).  Register tabs Updates / Installed /
          Available / News, NList package lists, details pane with
          screenshot thumbnail + large view, Install / Remove / Update All /
          Refresh with a live Gauge, Prefs window.

          Every libpkg call that may block (network, install, screenshot
          download) runs on a MUIC_Process worker owned by the main group
          class; results come back through MUIM_Application_PushMethod, the
          same shape as system/find.  The GUI task never touches the
          PkgContext while the worker is busy (buttons are disabled), so no
          locking is needed around libpkg.

    AI_PHASE: SOFTWARE_CENTER_GUI
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: HIGH
    AI_NOTE: Resolution independent by construction: no pixel geometry,
             PictureView boxes are percentages of the screen, fonts come
             from the Zune prefs.  Strings pushed across tasks are StrDup'ed
             by the worker and FreeVec'ed by the receiver.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/muimaster.h>
#include <proto/alib.h>

#include <exec/memory.h>
#include <dos/dostags.h>
#include <libraries/mui.h>
#include <libraries/asl.h>
#include <mui/NList_mcc.h>
#include <mui/NListview_mcc.h>
#include <zune/customclasses.h>

#include <stdio.h>
#include <string.h>

#include "pkg.h"
#include "pictureview.h"

/* SC_DEBUG: construction traces, one file per line in RAM:sc-debug-NN.log
 * (Type RAM:sc-debug-#? from the shell; a crash keeps the files intact). */
#ifdef SC_DEBUG
static void scdbg(const char *fmt, ...)
{
    static ULONG seq;
    TEXT name[40];
    BPTR fh;
    snprintf(name, sizeof(name), "RAM:sc-debug-%02lu.log", (unsigned long)seq++);
    fh = Open(name, MODE_NEWFILE);
    if (!fh) return;
    AROS_SLOWSTACKFORMAT_PRE(fmt);
    VFPrintf(fh, fmt, AROS_SLOWSTACKFORMAT_ARG(fmt));   /* %s/%ld/%ix: %l is 32-bit, %i is IPTR */
    AROS_SLOWSTACKFORMAT_POST(fmt);
    Close(fh);
}
#define SCDBG(...) scdbg(__VA_ARGS__)
#else
#define SCDBG(...)
#endif

#define SC_VERSTR "1.0"
static const char version[] __attribute__((used)) = "$VER: SoftwareCenter " SC_VERSTR " (06.09.2026)";

/* ------------------------------------------------------------------------ */
/* Tabs, jobs, methods                                                       */

enum { TAB_UPDATES, TAB_INSTALLED, TAB_AVAILABLE, TAB_NEWS, NTABS };
#define NPKGTABS TAB_NEWS                      /* tabs holding PkgRecords */

enum
{
    JOB_RELOAD,       /* rebuild the lists from cached state (no network)   */
    JOB_REFRESH,      /* pkg_refresh(force) then reload                     */
    JOB_INSTALL,      /* name                                               */
    JOB_REMOVE,       /* name                                               */
    JOB_UPGRADE,      /* name                                               */
    JOB_UPGRADEALL,
    JOB_SCREENSHOT,   /* name, index                                        */
    JOB_SAVEPREFS,    /* prefs + first source + optional trusted key file   */
    JOB_ANNSEEN       /* announcement id                                    */
};

struct Job
{
    struct MinNode node;
    ULONG  kind;
    TEXT   name[64];
    ULONG  index;
    /* JOB_SAVEPREFS */
    TEXT   url[PKG_URL_LEN];
    TEXT   keyid[40];
    TEXT   keyfile[256];
    ULONG  interval;
    BOOL   notify, notifyann;
};

/* Everything the worker hands back in one go after a reload */
struct ListSet
{
    struct List pkgs[NPKGTABS];    /* PkgRecord, per tab                    */
    struct List news;              /* PkgAnnouncement                       */
};

#define MUIB_SC              (TAG_USER | 0x5C710000)
#define MUIM_SC_Action       (MUIB_SC | 0x01)  /* kind (uses selection)   */
#define MUIM_SC_Lists        (MUIB_SC | 0x02)  /* pushed: ListSet *       */
#define MUIM_SC_Log          (MUIB_SC | 0x03)  /* pushed: STRPTR          */
#define MUIM_SC_Progress     (MUIB_SC | 0x04)  /* pushed: STRPTR, pct     */
#define MUIM_SC_Done         (MUIB_SC | 0x05)  /* pushed: kind, rc, STRPTR*/
#define MUIM_SC_Shot         (MUIB_SC | 0x06)  /* pushed: name, idx, path */
#define MUIM_SC_Select       (MUIB_SC | 0x07)
#define MUIM_SC_Cancel       (MUIB_SC | 0x08)
#define MUIM_SC_OpenLarge    (MUIB_SC | 0x09)
#define MUIM_SC_LargeStep    (MUIB_SC | 0x0A)  /* delta                   */
#define MUIM_SC_OpenPrefs    (MUIB_SC | 0x0B)
#define MUIM_SC_SavePrefs    (MUIB_SC | 0x0C)
#define MUIM_SC_DoubleClick  (MUIB_SC | 0x0D)
#define MUIM_SC_Raise        (MUIB_SC | 0x0E)  /* second start: to front  */

struct MUIP_SC_Action    { STACKED ULONG MethodID; STACKED ULONG kind; };
struct MUIP_SC_Lists     { STACKED ULONG MethodID; STACKED struct ListSet *set; };
struct MUIP_SC_Log       { STACKED ULONG MethodID; STACKED STRPTR text; };
struct MUIP_SC_Progress  { STACKED ULONG MethodID; STACKED STRPTR text; STACKED LONG pct; };
struct MUIP_SC_Done      { STACKED ULONG MethodID; STACKED ULONG kind; STACKED LONG rc; STACKED STRPTR detail; };
struct MUIP_SC_Shot      { STACKED ULONG MethodID; STACKED STRPTR name; STACKED ULONG index; STACKED STRPTR path; };
struct MUIP_SC_LargeStep { STACKED ULONG MethodID; STACKED LONG delta; };

/* ------------------------------------------------------------------------ */

struct ShotCache
{
    struct MinNode node;
    TEXT   name[64];
    ULONG  index;
    STRPTR path;                   /* NULL = fetch failed                   */
};

struct SCMain_DATA
{
    struct PkgContext *ctx;
    struct Hook        pkghook;
    Object            *proc;                 /* MUIC_Process worker         */
    struct SignalSemaphore qlock;
    struct MinList     queue;                /* Job                         */
    volatile ULONG     abort;
    ULONG              busy;                 /* GUI side: jobs outstanding  */
    Object            *self;

    /* widgets */
    Object *reg, *lists[NTABS], *txt_status;
    Object *txt_title, *txt_meta, *flt_desc, *pic;
    Object *btn_install, *btn_remove, *btn_updateall, *btn_refresh, *btn_prefs, *btn_cancel;
    Object *gauge, *lst_log;
    Object *win_large, *pic_large, *txt_largecount, *btn_prev, *btn_next;
    Object *win_prefs, *str_url, *str_keyid, *str_interval, *chk_notify, *chk_notifyann, *str_keyfile;

    struct ListSet    *set;                  /* owned by the GUI task       */
    struct MinList     shots;                /* ShotCache                   */
    STRPTR             desctext;             /* Floattext buffer            */
    TEXT               metabuf[256], titlebuf[160];

    /* selection */
    TEXT   selname[64];
    ULONG  selstate, selshots;
    BOOL   selpkg;                           /* a package (not news) row    */
    ULONG  largeindex;

    struct Hook pkgdisplay_hook, newsdisplay_hook;
};

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */

static ULONG count_tokens(CONST_STRPTR s)
{
    ULONG n = 0;
    if (!s) return 0;
    while (*s)
    {
        while (*s == ' ') s++;
        if (!*s) break;
        n++;
        while (*s && *s != ' ') s++;
    }
    return n;
}

static void fmt_size(UQUAD bytes, STRPTR out, ULONG outlen)
{
    if (bytes == 0)                out[0] = 0;
    else if (bytes < 1024)         snprintf(out, outlen, "%lu B", (unsigned long)bytes);
    else if (bytes < 1024 * 1024)  snprintf(out, outlen, "%lu KB", (unsigned long)(bytes / 1024));
    else                           snprintf(out, outlen, "%lu.%lu MB",
                                            (unsigned long)(bytes / (1024 * 1024)),
                                            (unsigned long)((bytes % (1024 * 1024)) * 10 / (1024 * 1024)));
}

static STRPTR dupstr(CONST_STRPTR s)
{
    return s ? StrDup(s) : NULL;
}

static CONST_STRPTR state_word(ULONG state)
{
    switch (state)
    {
    case PKGSTATE_INSTALLED:  return "installed";
    case PKGSTATE_UPGRADABLE: return "update available";
    case PKGSTATE_ORPHAN:     return "installed (no longer in repository)";
    default:                  return "available";
    }
}

static CONST_STRPTR type_word(ULONG type)
{
    switch (type)
    {
    case PKGTYPE_PLUGIN: return "plugin";
    case PKGTYPE_MODEL:  return "model";
    case PKGTYPE_ADDON:  return "add-on";
    default:             return "package";
    }
}

/* ------------------------------------------------------------------------ */
/* NList display hooks (title row when entry == NULL)                        */

AROS_UFH3S(LONG, pkgdisplay_func,
    AROS_UFHA(struct Hook *, h, A0),
    AROS_UFHA(char **, array, A2),
    AROS_UFHA(struct PkgRecord *, r, A1))
{
    AROS_USERFUNC_INIT

    static TEXT sizebuf[24];

    if (r)
    {
        fmt_size(r->Size, sizebuf, sizeof sizebuf);
        *array++ = (char *)PKG_STR(r->Name, "");
        *array++ = (char *)PKG_STR(r->InstalledVersion, "-");
        *array++ = (char *)(r->State == PKGSTATE_ORPHAN ? "-" : PKG_STR(r->Version, "-"));
        *array++ = (char *)sizebuf;
        *array++ = (char *)type_word(r->Type);
        *array++ = (char *)PKG_STR(r->Summary, "");
    }
    else
    {
        *array++ = "\33bName";
        *array++ = "\33bInstalled";
        *array++ = "\33bAvailable";
        *array++ = "\33bSize";
        *array++ = "\33bType";
        *array++ = "\33bSummary";
    }
    return 0;

    AROS_USERFUNC_EXIT
}

AROS_UFH3S(LONG, newsdisplay_func,
    AROS_UFHA(struct Hook *, h, A0),
    AROS_UFHA(char **, array, A2),
    AROS_UFHA(struct PkgAnnouncement *, a, A1))
{
    AROS_USERFUNC_INIT

    static TEXT tbuf[160];

    if (a)
    {
        /* unseen announcements are shown bold */
        snprintf(tbuf, sizeof tbuf, "%s%s", a->Seen ? "" : "\33b", PKG_STR(a->Title, ""));
        *array++ = (char *)PKG_STR(a->Date, "");
        *array++ = (char *)PKG_STR(a->Severity, "info");
        *array++ = (char *)tbuf;
        *array++ = (char *)PKG_STR(a->Package, "");
    }
    else
    {
        *array++ = "\33bDate";
        *array++ = "\33bSeverity";
        *array++ = "\33bTitle";
        *array++ = "\33bPackage";
    }
    return 0;

    AROS_USERFUNC_EXIT
}

/* ------------------------------------------------------------------------ */
/* Worker side                                                               */

static void push_log(Object *self, CONST_STRPTR text)
{
    DoMethod(_app(self), MUIM_Application_PushMethod, (IPTR)self, 2, MUIM_SC_Log, (IPTR)dupstr(text));
}

static void push_progress(Object *self, CONST_STRPTR text, LONG pct)
{
    DoMethod(_app(self), MUIM_Application_PushMethod, (IPTR)self, 3, MUIM_SC_Progress, (IPTR)dupstr(text), pct);
}

/* libpkg hook: runs on the worker */
AROS_UFH3S(IPTR, pkghook_func,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(APTR, ctx, A2),
    AROS_UFHA(struct PkgHookMsg *, msg, A1))
{
    AROS_USERFUNC_INIT

    struct SCMain_DATA *data = (struct SCMain_DATA *)hook->h_Data;
    Object *self = data->self;
    static TEXT last[128];
    static LONG lastpct = -2;
    TEXT buf[192];
    LONG pct;

    switch (msg->Type)
    {
    case PKGH_LOG:
        push_log(self, msg->Text);
        break;

    case PKGH_PROGRESS:
        if (msg->BytesTotal)      pct = (LONG)(msg->BytesDone * 100 / msg->BytesTotal);
        else if (msg->Total)      pct = (LONG)(msg->Done * 100 / msg->Total);
        else                      pct = -1;
        snprintf(buf, sizeof buf, "%s%s%s", PKG_STR(msg->Text, ""),
                 msg->Package ? " " : "", PKG_STR(msg->Package, ""));
        if (pct != lastpct || strcmp(buf, last) != 0)
        {
            strlcpy(last, buf, sizeof last);
            lastpct = pct;
            push_progress(self, buf, pct);
        }
        break;

    case PKGH_ABORTQUERY:
        return data->abort;
    }
    return 0;

    AROS_USERFUNC_EXIT
}

static struct ListSet *build_listset(struct PkgContext *ctx)
{
    struct ListSet *set = AllocVec(sizeof *set, MEMF_CLEAR);
    static const ULONG filters[NPKGTABS] = { PKGF_UPDATES, PKGF_INSTALLED, PKGF_AVAILABLE };
    ULONG i;

    if (!set) return NULL;
    for (i = 0; i < NPKGTABS; i++)
    {
        NewList(&set->pkgs[i]);
        pkg_list(ctx, filters[i], &set->pkgs[i]);
    }
    NewList(&set->news);
    pkg_announcements(ctx, FALSE, &set->news);
    return set;
}

static void free_listset(struct ListSet *set)
{
    ULONG i;
    if (!set) return;
    for (i = 0; i < NPKGTABS; i++) pkg_free_records(&set->pkgs[i]);
    pkg_free_announcements(&set->news);
    FreeVec(set);
}

static void worker_run(Object *self, struct SCMain_DATA *data, struct Job *job)
{
    struct PkgContext *ctx = data->ctx;
    LONG rc = 0;
    BOOL reload = FALSE;
    STRPTR detail = NULL;

    data->abort = 0;
    switch (job->kind)
    {
    case JOB_RELOAD:
        pkg_reload(ctx);               /* another process may have installed */
        reload = TRUE;
        break;

    case JOB_REFRESH:
        push_progress(self, "Refreshing repositories", -1);
        rc = pkg_refresh(ctx, TRUE);
        reload = TRUE;
        break;

    case JOB_INSTALL:
        rc = pkg_install(ctx, job->name);
        reload = TRUE;
        break;

    case JOB_REMOVE:
        rc = pkg_remove(ctx, job->name);
        reload = TRUE;
        break;

    case JOB_UPGRADE:
        rc = pkg_upgrade(ctx, job->name);
        reload = TRUE;
        break;

    case JOB_UPGRADEALL:
        rc = pkg_upgrade(ctx, NULL);
        reload = TRUE;
        break;

    case JOB_SCREENSHOT:
    {
        struct PkgRecord *rec = pkg_find(ctx, job->name);
        TEXT path[512];
        STRPTR result = NULL;
        if (rec)
        {
            if (pkg_screenshot(ctx, rec, job->index, path, sizeof path) == 0)
                result = dupstr(path);
            pkg_free_record(rec);
        }
        DoMethod(_app(self), MUIM_Application_PushMethod, (IPTR)self, 4, MUIM_SC_Shot,
                 (IPTR)dupstr(job->name), job->index, (IPTR)result);
        return;                                   /* no Done for shots     */
    }

    case JOB_SAVEPREFS:
    {
        struct PkgPrefs *p = pkg_prefs(ctx);
        struct List *srcs = pkg_sources(ctx);
        struct PkgSource *s = (struct PkgSource *)GetHead(srcs);

        p->CheckInterval = job->interval;
        p->Notify = job->notify;
        p->NotifyAnnouncements = job->notifyann;
        rc = pkg_prefs_save(ctx);
        if (!rc && job->url[0])
        {
            if (s)
            {
                strlcpy(s->URL, job->url, sizeof s->URL);
                if (s->URL[strlen(s->URL) - 1] != '/') strlcat(s->URL, "/", sizeof s->URL);
                strlcpy(s->KeyId, job->keyid, sizeof s->KeyId);
                s->Enabled = TRUE;
                rc = pkg_sources_save(ctx);
            }
            else
                rc = pkg_source_add(ctx, "main", job->url, job->keyid[0] ? job->keyid : NULL);
        }
        if (!rc && job->keyfile[0])
        {
            rc = pkg_trust_add(ctx, job->keyfile);
            if (!rc) push_log(self, "Trusted key added");
        }
        if (!rc) push_log(self, "Preferences saved");
        break;
    }

    case JOB_ANNSEEN:
        pkg_announcement_seen(ctx, job->index);
        break;
    }

    if (rc)
    {
        CONST_STRPTR d = pkg_last_error_text(ctx);
        TEXT buf[320];
        if (rc == PKGERR_HTTP)
            snprintf(buf, sizeof buf, "%s (HTTP %ld)", pkg_strerror(rc), (long)pkg_last_http_status(ctx));
        else
            snprintf(buf, sizeof buf, "%s%s%s", pkg_strerror(rc), d && *d ? ": " : "", PKG_STR(d, ""));
        detail = dupstr(buf);
    }
    if (reload)
    {
        struct ListSet *set = build_listset(ctx);
        DoMethod(_app(self), MUIM_Application_PushMethod, (IPTR)self, 2, MUIM_SC_Lists, (IPTR)set);
    }
    DoMethod(_app(self), MUIM_Application_PushMethod, (IPTR)self, 4, MUIM_SC_Done,
             job->kind, rc, (IPTR)detail);
}

/* MUIM_Process_Process — the worker loop, runs on the sub process */
static IPTR SCMain__MUIM_Process_Process(struct IClass *cl, Object *self, struct MUIP_Process_Process *msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    struct Process *me = (struct Process *)FindTask(NULL);

    me->pr_WindowPtr = (APTR)-1;                  /* no DOS requesters      */

    while (!*msg->kill)
    {
        struct Job *job;
        ULONG sigs;

        for (;;)
        {
            ObtainSemaphore(&data->qlock);
            job = (struct Job *)RemHead((struct List *)&data->queue);
            ReleaseSemaphore(&data->qlock);
            if (!job) break;
            worker_run(self, data, job);
            FreeVec(job);
        }
        if (*msg->kill) break;
        sigs = Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E);
        if (sigs & SIGBREAKF_CTRL_C) break;
    }
    return 0;
}

/* GUI side: queue a job and poke the worker */
static void enqueue(Object *self, struct SCMain_DATA *data, struct Job *job)
{
    if (!job) return;
    ObtainSemaphore(&data->qlock);
    AddTail((struct List *)&data->queue, (struct Node *)job);
    ReleaseSemaphore(&data->qlock);
    data->busy++;
    DoMethod(data->proc, MUIM_Process_Signal, SIGBREAKF_CTRL_E);
}

static struct Job *newjob(ULONG kind, CONST_STRPTR name, ULONG index)
{
    struct Job *job = AllocVec(sizeof *job, MEMF_CLEAR);
    if (!job) return NULL;
    job->kind = kind;
    if (name) strlcpy(job->name, name, sizeof job->name);
    job->index = index;
    return job;
}

/* ------------------------------------------------------------------------ */
/* GUI side helpers                                                          */

static void set_busy(struct SCMain_DATA *data, BOOL busy)
{
    BOOL sel = data->selpkg && data->selname[0];
    set(data->btn_refresh,   MUIA_Disabled, busy);
    set(data->btn_updateall, MUIA_Disabled, busy || !data->set || IsListEmpty(&data->set->pkgs[TAB_UPDATES]));
    set(data->btn_prefs,     MUIA_Disabled, busy);
    set(data->btn_cancel,    MUIA_Disabled, !busy);
    set(data->btn_install,   MUIA_Disabled, busy || !sel ||
        !(data->selstate == PKGSTATE_AVAILABLE || data->selstate == PKGSTATE_UPGRADABLE));
    set(data->btn_remove,    MUIA_Disabled, busy || !sel || data->selstate == PKGSTATE_AVAILABLE);
    set(data->btn_install, MUIA_Text_Contents,
        (IPTR)(data->selstate == PKGSTATE_UPGRADABLE ? "Update" : "Install"));
    if (!busy)
    {
        set(data->gauge, MUIA_Gauge_Current, 0);
        set(data->gauge, MUIA_Gauge_InfoText, (IPTR)"Ready");
    }
}

static void log_line(struct SCMain_DATA *data, CONST_STRPTR text)
{
    DoMethod(data->lst_log, MUIM_NList_InsertSingle, (IPTR)text, MUIV_NList_Insert_Bottom);
    DoMethod(data->lst_log, MUIM_NList_Jump, MUIV_NList_Jump_Bottom);
}

static struct ShotCache *shot_lookup(struct SCMain_DATA *data, CONST_STRPTR name, ULONG index)
{
    struct ShotCache *c;
    ForeachNode(&data->shots, c)
        if (c->index == index && Stricmp(c->name, name) == 0) return c;
    return NULL;
}

static void shots_flush(struct SCMain_DATA *data)
{
    struct ShotCache *c;
    while ((c = (struct ShotCache *)RemHead((struct List *)&data->shots)))
    {
        if (c->path) FreeVec(c->path);
        FreeVec(c);
    }
}

/* Show screenshot idx of the selected package in `pic` (thumbnail or
   large): cached → immediate, else placeholder + worker fetch. */
static void show_shot(Object *self, struct SCMain_DATA *data, Object *pic, ULONG idx)
{
    struct ShotCache *c;

    if (!data->selpkg || !data->selname[0] || idx >= data->selshots)
    {
        set(pic, MUIA_PictureView_Placeholder, (IPTR)"No screenshot");
        set(pic, MUIA_PictureView_File, 0);
        return;
    }
    c = shot_lookup(data, data->selname, idx);
    if (c)
    {
        set(pic, MUIA_PictureView_Placeholder, (IPTR)(c->path ? "" : "Screenshot unavailable"));
        set(pic, MUIA_PictureView_File, (IPTR)c->path);
        return;
    }
    set(pic, MUIA_PictureView_Placeholder, (IPTR)"Loading...");
    set(pic, MUIA_PictureView_File, 0);
    enqueue(self, data, newjob(JOB_SCREENSHOT, data->selname, idx));
}

static struct PkgRecord *active_record(struct SCMain_DATA *data, ULONG tab)
{
    struct PkgRecord *r = NULL;
    if (tab >= NPKGTABS) return NULL;
    DoMethod(data->lists[tab], MUIM_NList_GetEntry, MUIV_NList_GetEntry_Active, (IPTR)&r);
    return r;
}

static void append(STRPTR *buf, ULONG *len, ULONG *cap, CONST_STRPTR s)
{
    ULONG l = strlen((const char *)s);
    if (*len + l + 1 > *cap)
    {
        ULONG ncap = (*len + l + 1) * 2;
        STRPTR n = AllocVec(ncap, MEMF_ANY);
        if (!n) return;
        if (*buf) { memcpy(n, *buf, *len); FreeVec(*buf); }
        *buf = n;
        *cap = ncap;
    }
    memcpy(*buf + *len, s, l + 1);
    *len += l;
}

/* Reflow a multi-line stanza value (PROTOCOL.md §2) for Floattext: the
   index keeps the publisher's hard wraps, so continuation lines are joined
   with a space, an empty line (" .") is a paragraph break and a line that
   starts with a space is verbatim (preformatted).  Floattext then wraps
   paragraphs to the actual pane width. */
static void append_n(STRPTR *buf, ULONG *len, ULONG *cap, CONST_STRPTR s, ULONG l)
{
    TEXT seg[256];
    while (l)
    {
        ULONG n = l < sizeof seg - 1 ? l : sizeof seg - 1;
        memcpy(seg, s, n);
        seg[n] = 0;
        append(buf, len, cap, seg);
        s += n;
        l -= n;
    }
}

static void append_reflowed(STRPTR *buf, ULONG *len, ULONG *cap, CONST_STRPTR s)
{
    BOOL pending = FALSE;                  /* previous line wants a joiner */

    while (*s)
    {
        CONST_STRPTR nl = (CONST_STRPTR)strchr((const char *)s, '\n');
        ULONG l = nl ? (ULONG)(nl - s) : strlen((const char *)s);

        if (l == 0)                        /* " ." paragraph break         */
        {
            append(buf, len, cap, "\n\n");
            pending = FALSE;
        }
        else if (*s == ' ')                /* verbatim line                */
        {
            if (pending) append(buf, len, cap, "\n");
            append_n(buf, len, cap, s, l);
            append(buf, len, cap, "\n");
            pending = FALSE;
        }
        else
        {
            if (pending) append(buf, len, cap, " ");
            append_n(buf, len, cap, s, l);
            pending = TRUE;
        }
        if (!nl) break;
        s = nl + 1;
    }
    if (pending) append(buf, len, cap, "\n");
}

static void show_package(Object *self, struct SCMain_DATA *data, struct PkgRecord *r)
{
    STRPTR text = NULL;
    ULONG len = 0, cap = 0;
    TEXT line[320], size[24];

    strlcpy(data->selname, r->Name, sizeof data->selname);
    data->selstate = r->State;
    data->selshots = count_tokens(r->Screenshots);
    data->selpkg = TRUE;
    data->largeindex = 0;

    snprintf(data->titlebuf, sizeof data->titlebuf, "\33b%s\33n  %s", PKG_STR(r->Name, ""),
             PKG_STR(r->State == PKGSTATE_ORPHAN ? r->InstalledVersion : r->Version, ""));
    fmt_size(r->Size, size, sizeof size);
    snprintf(data->metabuf, sizeof data->metabuf, "%s | %s%s%s%s%s",
             state_word(r->State), type_word(r->Type),
             r->Section ? " | " : "", PKG_STR(r->Section, ""),
             size[0] ? " | " : "", size);
    set(data->txt_title, MUIA_Text_Contents, (IPTR)data->titlebuf);
    set(data->txt_meta,  MUIA_Text_Contents, (IPTR)data->metabuf);

    if (r->Summary)     { append(&text, &len, &cap, "\33b"); append(&text, &len, &cap, r->Summary); append(&text, &len, &cap, "\33n\n\n"); }
    if (r->Description) { append_reflowed(&text, &len, &cap, r->Description); append(&text, &len, &cap, "\n"); }
    if (r->State == PKGSTATE_UPGRADABLE && r->InstalledVersion)
    {
        snprintf(line, sizeof line, "Installed: %s  ->  Available: %s\n", r->InstalledVersion, PKG_STR(r->Version, "?"));
        append(&text, &len, &cap, line);
    }
    if (r->Extends)    { snprintf(line, sizeof line, "Extends: %s\n", r->Extends);       append(&text, &len, &cap, line); }
    if (r->Depends)    { snprintf(line, sizeof line, "Depends: %s\n", r->Depends);       append(&text, &len, &cap, line); }
    if (r->Homepage)   { snprintf(line, sizeof line, "Homepage: %s\n", r->Homepage);     append(&text, &len, &cap, line); }
    if (r->Licence)    { snprintf(line, sizeof line, "Licence: %s\n", r->Licence);       append(&text, &len, &cap, line); }
    if (r->Maintainer) { snprintf(line, sizeof line, "Maintainer: %s\n", r->Maintainer); append(&text, &len, &cap, line); }
    if (r->Reboot)     append(&text, &len, &cap, "A reboot is required after installing this package.\n");
    if (r->Changes)    { append(&text, &len, &cap, "\n\33bChanges:\33n\n"); append_reflowed(&text, &len, &cap, r->Changes); }

    set(data->flt_desc, MUIA_Floattext_Text, (IPTR)PKG_STR(text, ""));
    if (data->desctext) FreeVec(data->desctext);
    data->desctext = text;

    show_shot(self, data, data->pic, 0);
}

static void show_news(Object *self, struct SCMain_DATA *data, struct PkgAnnouncement *a)
{
    STRPTR text = NULL;
    ULONG len = 0, cap = 0;
    TEXT line[320];

    data->selname[0] = 0;
    data->selpkg = FALSE;
    data->selshots = 0;

    snprintf(data->titlebuf, sizeof data->titlebuf, "\33b%s", PKG_STR(a->Title, ""));
    snprintf(data->metabuf, sizeof data->metabuf, "%s | %s%s%s", PKG_STR(a->Date, ""),
             PKG_STR(a->Severity, "info"), a->Package ? " | " : "", PKG_STR(a->Package, ""));
    set(data->txt_title, MUIA_Text_Contents, (IPTR)data->titlebuf);
    set(data->txt_meta,  MUIA_Text_Contents, (IPTR)data->metabuf);

    if (a->Body) append_reflowed(&text, &len, &cap, a->Body);
    if (a->URL)  { snprintf(line, sizeof line, "\nMore: %s\n", a->URL); append(&text, &len, &cap, line); }
    set(data->flt_desc, MUIA_Floattext_Text, (IPTR)PKG_STR(text, ""));
    if (data->desctext) FreeVec(data->desctext);
    data->desctext = text;

    set(data->pic, MUIA_PictureView_Placeholder, (IPTR)"");
    set(data->pic, MUIA_PictureView_File, 0);

    if (!a->Seen)
    {
        a->Seen = TRUE;
        enqueue(self, data, newjob(JOB_ANNSEEN, NULL, a->Id));
        DoMethod(data->lists[TAB_NEWS], MUIM_NList_Redraw, MUIV_NList_Redraw_All);
    }
}

static void clear_details(struct SCMain_DATA *data)
{
    data->selname[0] = 0;
    data->selpkg = FALSE;
    data->selshots = 0;
    set(data->txt_title, MUIA_Text_Contents, (IPTR)"");
    set(data->txt_meta,  MUIA_Text_Contents, (IPTR)"");
    set(data->flt_desc,  MUIA_Floattext_Text, (IPTR)"");
    set(data->pic, MUIA_PictureView_Placeholder, (IPTR)"");
    set(data->pic, MUIA_PictureView_File, 0);
}

static void update_status(struct SCMain_DATA *data)
{
    static TEXT buf[160];
    ULONG n[NTABS] = { 0 }, i;
    struct Node *nd;

    if (data->set)
    {
        for (i = 0; i < NPKGTABS; i++) ForeachNode(&data->set->pkgs[i], nd) n[i]++;
        ForeachNode(&data->set->news, nd) n[TAB_NEWS]++;
    }
    snprintf(buf, sizeof buf, "%lu update%s | %lu installed | %lu available | %lu news",
             (unsigned long)n[TAB_UPDATES], n[TAB_UPDATES] == 1 ? "" : "s",
             (unsigned long)n[TAB_INSTALLED], (unsigned long)n[TAB_AVAILABLE],
             (unsigned long)n[TAB_NEWS]);
    set(data->txt_status, MUIA_Text_Contents, (IPTR)buf);
}

/* ------------------------------------------------------------------------ */
/* Methods                                                                   */

static IPTR SCMain__MUIM_SC_Lists(struct IClass *cl, Object *self, struct MUIP_SC_Lists *msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    ULONG i;
    struct Node *n;

    /* the NLists point into the old set: clear them before freeing it */
    for (i = 0; i < NTABS; i++)
    {
        set(data->lists[i], MUIA_NList_Quiet, TRUE);
        DoMethod(data->lists[i], MUIM_NList_Clear);
    }
    free_listset(data->set);
    data->set = msg->set;
    if (data->set)
    {
        for (i = 0; i < NPKGTABS; i++)
            ForeachNode(&data->set->pkgs[i], n)
                DoMethod(data->lists[i], MUIM_NList_InsertSingle, (IPTR)n, MUIV_NList_Insert_Bottom);
        ForeachNode(&data->set->news, n)
            DoMethod(data->lists[TAB_NEWS], MUIM_NList_InsertSingle, (IPTR)n, MUIV_NList_Insert_Bottom);
    }
    for (i = 0; i < NTABS; i++)
        set(data->lists[i], MUIA_NList_Quiet, FALSE);

    /* re-select by name on the current tab if it is still there */
    {
        IPTR tab = 0;
        LONG pos = 0;
        get(data->reg, MUIA_Group_ActivePage, &tab);
        if (data->set && tab < NPKGTABS && data->selname[0])
        {
            ForeachNode(&data->set->pkgs[tab], n)
            {
                if (Stricmp(((struct PkgRecord *)n)->Name, data->selname) == 0)
                {
                    set(data->lists[tab], MUIA_NList_Active, pos);
                    break;
                }
                pos++;
            }
        }
    }
    update_status(data);
    DoMethod(self, MUIM_SC_Select);
    return 0;
}

static IPTR SCMain__MUIM_SC_Log(struct IClass *cl, Object *self, struct MUIP_SC_Log *msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    if (msg->text)
    {
        log_line(data, msg->text);
        FreeVec(msg->text);
    }
    return 0;
}

static IPTR SCMain__MUIM_SC_Progress(struct IClass *cl, Object *self, struct MUIP_SC_Progress *msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    static TEXT info[192];
    STRPTR p;

    if (msg->text)
    {
        /* InfoText is a printf-ish format for the gauge: keep '%' out */
        strlcpy(info, msg->text, sizeof info - 8);
        for (p = info; *p; p++) if (*p == '%') *p = ' ';
        if (msg->pct >= 0) strlcat(info, " %ld%%", sizeof info);
        set(data->gauge, MUIA_Gauge_InfoText, (IPTR)info);
        FreeVec(msg->text);
    }
    set(data->gauge, MUIA_Gauge_Current, msg->pct >= 0 ? msg->pct : 0);
    return 0;
}

static IPTR SCMain__MUIM_SC_Done(struct IClass *cl, Object *self, struct MUIP_SC_Done *msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    static const char *verbs[] = { "Reload", "Refresh", "Install", "Remove", "Update", "Update all",
                                   "Screenshot", "Save preferences", "" };
    TEXT buf[400];

    if (msg->rc)
    {
        snprintf(buf, sizeof buf, "\33b%s failed:\33n %s", verbs[msg->kind], PKG_STR(msg->detail, "unknown error"));
        log_line(data, buf);
        if (msg->kind != JOB_RELOAD)
            MUI_Request(_app(self), _win(self), 0, "Software Center", "_OK", "%s failed:\n%s",
                        verbs[msg->kind], PKG_STR(msg->detail, "unknown error"));
    }
    else if (msg->kind != JOB_RELOAD && msg->kind != JOB_ANNSEEN)
    {
        snprintf(buf, sizeof buf, "%s: done", verbs[msg->kind]);
        log_line(data, buf);
    }
    if (msg->detail) FreeVec(msg->detail);
    if (data->busy) data->busy--;
    if (data->busy == 0) set_busy(data, FALSE);
    return 0;
}

static IPTR SCMain__MUIM_SC_Shot(struct IClass *cl, Object *self, struct MUIP_SC_Shot *msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    struct ShotCache *c;

    if (!msg->name) return 0;
    if (!(c = shot_lookup(data, msg->name, msg->index)))
    {
        c = AllocVec(sizeof *c, MEMF_CLEAR);
        if (c)
        {
            strlcpy(c->name, msg->name, sizeof c->name);
            c->index = msg->index;
            c->path = msg->path;
            msg->path = NULL;
            AddTail((struct List *)&data->shots, (struct Node *)c);
        }
    }
    if (data->selpkg && Stricmp(data->selname, msg->name) == 0)
    {
        if (msg->index == 0) show_shot(self, data, data->pic, 0);
        if (data->win_large && msg->index == data->largeindex)
        {
            IPTR open = 0;
            get(data->win_large, MUIA_Window_Open, &open);
            if (open) show_shot(self, data, data->pic_large, data->largeindex);
        }
    }
    FreeVec(msg->name);
    if (msg->path) FreeVec(msg->path);
    if (data->busy) data->busy--;
    if (data->busy == 0) set_busy(data, FALSE);
    return 0;
}

static IPTR SCMain__MUIM_SC_Select(struct IClass *cl, Object *self, Msg msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    IPTR tab = 0;

    get(data->reg, MUIA_Group_ActivePage, &tab);
    if (tab == TAB_NEWS)
    {
        struct PkgAnnouncement *a = NULL;
        DoMethod(data->lists[TAB_NEWS], MUIM_NList_GetEntry, MUIV_NList_GetEntry_Active, (IPTR)&a);
        if (a) show_news(self, data, a); else clear_details(data);
    }
    else
    {
        struct PkgRecord *r = active_record(data, tab);
        if (r) show_package(self, data, r); else clear_details(data);
    }
    if (data->win_large)
    {
        IPTR open = 0;
        get(data->win_large, MUIA_Window_Open, &open);
        if (open && !data->selpkg) set(data->win_large, MUIA_Window_Open, FALSE);
        else if (open) DoMethod(self, MUIM_SC_LargeStep, 0);
    }
    set_busy(data, data->busy != 0);
    return 0;
}

static IPTR SCMain__MUIM_SC_Action(struct IClass *cl, Object *self, struct MUIP_SC_Action *msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    ULONG kind = msg->kind;
    TEXT buf[160];

    if (data->busy) return 0;
    if (kind == JOB_INSTALL && data->selstate == PKGSTATE_UPGRADABLE) kind = JOB_UPGRADE;
    if ((kind == JOB_INSTALL || kind == JOB_REMOVE || kind == JOB_UPGRADE) && !(data->selpkg && data->selname[0]))
        return 0;
    if (kind == JOB_REMOVE)
    {
        if (!MUI_Request(_app(self), _win(self), 0, "Software Center", "_Remove|_Cancel",
                         "Remove %s and all files it installed?", data->selname))
            return 0;
    }
    switch (kind)
    {
    case JOB_INSTALL:    snprintf(buf, sizeof buf, "Installing %s...", data->selname); break;
    case JOB_REMOVE:     snprintf(buf, sizeof buf, "Removing %s...", data->selname); break;
    case JOB_UPGRADE:    snprintf(buf, sizeof buf, "Updating %s...", data->selname); break;
    case JOB_UPGRADEALL: strlcpy(buf, "Updating all packages...", sizeof buf); break;
    case JOB_REFRESH:    strlcpy(buf, "Refreshing repositories...", sizeof buf); break;
    default:             buf[0] = 0;
    }
    if (buf[0]) log_line(data, buf);
    set(data->gauge, MUIA_Gauge_Current, 0);
    set(data->gauge, MUIA_Gauge_InfoText, (IPTR)"Working...");
    enqueue(self, data, newjob(kind, data->selname, 0));
    set_busy(data, TRUE);
    return 0;
}

static IPTR SCMain__MUIM_SC_DoubleClick(struct IClass *cl, Object *self, Msg msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    if (data->selpkg && data->selstate == PKGSTATE_AVAILABLE)
        return DoMethod(self, MUIM_SC_Action, JOB_INSTALL);
    if (data->selpkg && data->selstate == PKGSTATE_UPGRADABLE)
        return DoMethod(self, MUIM_SC_Action, JOB_UPGRADE);
    return 0;
}

/* A second start (the UpdateNotifier's "Open", a second icon double-click)
   reaches the running instance as MUIA_Application_DoubleStart: bring the
   window up on the Updates tab and re-read the lists, since the reason is
   usually that something changed. */
static IPTR SCMain__MUIM_SC_Raise(struct IClass *cl, Object *self, Msg msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    Object *win = _win(self);

    if (win)
    {
        set(win, MUIA_Window_Open, TRUE);
        DoMethod(win, MUIM_Window_ToFront);
        set(win, MUIA_Window_Activate, TRUE);
    }
    set(data->reg, MUIA_Group_ActivePage, TAB_UPDATES);
    enqueue(self, data, newjob(JOB_RELOAD, NULL, 0));   /* queues behind a running job */
    return 0;
}

static IPTR SCMain__MUIM_SC_Cancel(struct IClass *cl, Object *self, Msg msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    data->abort = 1;
    log_line(data, "Cancel requested...");
    return 0;
}

/* ---- large view --------------------------------------------------------- */

static IPTR SCMain__MUIM_SC_LargeStep(struct IClass *cl, Object *self, struct MUIP_SC_LargeStep *msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    static TEXT cnt[48];
    LONG idx;

    if (!data->win_large || !data->selshots) return 0;
    idx = (LONG)data->largeindex + msg->delta;
    if (idx < 0) idx = data->selshots - 1;
    if (idx >= (LONG)data->selshots) idx = 0;
    data->largeindex = idx;
    snprintf(cnt, sizeof cnt, "%s  %ld / %lu", data->selname, (long)idx + 1, (unsigned long)data->selshots);
    set(data->txt_largecount, MUIA_Text_Contents, (IPTR)cnt);
    set(data->btn_prev, MUIA_Disabled, data->selshots < 2);
    set(data->btn_next, MUIA_Disabled, data->selshots < 2);
    show_shot(self, data, data->pic_large, idx);
    return 0;
}

static IPTR SCMain__MUIM_SC_OpenLarge(struct IClass *cl, Object *self, Msg msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);

    if (!data->selpkg || !data->selshots) return 0;
    if (!data->win_large)
    {
        Object *btn_close;
        data->win_large = WindowObject,
            MUIA_Window_Title, (IPTR)"Screenshot",
            MUIA_Window_ID,    MAKE_ID('S','C','S','H'),
            WindowContents, VGroup,
                Child, data->pic_large = PictureViewObject,
                    MUIA_PictureView_PercentW, 78,
                    MUIA_PictureView_PercentH, 78,
                    MUIA_PictureView_Placeholder, (IPTR)"Loading...",
                End,
                Child, HGroup,
                    Child, data->btn_prev = SimpleButton("< _Previous"),
                    Child, data->txt_largecount = TextObject, MUIA_Text_PreParse, (IPTR)"\33c", MUIA_Text_Contents, (IPTR)"", End,
                    Child, data->btn_next = SimpleButton("_Next >"),
                    Child, HSpace(0),
                    Child, btn_close = SimpleButton("_Close"),
                End,
            End,
        End;
        if (!data->win_large) return 0;
        DoMethod(_app(self), OM_ADDMEMBER, (IPTR)data->win_large);
        DoMethod(data->win_large, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
                 (IPTR)data->win_large, 3, MUIM_Set, MUIA_Window_Open, FALSE);
        DoMethod(data->btn_prev, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 2, MUIM_SC_LargeStep, -1);
        DoMethod(data->btn_next, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 2, MUIM_SC_LargeStep, 1);
        DoMethod(btn_close, MUIM_Notify, MUIA_Pressed, FALSE,
                 (IPTR)data->win_large, 3, MUIM_Set, MUIA_Window_Open, FALSE);
        /* pic_large is a MUIA_PictureView_Pressed source too: click = next */
        DoMethod(data->pic_large, MUIM_Notify, MUIA_PictureView_Pressed, TRUE,
                 (IPTR)self, 2, MUIM_SC_LargeStep, 1);
    }
    data->largeindex = 0;
    DoMethod(self, MUIM_SC_LargeStep, 0);
    set(data->win_large, MUIA_Window_Open, TRUE);
    return 0;
}

/* ---- prefs -------------------------------------------------------------- */

static IPTR SCMain__MUIM_SC_OpenPrefs(struct IClass *cl, Object *self, Msg msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    struct PkgPrefs *p;
    struct PkgSource *s;

    if (data->busy) return 0;
    if (!data->win_prefs)
    {
        Object *btn_save, *btn_cancel;
        data->win_prefs = WindowObject,
            MUIA_Window_Title, (IPTR)"Software Center Preferences",
            MUIA_Window_ID,    MAKE_ID('S','C','P','R'),
            WindowContents, VGroup,
                Child, ColGroup(2), GroupFrameT("Repository"),
                    Child, Label2("URL"),
                    Child, data->str_url = StringObject, StringFrame, MUIA_String_MaxLen, PKG_URL_LEN, MUIA_CycleChain, 1, End,
                    Child, Label2("Key ID"),
                    Child, data->str_keyid = StringObject, StringFrame, MUIA_String_MaxLen, 33, MUIA_CycleChain, 1, End,
                    Child, Label2("Trust key file"),
                    Child, PopaslObject, MUIA_Popasl_Type, ASL_FileRequest,
                        ASLFR_TitleText, (IPTR)"Select a publisher .pub key",
                        MUIA_Popstring_String, data->str_keyfile = StringObject, StringFrame, MUIA_String_MaxLen, 255, MUIA_CycleChain, 1, End,
                        MUIA_Popstring_Button, PopButton(MUII_PopFile),
                    End,
                End,
                Child, ColGroup(2), GroupFrameT("Update checks"),
                    Child, Label2("Check every (minutes, 0 = never)"),
                    Child, data->str_interval = StringObject, StringFrame, MUIA_String_Accept, (IPTR)"0123456789",
                        MUIA_String_MaxLen, 8, MUIA_CycleChain, 1, End,
                    Child, Label1("Notify about updates"),
                    Child, HGroup, Child, data->chk_notify = MUI_MakeObject(MUIO_Checkmark, NULL), Child, HSpace(0), End,
                    Child, Label1("Notify about announcements"),
                    Child, HGroup, Child, data->chk_notifyann = MUI_MakeObject(MUIO_Checkmark, NULL), Child, HSpace(0), End,
                End,
                Child, HGroup,
                    Child, btn_save = SimpleButton("_Save"),
                    Child, HSpace(0),
                    Child, btn_cancel = SimpleButton("_Cancel"),
                End,
            End,
        End;
        if (!data->win_prefs) return 0;
        DoMethod(_app(self), OM_ADDMEMBER, (IPTR)data->win_prefs);
        DoMethod(data->win_prefs, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
                 (IPTR)data->win_prefs, 3, MUIM_Set, MUIA_Window_Open, FALSE);
        DoMethod(btn_cancel, MUIM_Notify, MUIA_Pressed, FALSE,
                 (IPTR)data->win_prefs, 3, MUIM_Set, MUIA_Window_Open, FALSE);
        DoMethod(btn_save, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_SC_SavePrefs);
    }

    /* worker is idle (busy == 0): safe to read the context */
    p = pkg_prefs(data->ctx);
    s = (struct PkgSource *)GetHead(pkg_sources(data->ctx));
    {
        TEXT num[16];
        snprintf(num, sizeof num, "%lu", (unsigned long)p->CheckInterval);
        set(data->str_url,      MUIA_String_Contents, (IPTR)(s ? s->URL : (STRPTR)""));
        set(data->str_keyid,    MUIA_String_Contents, (IPTR)(s ? s->KeyId : (STRPTR)""));
        set(data->str_keyfile,  MUIA_String_Contents, (IPTR)"");
        set(data->str_interval, MUIA_String_Contents, (IPTR)num);
        set(data->chk_notify,    MUIA_Selected, p->Notify);
        set(data->chk_notifyann, MUIA_Selected, p->NotifyAnnouncements);
    }
    set(data->win_prefs, MUIA_Window_Open, TRUE);
    return 0;
}

static IPTR SCMain__MUIM_SC_SavePrefs(struct IClass *cl, Object *self, Msg msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    struct Job *job = newjob(JOB_SAVEPREFS, NULL, 0);
    IPTR v = 0;

    if (!job) return 0;
    get(data->str_url, MUIA_String_Contents, &v);      strlcpy(job->url, (STRPTR)v, sizeof job->url);
    get(data->str_keyid, MUIA_String_Contents, &v);    strlcpy(job->keyid, (STRPTR)v, sizeof job->keyid);
    get(data->str_keyfile, MUIA_String_Contents, &v);  strlcpy(job->keyfile, (STRPTR)v, sizeof job->keyfile);
    get(data->str_interval, MUIA_String_Integer, &v);  job->interval = v;
    get(data->chk_notify, MUIA_Selected, &v);          job->notify = v ? TRUE : FALSE;
    get(data->chk_notifyann, MUIA_Selected, &v);       job->notifyann = v ? TRUE : FALSE;
    set(data->win_prefs, MUIA_Window_Open, FALSE);
    enqueue(self, data, job);
    set_busy(data, TRUE);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Construction                                                              */

static Object *make_pkglist(struct Hook *hook)
{
    return NListviewObject,
        MUIA_CycleChain, 1,
        MUIA_NListview_NList, NListObject,
            MUIA_NList_Format,      (IPTR)"BAR,BAR,BAR,P=\33r BAR,BAR,",
            MUIA_NList_Title,       TRUE,
            MUIA_NList_TitleSeparator, TRUE,
            MUIA_NList_DisplayHook, (IPTR)hook,
            MUIA_Frame,             MUIV_Frame_InputList,
        End,
    End;
}

static IPTR SCMain__OM_NEW(struct IClass *cl, Object *self, struct opSet *msg)
{
    struct SCMain_DATA *data;
    struct PkgContext *ctx = (struct PkgContext *)GetTagData(MUIB_SC | 0xFF, 0, msg->ops_AttrList);
    static struct Hook pkgdisplay_hook, newsdisplay_hook;
    static const char *tabs[] = { "Updates", "Installed", "Available", "News", NULL };
    Object *lists[NTABS], *txt_status, *txt_title, *txt_meta, *flt_desc, *pic, *reg;
    Object *btn_install, *btn_remove, *btn_updateall, *btn_refresh, *btn_prefs, *btn_cancel, *gauge, *lst_log;
    ULONG i;

    pkgdisplay_hook.h_Entry  = (HOOKFUNC)AROS_ASMSYMNAME(pkgdisplay_func);
    newsdisplay_hook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(newsdisplay_func);

    for (i = 0; i < NPKGTABS; i++) lists[i] = make_pkglist(&pkgdisplay_hook);
    lists[TAB_NEWS] = NListviewObject,
        MUIA_CycleChain, 1,
        MUIA_NListview_NList, NListObject,
            MUIA_NList_Format,      (IPTR)"BAR,BAR,BAR,",
            MUIA_NList_Title,       TRUE,
            MUIA_NList_TitleSeparator, TRUE,
            MUIA_NList_DisplayHook, (IPTR)&newsdisplay_hook,
            MUIA_Frame,             MUIV_Frame_InputList,
        End,
    End;

    btn_install   = SimpleButton("_Install");
    btn_remove    = SimpleButton("_Remove");
    btn_updateall = SimpleButton("Update _All");
    btn_refresh   = SimpleButton("Re_fresh");
    btn_prefs     = SimpleButton("_Prefs...");
    btn_cancel    = SimpleButton("_Cancel");
    gauge = GaugeObject, GaugeFrame,
        MUIA_Gauge_Horiz,    TRUE,
        MUIA_Gauge_Max,      100,
        MUIA_Gauge_Current,  0,
        MUIA_Gauge_InfoText, (IPTR)"Ready",
    End;
    SCDBG("made: install=%ix remove=%ix updateall=%ix refresh=%ix prefs=%ix cancel=%ix gauge=%ix\n",
          (IPTR)btn_install, (IPTR)btn_remove, (IPTR)btn_updateall, (IPTR)btn_refresh,
          (IPTR)btn_prefs, (IPTR)btn_cancel, (IPTR)gauge);

    self = (Object *)DoSuperNewTags(cl, self, NULL,
        MUIA_Group_Horiz, FALSE,
        Child, HGroup,
            Child, TextObject, MUIA_Text_Contents, (IPTR)"\33bAROS Software Center", End,
            Child, HSpace(0),
            Child, txt_status = TextObject, MUIA_Text_PreParse, (IPTR)"\33r", MUIA_Text_Contents, (IPTR)"", End,
        End,
        Child, reg = RegisterObject,
            MUIA_Register_Titles, (IPTR)tabs,
            MUIA_Register_Frame,  TRUE,
            MUIA_Weight, 60,
            Child, lists[TAB_UPDATES],
            Child, lists[TAB_INSTALLED],
            Child, lists[TAB_AVAILABLE],
            Child, lists[TAB_NEWS],
        End,
        Child, HGroup, GroupFrameT("Details"),
            MUIA_Weight, 40,
            Child, VGroup,
                Child, txt_title = TextObject, MUIA_Text_Contents, (IPTR)"", End,
                Child, txt_meta  = TextObject, MUIA_Text_Contents, (IPTR)"", End,
                Child, ListviewObject,
                    MUIA_Listview_Input, FALSE,
                    MUIA_Listview_List, flt_desc = FloattextObject,
                        ReadListFrame,
                        MUIA_Floattext_Text, (IPTR)"",
                    End,
                End,
            End,
            Child, VGroup,
                Child, pic = PictureViewObject,
                    MUIA_PictureView_PercentW, 16,
                    MUIA_PictureView_PercentH, 16,
                    MUIA_PictureView_Placeholder, (IPTR)"",
                    MUIA_ShortHelp, (IPTR)"Click to enlarge",
                End,
                Child, VSpace(0),
            End,
        End,
        Child, HGroup,
            MUIA_Group_SameSize, TRUE,
            Child, btn_install,
            Child, btn_remove,
            Child, btn_updateall,
            Child, btn_refresh,
            Child, btn_prefs,
        End,
        Child, HGroup,
            Child, gauge,
            Child, btn_cancel,
        End,
        Child, lst_log = NListviewObject,
            MUIA_Weight, 15,
            MUIA_NListview_NList, NListObject,
                MUIA_NList_ConstructHook, MUIV_NList_ConstructHook_String,
                MUIA_NList_DestructHook,  MUIV_NList_DestructHook_String,
                MUIA_NList_Input, FALSE,
                MUIA_Frame, MUIV_Frame_ReadList,
            End,
        End,
        TAG_MORE, (IPTR)msg->ops_AttrList);
    if (!self) return 0;

    data = INST_DATA(cl, self);
    SCDBG("OM_NEW self=%ix data=%ix instsize=%id sizeof=%id\n", (IPTR)self, (IPTR)data,
          (IPTR)cl->cl_InstSize, (IPTR)sizeof(struct SCMain_DATA));
    SCDBG(" install=%ix remove=%ix updateall=%ix refresh=%ix prefs=%ix cancel=%ix gauge=%ix lst_log=%ix\n",
          (IPTR)btn_install, (IPTR)btn_remove, (IPTR)btn_updateall, (IPTR)btn_refresh,
          (IPTR)btn_prefs, (IPTR)btn_cancel, (IPTR)gauge, (IPTR)lst_log);
    data->ctx = ctx;
    for (i = 0; i < NTABS; i++) data->lists[i] = lists[i];
    data->reg = reg;             data->txt_status = txt_status;
    data->txt_title = txt_title; data->txt_meta = txt_meta;
    data->flt_desc = flt_desc;   data->pic = pic;
    data->btn_install = btn_install;     data->btn_remove = btn_remove;
    data->btn_updateall = btn_updateall; data->btn_refresh = btn_refresh;
    data->btn_prefs = btn_prefs;         data->btn_cancel = btn_cancel;
    data->gauge = gauge;                 data->lst_log = lst_log;

    InitSemaphore(&data->qlock);
    NewList((struct List *)&data->queue);
    NewList((struct List *)&data->shots);

    data->pkghook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(pkghook_func);
    data->pkghook.h_Data  = data;
    data->self = self;

    data->proc = MUI_NewObject(MUIC_Process,
        MUIA_Process_SourceClass,  (IPTR)cl,
        MUIA_Process_SourceObject, (IPTR)self,
        MUIA_Process_Name,         (IPTR)"SoftwareCenter worker",
        MUIA_Process_Priority,     -1,
        MUIA_Process_StackSize,    256 * 1024,
        MUIA_Process_AutoLaunch,   FALSE,
        TAG_DONE);
    if (!data->proc)
    {
        CoerceMethod(cl, self, OM_DISPOSE);
        return 0;
    }

    for (i = 0; i < NTABS; i++)
    {
        DoMethod(lists[i], MUIM_Notify, MUIA_NList_Active, MUIV_EveryTime, (IPTR)self, 1, MUIM_SC_Select);
        if (i < NPKGTABS)
            DoMethod(lists[i], MUIM_Notify, MUIA_NList_DoubleClick, MUIV_EveryTime, (IPTR)self, 1, MUIM_SC_DoubleClick);
    }
    DoMethod(reg, MUIM_Notify, MUIA_Group_ActivePage, MUIV_EveryTime, (IPTR)self, 1, MUIM_SC_Select);
    DoMethod(btn_install,   MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 2, MUIM_SC_Action, JOB_INSTALL);
    DoMethod(btn_remove,    MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 2, MUIM_SC_Action, JOB_REMOVE);
    DoMethod(btn_updateall, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 2, MUIM_SC_Action, JOB_UPGRADEALL);
    DoMethod(btn_refresh,   MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 2, MUIM_SC_Action, JOB_REFRESH);
    DoMethod(btn_prefs,     MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_SC_OpenPrefs);
    DoMethod(btn_cancel,    MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_SC_Cancel);
    DoMethod(pic, MUIM_Notify, MUIA_PictureView_Pressed, TRUE, (IPTR)self, 1, MUIM_SC_OpenLarge);

    SCDBG(" before set_busy: prefs=%ix cancel=%ix gauge=%ix &cancel=%ix\n",
          (IPTR)data->btn_prefs, (IPTR)data->btn_cancel, (IPTR)data->gauge, (IPTR)&data->btn_cancel);
    set_busy(data, TRUE);
    return (IPTR)self;
}

static IPTR SCMain__OM_DISPOSE(struct IClass *cl, Object *self, Msg msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    struct Job *job;
    ULONG i;

    if (data->proc)
    {
        data->abort = 1;
        DoMethod(data->proc, MUIM_Process_Kill, 0);
        MUI_DisposeObject(data->proc);
    }
    while ((job = (struct Job *)RemHead((struct List *)&data->queue))) FreeVec(job);
    for (i = 0; i < NTABS; i++)
        if (data->lists[i]) DoMethod(data->lists[i], MUIM_NList_Clear);
    free_listset(data->set);
    shots_flush(data);
    if (data->desctext) FreeVec(data->desctext);
    return DoSuperMethodA(cl, self, msg);
}

/* Kick the worker: first thing after the window is open */
static IPTR SCMain__MUIM_Show(struct IClass *cl, Object *self, Msg msg)
{
    struct SCMain_DATA *data = INST_DATA(cl, self);
    IPTR rc = DoSuperMethodA(cl, self, msg);
    static BOOL started;

    if (!started)
    {
        started = TRUE;
        DoMethod(data->proc, MUIM_Process_Launch);
        log_line(data, "Loading package lists...");
        enqueue(self, data, newjob(JOB_RELOAD, NULL, 0));
    }
    return rc;
}

ZUNE_CUSTOMCLASS_18
(
    SCMain, NULL, MUIC_Group, NULL,
    OM_NEW,               struct opSet *,
    OM_DISPOSE,           Msg,
    MUIM_Show,            Msg,
    MUIM_Process_Process, struct MUIP_Process_Process *,
    MUIM_SC_Action,       struct MUIP_SC_Action *,
    MUIM_SC_Lists,        struct MUIP_SC_Lists *,
    MUIM_SC_Log,          struct MUIP_SC_Log *,
    MUIM_SC_Progress,     struct MUIP_SC_Progress *,
    MUIM_SC_Done,         struct MUIP_SC_Done *,
    MUIM_SC_Shot,         struct MUIP_SC_Shot *,
    MUIM_SC_Select,       Msg,
    MUIM_SC_Cancel,       Msg,
    MUIM_SC_OpenLarge,    Msg,
    MUIM_SC_LargeStep,    struct MUIP_SC_LargeStep *,
    MUIM_SC_OpenPrefs,    Msg,
    MUIM_SC_SavePrefs,    Msg,
    MUIM_SC_DoubleClick,  Msg,
    MUIM_SC_Raise,        Msg
);

/* ------------------------------------------------------------------------ */

/* The Zune tree (five NLists, datatypes, a worker process) needs far more
 * than the 40 KiB a CLI hands a program by default: on a small stack the
 * object construction silently corrupts the heap and the very first
 * set() dies inside intuition.  Run the real main on our own 1 MiB stack
 * unless the launcher (icon STACK tooltype, Stack command) already gave
 * us enough. */
#define SC_STACK_MIN  (512 * 1024)
#define SC_STACK_SIZE (1024 * 1024)

static int real_main(void);

static IPTR sc_stack_entry(void)
{
    return (IPTR)real_main();
}

int main(void)
{
    struct Task *me = FindTask(NULL);
    IPTR have = (IPTR)me->tc_SPUpper - (IPTR)me->tc_SPLower;
    struct StackSwapStruct sss;
    struct StackSwapArgs args = { { 0 } };
    APTR stack;
    int rc;

    if (have >= SC_STACK_MIN)
        return real_main();

    stack = AllocMem(SC_STACK_SIZE, MEMF_ANY);
    if (!stack)
        return real_main();

    sss.stk_Lower   = stack;
    sss.stk_Upper   = (APTR)((IPTR)stack + SC_STACK_SIZE);
    sss.stk_Pointer = sss.stk_Upper;
    rc = (int)NewStackSwap(&sss, (APTR)sc_stack_entry, &args);
    FreeMem(stack, SC_STACK_SIZE);
    return rc;
}

static int real_main(void)
{
    struct PkgContext *ctx;
    Object *app, *win, *root;
    int rc = RETURN_OK;

    ctx = pkg_open(NULL);                          /* hook is set per-worker */
    if (!ctx)
    {
        MUI_Request(NULL, NULL, 0, "Software Center", "_OK",
                    "Could not initialise the package system (SYS:Prefs/Packages).");
        return RETURN_FAIL;
    }

    app = ApplicationObject,
        MUIA_Application_Title,       (IPTR)"SoftwareCenter",
        MUIA_Application_Version,     (IPTR)version,
        MUIA_Application_Copyright,   (IPTR)"Copyright (C) 2026, The AROS Development Team",
        MUIA_Application_Author,      (IPTR)"The AROS Development Team",
        MUIA_Application_Description, (IPTR)"Install, remove and update software packages",
        MUIA_Application_Base,        (IPTR)"SOFTWARECENTER",
        MUIA_Application_SingleTask,  TRUE,
        SubWindow, win = WindowObject,
            MUIA_Window_Title, (IPTR)"Software Center",
            MUIA_Window_ID,    MAKE_ID('S','C','M','N'),
            WindowContents, root = NewObject(SCMain_CLASS->mcc_Class, NULL,
                MUIB_SC | 0xFF, (IPTR)ctx,
                TAG_DONE),
        End,
    End;

    if (app && root)
    {
        ULONG sigs = 0;

        DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
                 (IPTR)app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
        DoMethod(app, MUIM_Notify, MUIA_Application_DoubleStart, TRUE,
                 (IPTR)root, 1, MUIM_SC_Raise);
        set(win, MUIA_Window_Open, TRUE);
        while (DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs) != MUIV_Application_ReturnID_Quit)
        {
            if (sigs)
            {
                sigs = Wait(sigs | SIGBREAKF_CTRL_C);
                if (sigs & SIGBREAKF_CTRL_C) break;
            }
        }
        set(win, MUIA_Window_Open, FALSE);
    }
    else
        rc = RETURN_FAIL;

    if (app) MUI_DisposeObject(app);
    pkg_close(ctx);
    return rc;
}
