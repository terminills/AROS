/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Notification server. Shows the pop-ups that application.library's
          Notify() asks for, and takes the same requests over ARexx at the
          RINGHIO port, so AmigaOS 4 scripts work unchanged.
*/

#include <aros/debug.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/intuitionbase.h>
#include <graphics/rastport.h>
#include <datatypes/datatypes.h>
#include <datatypes/pictureclass.h>
#include <cybergraphx/cybergraphics.h>
#include <rexx/storage.h>
#include <rexx/errors.h>
#include <libraries/application.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/datatypes.h>
#include <proto/cybergraphics.h>
#include <proto/rexxsyslib.h>
#include <proto/application.h>
#include <proto/openurl.h>
#include <proto/utility.h>

#include <string.h>

#include "../../libs/application/application_server.h"

#define VERSTAG "\0$VER: Notifications 1.0 (18.10.2026)"
static const char version[] __attribute__((used)) = VERSTAG;

#define REXX_PORTNAME   "RINGHIO"
#define POPUP_WIDTH     320
#define POPUP_PAD       8
#define POPUP_GAP       6
#define POPUP_MARGIN    16
#define POPUP_IMAGE     32
#define TICK_MICROS     250000
#define MAX_LINES       8

struct Library *ApplicationBase;
struct Library *OpenURLBase;
struct Library *CyberGfxBase;
struct Library *DataTypesBase;
struct RxsLib *RexxSysBase;

struct Popup
{
    struct MinNode   p_Node;
    struct Window   *p_Window;
    struct Screen   *p_Screen;      /* locked while the pop-up is open */
    struct TextFont *p_Font;
    ULONG            p_AppID;
    LONG             p_TicksLeft;
    BOOL             p_CloseOnDC;
    ULONG            p_LastSecs, p_LastMicros;
    ULONG           *p_ARGB;        /* image, POPUP_IMAGE x POPUP_IMAGE, or NULL */
    UWORD            p_ImgW, p_ImgH;
    UBYTE            p_VAlign;
    UWORD            p_NumLines;
    UWORD            p_LineStart[MAX_LINES], p_LineLen[MAX_LINES];
    TEXT             p_Title[APPNOTIFY_TITLE_MAX + 1];
    TEXT             p_Text[APPNOTIFY_TEXT_MAX + 1];
    TEXT             p_BackMsg[NOTIFYSERVER_BACKMAX];
};

/* Applications registered on behalf of ARexx scripts (REGISTERAPP). */
struct RexxApp
{
    struct MinNode ra_Node;
    ULONG          ra_AppID;
    TEXT           ra_Name[APPNOTIFY_TITLE_MAX + 1];
};

static struct MinList popups, rexxapps;
static struct TextAttr topaz8 = { "topaz.font", 8, FS_NORMAL, FPF_ROMFONT };
static struct MsgPort *idcmpport;

/***************************************************************************/

static void CopyStr(STRPTR dst, CONST_STRPTR src, ULONG size)
{
    if (!src)
        src = "";
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/* "FRONT" is the frontmost screen if it is public, otherwise Workbench. */
static struct Screen *LockTargetScreen(CONST_STRPTR name)
{
    struct Screen *scr = NULL;

    if (name && *name && Stricmp(name, "FRONT") == 0)
    {
        struct List *list;
        struct PubScreenNode *psn;
        struct Screen *front;
        TEXT pubname[MAXPUBSCREENNAME + 1] = "";
        ULONG ilock = LockIBase(0);

        front = IntuitionBase->FirstScreen;
        UnlockIBase(ilock);

        list = LockPubScreenList();
        ForeachNode(list, psn)
        {
            if (psn->psn_Screen == front)
            {
                CopyStr(pubname, psn->psn_Node.ln_Name, sizeof(pubname));
                break;
            }
        }
        UnlockPubScreenList();

        if (pubname[0])
            scr = LockPubScreen(pubname);
    }
    else if (name && *name)
        scr = LockPubScreen(name);

    if (!scr)
        scr = LockPubScreen(NULL);
    return scr;
}

/* Load a picture as ARGB, scaled down to fit POPUP_IMAGE x POPUP_IMAGE. */
static void LoadImage(struct Popup *p, CONST_STRPTR file)
{
    struct BitMapHeader *bmh = NULL;
    ULONG *src;
    Object *o;
    UWORD w, h, x, y, ow, oh;

    if (!file || !*file || !DataTypesBase)
        return;

    if (!(o = NewDTObject((APTR)file,
                          DTA_GroupID,      GID_PICTURE,
                          PDTA_DestMode,    PMODE_V43,
                          PDTA_Remap,       FALSE,
                          TAG_DONE)))
        return;

    if (GetDTAttrs(o, PDTA_BitMapHeader, (IPTR)&bmh, TAG_DONE) && bmh
        && bmh->bmh_Width && bmh->bmh_Height)
    {
        w = bmh->bmh_Width;
        h = bmh->bmh_Height;
        if ((src = AllocVec(w * h * 4, MEMF_ANY)))
        {
            if (DoMethod(o, PDTM_READPIXELARRAY, (IPTR)src, PBPAFMT_ARGB, w * 4, 0, 0, w, h))
            {
                ow = w > POPUP_IMAGE ? POPUP_IMAGE : w;
                oh = h > POPUP_IMAGE ? POPUP_IMAGE : h;
                if (w > POPUP_IMAGE || h > POPUP_IMAGE)
                {
                    /* keep the aspect ratio */
                    if (w >= h)
                        oh = (h * POPUP_IMAGE) / w ? (h * POPUP_IMAGE) / w : 1;
                    else
                        ow = (w * POPUP_IMAGE) / h ? (w * POPUP_IMAGE) / h : 1;
                }
                if ((p->p_ARGB = AllocVec(ow * oh * 4, MEMF_ANY)))
                {
                    for (y = 0; y < oh; y++)
                        for (x = 0; x < ow; x++)
                            p->p_ARGB[y * ow + x] = src[(y * h / oh) * w + (x * w / ow)];
                    p->p_ImgW = ow;
                    p->p_ImgH = oh;
                }
            }
            FreeVec(src);
        }
    }
    DisposeDTObject(o);
}

/* Break the text into lines no wider than 'width' pixels. */
static void WrapText(struct Popup *p, struct RastPort *rp, UWORD width)
{
    STRPTR t = p->p_Text;
    UWORD start = 0, len = strlen(t);

    p->p_NumLines = 0;
    while (start < len && p->p_NumLines < MAX_LINES)
    {
        UWORD end = start, lastspace = 0;

        while (end < len && t[end] != '\n')
        {
            if (TextLength(rp, t + start, end - start + 1) > width)
                break;
            if (t[end] == ' ')
                lastspace = end;
            end++;
        }
        if (end < len && t[end] != '\n' && lastspace > start)
            end = lastspace;

        p->p_LineStart[p->p_NumLines] = start;
        p->p_LineLen[p->p_NumLines] = end - start;
        p->p_NumLines++;

        start = end;
        while (start < len && (t[start] == ' ' || t[start] == '\n'))
            start++;
    }
}

static void DrawPopup(struct Popup *p)
{
    struct Window *win = p->p_Window;
    struct RastPort *rp = win->RPort;
    struct DrawInfo *dri = GetScreenDrawInfo(p->p_Screen);
    UWORD w = win->Width, h = win->Height;
    UWORD tx = POPUP_PAD, ty, i;
    UWORD bg = dri ? dri->dri_Pens[BACKGROUNDPEN] : 0;
    UWORD fg = dri ? dri->dri_Pens[TEXTPEN] : 1;
    UWORD shine = dri ? dri->dri_Pens[SHINEPEN] : 2;
    UWORD shadow = dri ? dri->dri_Pens[SHADOWPEN] : 1;
    UWORD hl = dri ? dri->dri_Pens[FILLPEN] : 3;

    SetFont(rp, p->p_Font);
    SetDrMd(rp, JAM1);

    SetAPen(rp, bg);
    RectFill(rp, 0, 0, w - 1, h - 1);

    /* a raised frame with an accent line along the top */
    SetAPen(rp, shine);
    Move(rp, 0, h - 1);
    Draw(rp, 0, 0);
    Draw(rp, w - 1, 0);
    SetAPen(rp, shadow);
    Draw(rp, w - 1, h - 1);
    Draw(rp, 0, h - 1);
    SetAPen(rp, hl);
    RectFill(rp, 1, 1, w - 2, 2);

    if (p->p_ARGB && CyberGfxBase)
    {
        UWORD iy = POPUP_PAD + 2;

        if (p->p_VAlign == 1)
            iy = (h - p->p_ImgH) / 2;
        else if (p->p_VAlign == 2)
            iy = h - POPUP_PAD - p->p_ImgH;
        WritePixelArrayAlpha(p->p_ARGB, 0, 0, p->p_ImgW * 4, rp,
                             POPUP_PAD + (POPUP_IMAGE - p->p_ImgW) / 2, iy,
                             p->p_ImgW, p->p_ImgH, 0xffffffff);
        tx += POPUP_IMAGE + POPUP_PAD;
    }

    ty = POPUP_PAD + 2 + p->p_Font->tf_Baseline;
    SetAPen(rp, fg);
    if (p->p_Title[0])
    {
        SetSoftStyle(rp, FSF_BOLD, AskSoftStyle(rp));
        Move(rp, tx, ty);
        Text(rp, p->p_Title, strlen(p->p_Title));
        SetSoftStyle(rp, FS_NORMAL, AskSoftStyle(rp));
        ty += p->p_Font->tf_YSize + 4;
    }
    for (i = 0; i < p->p_NumLines; i++)
    {
        Move(rp, tx, ty);
        Text(rp, p->p_Text + p->p_LineStart[i], p->p_LineLen[i]);
        ty += p->p_Font->tf_YSize;
    }

    if (dri)
        FreeScreenDrawInfo(p->p_Screen, dri);
}

/* Stack the pop-ups of each screen up from its bottom right corner. */
static void Restack(void)
{
    struct Popup *p, *q;

    ForeachNode(&popups, p)
    {
        LONG bottom = p->p_Screen->Height - POPUP_MARGIN;

        ForeachNode(&popups, q)
        {
            if (q == p)
                break;
            if (q->p_Screen == p->p_Screen)
                bottom -= q->p_Window->Height + POPUP_GAP;
        }
        if (bottom - p->p_Window->Height < p->p_Screen->BarHeight + 1)
            bottom = p->p_Screen->BarHeight + 1 + p->p_Window->Height;
        if (p->p_Window->TopEdge != bottom - p->p_Window->Height)
            ChangeWindowBox(p->p_Window, p->p_Window->LeftEdge, bottom - p->p_Window->Height,
                            p->p_Window->Width, p->p_Window->Height);
    }
}

/* CloseWindow() on a window sharing a port: drop its messages first. */
static void ClosePopup(struct Popup *p)
{
    struct IntuiMessage *im;
    struct Node *succ;

    REMOVE(p);

    Forbid();
    im = (struct IntuiMessage *)idcmpport->mp_MsgList.lh_Head;
    while ((succ = im->ExecMessage.mn_Node.ln_Succ))
    {
        if (im->IDCMPWindow == p->p_Window)
        {
            REMOVE(im);
            ReplyMsg(&im->ExecMessage);
        }
        im = (struct IntuiMessage *)succ;
    }
    p->p_Window->UserPort = NULL;
    ModifyIDCMP(p->p_Window, 0);
    Permit();

    CloseWindow(p->p_Window);
    CloseFont(p->p_Font);
    UnlockPubScreen(NULL, p->p_Screen);
    FreeVec(p->p_ARGB);
    FreeVec(p);

    Restack();
}

static LONG ShowPopup(ULONG appID, CONST_STRPTR title, CONST_STRPTR text, CONST_STRPTR screen,
                      CONST_STRPTR image, UBYTE valign, CONST_STRPTR backmsg, BOOL closeondc)
{
    struct Popup *p;
    struct RastPort tmprp;
    UWORD textw, h, minh, len;

    if (!text || !*text)
        return 0;
    if (!(p = AllocVec(sizeof(*p), MEMF_ANY | MEMF_CLEAR)))
        return 0;

    CopyStr(p->p_Title, title, sizeof(p->p_Title));
    CopyStr(p->p_Text, text, sizeof(p->p_Text));
    CopyStr(p->p_BackMsg, backmsg, sizeof(p->p_BackMsg));
    p->p_AppID = appID;
    p->p_CloseOnDC = closeondc;
    p->p_VAlign = valign;

    if (!(p->p_Screen = LockTargetScreen(screen)))
    {
        FreeVec(p);
        return 0;
    }
    if (!(p->p_Font = OpenFont(p->p_Screen->Font)))
        p->p_Font = OpenFont(&topaz8);

    if (!p->p_Font)
    {
        UnlockPubScreen(NULL, p->p_Screen);
        FreeVec(p);
        return 0;
    }

    LoadImage(p, image);

    textw = POPUP_WIDTH - 2 * POPUP_PAD - (p->p_ARGB ? POPUP_IMAGE + POPUP_PAD : 0);
    InitRastPort(&tmprp);
    SetFont(&tmprp, p->p_Font);
    WrapText(p, &tmprp, textw);

    h = POPUP_PAD + 2 + POPUP_PAD + (p->p_Title[0] ? p->p_Font->tf_YSize + 4 : 0)
        + p->p_NumLines * p->p_Font->tf_YSize;
    minh = 2 * POPUP_PAD + 2 + (p->p_ARGB ? POPUP_IMAGE : 0);
    if (h < minh)
        h = minh;

    /* Long texts stay up longer: 4 s, plus 1 s per 20 characters, up to 10 s. */
    len = strlen(p->p_Text) + strlen(p->p_Title);
    p->p_TicksLeft = (4 + len / 20) * (1000000 / TICK_MICROS);
    if (p->p_TicksLeft > 10 * (1000000 / TICK_MICROS))
        p->p_TicksLeft = 10 * (1000000 / TICK_MICROS);

    p->p_Window = OpenWindowTags(NULL,
                                 WA_PubScreen,   (IPTR)p->p_Screen,
                                 WA_Left,        p->p_Screen->Width - POPUP_WIDTH - POPUP_MARGIN,
                                 WA_Top,         p->p_Screen->Height - POPUP_MARGIN - h,
                                 WA_Width,       POPUP_WIDTH,
                                 WA_Height,      h,
                                 WA_Borderless,  TRUE,
                                 WA_Activate,    FALSE,
                                 WA_ToolBox,     TRUE,
                                 WA_RMBTrap,     TRUE,
                                 WA_SmartRefresh, TRUE,
                                 WA_NoCareRefresh, TRUE,
                                 WA_Title,       (IPTR)p->p_Title,
                                 TAG_DONE);
    if (!p->p_Window)
    {
        CloseFont(p->p_Font);
        UnlockPubScreen(NULL, p->p_Screen);
        FreeVec(p->p_ARGB);
        FreeVec(p);
        return 0;
    }

    p->p_Window->UserPort = idcmpport;
    ModifyIDCMP(p->p_Window, IDCMP_MOUSEBUTTONS);

    ADDHEAD((struct List *)&popups, p);
    DrawPopup(p);
    Restack();
    return 1;
}

static void DoubleClicked(struct Popup *p)
{
    if (p->p_BackMsg[0])
    {
        if (Strnicmp(p->p_BackMsg, "URL:", 4) == 0)
        {
            if (OpenURLBase)
                URL_OpenA(p->p_BackMsg + 4, NULL);
        }
        else if (p->p_AppID && ApplicationBase)
        {
            struct ApplicationCustomMsg acm;

            memset(&acm, 0, sizeof(acm));
            acm.customMsg = p->p_BackMsg;
            SendApplicationMsg(0, p->p_AppID, &acm.almsg, APPLIBMT_CustomMsg);
        }
    }
    if (p->p_CloseOnDC)
        ClosePopup(p);
}

static void HandleIDCMP(void)
{
    struct IntuiMessage *im;

    while ((im = (struct IntuiMessage *)GetMsg(idcmpport)))
    {
        struct Window *win = im->IDCMPWindow;
        ULONG class = im->Class;
        UWORD code = im->Code;
        ULONG secs = im->Seconds, micros = im->Micros;
        struct Popup *p, *hit = NULL;

        ReplyMsg(&im->ExecMessage);

        ForeachNode(&popups, p)
        {
            if (p->p_Window == win)
            {
                hit = p;
                break;
            }
        }
        if (!hit || class != IDCMP_MOUSEBUTTONS)
            continue;

        if (code == MENUDOWN)
            ClosePopup(hit);
        else if (code == SELECTDOWN)
        {
            if (DoubleClick(hit->p_LastSecs, hit->p_LastMicros, secs, micros))
            {
                hit->p_LastSecs = hit->p_LastMicros = 0;
                DoubleClicked(hit);
            }
            else
            {
                hit->p_LastSecs = secs;
                hit->p_LastMicros = micros;
            }
        }
    }
}

/* A pop-up under the pointer does not expire, so it can be read and clicked. */
static BOOL PointerOver(struct Popup *p)
{
    struct Window *w = p->p_Window;
    WORD x = p->p_Screen->MouseX, y = p->p_Screen->MouseY;

    return x >= w->LeftEdge && x < w->LeftEdge + w->Width
           && y >= w->TopEdge && y < w->TopEdge + w->Height;
}

static void Tick(void)
{
    struct Popup *p, *next;

    ForeachNodeSafe(&popups, p, next)
    {
        if (PointerOver(p))
        {
            if (p->p_TicksLeft < 2 * (1000000 / TICK_MICROS))
                p->p_TicksLeft = 2 * (1000000 / TICK_MICROS);
        }
        else if (--p->p_TicksLeft <= 0)
            ClosePopup(p);
    }
}

/***************************************************************************
 * ARexx: REGISTERAPP APP=name
 *        RINGHIO APP=name [SCREEN=pub] [TITLE=t] [IMG=file] [IMGVALIGN=n]
 *                [CLOSEONDC] [BACKMSG=msg] TEXT=rest of the line
 ***************************************************************************/

static ULONG RexxAppID(CONST_STRPTR name, BOOL create)
{
    struct RexxApp *ra;
    ULONG id;

    ForeachNode(&rexxapps, ra)
    {
        if (Stricmp(ra->ra_Name, name) == 0)
            return ra->ra_AppID;
    }
    if (!ApplicationBase)
        return 0;
    if ((id = FindApplication(FINDAPP_Name, (IPTR)name, TAG_DONE)) || !create)
        return id;

    if (!(ra = AllocVec(sizeof(*ra), MEMF_ANY | MEMF_CLEAR)))
        return 0;
    if (!(ra->ra_AppID = RegisterApplication(name, REGAPP_Description, (IPTR)"ARexx script", TAG_DONE)))
    {
        FreeVec(ra);
        return 0;
    }
    CopyStr(ra->ra_Name, name, sizeof(ra->ra_Name));
    ADDTAIL((struct List *)&rexxapps, ra);
    return ra->ra_AppID;
}

static LONG RexxCommand(STRPTR line)
{
    struct RDArgs *rda;
    STRPTR args;
    TEXT cmd[16];
    UWORD i = 0;
    LONG rc = RC_ERROR;
    IPTR a[8];

    while (*line == ' ' || *line == '\t')
        line++;
    while (line[i] && line[i] != ' ' && line[i] != '\t' && i < sizeof(cmd) - 1)
    {
        cmd[i] = line[i];
        i++;
    }
    cmd[i] = '\0';
    args = line + i;

    if (!(rda = AllocDosObject(DOS_RDARGS, NULL)))
        return RC_FATAL;

    /* ReadArgs() wants the line to end in a newline. */
    {
        ULONG alen = strlen(args);
        STRPTR buf = AllocVec(alen + 2, MEMF_ANY);

        if (!buf)
        {
            FreeDosObject(DOS_RDARGS, rda);
            return RC_FATAL;
        }
        CopyMem(args, buf, alen);
        buf[alen] = '\n';
        buf[alen + 1] = '\0';
        rda->RDA_Source.CS_Buffer = buf;
        rda->RDA_Source.CS_Length = alen + 1;
        rda->RDA_Source.CS_CurChr = 0;
        rda->RDA_Flags |= RDAF_NOPROMPT;
        memset(a, 0, sizeof(a));

        if (Stricmp(cmd, "REGISTERAPP") == 0)
        {
            if (ReadArgs("APP/A", a, rda))
            {
                rc = RexxAppID((STRPTR)a[0], TRUE) ? RC_OK : RC_ERROR;
                FreeArgs(rda);
            }
        }
        else if (Stricmp(cmd, "RINGHIO") == 0)
        {
            if (ReadArgs("APP/A,SCREEN/K,TITLE/K,IMG/K,IMGVALIGN/K/N,CLOSEONDC/S,BACKMSG/K,TEXT/K/F",
                         a, rda))
            {
                ULONG id = RexxAppID((STRPTR)a[0], TRUE);
                UBYTE valign = a[4] ? *(LONG *)a[4] : 0;

                if (id && ShowPopup(id, a[2] ? (STRPTR)a[2] : (STRPTR)a[0], (STRPTR)a[7],
                                    (STRPTR)a[1], (STRPTR)a[3], valign, (STRPTR)a[6], a[5] != 0))
                    rc = RC_OK;
                FreeArgs(rda);
            }
        }
        FreeVec(buf);
    }
    FreeDosObject(DOS_RDARGS, rda);
    return rc;
}

static void HandleRexx(struct MsgPort *port)
{
    struct RexxMsg *rm;

    while ((rm = (struct RexxMsg *)GetMsg(port)))
    {
        if (IsRexxMsg(rm) && (rm->rm_Action & RXCODEMASK) == RXCOMM && rm->rm_Args[0])
            rm->rm_Result1 = RexxCommand((STRPTR)rm->rm_Args[0]);
        else
            rm->rm_Result1 = RC_ERROR;
        rm->rm_Result2 = 0;
        ReplyMsg(&rm->rm_Node);
    }
}

/***************************************************************************/

static void HandleRequests(struct MsgPort *port)
{
    struct NotifyServerMsg *nsm;

    while ((nsm = (struct NotifyServerMsg *)GetMsg(port)))
    {
        if (nsm->nsm_Version == NOTIFYSERVER_VERSION)
            nsm->nsm_Result = ShowPopup(nsm->nsm_AppID, nsm->nsm_Title, nsm->nsm_Text,
                                        nsm->nsm_PubScreen, nsm->nsm_ImageFile,
                                        nsm->nsm_ImageVertAlign, nsm->nsm_BackMsg,
                                        nsm->nsm_CloseOnDC);
        else
            nsm->nsm_Result = 0;
        ReplyMsg(&nsm->nsm_Msg);
    }
}

static struct MsgPort *MakePublicPort(CONST_STRPTR name)
{
    struct MsgPort *port;

    if (!(port = CreateMsgPort()))
        return NULL;
    port->mp_Node.ln_Name = (STRPTR)name;
    port->mp_Node.ln_Pri = 0;
    Forbid();
    if (FindPort(name))
    {
        Permit();
        DeleteMsgPort(port);
        return NULL;
    }
    AddPort(port);
    Permit();
    return port;
}

static void DropPublicPort(struct MsgPort *port, void (*drain)(struct MsgPort *))
{
    if (!port)
        return;
    Forbid();
    RemPort(port);
    Permit();
    drain(port);
    DeleteMsgPort(port);
}

static void RefuseRequests(struct MsgPort *port)
{
    struct NotifyServerMsg *nsm;

    while ((nsm = (struct NotifyServerMsg *)GetMsg(port)))
    {
        nsm->nsm_Result = 0;
        ReplyMsg(&nsm->nsm_Msg);
    }
}

static void RefuseRexx(struct MsgPort *port)
{
    struct RexxMsg *rm;

    while ((rm = (struct RexxMsg *)GetMsg(port)))
    {
        rm->rm_Result1 = RC_FATAL;
        rm->rm_Result2 = 0;
        ReplyMsg(&rm->rm_Node);
    }
}

int main(void)
{
    struct MsgPort *reqport = NULL, *rexxport = NULL, *timerport = NULL;
    struct timerequest *treq = NULL;
    struct RexxApp *ra;
    BOOL timeropen = FALSE;
    int ret = RETURN_FAIL;

    NEWLIST((struct List *)&popups);
    NEWLIST((struct List *)&rexxapps);

    ApplicationBase = OpenLibrary("application.library", 0);
    DataTypesBase = OpenLibrary("datatypes.library", 39);
    CyberGfxBase = OpenLibrary("cybergraphics.library", 0);
    RexxSysBase = (struct RxsLib *)OpenLibrary("rexxsyslib.library", 0);
    OpenURLBase = OpenLibrary("openurl.library", 0);

    if (!(reqport = MakePublicPort(NOTIFYSERVER_PORTNAME)))
    {
        /* Already running. */
        ret = RETURN_WARN;
        goto out;
    }
    if (RexxSysBase)
        rexxport = MakePublicPort(REXX_PORTNAME);

    if (!(idcmpport = CreateMsgPort()) || !(timerport = CreateMsgPort())
        || !(treq = (struct timerequest *)CreateIORequest(timerport, sizeof(*treq))))
        goto out;
    if (OpenDevice(TIMERNAME, UNIT_VBLANK, &treq->tr_node, 0))
        goto out;
    timeropen = TRUE;

    treq->tr_node.io_Command = TR_ADDREQUEST;
    treq->tr_time.tv_secs = 0;
    treq->tr_time.tv_micro = TICK_MICROS;
    SendIO(&treq->tr_node);

    ret = RETURN_OK;
    for (;;)
    {
        ULONG sigs = Wait((1UL << reqport->mp_SigBit) | (1UL << idcmpport->mp_SigBit)
                          | (1UL << timerport->mp_SigBit)
                          | (rexxport ? 1UL << rexxport->mp_SigBit : 0)
                          | SIGBREAKF_CTRL_C);

        if (sigs & (1UL << reqport->mp_SigBit))
            HandleRequests(reqport);
        if (rexxport && (sigs & (1UL << rexxport->mp_SigBit)))
            HandleRexx(rexxport);
        if (sigs & (1UL << idcmpport->mp_SigBit))
            HandleIDCMP();
        if ((sigs & (1UL << timerport->mp_SigBit)) && CheckIO(&treq->tr_node))
        {
            WaitIO(&treq->tr_node);
            Tick();
            treq->tr_node.io_Command = TR_ADDREQUEST;
            treq->tr_time.tv_secs = 0;
            treq->tr_time.tv_micro = TICK_MICROS;
            SendIO(&treq->tr_node);
        }
        if (sigs & SIGBREAKF_CTRL_C)
            break;
    }

out:
    DropPublicPort(reqport, RefuseRequests);
    DropPublicPort(rexxport, RefuseRexx);

    while (!IsListEmpty((struct List *)&popups))
        ClosePopup((struct Popup *)popups.mlh_Head);

    while ((ra = (struct RexxApp *)REMHEAD((struct List *)&rexxapps)))
    {
        UnregisterApplication(ra->ra_AppID, NULL);
        FreeVec(ra);
    }

    if (timeropen)
    {
        AbortIO(&treq->tr_node);
        WaitIO(&treq->tr_node);
        CloseDevice(&treq->tr_node);
    }
    if (treq)
        DeleteIORequest(&treq->tr_node);
    if (timerport)
        DeleteMsgPort(timerport);
    if (idcmpport)
        DeleteMsgPort(idcmpport);

    if (OpenURLBase)
        CloseLibrary(OpenURLBase);
    if (RexxSysBase)
        CloseLibrary((struct Library *)RexxSysBase);
    if (CyberGfxBase)
        CloseLibrary(CyberGfxBase);
    if (DataTypesBase)
        CloseLibrary(DataTypesBase);
    if (ApplicationBase)
        CloseLibrary(ApplicationBase);

    return ret;
}
