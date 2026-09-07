/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: UpdateNotifier — background update/announcement notifier for the
          Software Center (SYS:WBStartup/UpdateNotifier, DESIGN.md §1.3).

          Zune commodity modelled on tools/commodities/Exchange.c: broker
          hook, DiskObject for iconify, no window at start, never quits on
          its own.  A timer.device request fires every CheckInterval
          minutes (prefs; first check FIRSTCHECK seconds after start); the
          poll itself (pkg_check(): conditional GET on Release, PROTOCOL.md
          §6) runs on a MUIC_Process worker so the Wanderer-facing task
          never blocks on the network.  When updates or an unseen
          announcement appear, a small borderless popup opens top-right
          with [Open] [Later]; Open launches SYS:System/SoftwareCenter via
          OpenWorkbenchObject().  Critical announcements are not
          auto-dismissed.  A status window (CX_POPUP / Exchange "Show")
          shows the last result and offers Check now.

    AI_PHASE: SOFTWARE_CENTER_NOTIFIER
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: MEDIUM
    AI_NOTE: No notification-area concept exists in the tree; this is the
             first one, kept inside the tool so it can become
             notification.library later.  All PkgContext access happens
             either on the worker (during a check) or on the main task while
             the worker is idle (busy == FALSE), like SoftwareCenter.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/muimaster.h>
#include <proto/icon.h>
#include <proto/commodities.h>
#include <proto/workbench.h>
#include <proto/timer.h>
#include <proto/alib.h>

#include <exec/memory.h>
#include <devices/timer.h>
#include <intuition/screens.h>
#include <libraries/mui.h>
#include <libraries/commodities.h>
#include <workbench/startup.h>
#include <zune/customclasses.h>

#include <stdio.h>
#include <string.h>

#include "pkg.h"

#define UN_VERSTR "1.0"
static const char version[] __attribute__((used)) = "$VER: UpdateNotifier " UN_VERSTR " (06.09.2026)";

#define SC_PATH          "SYS:System/SoftwareCenter"
#define SC_PATH_VAR      "SoftwareCenter/Program" /* ENV: override of SC_PATH  */
#define POPUP_WRAP       52           /* body wrap column (Text does not)   */
#define DEF_FIRSTCHECK   120          /* seconds after start (network up)   */
#define POPUP_DISMISS    30           /* seconds, non-critical popups       */
#define POPUP_WIDTH_PCT  30           /* of screen width                    */
#define POPUP_MARGIN     12

/* ------------------------------------------------------------------------ */
/* Methods                                                                   */

#define MUIB_UN            (TAG_USER | 0x5C720000)
#define MUIA_UN_Context    (MUIB_UN | 0xFF)   /* [I..] struct PkgContext *   */
#define MUIA_UN_Popup      (MUIB_UN | 0xFE)   /* [..G] the popup window      */
#define MUIA_UN_PopupState (MUIB_UN | 0xFD)   /* [..G] POPUP_#?              */
#define MUIM_UN_Check      (MUIB_UN | 0x01)   /* start a poll (if idle)      */
#define MUIM_UN_Result     (MUIB_UN | 0x02)   /* pushed by the worker        */
#define MUIM_UN_Open       (MUIB_UN | 0x03)   /* launch SoftwareCenter       */
#define MUIM_UN_Later      (MUIB_UN | 0x04)   /* close the popup             */
#define MUIM_UN_Dismiss    (MUIB_UN | 0x05)   /* auto-close (timer)          */

enum { POPUP_CLOSED, POPUP_OPEN, POPUP_CRITICAL };

struct MUIP_UN_Result
{
    STACKED ULONG MethodID;
    STACKED LONG  rc;
    STACKED ULONG updates;
    STACKED ULONG newann;
    STACKED ULONG annid;      /* newest unseen announcement id, 0 = none    */
    STACKED STRPTR anntitle;  /* AllocVec'ed by the worker, freed here      */
    STACKED STRPTR annbody;
    STACKED ULONG critical;
};

struct UN_DATA
{
    struct PkgContext *ctx;
    struct Hook        pkghook;
    Object            *proc;                 /* MUIC_Process worker         */
    Object            *self;
    BOOL               busy;
    volatile ULONG     abort;

    /* status window */
    Object *win_status, *txt_state, *txt_last, *txt_detail, *btn_check, *btn_open;
    /* popup */
    Object *win_popup, *txt_ptitle, *txt_pbody, *btn_popen, *btn_plater;
    BOOL    popup_open;
    BOOL    critical;

    /* what has already been announced this session, so "Later" does not
       nag every interval with the same news */
    ULONG   shown_updates;
    ULONG   shown_annid;

    TEXT    statebuf[96];
    TEXT    lastbuf[96];
    TEXT    detailbuf[160];
    TEXT    ptitlebuf[160];
    TEXT    pbodybuf[512];
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */

/* Word-wrap src into dst at col: Zune's Text object never wraps, so a long
   announcement body would push the popup past its screen-relative width.
   Existing newlines are kept. */
static void wrap_text(STRPTR dst, ULONG dstlen, CONST_STRPTR src, ULONG col)
{
    ULONG o = 0, line = 0;

    while (*src && o + 1 < dstlen)
    {
        CONST_STRPTR e = src;
        ULONG wl;
        if (*src == '\n') { dst[o++] = '\n'; line = 0; src++; continue; }
        if (*src == ' ')  { src++; if (line) { dst[o++] = ' '; line++; } continue; }
        while (*e && *e != ' ' && *e != '\n') e++;
        wl = (ULONG)(e - src);
        if (line && line + wl > col)
        {
            if (dst[o - 1] == ' ') o--;
            dst[o++] = '\n';
            line = 0;
        }
        while (src < e && o + 1 < dstlen) { dst[o++] = *src++; line++; }
    }
    dst[o] = 0;
}

static STRPTR dupstr(CONST_STRPTR s)
{
    STRPTR d;
    if (!s) s = "";
    d = AllocVec(strlen(s) + 1, MEMF_ANY);
    if (d) strcpy(d, s);
    return d;
}

static void stamp_now(STRPTR buf, ULONG len)
{
    struct DateTime dt;
    TEXT date[LEN_DATSTRING], time[LEN_DATSTRING];

    DateStamp(&dt.dat_Stamp);
    dt.dat_Format  = FORMAT_DOS;
    dt.dat_Flags   = 0;
    dt.dat_StrDay  = NULL;
    dt.dat_StrDate = date;
    dt.dat_StrTime = time;
    if (DateToStr(&dt))
        snprintf(buf, len, "%s %s", date, time);
    else
        strlcpy(buf, "-", len);
}

/* libpkg hook: runs on the worker; only the abort query matters here */
AROS_UFH3S(IPTR, pkghook_func,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(APTR, ctx, A2),
    AROS_UFHA(struct PkgHookMsg *, msg, A1))
{
    AROS_USERFUNC_INIT

    struct UN_DATA *data = (struct UN_DATA *)hook->h_Data;

    if (msg->Type == PKGH_ABORTQUERY)
        return data->abort;
    return 0;

    AROS_USERFUNC_EXIT
}

/* ------------------------------------------------------------------------ */
/* Worker                                                                    */

static void worker_check(Object *self, struct UN_DATA *data)
{
    ULONG updates = 0, newann = 0, annid = 0, critical = 0;
    STRPTR title = NULL, body = NULL;
    LONG rc;

    rc = pkg_check(data->ctx, &updates, &newann);
    if (rc == 0 && newann)
    {
        struct List anns;
        struct PkgAnnouncement *a, *newest = NULL;

        if (pkg_announcements(data->ctx, TRUE, &anns) == 0)
        {
            ForeachNode(&anns, a)
            {
                if (!newest || a->Id > newest->Id) newest = a;
                if (a->Severity && Stricmp(a->Severity, "critical") == 0) critical = 1;
            }
            if (newest)
            {
                annid = newest->Id;
                title = dupstr(newest->Title);
                body  = dupstr(newest->Body);
            }
            pkg_free_announcements(&anns);
        }
    }
    DoMethod(_app(self), MUIM_Application_PushMethod, (IPTR)self, 8, MUIM_UN_Result,
             rc, updates, newann, annid, (IPTR)title, (IPTR)body, critical);
}

/* MUIM_Process_Process — runs on the sub process */
static IPTR UN__MUIM_Process_Process(struct IClass *cl, Object *self, struct MUIP_Process_Process *msg)
{
    struct UN_DATA *data = INST_DATA(cl, self);
    struct Process *me = (struct Process *)FindTask(NULL);

    me->pr_WindowPtr = (APTR)-1;                  /* no DOS requesters      */

    while (!*msg->kill)
    {
        ULONG sigs = Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E);
        if ((sigs & SIGBREAKF_CTRL_C) || *msg->kill) break;
        if (sigs & SIGBREAKF_CTRL_E)
            worker_check(self, data);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Popup placement: top-right of the default public screen                  */

static void place_popup(struct UN_DATA *data)
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

/* ------------------------------------------------------------------------ */
/* Methods on the main task                                                  */

static IPTR UN__MUIM_UN_Check(struct IClass *cl, Object *self, Msg msg)
{
    struct UN_DATA *data = INST_DATA(cl, self);

    if (data->busy) return 0;
    data->busy = TRUE;
    set(data->btn_check, MUIA_Disabled, TRUE);
    set(data->txt_state, MUIA_Text_Contents, (IPTR)"Checking for updates...");
    DoMethod(data->proc, MUIM_Process_Signal, SIGBREAKF_CTRL_E);
    return 0;
}

static IPTR UN__MUIM_UN_Result(struct IClass *cl, Object *self, struct MUIP_UN_Result *msg)
{
    struct UN_DATA *data = INST_DATA(cl, self);
    struct PkgPrefs *prefs = pkg_prefs(data->ctx);
    BOOL show = FALSE;

    data->busy = FALSE;
    set(data->btn_check, MUIA_Disabled, FALSE);
    stamp_now(data->lastbuf, sizeof data->lastbuf);
    set(data->txt_last, MUIA_Text_Contents, (IPTR)data->lastbuf);

    if (msg->rc)
    {
        snprintf(data->statebuf, sizeof data->statebuf, "Check failed: %s",
                 PKG_STR(pkg_strerror(msg->rc), "?"));
        strlcpy(data->detailbuf, PKG_STR(pkg_last_error_text(data->ctx), ""), sizeof data->detailbuf);
    }
    else
    {
        if (msg->updates == 0 && msg->newann == 0)
            strlcpy(data->statebuf, "Everything is up to date.", sizeof data->statebuf);
        else
            snprintf(data->statebuf, sizeof data->statebuf, "%lu update%s available, %lu new announcement%s",
                     (unsigned long)msg->updates, msg->updates == 1 ? "" : "s",
                     (unsigned long)msg->newann,  msg->newann  == 1 ? "" : "s");
        data->detailbuf[0] = 0;

        if (prefs->Notify)
        {
            if (msg->updates && msg->updates != data->shown_updates)
            {
                snprintf(data->ptitlebuf, sizeof data->ptitlebuf, "\33b%lu software update%s available",
                         (unsigned long)msg->updates, msg->updates == 1 ? "" : "s");
                strlcpy(data->pbodybuf, "Open Software Center to review and install them.", sizeof data->pbodybuf);
                data->critical = FALSE;
                show = TRUE;
            }
            if (prefs->NotifyAnnouncements && msg->annid && msg->annid > data->shown_annid)
            {
                /* an announcement wins the popup: it carries real text */
                snprintf(data->ptitlebuf, sizeof data->ptitlebuf, "\33b%s", PKG_STR(msg->anntitle, "Announcement"));
                wrap_text(data->pbodybuf, sizeof data->pbodybuf, PKG_STR(msg->annbody, ""), POPUP_WRAP);
                if (msg->updates)
                {
                    TEXT tail[64];
                    snprintf(tail, sizeof tail, "\n\n(%lu update%s also available)",
                             (unsigned long)msg->updates, msg->updates == 1 ? "" : "s");
                    strlcat(data->pbodybuf, tail, sizeof data->pbodybuf);
                }
                data->critical = msg->critical ? TRUE : FALSE;
                show = TRUE;
            }
        }
        data->shown_updates = msg->updates;
        if (msg->annid > data->shown_annid) data->shown_annid = msg->annid;
    }
    set(data->txt_state,  MUIA_Text_Contents, (IPTR)data->statebuf);
    set(data->txt_detail, MUIA_Text_Contents, (IPTR)data->detailbuf);
    set(data->btn_open,   MUIA_Disabled, FALSE);

    if (show)
    {
        set(data->txt_ptitle, MUIA_Text_Contents, (IPTR)data->ptitlebuf);
        set(data->txt_pbody,  MUIA_Text_Contents, (IPTR)data->pbodybuf);
        place_popup(data);
        set(data->win_popup, MUIA_Window_Open, TRUE);
        data->popup_open = TRUE;
    }

    if (msg->anntitle) FreeVec(msg->anntitle);
    if (msg->annbody)  FreeVec(msg->annbody);
    return 0;
}

static IPTR UN__MUIM_UN_Later(struct IClass *cl, Object *self, Msg msg)
{
    struct UN_DATA *data = INST_DATA(cl, self);
    set(data->win_popup, MUIA_Window_Open, FALSE);
    data->popup_open = FALSE;
    return 0;
}

static IPTR UN__MUIM_UN_Dismiss(struct IClass *cl, Object *self, Msg msg)
{
    struct UN_DATA *data = INST_DATA(cl, self);
    if (data->popup_open && !data->critical)
        DoMethod(self, MUIM_UN_Later);
    return 0;
}

static IPTR UN__MUIM_UN_Open(struct IClass *cl, Object *self, Msg msg)
{
    struct UN_DATA *data = INST_DATA(cl, self);

    TEXT path[128];

    DoMethod(self, MUIM_UN_Later);
    /* ENV:SoftwareCenter/Program points at a GUI that is not in SYS:System
       (a development build, a live CD with the tools elsewhere) */
    if (GetVar(SC_PATH_VAR, path, sizeof path, GVF_GLOBAL_ONLY) <= 0)
        strlcpy(path, SC_PATH, sizeof path);
    /* a running instance sees this as MUIA_Application_DoubleStart and
       raises its window; otherwise a new one starts */
    if (!OpenWorkbenchObject(path, TAG_DONE))
    {
        snprintf(data->detailbuf, sizeof data->detailbuf, "Could not start %s", path);
        set(data->txt_detail, MUIA_Text_Contents, (IPTR)data->detailbuf);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Construction                                                              */

static IPTR UN__OM_NEW(struct IClass *cl, Object *self, struct opSet *msg)
{
    struct UN_DATA *data;
    struct PkgContext *ctx = (struct PkgContext *)GetTagData(MUIA_UN_Context, 0, msg->ops_AttrList);
    Object *txt_state, *txt_last, *txt_detail, *btn_check, *btn_open;
    Object *win_popup, *txt_ptitle, *txt_pbody, *btn_popen, *btn_plater;

    if (!ctx) return 0;

    btn_check = SimpleButton("Check _now");
    btn_open  = SimpleButton("_Open Software Center");

    /* the popup is a second window owned by the application, built here so
       the group can drive it; it is added to the app in real_main() */
    win_popup = WindowObject,
        MUIA_Window_Title,       (IPTR)"Software Center",
        /* no Window_ID: a remembered position must not beat place_popup() */
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
            /* a card, not a window: light fill + group frame so it reads
               as a notification on a grey Workbench */
            MUIA_Background, MUII_SHINE,
            MUIA_Frame,      MUIV_Frame_Group,
            MUIA_Group_Spacing, 6,
            Child, txt_ptitle = TextObject, MUIA_Text_Contents, (IPTR)"", End,
            Child, txt_pbody  = TextObject, MUIA_Text_Contents, (IPTR)"", End,
            Child, HGroup,
                Child, HVSpace,
                Child, btn_popen  = SimpleButton("_Open"),
                Child, btn_plater = SimpleButton("_Later"),
            End,
        End,
    End;

    self = (Object *)DoSuperNewTags(cl, self, NULL,
        MUIA_Group_Horiz, FALSE,
        Child, TextObject, MUIA_Text_Contents, (IPTR)"\33bSoftware Center update notifier", End,
        Child, ColGroup(2),
            Child, Label("Status:"),
            Child, txt_state = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                MUIA_Text_SetMin, FALSE,               /* results are longer */
                MUIA_Text_Contents, (IPTR)"Waiting for the first check.", End,
            Child, Label("Last check:"),
            Child, txt_last = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                MUIA_Text_Contents, (IPTR)"-", End,
        End,
        Child, txt_detail = TextObject, MUIA_Text_Contents, (IPTR)"", End,
        Child, HGroup,
            Child, btn_check,
            Child, btn_open,
        End,
        TAG_MORE, (IPTR)msg->ops_AttrList);
    if (!self)
    {
        if (win_popup) MUI_DisposeObject(win_popup);
        return 0;
    }

    data = INST_DATA(cl, self);
    data->ctx        = ctx;
    data->self       = self;
    data->txt_state  = txt_state;
    data->txt_last   = txt_last;
    data->txt_detail = txt_detail;
    data->btn_check  = btn_check;
    data->btn_open   = btn_open;
    data->win_popup  = win_popup;
    data->txt_ptitle = txt_ptitle;
    data->txt_pbody  = txt_pbody;
    data->btn_popen  = btn_popen;
    data->btn_plater = btn_plater;

    data->pkghook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(pkghook_func);
    data->pkghook.h_Data  = data;

    data->proc = MUI_NewObject(MUIC_Process,
        MUIA_Process_SourceClass,  (IPTR)cl,
        MUIA_Process_SourceObject, (IPTR)self,
        MUIA_Process_Name,         (IPTR)"UpdateNotifier worker",
        MUIA_Process_Priority,     -1,
        MUIA_Process_StackSize,    256 * 1024,
        MUIA_Process_AutoLaunch,   TRUE,
        TAG_DONE);
    if (!data->proc || !win_popup)
    {
        CoerceMethod(cl, self, OM_DISPOSE);
        return 0;
    }

    DoMethod(btn_check,  MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_UN_Check);
    DoMethod(btn_open,   MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_UN_Open);
    DoMethod(btn_popen,  MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_UN_Open);
    DoMethod(btn_plater, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)self, 1, MUIM_UN_Later);
    DoMethod(win_popup,  MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)self, 1, MUIM_UN_Later);
    return (IPTR)self;
}

static IPTR UN__OM_DISPOSE(struct IClass *cl, Object *self, Msg msg)
{
    struct UN_DATA *data = INST_DATA(cl, self);

    if (data->proc)
    {
        data->abort = 1;
        DoMethod(data->proc, MUIM_Process_Kill, 0);
        MUI_DisposeObject(data->proc);
    }
    /* win_popup belongs to the application once added there */
    return DoSuperMethodA(cl, self, msg);
}

static IPTR UN__OM_GET(struct IClass *cl, Object *self, struct opGet *msg)
{
    struct UN_DATA *data = INST_DATA(cl, self);

    switch (msg->opg_AttrID)
    {
    case MUIA_UN_Popup:
        *msg->opg_Storage = (IPTR)data->win_popup;
        return TRUE;
    case MUIA_UN_PopupState:
        *msg->opg_Storage = !data->popup_open ? POPUP_CLOSED
                          : data->critical    ? POPUP_CRITICAL : POPUP_OPEN;
        return TRUE;
    }
    return DoSuperMethodA(cl, self, msg);
}

ZUNE_CUSTOMCLASS_9
(
    UN, NULL, MUIC_Group, NULL,
    OM_NEW,               struct opSet *,
    OM_DISPOSE,           Msg,
    OM_GET,               struct opGet *,
    MUIM_Process_Process, struct MUIP_Process_Process *,
    MUIM_UN_Check,        Msg,
    MUIM_UN_Result,       struct MUIP_UN_Result *,
    MUIM_UN_Open,         Msg,
    MUIM_UN_Later,        Msg,
    MUIM_UN_Dismiss,      Msg
);

/* ------------------------------------------------------------------------ */
/* Commodity plumbing                                                        */

static Object *app, *win_status, *root;
static struct DiskObject *disko;
static struct Hook broker_hook;
static LONG cx_pri;
static BOOL cx_popup;
static ULONG firstcheck = DEF_FIRSTCHECK;

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
            DoMethod(root, MUIM_UN_Later);
        }
    }

    AROS_USERFUNC_EXIT
}

/* CLI: CX_PRIORITY/N,CX_POPUP/S,FIRSTCHECK/N/K ; WB: the same as tooltypes */
static void parse_args(int argc, char **argv)
{
    CONST_STRPTR cxname = "UpdateNotifier";

    if (argc)
    {
        IPTR args[3] = { 0, 0, 0 };
        struct RDArgs *rda = ReadArgs("CX_PRIORITY/N,CX_POPUP/S,FIRSTCHECK/N/K", args, NULL);
        if (rda)
        {
            if (args[0]) cx_pri = *(LONG *)args[0];
            if (args[1]) cx_popup = TRUE;
            if (args[2]) firstcheck = *(LONG *)args[2];
            FreeArgs(rda);
        }
        cxname = argv[0];
    }
    else
    {
        struct WBStartup *wbs = (struct WBStartup *)argv;
        UBYTE **tt = ArgArrayInit(argc, (UBYTE **)argv);
        cxname = wbs->sm_ArgList->wa_Name;
        cx_pri     = ArgInt(tt, "CX_PRIORITY", 0);
        cx_popup   = strnicmp(ArgString(tt, "CX_POPUP", "NO"), "Y", 1) == 0;
        firstcheck = ArgInt(tt, "FIRSTCHECK", DEF_FIRSTCHECK);
        ArgArrayDone();
    }
    disko = GetDiskObject(cxname);
}

/* ------------------------------------------------------------------------ */
/* Timers: one request for the poll interval, one for popup auto-dismiss    */

struct UNTimer
{
    struct MsgPort     *port;
    struct timerequest *req;
    BOOL                pending;
};

static BOOL timer_open(struct UNTimer *t)
{
    t->port = CreateMsgPort();
    if (!t->port) return FALSE;
    t->req = (struct timerequest *)CreateIORequest(t->port, sizeof(struct timerequest));
    if (!t->req) return FALSE;
    if (OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest *)t->req, 0))
    {
        DeleteIORequest((struct IORequest *)t->req);
        t->req = NULL;
        return FALSE;
    }
    return TRUE;
}

static void timer_start(struct UNTimer *t, ULONG secs)
{
    if (t->pending)
    {
        AbortIO((struct IORequest *)t->req);
        WaitIO((struct IORequest *)t->req);
        t->pending = FALSE;
    }
    if (!secs) return;
    t->req->tr_node.io_Command = TR_ADDREQUEST;
    t->req->tr_time.tv_secs    = secs;
    t->req->tr_time.tv_micro   = 0;
    SendIO((struct IORequest *)t->req);
    t->pending = TRUE;
}

static BOOL timer_fired(struct UNTimer *t)
{
    if (t->pending && GetMsg(t->port))
    {
        t->pending = FALSE;
        return TRUE;
    }
    return FALSE;
}

static void timer_close(struct UNTimer *t)
{
    if (t->req)
    {
        timer_start(t, 0);
        CloseDevice((struct IORequest *)t->req);
        DeleteIORequest((struct IORequest *)t->req);
    }
    if (t->port) DeleteMsgPort(t->port);
}

/* ------------------------------------------------------------------------ */

static int real_main(int argc, char **argv)
{
    struct PkgContext *ctx;
    struct UNTimer poll = { 0 }, dismiss = { 0 };
    Object *win_popup = NULL;
    int rc = RETURN_OK;

    parse_args(argc, argv);

    ctx = pkg_open(NULL);
    if (!ctx)
    {
        if (disko) FreeDiskObject(disko);
        return RETURN_FAIL;                 /* nothing to notify about     */
    }
    if (!timer_open(&poll) || !timer_open(&dismiss))
    {
        timer_close(&poll); timer_close(&dismiss);
        pkg_close(ctx);
        if (disko) FreeDiskObject(disko);
        return RETURN_FAIL;
    }

    broker_hook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(broker_func);

    app = ApplicationObject,
        MUIA_Application_Title,       (IPTR)"UpdateNotifier",
        MUIA_Application_Version,     (IPTR)version,
        MUIA_Application_Copyright,   (IPTR)"Copyright (C) 2026, The AROS Development Team",
        MUIA_Application_Author,      (IPTR)"The AROS Development Team",
        MUIA_Application_Description, (IPTR)"Notifies about software updates and announcements",
        MUIA_Application_Base,        (IPTR)"UPDATENOTIFIER",
        MUIA_Application_BrokerPri,   cx_pri,
        MUIA_Application_BrokerHook,  (IPTR)&broker_hook,
        MUIA_Application_SingleTask,  TRUE,
        disko ? MUIA_Application_DiskObject : TAG_IGNORE, (IPTR)disko,
        SubWindow, win_status = WindowObject,
            MUIA_Window_Title, (IPTR)"Update Notifier",
            MUIA_Window_ID,    MAKE_ID('U','N','S','T'),
            MUIA_Window_Width, MUIV_Window_Width_Screen(36),
            WindowContents, root = NewObject(UN_CLASS->mcc_Class, NULL,
                MUIA_UN_Context, (IPTR)ctx,
                TAG_DONE),
        End,
    End;

    if (app && root)
    {
        ULONG sigs = 0;
        ULONG pollsig = 1UL << poll.port->mp_SigBit;
        ULONG dismisssig = 1UL << dismiss.port->mp_SigBit;
        struct PkgPrefs *prefs = pkg_prefs(ctx);
        IPTR popstate, popwas = POPUP_CLOSED;

        get(root, MUIA_UN_Popup, &win_popup);
        DoMethod(app, OM_ADDMEMBER, (IPTR)win_popup);

        DoMethod(win_status, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
                 (IPTR)win_status, 3, MUIM_Set, MUIA_Window_Open, FALSE);
        DoMethod(app, MUIM_Notify, MUIA_Application_DoubleStart, TRUE,
                 (IPTR)win_status, 3, MUIM_Set, MUIA_Window_Open, TRUE);

        set(win_status, MUIA_Window_Open, cx_popup);
        timer_start(&poll, firstcheck);

        while (DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs) != MUIV_Application_ReturnID_Quit)
        {
            /* arm the auto-dismiss when a non-critical popup has just opened */
            get(root, MUIA_UN_PopupState, &popstate);
            if (popstate == POPUP_OPEN && popwas == POPUP_CLOSED)
                timer_start(&dismiss, POPUP_DISMISS);
            popwas = popstate;

            if (sigs)
            {
                sigs = Wait(sigs | pollsig | dismisssig | SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F);
                if (sigs & SIGBREAKF_CTRL_C) break;
                if (sigs & SIGBREAKF_CTRL_F) set(win_status, MUIA_Window_Open, TRUE);
                if ((sigs & pollsig) && timer_fired(&poll))
                {
                    DoMethod(root, MUIM_UN_Check);
                    /* prefs may have been changed by SoftwareCenter meanwhile */
                    timer_start(&poll, prefs->CheckInterval ? prefs->CheckInterval * 60 : 0);
                }
                if ((sigs & dismisssig) && timer_fired(&dismiss))
                    DoMethod(root, MUIM_UN_Dismiss);
            }
        }
        set(win_status, MUIA_Window_Open, FALSE);
        set(win_popup,  MUIA_Window_Open, FALSE);
    }
    else
        rc = RETURN_FAIL;

    if (app) MUI_DisposeObject(app);
    timer_close(&poll);
    timer_close(&dismiss);
    pkg_close(ctx);
    if (disko) FreeDiskObject(disko);
    return rc;
}

/* Same stack story as SoftwareCenter: Zune + a worker want more than the
   CLI default, and WBStartup honours the icon STACK only if set. */
#define UN_STACK_MIN  (128 * 1024)
#define UN_STACK_SIZE (256 * 1024)

struct un_args { int argc; char **argv; };

static IPTR un_stack_entry(struct un_args *a)
{
    return (IPTR)real_main(a->argc, a->argv);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    IPTR have = (IPTR)me->tc_SPUpper - (IPTR)me->tc_SPLower;
    struct StackSwapStruct sss;
    struct StackSwapArgs args;
    struct un_args a = { argc, argv };
    APTR stack;
    int rc;

    if (have >= UN_STACK_MIN)
        return real_main(argc, argv);

    stack = AllocMem(UN_STACK_SIZE, MEMF_ANY);
    if (!stack)
        return real_main(argc, argv);

    args.Args[0] = (IPTR)&a;
    sss.stk_Lower   = stack;
    sss.stk_Upper   = (APTR)((IPTR)stack + UN_STACK_SIZE);
    sss.stk_Pointer = sss.stk_Upper;
    rc = (int)NewStackSwap(&sss, (APTR)un_stack_entry, &args);
    FreeMem(stack, UN_STACK_SIZE);
    return rc;
}
