/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CrashReporter — soft crash reporter for distributions
          (SYS:WBStartup/CrashReporter, DESIGN.md).

          A Zune commodity modelled on SoftwareCenter's UpdateNotifier.  At
          start it installs the hooks from crashhook.c (trap observers +
          Alert() thunk) — the ROM is untouched and the usual "Program
          failed" requester still appears.  When a task hits Alert() the
          hook fills a record and signals us; we format it (report.c),
          save it under ENVARC:CrashReports, publish a Critical entry to
          log.resource and open a small card top-right:

              Program failed: <task>
              <summary>
              What were you doing when this happened?
              [__________________________________]
                                               [View] [Send] [Later]

          The description is appended to the report as a [Description]
          section when the card is dismissed.  Send POSTs the report to the distribution's server
          (ENV:CrashReporter/URL, default: the first enabled package
          source + api/crash/v1/report, PROTOCOL.md §9) from a MUIC_Process
          worker.  ENV:CrashReporter/AutoSend=1 sends without asking.
          Reports that could not be sent stay in the drawer and are
          retried from the status window.

    AI_PHASE: CRASH_REPORTER
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: MEDIUM
    AI_NOTE: The record → report step runs on this task, not in the hook:
             by then the failing task is parked in the requester and every
             library is safe to call.  Quitting is refused while another
             patch sits on top of our Alert() thunk (see crashhook_remove).
*/

#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/muimaster.h>
#include <proto/icon.h>
#include <proto/commodities.h>
#include <proto/workbench.h>
#include <proto/alib.h>

#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/exall.h>
#include <intuition/screens.h>
#include <libraries/mui.h>
#include <libraries/commodities.h>
#include <workbench/startup.h>
#include <zune/customclasses.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pkg.h"
#include "pkg_private.h"
#include "crashrec.h"
#include "report.h"

#define CR_VERSTR "1.0"
static const char version[] __attribute__((used)) = "$VER: CrashReporter " CR_VERSTR " (06.09.2026)";

#define CR_API_PATH      "api/crash/v1/report"
#define POPUP_WIDTH_PCT  30
#define POPUP_MARGIN     12
#define POPUP_WRAP_COLS  56         /* summary line width in the popup      */

/* ------------------------------------------------------------------------ */
/* Methods                                                                   */

#define MUIB_CR             (TAG_USER | 0x5C730000)
#define MUIA_CR_Context     (MUIB_CR | 0xFF)   /* [I..] struct PkgContext *  */
#define MUIA_CR_Popup       (MUIB_CR | 0xFE)   /* [..G] the popup window     */
#define MUIA_CR_Viewer      (MUIB_CR | 0xFD)   /* [..G] the report window    */
#define MUIM_CR_Crash       (MUIB_CR | 0x01)   /* a record arrived            */
#define MUIM_CR_SendLatest  (MUIB_CR | 0x02)   /* popup [Send]                */
#define MUIM_CR_SendAll     (MUIB_CR | 0x03)   /* status [Send unsent]        */
#define MUIM_CR_SendResult  (MUIB_CR | 0x04)   /* pushed by the worker        */
#define MUIM_CR_View        (MUIB_CR | 0x05)   /* popup [View]                */
#define MUIM_CR_Later       (MUIB_CR | 0x06)
#define MUIM_CR_AutoSend    (MUIB_CR | 0x07)   /* checkbox changed            */
#define MUIM_CR_OpenDrawer  (MUIB_CR | 0x08)

#define CR_DESC_MAXLEN      400                /* popup description field    */

struct MUIP_CR_Crash      { STACKED ULONG MethodID; STACKED struct CrashRecord *rec; };
struct MUIP_CR_SendResult { STACKED ULONG MethodID; STACKED STRPTR path; STACKED LONG rc;
                            STACKED LONG status; STACKED STRPTR reply; };

/* a report waiting to be sent (main task owns the list; the worker takes
   items under the semaphore) */
struct SendItem
{
    struct Node node;
    TEXT        path[PKG_PATHLEN];
};

struct CR_DATA
{
    struct PkgContext *ctx;
    Object            *proc;
    Object            *self;
    struct SignalSemaphore qlock;
    struct List        queue;                /* SendItem                   */
    ULONG              sending;              /* items the worker holds     */

    /* status window */
    Object *txt_state, *txt_last, *txt_url, *txt_detail, *chk_auto, *btn_sendall, *btn_drawer;
    /* popup */
    Object *win_popup, *txt_ptitle, *txt_pbody, *str_pdesc, *btn_pview, *btn_psend, *btn_plater;
    BOOL    popup_open;
    BOOL    desc_done;                       /* latest already annotated   */
    /* viewer */
    Object *win_view, *flt_view;
    STRPTR  viewtext;

    ULONG   captured;                        /* this session               */
    ULONG   sent, failed;
    BOOL    autosend;
    TEXT    url[PKG_URL_LEN];
    TEXT    latest[PKG_PATHLEN];             /* newest report path         */
    TEXT    statebuf[128];
    TEXT    lastbuf[200];
    TEXT    detailbuf[200];
    TEXT    ptitlebuf[CR_NAMELEN + 32];
    TEXT    pbodybuf[400];
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */

static STRPTR dupstr(CONST_STRPTR s)
{
    STRPTR d;
    if (!s) s = "";
    d = AllocVec(strlen(s) + 1, MEMF_ANY);
    if (d) strcpy(d, s);
    return d;
}

static BOOL ends_with(CONST_STRPTR s, CONST_STRPTR suffix)
{
    ULONG l = strlen(s), m = strlen(suffix);
    return l >= m && strcmp(s + l - m, suffix) == 0;
}

/* server URL: ENV override, else first enabled source + CR_API_PATH */
static void resolve_url(struct PkgContext *ctx, STRPTR out, ULONG len)
{
    struct PkgSource *src;

    if (GetVar(CR_ENV_URL, out, len, GVF_GLOBAL_ONLY) > 0 && out[0])
        return;
    ForeachNode(pkg_sources(ctx), src)
    {
        if (src->Enabled && src->URL[0])
        {
            snprintf(out, len, "%s%s", src->URL, CR_API_PATH);
            return;
        }
    }
    out[0] = 0;
}

static BOOL env_bool(CONST_STRPTR name)
{
    TEXT buf[8];
    if (GetVar(name, buf, sizeof buf, GVF_GLOBAL_ONLY) <= 0) return FALSE;
    return buf[0] == '1' || buf[0] == 'Y' || buf[0] == 'y' || buf[0] == 'T' || buf[0] == 't';
}

/* every "*.report" in the drawer, oldest name first is good enough */
static ULONG scan_unsent(struct List *out)
{
    TEXT dir[PKG_PATHLEN];
    BPTR lock;
    struct ExAllControl *eac;
    struct ExAllData *ead;
    UBYTE *buf;
    BOOL more;
    ULONG n = 0;

    report_dir(dir, sizeof dir);
    lock = Lock(dir, SHARED_LOCK);
    if (!lock) return 0;
    buf = AllocMem(4096, MEMF_ANY);
    eac = AllocDosObject(DOS_EXALLCONTROL, NULL);
    if (buf && eac)
    {
        eac->eac_LastKey = 0;
        do
        {
            more = ExAll(lock, (struct ExAllData *)buf, 4096, ED_TYPE, eac);
            if (!more && IoErr() != ERROR_NO_MORE_ENTRIES) break;
            for (ead = (struct ExAllData *)buf; eac->eac_Entries && ead; ead = ead->ed_Next)
            {
                struct SendItem *it;
                if (ead->ed_Type > 0 || !ends_with(ead->ed_Name, CR_REPORT_EXT)) continue;
                it = AllocVec(sizeof *it, MEMF_ANY | MEMF_CLEAR);
                if (!it) continue;
                pkg_join(it->path, sizeof it->path, dir, ead->ed_Name);
                it->node.ln_Name = it->path;
                AddTail(out, &it->node);
                n++;
            }
        } while (more);
    }
    if (eac) FreeDosObject(DOS_EXALLCONTROL, eac);
    if (buf) FreeMem(buf, 4096);
    UnLock(lock);
    return n;
}

static void queue_add(struct CR_DATA *data, CONST_STRPTR path)
{
    struct SendItem *it;

    ObtainSemaphore(&data->qlock);
    ForeachNode(&data->queue, it)
        if (strcmp(it->path, path) == 0) { ReleaseSemaphore(&data->qlock); return; }
    it = AllocVec(sizeof *it, MEMF_ANY | MEMF_CLEAR);
    if (it)
    {
        strlcpy(it->path, path, sizeof it->path);
        it->node.ln_Name = it->path;
        AddTail(&data->queue, &it->node);
    }
    ReleaseSemaphore(&data->qlock);
}

static void update_status(struct CR_DATA *data)
{
    struct List l;
    ULONG unsent;

    NewList(&l);
    unsent = scan_unsent(&l);
    while (!IsListEmpty(&l)) { struct Node *n = RemHead(&l); FreeVec(n); }

    snprintf(data->statebuf, sizeof data->statebuf,
             "Watching. %lu crash%s this session, %lu sent, %lu failed, %lu unsent on disk.",
             (unsigned long)data->captured, data->captured == 1 ? "" : "es",
             (unsigned long)data->sent, (unsigned long)data->failed, (unsigned long)unsent);
    set(data->txt_state, MUIA_Text_Contents, (IPTR)data->statebuf);
    set(data->txt_url,   MUIA_Text_Contents, (IPTR)(data->url[0] ? data->url : (TEXT *)"(no server: no package source configured)"));
    set(data->btn_sendall, MUIA_Disabled, unsent == 0 || !data->url[0]);
}

/* ------------------------------------------------------------------------ */
/* Worker: drains the queue, one POST per report                             */

static void worker_send(Object *self, struct CR_DATA *data)
{
    for (;;)
    {
        struct SendItem *it;
        UBYTE *body = NULL;
        ULONG len = 0;
        STRPTR reply = NULL;
        LONG status = 0, rc;

        ObtainSemaphore(&data->qlock);
        it = (struct SendItem *)RemHead(&data->queue);
        if (it) data->sending++;
        ReleaseSemaphore(&data->qlock);
        if (!it) break;

        rc = pkg_file_read(it->path, &body, &len, 1024 * 1024);
        if (rc == PKGERR_OK)
        {
            rc = pkg_http_post(data->ctx, data->url, "text/plain; charset=utf-8", body, len, &reply, &status);
            free(body);                             /* pkg_file_read() is malloc() */
        }
        if (rc == PKGERR_OK)
        {
            /* keep it, but out of the unsent set */
            TEXT sent[PKG_PATHLEN];
            snprintf(sent, sizeof sent, "%s.sent", it->path);
            DeleteFile(sent);
            Rename(it->path, sent);
        }
        DoMethod(_app(self), MUIM_Application_PushMethod, (IPTR)self, 5, MUIM_CR_SendResult,
                 (IPTR)dupstr(it->path), rc, status, (IPTR)reply);
        FreeVec(it);
    }
}

static IPTR CR__MUIM_Process_Process(struct IClass *cl, Object *self, struct MUIP_Process_Process *msg)
{
    struct CR_DATA *data = INST_DATA(cl, self);
    struct Process *me = (struct Process *)FindTask(NULL);

    me->pr_WindowPtr = (APTR)-1;

    while (!*msg->kill)
    {
        ULONG sigs = Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E);
        if ((sigs & SIGBREAKF_CTRL_C) || *msg->kill) break;
        if (sigs & SIGBREAKF_CTRL_E)
            worker_send(self, data);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */

static void place_popup(struct CR_DATA *data)
{
    struct Screen *scr = LockPubScreen(NULL);
    LONG w = 640, top = 0;

    if (scr)
    {
        w   = scr->Width;
        top = scr->BarHeight + 1;
        UnlockPubScreen(NULL, scr);
    }
    set(data->win_popup, MUIA_Window_LeftEdge, w - w * POPUP_WIDTH_PCT / 100 - POPUP_MARGIN);
    set(data->win_popup, MUIA_Window_TopEdge,  top + POPUP_MARGIN);
}

/* The popup's natural width follows its longest line (Zune Text objects set
   their minimum width from the text), so a long summary pushes it past the
   right screen edge.  Once it is open its real width is known: if it hangs
   over, move it back (LeftEdge only takes effect on open, so reopen). */
static void fit_popup(struct CR_DATA *data)
{
    struct Screen *scr = LockPubScreen(NULL);
    IPTR ww = 0, wl = 0;
    LONG w = 640;

    if (scr)
    {
        w = scr->Width;
        UnlockPubScreen(NULL, scr);
    }
    get(data->win_popup, MUIA_Window_Width,    &ww);
    get(data->win_popup, MUIA_Window_LeftEdge, &wl);
    if (ww && (LONG)(wl + ww) > w - POPUP_MARGIN)
    {
        LONG left = w - (LONG)ww - POPUP_MARGIN;
        set(data->win_popup, MUIA_Window_Open, FALSE);
        set(data->win_popup, MUIA_Window_LeftEdge, left < 0 ? 0 : left);
        set(data->win_popup, MUIA_Window_Open, TRUE);
    }
}

/* Word-wrap src into dst at about width columns.  Breaks after a space or,
   in the path-like summaries a crash produces ("...chrome!_ZN5blink...+0x1a"),
   after '/', '!', ':' or '+', so the popup keeps a readable width. */
static void wrap_text(STRPTR dst, ULONG dstlen, CONST_STRPTR src, ULONG width)
{
    ULONG col = 0, out = 0, lastbreak = 0, lastbreakcol = 0;

    while (*src && out + 1 < dstlen)
    {
        TEXT c = *src++;

        if (c == '\n') { dst[out++] = c; col = 0; lastbreak = 0; continue; }
        dst[out++] = c;
        col++;
        if (c == ' ' || c == '/' || c == '!' || c == ':' || c == '+')
        {
            lastbreak = out;
            lastbreakcol = col;
        }
        if (col >= width && lastbreak && out + 1 < dstlen)
        {
            /* insert a newline at the last break point (drop a trailing space) */
            ULONG at = lastbreak;
            if (dst[at - 1] == ' ') at--;
            memmove(dst + at + 1, dst + lastbreak, out - lastbreak);
            dst[at] = '\n';
            out = at + 1 + (out - lastbreak);
            col = col - lastbreakcol;
            lastbreak = 0;
        }
    }
    dst[out] = 0;
}

/* ------------------------------------------------------------------------ */
/* Methods on the main task                                                  */

static IPTR CR__MUIM_CR_Crash(struct IClass *cl, Object *self, struct MUIP_CR_Crash *msg)
{
    struct CR_DATA *data = INST_DATA(cl, self);
    struct CrashReport rep;
    LONG rc;

    data->captured++;
    rc = report_build(msg->rec, &rep);
    if (rc != PKGERR_OK)
    {
        snprintf(data->detailbuf, sizeof data->detailbuf, "Could not format report: %s", pkg_strerror(rc));
        set(data->txt_detail, MUIA_Text_Contents, (IPTR)data->detailbuf);
        return 0;
    }
    rc = report_store(&rep);

    snprintf(data->lastbuf, sizeof data->lastbuf, "%s: %s", rep.Task, rep.Summary);
    set(data->txt_last, MUIA_Text_Contents, (IPTR)data->lastbuf);
    if (rc != PKGERR_OK)
    {
        snprintf(data->detailbuf, sizeof data->detailbuf, "Report not saved: %s", pkg_strerror(rc));
        set(data->txt_detail, MUIA_Text_Contents, (IPTR)data->detailbuf);
        report_free(&rep);
        update_status(data);
        return 0;
    }
    strlcpy(data->latest, rep.Path, sizeof data->latest);

    /* the viewer shows the newest report until the user opens another */
    if (data->viewtext) FreeVec(data->viewtext);
    data->viewtext = rep.Text;              /* ownership moves here */
    rep.Text = NULL;
    set(data->flt_view, MUIA_Floattext_Text, (IPTR)data->viewtext);
    set(data->win_view, MUIA_Window_Title, (IPTR)data->latest);

    if (data->autosend && data->url[0])
    {
        queue_add(data, rep.Path);
        DoMethod(data->proc, MUIM_Process_Signal, SIGBREAKF_CTRL_E);
    }
    else
    {
        TEXT summary[256];

        wrap_text(summary, sizeof summary, rep.Summary, POPUP_WRAP_COLS);
        snprintf(data->ptitlebuf, sizeof data->ptitlebuf, "\33bProgram failed: %s", rep.Task);
        snprintf(data->pbodybuf, sizeof data->pbodybuf, "%s\n\nA crash report was saved%s.",
                 summary, data->url[0] ? " and can be sent to the distribution" : "");
        set(data->txt_ptitle, MUIA_Text_Contents, (IPTR)data->ptitlebuf);
        set(data->txt_pbody,  MUIA_Text_Contents, (IPTR)data->pbodybuf);
        set(data->str_pdesc,  MUIA_String_Contents, (IPTR)"");
        data->desc_done = FALSE;
        set(data->btn_psend,  MUIA_Disabled, !data->url[0]);
        if (!data->popup_open) place_popup(data);
        set(data->win_popup, MUIA_Window_Open, TRUE);
        fit_popup(data);
        data->popup_open = TRUE;
    }
    report_free(&rep);
    update_status(data);
    return 0;
}

static IPTR CR__MUIM_CR_SendLatest(struct IClass *cl, Object *self, Msg msg)
{
    struct CR_DATA *data = INST_DATA(cl, self);

    DoMethod(self, MUIM_CR_Later);
    if (!data->latest[0] || !data->url[0]) return 0;
    queue_add(data, data->latest);
    set(data->txt_detail, MUIA_Text_Contents, (IPTR)"Sending report...");
    DoMethod(data->proc, MUIM_Process_Signal, SIGBREAKF_CTRL_E);
    return 0;
}

static IPTR CR__MUIM_CR_SendAll(struct IClass *cl, Object *self, Msg msg)
{
    struct CR_DATA *data = INST_DATA(cl, self);
    struct List l;
    struct Node *n;
    ULONG count;

    if (!data->url[0]) return 0;
    NewList(&l);
    count = scan_unsent(&l);
    while ((n = RemHead(&l)))
    {
        queue_add(data, n->ln_Name);
        FreeVec(n);
    }
    if (count)
    {
        snprintf(data->detailbuf, sizeof data->detailbuf, "Sending %lu report%s...",
                 (unsigned long)count, count == 1 ? "" : "s");
        set(data->txt_detail, MUIA_Text_Contents, (IPTR)data->detailbuf);
        DoMethod(data->proc, MUIM_Process_Signal, SIGBREAKF_CTRL_E);
    }
    return 0;
}

static IPTR CR__MUIM_CR_SendResult(struct IClass *cl, Object *self, struct MUIP_CR_SendResult *msg)
{
    struct CR_DATA *data = INST_DATA(cl, self);
    CONST_STRPTR name = FilePart(msg->path ? msg->path : (STRPTR)"?");

    ObtainSemaphore(&data->qlock);
    if (data->sending) data->sending--;
    ReleaseSemaphore(&data->qlock);

    if (msg->rc == PKGERR_OK)
    {
        data->sent++;
        snprintf(data->detailbuf, sizeof data->detailbuf, "Sent %s (%ld)%s%s", name, (long)msg->status,
                 msg->reply && msg->reply[0] ? ": " : "", msg->reply && msg->reply[0] ? (char *)msg->reply : "");
    }
    else
    {
        CONST_STRPTR why = pkg_last_error_text(data->ctx);

        data->failed++;
        snprintf(data->detailbuf, sizeof data->detailbuf, "Send failed for %s: %s%s%s", name,
                 pkg_strerror(msg->rc), why && why[0] ? " - " : "", why && why[0] ? why : (CONST_STRPTR)"");
        if (msg->status)
        {
            TEXT st[24];
            snprintf(st, sizeof st, " HTTP %ld", (long)msg->status);
            strlcat(data->detailbuf, st, sizeof data->detailbuf);
        }
    }
    /* one line per attempt on the debug console: the status window is a
       hidden commodity, and on an unattended machine this is the only place
       a failed upload is ever visible */
    bug("[CrashReporter] %s\n", data->detailbuf);
    /* the reply may span lines; keep the first for the status line */
    { STRPTR nl = strchr(data->detailbuf, '\n'); if (nl) *nl = 0; }
    set(data->txt_detail, MUIA_Text_Contents, (IPTR)data->detailbuf);
    update_status(data);

    if (msg->path)  FreeVec(msg->path);
    if (msg->reply) pkg_free_reply(msg->reply);
    return 0;
}

static IPTR CR__MUIM_CR_View(struct IClass *cl, Object *self, Msg msg)
{
    struct CR_DATA *data = INST_DATA(cl, self);

    DoMethod(self, MUIM_CR_Later);
    if (!data->viewtext) return 0;
    set(data->win_view, MUIA_Window_Open, TRUE);
    return 0;
}

/* what the user typed goes into the report the moment the popup is
   dismissed, whichever button did it: the maintainer's "what were you
   doing?" is the one thing the machine cannot capture */
static void flush_description(struct CR_DATA *data)
{
    STRPTR text = NULL;

    if (data->desc_done || !data->latest[0]) return;
    get(data->str_pdesc, MUIA_String_Contents, &text);
    if (!text || !text[0]) return;
    data->desc_done = TRUE;
    if (report_annotate(data->latest, text) != PKGERR_OK)
    {
        set(data->txt_detail, MUIA_Text_Contents, (IPTR)"Could not add the description to the report.");
        return;
    }
    if (data->viewtext)
    {
        ULONG l = strlen(data->viewtext), m = strlen(text);
        STRPTR nt = AllocVec(l + m + 32, MEMF_ANY);
        if (nt)
        {
            snprintf(nt, l + m + 32, "%s[Description]\n%s\n\n", data->viewtext, text);
            FreeVec(data->viewtext);
            data->viewtext = nt;
            set(data->flt_view, MUIA_Floattext_Text, (IPTR)data->viewtext);
        }
    }
}

static IPTR CR__MUIM_CR_Later(struct IClass *cl, Object *self, Msg msg)
{
    struct CR_DATA *data = INST_DATA(cl, self);
    flush_description(data);
    set(data->win_popup, MUIA_Window_Open, FALSE);
    data->popup_open = FALSE;
    return 0;
}

static IPTR CR__MUIM_CR_AutoSend(struct IClass *cl, Object *self, Msg msg)
{
    struct CR_DATA *data = INST_DATA(cl, self);
    IPTR on = 0;

    get(data->chk_auto, MUIA_Selected, &on);
    data->autosend = on ? TRUE : FALSE;
    SetVar(CR_ENV_AUTOSEND, on ? "1" : "0", -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    return 0;
}

static IPTR CR__MUIM_CR_OpenDrawer(struct IClass *cl, Object *self, Msg msg)
{
    TEXT dir[PKG_PATHLEN];

    report_dir(dir, sizeof dir);
    pkg_mkdir_p(dir);
    OpenWorkbenchObject(dir, TAG_DONE);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Construction                                                              */

static IPTR CR__OM_NEW(struct IClass *cl, Object *self, struct opSet *msg)
{
    struct CR_DATA *data;
    struct PkgContext *ctx = (struct PkgContext *)GetTagData(MUIA_CR_Context, 0, msg->ops_AttrList);
    Object *txt_state, *txt_last, *txt_url, *txt_detail, *chk_auto, *btn_sendall, *btn_drawer;
    Object *win_popup, *txt_ptitle, *txt_pbody, *str_pdesc, *btn_pview, *btn_psend, *btn_plater;
    Object *win_view, *flt_view, *lv_view, *btn_vclose;

    if (!ctx) return 0;

    btn_sendall = SimpleButton("_Send unsent reports");
    btn_drawer  = SimpleButton("Open _reports drawer");
    chk_auto    = MUI_MakeObject(MUIO_Checkmark, NULL);

    win_popup = WindowObject,
        MUIA_Window_Title,       (IPTR)"Crash Reporter",
        MUIA_Window_Borderless,  TRUE,
        MUIA_Window_DragBar,     FALSE,
        MUIA_Window_CloseGadget, FALSE,
        MUIA_Window_DepthGadget, FALSE,
        MUIA_Window_SizeGadget,  FALSE,
        MUIA_Window_Activate,    FALSE,
        MUIA_Window_NoMenus,     TRUE,
        MUIA_Window_Width,       MUIV_Window_Width_Screen(POPUP_WIDTH_PCT),
        MUIA_Window_Height,      MUIV_Window_Height_MinMax(0),
        WindowContents, VGroup,
            MUIA_Background, MUII_SHINE,
            MUIA_Frame,      MUIV_Frame_Group,
            MUIA_Group_Spacing, 6,
            Child, txt_ptitle = TextObject, MUIA_Text_Contents, (IPTR)"", End,
            Child, txt_pbody  = TextObject, MUIA_Text_Contents, (IPTR)"", End,
            Child, TextObject, MUIA_Text_Contents, (IPTR)"What were you doing when this happened?", End,
            Child, str_pdesc  = StringObject,
                StringFrame,
                MUIA_String_MaxLen,   CR_DESC_MAXLEN,
                MUIA_String_Contents, (IPTR)"",
                MUIA_CycleChain,      1,
            End,
            Child, HGroup,
                Child, HVSpace,
                Child, btn_pview  = SimpleButton("_View"),
                Child, btn_psend  = SimpleButton("_Send"),
                Child, btn_plater = SimpleButton("_Later"),
            End,
        End,
    End;

    win_view = WindowObject,
        MUIA_Window_Title,  (IPTR)"Crash report",
        MUIA_Window_ID,     MAKE_ID('C','R','V','W'),
        MUIA_Window_Width,  MUIV_Window_Width_Screen(60),
        MUIA_Window_Height, MUIV_Window_Height_Screen(60),
        WindowContents, VGroup,
            Child, lv_view = ListviewObject,
                MUIA_CycleChain,     1,
                MUIA_Listview_Input, FALSE,
                MUIA_Listview_List, flt_view = FloattextObject,
                    ReadListFrame,
                    MUIA_Font, MUIV_Font_Fixed,
                    MUIA_Floattext_Text, (IPTR)"",
                End,
            End,
            Child, HGroup,
                Child, HVSpace,
                Child, btn_vclose = SimpleButton("_Close"),
            End,
        End,
    End;

    self = (Object *)DoSuperNewTags(cl, self, NULL,
        MUIA_Group_Horiz, FALSE,
        Child, TextObject, MUIA_Text_Contents, (IPTR)"\33bCrash Reporter", End,
        Child, ColGroup(2),
            Child, Label("Status:"),
            Child, txt_state = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                MUIA_Text_SetMin, FALSE,
                MUIA_Text_Contents, (IPTR)"Starting...", End,
            Child, Label("Last crash:"),
            Child, txt_last = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                MUIA_Text_SetMin, FALSE,
                MUIA_Text_Contents, (IPTR)"-", End,
            Child, Label("Server:"),
            Child, txt_url = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                MUIA_Text_SetMin, FALSE,
                MUIA_Text_Contents, (IPTR)"", End,
            Child, Label("Send automatically:"),
            Child, HGroup, Child, chk_auto, Child, HVSpace, End,
        End,
        Child, txt_detail = TextObject, MUIA_Text_Contents, (IPTR)"", End,
        Child, HGroup,
            Child, btn_sendall,
            Child, btn_drawer,
        End,
        TAG_MORE, (IPTR)msg->ops_AttrList);
    if (!self)
    {
        if (win_popup) MUI_DisposeObject(win_popup);
        if (win_view)  MUI_DisposeObject(win_view);
        return 0;
    }

    data = INST_DATA(cl, self);
    data->ctx         = ctx;
    data->self        = self;
    data->txt_state   = txt_state;
    data->txt_last    = txt_last;
    data->txt_url     = txt_url;
    data->txt_detail  = txt_detail;
    data->chk_auto    = chk_auto;
    data->btn_sendall = btn_sendall;
    data->btn_drawer  = btn_drawer;
    data->win_popup   = win_popup;
    data->txt_ptitle  = txt_ptitle;
    data->txt_pbody   = txt_pbody;
    data->str_pdesc   = str_pdesc;
    data->btn_pview   = btn_pview;
    data->btn_psend   = btn_psend;
    data->btn_plater  = btn_plater;
    data->win_view    = win_view;
    data->flt_view    = flt_view;
    set(win_view, MUIA_Window_DefaultObject, (IPTR)lv_view);
    InitSemaphore(&data->qlock);
    NewList(&data->queue);

    resolve_url(ctx, data->url, sizeof data->url);
    data->autosend = env_bool(CR_ENV_AUTOSEND);
    SetAttrs(chk_auto, MUIA_NoNotify, TRUE, MUIA_Selected, data->autosend, TAG_DONE);
    bug("[CrashReporter] server '%s' autosend %d\n", data->url, (int)data->autosend);

    data->proc = MUI_NewObject(MUIC_Process,
        MUIA_Process_SourceClass,  (IPTR)cl,
        MUIA_Process_SourceObject, (IPTR)self,
        MUIA_Process_Name,         (IPTR)"CrashReporter sender",
        MUIA_Process_Priority,     -1,
        MUIA_Process_StackSize,    256 * 1024,
        MUIA_Process_AutoLaunch,   TRUE,
        TAG_DONE);
    if (!data->proc || !win_popup || !win_view)
    {
        CoerceMethod(cl, self, OM_DISPOSE);
        return 0;
    }

    DoMethod(btn_sendall, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_CR_SendAll);
    DoMethod(btn_drawer,  MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_CR_OpenDrawer);
    DoMethod(chk_auto,    MUIM_Notify, MUIA_Selected, MUIV_EveryTime, (IPTR)self, 1, MUIM_CR_AutoSend);
    DoMethod(btn_pview,   MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_CR_View);
    DoMethod(btn_psend,   MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_CR_SendLatest);
    DoMethod(btn_plater,  MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_CR_Later);
    DoMethod(win_popup,   MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)self, 1, MUIM_CR_Later);
    DoMethod(btn_vclose,  MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)win_view, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(win_view,    MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)win_view, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    return (IPTR)self;
}

static IPTR CR__OM_DISPOSE(struct IClass *cl, Object *self, Msg msg)
{
    struct CR_DATA *data = INST_DATA(cl, self);
    struct Node *n;

    if (data->proc)
    {
        DoMethod(data->proc, MUIM_Process_Kill, 0);
        MUI_DisposeObject(data->proc);
    }
    while ((n = RemHead(&data->queue))) FreeVec(n);
    if (data->viewtext) FreeVec(data->viewtext);
    return DoSuperMethodA(cl, self, msg);
}

static IPTR CR__OM_GET(struct IClass *cl, Object *self, struct opGet *msg)
{
    struct CR_DATA *data = INST_DATA(cl, self);

    switch (msg->opg_AttrID)
    {
    case MUIA_CR_Popup:  *msg->opg_Storage = (IPTR)data->win_popup; return TRUE;
    case MUIA_CR_Viewer: *msg->opg_Storage = (IPTR)data->win_view;  return TRUE;
    }
    return DoSuperMethodA(cl, self, msg);
}

ZUNE_CUSTOMCLASS_12
(
    CR, NULL, MUIC_Group, NULL,
    OM_NEW,               struct opSet *,
    OM_DISPOSE,           Msg,
    OM_GET,               struct opGet *,
    MUIM_Process_Process, struct MUIP_Process_Process *,
    MUIM_CR_Crash,        struct MUIP_CR_Crash *,
    MUIM_CR_SendLatest,   Msg,
    MUIM_CR_SendAll,      Msg,
    MUIM_CR_SendResult,   struct MUIP_CR_SendResult *,
    MUIM_CR_View,         Msg,
    MUIM_CR_Later,        Msg,
    MUIM_CR_AutoSend,     Msg,
    MUIM_CR_OpenDrawer,   Msg
);

/* ------------------------------------------------------------------------ */
/* Commodity plumbing                                                        */

static Object *app, *win_status, *root;
static struct DiskObject *disko;
static struct Hook broker_hook;
static LONG cx_pri;
static BOOL cx_popup;

AROS_UFH3(static void, broker_func,
    AROS_UFHA(struct Hook *, h,      A0),
    AROS_UFHA(Object *,      object, A2),
    AROS_UFHA(CxMsg *,       msg,    A1))
{
    AROS_USERFUNC_INIT

    if (CxMsgType(msg) == CXM_COMMAND)
    {
        if (CxMsgID(msg) == CXCMD_APPEAR)
        {
            if (XGET(app, MUIA_Application_Iconified))
                set(app, MUIA_Application_Iconified, FALSE);
            set(win_status, MUIA_Window_Open, TRUE);
        }
        else if (CxMsgID(msg) == CXCMD_DISAPPEAR)
        {
            set(win_status, MUIA_Window_Open, FALSE);
            DoMethod(root, MUIM_CR_Later);
        }
    }

    AROS_USERFUNC_EXIT
}

/* CLI: CX_PRIORITY/N,CX_POPUP/S ; WB: the same as tooltypes */
static void parse_args(int argc, char **argv)
{
    CONST_STRPTR cxname = "CrashReporter";

    if (argc)
    {
        IPTR args[2] = { 0, 0 };
        struct RDArgs *rda = ReadArgs("CX_PRIORITY/N,CX_POPUP/S", args, NULL);
        if (rda)
        {
            if (args[0]) cx_pri = *(LONG *)args[0];
            if (args[1]) cx_popup = TRUE;
            FreeArgs(rda);
        }
        cxname = argv[0];
    }
    else
    {
        struct WBStartup *wbs = (struct WBStartup *)argv;
        UBYTE **tt = ArgArrayInit(argc, (UBYTE **)argv);
        cxname   = wbs->sm_ArgList->wa_Name;
        cx_pri   = ArgInt(tt, "CX_PRIORITY", 0);
        cx_popup = strnicmp(ArgString(tt, "CX_POPUP", "NO"), "Y", 1) == 0;
        ArgArrayDone();
    }
    disko = GetDiskObject(cxname);
}

/* ------------------------------------------------------------------------ */

static int real_main(int argc, char **argv)
{
    struct PkgContext *ctx;
    struct CrashRing *ring = NULL;
    Object *win_popup = NULL, *win_view = NULL;
    BYTE crashbit;
    int rc = RETURN_OK;

    parse_args(argc, argv);

    ctx = pkg_open(NULL);
    if (!ctx)
    {
        if (disko) FreeDiskObject(disko);
        return RETURN_FAIL;
    }
    crashbit = AllocSignal(-1);
    if (crashbit < 0)
    {
        pkg_close(ctx);
        if (disko) FreeDiskObject(disko);
        return RETURN_FAIL;
    }
    report_init();

    broker_hook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(broker_func);

    app = ApplicationObject,
        MUIA_Application_Title,       (IPTR)"CrashReporter",
        MUIA_Application_Version,     (IPTR)version,
        MUIA_Application_Copyright,   (IPTR)"Copyright (C) 2026, The AROS Development Team",
        MUIA_Application_Author,      (IPTR)"The AROS Development Team",
        MUIA_Application_Description, (IPTR)"Captures crash reports and sends them to the distribution",
        MUIA_Application_Base,        (IPTR)"CRASHREPORTER",
        MUIA_Application_BrokerPri,   cx_pri,
        MUIA_Application_BrokerHook,  (IPTR)&broker_hook,
        MUIA_Application_SingleTask,  TRUE,
        disko ? MUIA_Application_DiskObject : TAG_IGNORE, (IPTR)disko,
    End;

    /*
     * The window (and the sender process the root object launches) is
     * created only once the application exists.  A SubWindow passed to a
     * SingleTask application that finds another instance is never attached
     * and never disposed: the sender would outlive this program and run
     * into unloaded code.
     */
    if (app)
    {
        win_status = WindowObject,
            MUIA_Window_Title, (IPTR)"Crash Reporter",
            MUIA_Window_ID,    MAKE_ID('C','R','S','T'),
            MUIA_Window_Width, MUIV_Window_Width_Screen(40),
            WindowContents, root = NewObject(CR_CLASS->mcc_Class, NULL,
                MUIA_CR_Context, (IPTR)ctx,
                TAG_DONE),
        End;
        if (win_status)
            DoMethod(app, OM_ADDMEMBER, (IPTR)win_status);
        else
            root = NULL;                    /* a failed window disposes its contents */
    }

    if (app && root)
    {
        ULONG sigs = 0;
        ULONG crashsig = 1UL << crashbit;
        struct CR_DATA *data = INST_DATA(CR_CLASS->mcc_Class, root);

        get(root, MUIA_CR_Popup,  &win_popup);
        get(root, MUIA_CR_Viewer, &win_view);
        DoMethod(app, OM_ADDMEMBER, (IPTR)win_popup);
        DoMethod(app, OM_ADDMEMBER, (IPTR)win_view);

        DoMethod(win_status, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
                 (IPTR)win_status, 3, MUIM_Set, MUIA_Window_Open, FALSE);
        DoMethod(app, MUIM_Notify, MUIA_Application_DoubleStart, TRUE,
                 (IPTR)win_status, 3, MUIM_Set, MUIA_Window_Open, TRUE);

        ring = crashhook_install(FindTask(NULL), crashsig);
        if (!ring)
            set(data->txt_detail, MUIA_Text_Contents, (IPTR)"Hooks not installed (another instance running?)");
        update_status(data);
        set(win_status, MUIA_Window_Open, cx_popup);

        /* leftovers from the last boot go out now when the user asked */
        if (data->autosend) DoMethod(root, MUIM_CR_SendAll);

        for (;;)
        {
            if (DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs) == MUIV_Application_ReturnID_Quit)
            {
                if (crashhook_remove(ring))
                {
                    ring = NULL;
                    break;
                }
                set(data->txt_detail, MUIA_Text_Contents,
                    (IPTR)"Cannot quit: another program patched Alert() after CrashReporter.");
                set(win_status, MUIA_Window_Open, TRUE);
            }
            if (sigs)
            {
                sigs = Wait(sigs | crashsig | SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F);
                if (sigs & SIGBREAKF_CTRL_C)
                {
                    if (crashhook_remove(ring)) { ring = NULL; break; }
                    set(data->txt_detail, MUIA_Text_Contents,
                        (IPTR)"Cannot quit: another program patched Alert() after CrashReporter.");
                    set(win_status, MUIA_Window_Open, TRUE);
                }
                if (sigs & SIGBREAKF_CTRL_F) set(win_status, MUIA_Window_Open, TRUE);
                if ((sigs & crashsig) && ring)
                {
                    struct CrashRecord rec;
                    while (crashhook_next(ring, &rec))
                        DoMethod(root, MUIM_CR_Crash, (IPTR)&rec);
                }
            }
        }
        set(win_status, MUIA_Window_Open, FALSE);
        set(win_popup,  MUIA_Window_Open, FALSE);
        set(win_view,   MUIA_Window_Open, FALSE);
    }
    else
        rc = RETURN_FAIL;

    if (ring) crashhook_remove(ring);
    if (app) MUI_DisposeObject(app);
    report_exit();
    FreeSignal(crashbit);
    pkg_close(ctx);
    if (disko) FreeDiskObject(disko);
    return rc;
}

#define CR_STACK_MIN  (128 * 1024)
#define CR_STACK_SIZE (256 * 1024)

struct cr_args { int argc; char **argv; };

static IPTR cr_stack_entry(struct cr_args *a)
{
    return (IPTR)real_main(a->argc, a->argv);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    IPTR have = (IPTR)me->tc_SPUpper - (IPTR)me->tc_SPLower;
    struct StackSwapStruct sss;
    struct StackSwapArgs args;
    struct cr_args a = { argc, argv };
    APTR stack;
    int rc;

    if (have >= CR_STACK_MIN)
        return real_main(argc, argv);

    stack = AllocMem(CR_STACK_SIZE, MEMF_ANY);
    if (!stack)
        return real_main(argc, argv);

    args.Args[0] = (IPTR)&a;
    sss.stk_Lower   = stack;
    sss.stk_Upper   = (APTR)((IPTR)stack + CR_STACK_SIZE);
    sss.stk_Pointer = sss.stk_Upper;
    rc = (int)NewStackSwap(&sss, (APTR)cr_stack_entry, &args);
    FreeMem(stack, CR_STACK_SIZE);
    return rc;
}
