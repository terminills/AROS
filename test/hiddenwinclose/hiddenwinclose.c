/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Does a window opened WA_Hidden and shown with ShowWindow() behave
          like an ordinary window once another window overlaps it, and does
          HideWindow()/CloseWindow() take it off the screen?

    Written for Chromium's Ozone AROS backend, which opens every window
    WA_Hidden and calls ShowWindow() at Show() time, and paints into it with
    WritePixelArray()/ClipBlit() from a second task.  With two such windows
    overlapping, the first stayed black for its whole life, and closing it
    (HideWindow, then CloseWindow) left a frozen copy of it on the screen.
    Side by side, both painted, but the closed one still stayed on screen.

    This is the same sequence without Chromium:
        A = OpenWindow(WA_Hidden) ; ShowWindow(A)
        B = OpenWindow(WA_Hidden) ; ShowWindow(B)      (over A by default)
        a painter task draws a counter into A once a second
        HideWindow(A) ; CloseWindow(A)                 (after CLOSEAFTER s)
        B keeps counting until the program exits.

    hiddenwinclose SIDEBYSIDE/S,SHOWN/S,SAMETASK/S,NOHIDE/S,BLIT/S,BOX/S,
                   IDCMP/S,BOXFIRST/S,TITLE/S,LIMITS/S,FRONT/S,
                   CLOSEAFTER/N,EXITAFTER/N
      SIDEBYSIDE  B next to A instead of over it
      SHOWN       open both windows visible instead of WA_Hidden+ShowWindow
      SAMETASK    paint from the main task instead of a painter task
      NOHIDE      skip HideWindow() before CloseWindow()
      BLIT        paint into a BMF_DISPLAYABLE friend bitmap of the screen
                  and ClipBlit() it into the window, as Chromium's
                  BlitNativeBackingStore() does, instead of WritePixelArray()
                  straight into the window
      BOX         ChangeWindowBox() each window to the box it already has
                  right after showing it, as ElectronShell's geometry sync
                  does for a new BrowserWindow
      IDCMP       one shared UserPort for both windows with Chromium's IDCMP
                  mask (ModifyIDCMP after OpenWindow), IDCMP_REFRESHWINDOW
                  answered with BeginRefresh()/EndRefresh(), and the port
                  detached (UserPort = NULL; ModifyIDCMP(0)) before
                  CloseWindow(), as Chromium's ArosWindow does; every message
                  class received is printed
      BOXFIRST    ChangeWindowBox() the window while it is still hidden,
                  before ShowWindow(): Chromium sets its bounds between
                  OpenWindow() and Show(), so the layer is moved (to where it
                  already is) while it is not visible
      TITLE       SetWindowTitles() right after ShowWindow(), while the show
                  is still queued for the input handler, as Chromium's
                  Widget title update and ElectronShell's create-time sync do
      LIMITS      WindowLimits() at the same point (ElectronShell sync)
      FRONT       WindowToFront() + ActivateWindow() at the same point
                  (ElectronShell's browser-window.show)
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include <proto/cybergraphics.h>

#include <intuition/intuition.h>
#include <graphics/rastport.h>
#include <cybergraphx/cybergraphics.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_WIDTH   360
#define WIN_HEIGHT  200

struct Painter
{
    struct Window   *window;
    ULONG            colour;    /* 0x00RRGGBB */
    LONG             count;
    LONG             started;
    volatile LONG    stop;
    volatile LONG    done;
    /* BLIT mode: the off-screen frame and the rastport that draws into it */
    struct BitMap   *backing;
    struct RastPort  backing_rp;
};

static struct Painter painters[2];
static struct MsgPort *shared_port;

/* Chromium's kWindowEventMask */
#define SHARED_IDCMP (IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW | \
    IDCMP_REFRESHWINDOW | IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW | \
    IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY)

static const char *window_name(struct Window *w)
{
    return w == painters[0].window ? "A" : w == painters[1].window ? "B" : "?";
}

/* ArosWindow::DisableNativeEvents(): the port is shared, so it is taken away
   from the window before ModifyIDCMP()/CloseWindow() can free it. */
static void detach_shared_port(struct Window *w)
{
    struct IntuiMessage *msg;

    if (shared_port == NULL)
        return;
    Forbid();
    w->UserPort = NULL;
    ModifyIDCMP(w, 0);
    Permit();
    while ((msg = (struct IntuiMessage *)GetMsg(shared_port)) != NULL)
        ReplyMsg(&msg->ExecMessage);
}

/* ArosWindow::HandleNativeEvent(): answer refresh requests, report the rest. */
static void handle_shared_port(void)
{
    struct IntuiMessage *msg;

    if (shared_port == NULL)
        return;
    while ((msg = (struct IntuiMessage *)GetMsg(shared_port)) != NULL)
    {
        struct Window *w = msg->IDCMPWindow;
        ULONG class = msg->Class;

        ReplyMsg(&msg->ExecMessage);
        if (class == IDCMP_MOUSEMOVE)
            continue;
        printf("  IDCMP 0x%08lx window %s\n", (unsigned long)class,
            window_name(w));
        if (class == IDCMP_REFRESHWINDOW)
        {
            struct Painter *p = w == painters[0].window ? &painters[0]
                : w == painters[1].window ? &painters[1] : NULL;

            BeginRefresh(w);
            if (p && p->backing)
                ClipBlit(&p->backing_rp, 0, 0, w->RPort, w->BorderLeft,
                    w->BorderTop, WIN_WIDTH, WIN_HEIGHT, 0xC0);
            EndRefresh(w, TRUE);
        }
    }
}

/* One frame: a solid fill through WritePixelArray (as Chromium's
   PresentPixels does), then the counter in the window's font. */
static void paint_frame(struct Painter *p)
{
    struct Window *w = p->window;
    struct RastPort *rp = w->RPort;
    static ULONG pixels[WIN_WIDTH * 4];   /* one 4-row strip */
    char text[32];
    LONG y, i;

    for (i = 0; i < WIN_WIDTH * 4; ++i)
        pixels[i] = p->colour;
    snprintf(text, sizeof(text), "%ld", (long)p->count);

    if (p->backing)
    {
        /* Chromium: WritePixelArray() into the friend bitmap, then ClipBlit()
           the damaged rectangle into the window under LockLayer(). */
        for (y = 0; y < WIN_HEIGHT; y += 4)
            WritePixelArray(pixels, 0, 0, WIN_WIDTH * 4, &p->backing_rp,
                0, y, WIN_WIDTH, 4, RECTFMT_ARGB);
        SetABPenDrMd(&p->backing_rp, 1, 0, JAM1);
        Move(&p->backing_rp, 16, 40);
        Text(&p->backing_rp, text, strlen(text));
        LockLayer(0, rp->Layer);
        ClipBlit(&p->backing_rp, 0, 0, rp, w->BorderLeft, w->BorderTop,
            WIN_WIDTH, WIN_HEIGHT, 0xC0);
        UnlockLayer(rp->Layer);
    }
    else
    {
        LockLayer(0, rp->Layer);
        for (y = 0; y < WIN_HEIGHT; y += 4)
            WritePixelArray(pixels, 0, 0, WIN_WIDTH * 4, rp,
                w->BorderLeft, w->BorderTop + y, WIN_WIDTH, 4, RECTFMT_ARGB);
        SetABPenDrMd(rp, 1, 0, JAM1);
        Move(rp, w->BorderLeft + 16, w->BorderTop + 40);
        Text(rp, text, strlen(text));
        UnlockLayer(rp->Layer);
    }
    p->count++;
}

/* The backing store Chromium's EnsureNativeBackingStore() allocates: a
   displayable friend of the screen bitmap, same depth. */
static BOOL alloc_backing(struct Painter *p)
{
    struct BitMap *screen_bm = p->window->RPort->BitMap;
    ULONG depth = GetBitMapAttr(screen_bm, BMA_DEPTH);

    p->backing = AllocBitMap(WIN_WIDTH, WIN_HEIGHT, depth,
        BMF_CLEAR | BMF_DISPLAYABLE | BMF_MINPLANES, screen_bm);
    if (p->backing == NULL)
    {
        printf("AllocBitMap(%dx%d depth %lu friend %p) failed\n",
            WIN_WIDTH, WIN_HEIGHT, (unsigned long)depth, screen_bm);
        return FALSE;
    }
    InitRastPort(&p->backing_rp);
    p->backing_rp.BitMap = p->backing;
    printf("backing bitmap %p for window %p\n", p->backing, p->window);
    return TRUE;
}

static void free_backing(struct Painter *p)
{
    if (p->backing)
    {
        FreeBitMap(p->backing);
        p->backing = NULL;
    }
}

static void painter_task(void)
{
    struct Painter *p = (struct Painter *)FindTask(NULL)->tc_UserData;

    while (!p->stop)
    {
        paint_frame(p);
        Delay(50);
    }
    p->done = 1;
}

static BOOL box_first, title_after, limits_after, front_after;

static struct Window *open_probe_window(LONG x, LONG y, BOOL hidden,
    BOOL box, const char *title)
{
    struct Window *w = OpenWindowTags(NULL,
        WA_Title, (IPTR)title,
        WA_Left, x, WA_Top, y,
        WA_InnerWidth, WIN_WIDTH, WA_InnerHeight, WIN_HEIGHT,
        WA_CloseGadget, TRUE, WA_DepthGadget, TRUE, WA_DragBar, TRUE,
        WA_SizeGadget, TRUE, WA_SizeBBottom, TRUE,
        WA_Activate, FALSE, WA_Hidden, hidden,
        WA_AutoAdjust, TRUE, WA_SmartRefresh, TRUE, WA_ReportMouse, TRUE,
        WA_RMBTrap, TRUE,
        shared_port ? TAG_IGNORE : WA_IDCMP, IDCMP_CLOSEWINDOW,
        TAG_DONE);

    if (w == NULL)
        printf("OpenWindow(%s) failed\n", title);
    else
    {
        if (shared_port)
        {
            w->UserPort = shared_port;
            printf("%s: ModifyIDCMP -> %ld\n", title,
                (long)ModifyIDCMP(w, SHARED_IDCMP));
        }
        printf("%s: window %p layer %p visible=%ld\n", title, w,
            w->RPort->Layer, (long)IsLayerVisible(w->RPort->Layer));
        if (box_first)
        {
            printf("  ChangeWindowBox(%ld,%ld %ldx%ld) while hidden\n",
                (long)w->LeftEdge, (long)w->TopEdge, (long)w->Width,
                (long)w->Height);
            ChangeWindowBox(w, w->LeftEdge, w->TopEdge, w->Width, w->Height);
        }
        if (hidden)
        {
            ShowWindow(w, NULL);
            ActivateWindow(w);
        }
        /* ShowWindow() is queued for the input handler, so the layer is
           still hidden while these run, as it is for Chromium */
        if (title_after)
        {
            printf("  SetWindowTitles while show pending\n");
            SetWindowTitles(w, (STRPTR)"AROS Electron Shell", (STRPTR)~0);
        }
        if (limits_after)
        {
            printf("  WindowLimits while show pending\n");
            WindowLimits(w, w->BorderLeft + w->BorderRight + 100,
                w->BorderTop + w->BorderBottom + 100, ~0, ~0);
        }
        if (front_after)
        {
            printf("  WindowToFront + ActivateWindow while show pending\n");
            WindowToFront(w);
            ActivateWindow(w);
        }
        if (box)
        {
            Delay(10);
            printf("  ChangeWindowBox(%ld,%ld %ldx%ld) (no change)\n",
                (long)w->LeftEdge, (long)w->TopEdge, (long)w->Width,
                (long)w->Height);
            ChangeWindowBox(w, w->LeftEdge, w->TopEdge, w->Width, w->Height);
        }
    }
    return w;
}

static struct Task *start_painter(struct Painter *p, const char *name)
{
    struct Process *proc = CreateNewProcTags(
        NP_Entry, (IPTR)painter_task,
        NP_Name, (IPTR)name,
        NP_UserData, (IPTR)p,
        NP_Priority, 0,
        TAG_DONE);
    p->started = proc != NULL;
    return proc ? &proc->pr_Task : NULL;
}

static void stop_painter(struct Painter *p)
{
    if (p->window == NULL || !p->started)
        return;
    p->stop = 1;
    while (!p->done)
        Delay(5);
}

int main(void)
{
    static struct RDArgs *rdargs;
    IPTR args[13] = { 0 };
    BOOL side_by_side, shown, same_task, no_hide, blit, box, idcmp;
    LONG close_after = 8, exit_after = 20;
    struct Window *a, *b;
    LONG t;

    rdargs = ReadArgs("SIDEBYSIDE/S,SHOWN/S,SAMETASK/S,NOHIDE/S,BLIT/S,BOX/S,"
        "IDCMP/S,BOXFIRST/S,TITLE/S,LIMITS/S,FRONT/S,CLOSEAFTER/N,EXITAFTER/N",
        args, NULL);
    if (rdargs == NULL)
    {
        PrintFault(IoErr(), "hiddenwinclose");
        return RETURN_FAIL;
    }
    side_by_side = args[0] != 0;
    shown = args[1] != 0;
    same_task = args[2] != 0;
    no_hide = args[3] != 0;
    blit = args[4] != 0;
    box = args[5] != 0;
    idcmp = args[6] != 0;
    box_first = args[7] != 0;
    title_after = args[8] != 0;
    limits_after = args[9] != 0;
    front_after = args[10] != 0;
    if (args[11])
        close_after = *(LONG *)args[11];
    if (args[12])
        exit_after = *(LONG *)args[12];
    FreeArgs(rdargs);

    printf("hiddenwinclose: %s, %s, paint from %s, %s, %s, %s, %s, %s, "
        "%s%s%s, close A after %lds, exit after %lds\n",
        side_by_side ? "side by side" : "B over A",
        shown ? "opened visible" : "WA_Hidden + ShowWindow",
        same_task ? "main task" : "painter tasks",
        no_hide ? "CloseWindow only" : "HideWindow then CloseWindow",
        blit ? "ClipBlit from a friend bitmap" : "WritePixelArray",
        box ? "ChangeWindowBox after show" : "no ChangeWindowBox",
        idcmp ? "shared IDCMP port" : "no IDCMP",
        box_first ? "ChangeWindowBox while hidden" : "no box while hidden",
        title_after ? "SetWindowTitles " : "",
        limits_after ? "WindowLimits " : "",
        front_after ? "WindowToFront " : "",
        (long)close_after, (long)exit_after);

    if (idcmp)
    {
        shared_port = CreateMsgPort();
        if (shared_port == NULL)
            return RETURN_FAIL;
    }
    a = open_probe_window(40, 60, !shown, box, "hiddenwinclose A");
    if (a == NULL)
        return RETURN_FAIL;
    Delay(25);
    b = open_probe_window(side_by_side ? 460 : 200, side_by_side ? 60 : 180,
        !shown, box, "hiddenwinclose B");
    if (b == NULL)
    {
        CloseWindow(a);
        return RETURN_FAIL;
    }

    painters[0].window = a;
    painters[0].colour = 0x7e4c2b;
    painters[1].window = b;
    painters[1].colour = 0x2b4c7e;
    if (blit && (!alloc_backing(&painters[0]) || !alloc_backing(&painters[1])))
    {
        free_backing(&painters[0]);
        CloseWindow(b);
        CloseWindow(a);
        return RETURN_FAIL;
    }
    if (!same_task)
    {
        start_painter(&painters[0], "hiddenwinclose painter A");
        start_painter(&painters[1], "hiddenwinclose painter B");
    }

    for (t = 0; t < exit_after; ++t)
    {
        if (same_task)
        {
            if (painters[0].window)
                paint_frame(&painters[0]);
            paint_frame(&painters[1]);
        }
        {
            LONG tick;

            for (tick = 0; tick < 10; ++tick)
            {
                Delay(5);
                handle_shared_port();
            }
        }
        if (t + 1 == close_after && painters[0].window)
        {
            printf("t=%ld: closing A (layer visible=%ld)\n", (long)t + 1,
                (long)IsLayerVisible(a->RPort->Layer));
            stop_painter(&painters[0]);
            painters[0].window = NULL;
            free_backing(&painters[0]);
            if (!no_hide)
            {
                HideWindow(a);
                Delay(25);
                handle_shared_port();
                printf("  after HideWindow: layer visible=%ld\n",
                    (long)IsLayerVisible(a->RPort->Layer));
            }
            detach_shared_port(a);
            CloseWindow(a);
            a = NULL;
            printf("  A closed\n");
        }
    }

    stop_painter(&painters[1]);
    painters[1].window = NULL;
    free_backing(&painters[1]);
    if (a)
    {
        /* CLOSEAFTER past EXITAFTER: A is still open and its painter still
           running; stop it before its window, bitmap and code go away */
        stop_painter(&painters[0]);
        painters[0].window = NULL;
        free_backing(&painters[0]);
        detach_shared_port(a);
        CloseWindow(a);
    }
    detach_shared_port(b);
    CloseWindow(b);
    if (shared_port)
        DeleteMsgPort(shared_port);
    printf("hiddenwinclose: done\n");
    return RETURN_OK;
}
