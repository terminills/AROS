/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Log every layers.library call that can change a layer's visible
          region, with the calling task and the layer list afterwards.

    A window's layer can end up with an empty VisibleRegion (the whole layer
    "obscured") while the layers in front of it do not cover it, and nothing
    the caller did explains it: the same OpenWindow/ShowWindow/... sequence
    from a test program keeps the regions right.  This patches the layer
    operations (SetFunction) and writes to the debug log (KPrintF), so a
    run's aros-debug.log shows which call, from which task, was the one
    after which the regions went wrong.

    layerspy [ALL/S]
      Installs the patches and waits for CTRL-C, then removes them.  Every
      call is logged as one line; the layer list of the affected Layer_Info
      is logged after a call only when it differs from the last one logged
      (so lock/clip traffic does not flood the log).
      ALL   also log the calls that never change regions (LockLayer,
            BeginUpdate/EndUpdate, InstallClipRegion)

    Hosted only in practice: KPrintF goes to the bootstrap's debug output.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/layers.h>
#include <proto/timer.h>
#include <proto/arossupport.h>

#include <aros/libcall.h>
#include <exec/libraries.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <graphics/regions.h>
#include <intuition/intuition.h>

#include <stdio.h>
#include <string.h>

#define LIB_VECTSIZE_LAYERS LIB_VECTSIZE

struct Device *TimerBase;
static struct timerequest *timer_req;
static struct MsgPort *timer_port;
static ULONG eclock_freq;
static BOOL log_all;

/* Signature of the last layer list logged, one word per field. */
#define SIG_WORDS 512
static IPTR last_sig[SIG_WORDS];
static LONG last_sig_len;

/* Milliseconds on the EClock, wrapped; enough to order events in a run. */
static ULONG spy_ms(void)
{
    struct EClockVal ev;

    if (TimerBase == NULL || eclock_freq == 0)
        return 0;
    ReadEClock(&ev);
    return (ULONG)(((unsigned long long)ev.ev_lo * 1000ULL) / eclock_freq);
}

static const char *spy_task(void)
{
    struct Task *t = FindTask(NULL);

    return t && t->tc_Node.ln_Name ? t->tc_Node.ln_Name : "?";
}

/* Log the layer list front to back if it changed since the last log. */
static void spy_walk(struct Layer_Info *li, const char *why)
{
    IPTR sig[SIG_WORDS];
    LONG n = 0;
    struct Layer *l;
    BOOL changed;

    if (li == NULL)
        return;
    LockLayers(li);
    for (l = li->top_layer; l != NULL && n + 8 <= SIG_WORDS; l = l->back)
    {
        struct ClipRect *cr;
        LONG crs = 0, obscured = 0;

        for (cr = l->ClipRect; cr != NULL; cr = cr->Next, ++crs)
            if (cr->lobs)
                ++obscured;
        sig[n++] = (IPTR)l;
        sig[n++] = (IPTR)(l->bounds.MinX | (l->bounds.MinY << 16));
        sig[n++] = (IPTR)(l->bounds.MaxX | (l->bounds.MaxY << 16));
        sig[n++] = (IPTR)l->visible;
        sig[n++] = (IPTR)(l->VisibleRegion->bounds.MinX
            | (l->VisibleRegion->bounds.MinY << 16));
        sig[n++] = (IPTR)(l->VisibleRegion->bounds.MaxX
            | (l->VisibleRegion->bounds.MaxY << 16));
        sig[n++] = (IPTR)crs;
        sig[n++] = (IPTR)obscured;
    }
    changed = n != last_sig_len || memcmp(sig, last_sig, n * sizeof(IPTR));
    if (changed)
    {
        memcpy(last_sig, sig, n * sizeof(IPTR));
        last_sig_len = n;
        kprintf("[LAYERSPY] layers after %s:\n", why);
        for (l = li->top_layer; l != NULL; l = l->back)
        {
            struct ClipRect *cr;
            struct RegionRectangle *rr;
            struct Window *w = (struct Window *)l->Window;
            LONG crs = 0, obscured = 0, rects = 0;

            for (cr = l->ClipRect; cr != NULL; cr = cr->Next, ++crs)
                if (cr->lobs)
                    ++obscured;
            for (rr = l->VisibleRegion->RegionRectangle; rr != NULL;
                rr = rr->Next)
                ++rects;
            kprintf("[LAYERSPY]   %p %ld,%ld-%ld,%ld%s vis %ld,%ld-%ld,%ld "
                "(%ld rects) cr %ld/%ld \"%s\"\n", l, (LONG)l->bounds.MinX,
                (LONG)l->bounds.MinY, (LONG)l->bounds.MaxX,
                (LONG)l->bounds.MaxY, l->visible ? "" : " HIDDEN",
                (LONG)l->VisibleRegion->bounds.MinX,
                (LONG)l->VisibleRegion->bounds.MinY,
                (LONG)l->VisibleRegion->bounds.MaxX,
                (LONG)l->VisibleRegion->bounds.MaxY, rects, obscured, crs,
                w && w->Title ? (char *)w->Title : "");
        }
    }
    UnlockLayers(li);
}

static void spy_log(const char *what, struct Layer *l, LONG a, LONG b,
    LONG c, LONG d, IPTR result)
{
    ULONG ms = spy_ms();

    kprintf("[LAYERSPY] %lu.%03lu %s: %s(%p%s %ld,%ld,%ld,%ld) = %p\n",
        ms / 1000, ms % 1000, spy_task(), what, l, l && !l->visible ? " HIDDEN" : "", a, b, c, d,
        (APTR)result);
}

/* ---- the patches ------------------------------------------------------ */

struct patch
{
    const char *name;
    WORD lvo;
    APTR newfunc;
    APTR oldfunc;
    BOOL quiet;     /* only with ALL */
};

enum
{
    P_UPFRONTLAYER, P_BEHINDLAYER, P_MOVELAYER, P_SIZELAYER,
    P_BEGINUPDATE, P_ENDUPDATE, P_DELETELAYER, P_LOCKLAYER,
    P_MOVELAYERINFRONTOF, P_INSTALLCLIPREGION, P_MOVESIZELAYER,
    P_CREATEUPFRONTHOOKLAYER, P_CREATEBEHINDHOOKLAYER, P_SORTLAYERCR,
    P_CHANGELAYERSHAPE, P_CREATEUPFRONTLAYERTAGLIST,
    P_CREATEBEHINDLAYERTAGLIST, P_CHANGELAYERVISIBILITY,
    P_COUNT
};

static struct patch patches[P_COUNT];

#define OLD(p) patches[p].oldfunc

AROS_LH2(LONG, spy_UpfrontLayer,
    AROS_LHA(LONG, dummy, A0),
    AROS_LHA(struct Layer *, l, A1),
    struct Library *, LayersBase, 8, Layers)
{
    AROS_LIBFUNC_INIT
    LONG r = AROS_CALL2(LONG, OLD(P_UPFRONTLAYER),
        AROS_LDA(LONG, dummy, A0), AROS_LDA(struct Layer *, l, A1),
        struct Library *, LayersBase);
    spy_log("UpfrontLayer", l, 0, 0, 0, 0, r);
    spy_walk(l->LayerInfo, "UpfrontLayer");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, spy_BehindLayer,
    AROS_LHA(LONG, dummy, A0),
    AROS_LHA(struct Layer *, l, A1),
    struct Library *, LayersBase, 9, Layers)
{
    AROS_LIBFUNC_INIT
    LONG r = AROS_CALL2(LONG, OLD(P_BEHINDLAYER),
        AROS_LDA(LONG, dummy, A0), AROS_LDA(struct Layer *, l, A1),
        struct Library *, LayersBase);
    spy_log("BehindLayer", l, 0, 0, 0, 0, r);
    spy_walk(l->LayerInfo, "BehindLayer");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH4(LONG, spy_MoveLayer,
    AROS_LHA(LONG, dummy, A0),
    AROS_LHA(struct Layer *, l, A1),
    AROS_LHA(LONG, dx, D0),
    AROS_LHA(LONG, dy, D1),
    struct Library *, LayersBase, 10, Layers)
{
    AROS_LIBFUNC_INIT
    LONG r = AROS_CALL4(LONG, OLD(P_MOVELAYER),
        AROS_LDA(LONG, dummy, A0), AROS_LDA(struct Layer *, l, A1),
        AROS_LDA(LONG, dx, D0), AROS_LDA(LONG, dy, D1),
        struct Library *, LayersBase);
    spy_log("MoveLayer", l, dx, dy, 0, 0, r);
    spy_walk(l->LayerInfo, "MoveLayer");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH4(LONG, spy_SizeLayer,
    AROS_LHA(LONG, dummy, A0),
    AROS_LHA(struct Layer *, l, A1),
    AROS_LHA(LONG, dw, D0),
    AROS_LHA(LONG, dh, D1),
    struct Library *, LayersBase, 11, Layers)
{
    AROS_LIBFUNC_INIT
    LONG r = AROS_CALL4(LONG, OLD(P_SIZELAYER),
        AROS_LDA(LONG, dummy, A0), AROS_LDA(struct Layer *, l, A1),
        AROS_LDA(LONG, dw, D0), AROS_LDA(LONG, dh, D1),
        struct Library *, LayersBase);
    spy_log("SizeLayer", l, dw, dh, 0, 0, r);
    spy_walk(l->LayerInfo, "SizeLayer");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, spy_BeginUpdate,
    AROS_LHA(struct Layer *, l, A0),
    struct Library *, LayersBase, 13, Layers)
{
    AROS_LIBFUNC_INIT
    LONG r = AROS_CALL1(LONG, OLD(P_BEGINUPDATE),
        AROS_LDA(struct Layer *, l, A0), struct Library *, LayersBase);
    if (log_all)
        spy_log("BeginUpdate", l, 0, 0, 0, 0, r);
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH2(void, spy_EndUpdate,
    AROS_LHA(struct Layer *, l, A0),
    AROS_LHA(UWORD, flag, D0),
    struct Library *, LayersBase, 14, Layers)
{
    AROS_LIBFUNC_INIT
    AROS_CALL2NR(void, OLD(P_ENDUPDATE),
        AROS_LDA(struct Layer *, l, A0), AROS_LDA(UWORD, flag, D0),
        struct Library *, LayersBase);
    if (log_all)
        spy_log("EndUpdate", l, flag, 0, 0, 0, 0);
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, spy_DeleteLayer,
    AROS_LHA(LONG, dummy, A0),
    AROS_LHA(struct Layer *, l, A1),
    struct Library *, LayersBase, 15, Layers)
{
    AROS_LIBFUNC_INIT
    struct Layer_Info *li = l ? l->LayerInfo : NULL;
    LONG r;

    spy_log("DeleteLayer", l, 0, 0, 0, 0, 0);
    r = AROS_CALL2(LONG, OLD(P_DELETELAYER),
        AROS_LDA(LONG, dummy, A0), AROS_LDA(struct Layer *, l, A1),
        struct Library *, LayersBase);
    spy_walk(li, "DeleteLayer");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH2(void, spy_LockLayer,
    AROS_LHA(LONG, dummy, A0),
    AROS_LHA(struct Layer *, l, A1),
    struct Library *, LayersBase, 16, Layers)
{
    AROS_LIBFUNC_INIT
    AROS_CALL2NR(void, OLD(P_LOCKLAYER),
        AROS_LDA(LONG, dummy, A0), AROS_LDA(struct Layer *, l, A1),
        struct Library *, LayersBase);
    if (log_all)
        spy_log("LockLayer", l, 0, 0, 0, 0, 0);
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, spy_MoveLayerInFrontOf,
    AROS_LHA(struct Layer *, l, A0),
    AROS_LHA(struct Layer *, other, A1),
    struct Library *, LayersBase, 28, Layers)
{
    AROS_LIBFUNC_INIT
    LONG r = AROS_CALL2(LONG, OLD(P_MOVELAYERINFRONTOF),
        AROS_LDA(struct Layer *, l, A0), AROS_LDA(struct Layer *, other, A1),
        struct Library *, LayersBase);
    spy_log("MoveLayerInFrontOf", l, (LONG)(IPTR)other, 0, 0, 0, r);
    spy_walk(l->LayerInfo, "MoveLayerInFrontOf");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH2(struct Region *, spy_InstallClipRegion,
    AROS_LHA(struct Layer *, l, A0),
    AROS_LHA(struct Region *, region, A1),
    struct Library *, LayersBase, 29, Layers)
{
    AROS_LIBFUNC_INIT
    struct Region *r = AROS_CALL2(struct Region *, OLD(P_INSTALLCLIPREGION),
        AROS_LDA(struct Layer *, l, A0), AROS_LDA(struct Region *, region, A1),
        struct Library *, LayersBase);
    if (log_all)
        spy_log("InstallClipRegion", l, (LONG)(IPTR)region, 0, 0, 0, (IPTR)r);
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH5(LONG, spy_MoveSizeLayer,
    AROS_LHA(struct Layer *, l, A0),
    AROS_LHA(LONG, dx, D0),
    AROS_LHA(LONG, dy, D1),
    AROS_LHA(LONG, dw, D2),
    AROS_LHA(LONG, dh, D3),
    struct Library *, LayersBase, 30, Layers)
{
    AROS_LIBFUNC_INIT
    LONG r = AROS_CALL5(LONG, OLD(P_MOVESIZELAYER),
        AROS_LDA(struct Layer *, l, A0), AROS_LDA(LONG, dx, D0),
        AROS_LDA(LONG, dy, D1), AROS_LDA(LONG, dw, D2), AROS_LDA(LONG, dh, D3),
        struct Library *, LayersBase);
    spy_log("MoveSizeLayer", l, dx, dy, dw, dh, r);
    spy_walk(l->LayerInfo, "MoveSizeLayer");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH9(struct Layer *, spy_CreateUpfrontHookLayer,
    AROS_LHA(struct Layer_Info *, li, A0),
    AROS_LHA(struct BitMap *, bm, A1),
    AROS_LHA(LONG, x0, D0),
    AROS_LHA(LONG, y0, D1),
    AROS_LHA(LONG, x1, D2),
    AROS_LHA(LONG, y1, D3),
    AROS_LHA(LONG, flags, D4),
    AROS_LHA(struct Hook *, hook, A3),
    AROS_LHA(struct BitMap *, bm2, A2),
    struct Library *, LayersBase, 31, Layers)
{
    AROS_LIBFUNC_INIT
    struct Layer *r = AROS_CALL9(struct Layer *, OLD(P_CREATEUPFRONTHOOKLAYER),
        AROS_LDA(struct Layer_Info *, li, A0), AROS_LDA(struct BitMap *, bm, A1),
        AROS_LDA(LONG, x0, D0), AROS_LDA(LONG, y0, D1), AROS_LDA(LONG, x1, D2),
        AROS_LDA(LONG, y1, D3), AROS_LDA(LONG, flags, D4),
        AROS_LDA(struct Hook *, hook, A3), AROS_LDA(struct BitMap *, bm2, A2),
        struct Library *, LayersBase);
    spy_log("CreateUpfrontHookLayer", r, x0, y0, x1, y1, flags);
    spy_walk(li, "CreateUpfrontHookLayer");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH9(struct Layer *, spy_CreateBehindHookLayer,
    AROS_LHA(struct Layer_Info *, li, A0),
    AROS_LHA(struct BitMap *, bm, A1),
    AROS_LHA(LONG, x0, D0),
    AROS_LHA(LONG, y0, D1),
    AROS_LHA(LONG, x1, D2),
    AROS_LHA(LONG, y1, D3),
    AROS_LHA(LONG, flags, D4),
    AROS_LHA(struct Hook *, hook, A3),
    AROS_LHA(struct BitMap *, bm2, A2),
    struct Library *, LayersBase, 32, Layers)
{
    AROS_LIBFUNC_INIT
    struct Layer *r = AROS_CALL9(struct Layer *, OLD(P_CREATEBEHINDHOOKLAYER),
        AROS_LDA(struct Layer_Info *, li, A0), AROS_LDA(struct BitMap *, bm, A1),
        AROS_LDA(LONG, x0, D0), AROS_LDA(LONG, y0, D1), AROS_LDA(LONG, x1, D2),
        AROS_LDA(LONG, y1, D3), AROS_LDA(LONG, flags, D4),
        AROS_LDA(struct Hook *, hook, A3), AROS_LDA(struct BitMap *, bm2, A2),
        struct Library *, LayersBase);
    spy_log("CreateBehindHookLayer", r, x0, y0, x1, y1, flags);
    spy_walk(li, "CreateBehindHookLayer");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH3(void, spy_SortLayerCR,
    AROS_LHA(struct Layer *, l, A0),
    AROS_LHA(LONG, dx, D0),
    AROS_LHA(LONG, dy, D1),
    struct Library *, LayersBase, 35, Layers)
{
    AROS_LIBFUNC_INIT
    AROS_CALL3NR(void, OLD(P_SORTLAYERCR),
        AROS_LDA(struct Layer *, l, A0), AROS_LDA(LONG, dx, D0),
        AROS_LDA(LONG, dy, D1), struct Library *, LayersBase);
    if (log_all)
        spy_log("SortLayerCR", l, dx, dy, 0, 0, 0);
    AROS_LIBFUNC_EXIT
}

AROS_LH3(struct Region *, spy_ChangeLayerShape,
    AROS_LHA(struct Layer *, l, A0),
    AROS_LHA(struct Region *, newshape, A1),
    AROS_LHA(struct Hook *, callback, A2),
    struct Library *, LayersBase, 37, Layers)
{
    AROS_LIBFUNC_INIT
    struct Region *r = AROS_CALL3(struct Region *, OLD(P_CHANGELAYERSHAPE),
        AROS_LDA(struct Layer *, l, A0), AROS_LDA(struct Region *, newshape, A1),
        AROS_LDA(struct Hook *, callback, A2), struct Library *, LayersBase);
    spy_log("ChangeLayerShape", l, (LONG)(IPTR)newshape, 0, 0, 0, (IPTR)r);
    spy_walk(l->LayerInfo, "ChangeLayerShape");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH8(struct Layer *, spy_CreateUpfrontLayerTagList,
    AROS_LHA(struct Layer_Info *, li, A0),
    AROS_LHA(struct BitMap *, bm, A1),
    AROS_LHA(LONG, x0, D0),
    AROS_LHA(LONG, y0, D1),
    AROS_LHA(LONG, x1, D2),
    AROS_LHA(LONG, y1, D3),
    AROS_LHA(LONG, flags, D4),
    AROS_LHA(struct TagItem *, tags, A2),
    struct Library *, LayersBase, 39, Layers)
{
    AROS_LIBFUNC_INIT
    struct Layer *r = AROS_CALL8(struct Layer *,
        OLD(P_CREATEUPFRONTLAYERTAGLIST),
        AROS_LDA(struct Layer_Info *, li, A0), AROS_LDA(struct BitMap *, bm, A1),
        AROS_LDA(LONG, x0, D0), AROS_LDA(LONG, y0, D1), AROS_LDA(LONG, x1, D2),
        AROS_LDA(LONG, y1, D3), AROS_LDA(LONG, flags, D4),
        AROS_LDA(struct TagItem *, tags, A2), struct Library *, LayersBase);
    spy_log("CreateUpfrontLayerTagList", r, x0, y0, x1, y1, flags);
    spy_walk(li, "CreateUpfrontLayerTagList");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH8(struct Layer *, spy_CreateBehindLayerTagList,
    AROS_LHA(struct Layer_Info *, li, A0),
    AROS_LHA(struct BitMap *, bm, A1),
    AROS_LHA(LONG, x0, D0),
    AROS_LHA(LONG, y0, D1),
    AROS_LHA(LONG, x1, D2),
    AROS_LHA(LONG, y1, D3),
    AROS_LHA(LONG, flags, D4),
    AROS_LHA(struct TagItem *, tags, A2),
    struct Library *, LayersBase, 40, Layers)
{
    AROS_LIBFUNC_INIT
    struct Layer *r = AROS_CALL8(struct Layer *,
        OLD(P_CREATEBEHINDLAYERTAGLIST),
        AROS_LDA(struct Layer_Info *, li, A0), AROS_LDA(struct BitMap *, bm, A1),
        AROS_LDA(LONG, x0, D0), AROS_LDA(LONG, y0, D1), AROS_LDA(LONG, x1, D2),
        AROS_LDA(LONG, y1, D3), AROS_LDA(LONG, flags, D4),
        AROS_LDA(struct TagItem *, tags, A2), struct Library *, LayersBase);
    spy_log("CreateBehindLayerTagList", r, x0, y0, x1, y1, flags);
    spy_walk(li, "CreateBehindLayerTagList");
    return r;
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, spy_ChangeLayerVisibility,
    AROS_LHA(struct Layer *, l, A0),
    AROS_LHA(int, visible, D0),
    struct Library *, LayersBase, 41, Layers)
{
    AROS_LIBFUNC_INIT
    LONG r;

    spy_log("ChangeLayerVisibility (before)", l, visible, 0, 0, 0, 0);
    spy_walk(l->LayerInfo, "ChangeLayerVisibility (before)");
    r = AROS_CALL2(LONG, OLD(P_CHANGELAYERVISIBILITY),
        AROS_LDA(struct Layer *, l, A0), AROS_LDA(int, visible, D0),
        struct Library *, LayersBase);
    spy_log("ChangeLayerVisibility", l, visible, 0, 0, 0, r);
    spy_walk(l->LayerInfo, "ChangeLayerVisibility");
    return r;
    AROS_LIBFUNC_EXIT
}

#define PATCH(idx, vec, fn, q) \
    patches[idx].name = #fn; patches[idx].lvo = vec; \
    patches[idx].newfunc = (APTR)AROS_SLIB_ENTRY(spy_##fn, Layers, vec); \
    patches[idx].quiet = q;

static void setup_patches(void)
{
    PATCH(P_UPFRONTLAYER, 8, UpfrontLayer, FALSE)
    PATCH(P_BEHINDLAYER, 9, BehindLayer, FALSE)
    PATCH(P_MOVELAYER, 10, MoveLayer, FALSE)
    PATCH(P_SIZELAYER, 11, SizeLayer, FALSE)
    PATCH(P_BEGINUPDATE, 13, BeginUpdate, TRUE)
    PATCH(P_ENDUPDATE, 14, EndUpdate, TRUE)
    PATCH(P_DELETELAYER, 15, DeleteLayer, FALSE)
    PATCH(P_LOCKLAYER, 16, LockLayer, TRUE)
    PATCH(P_MOVELAYERINFRONTOF, 28, MoveLayerInFrontOf, FALSE)
    PATCH(P_INSTALLCLIPREGION, 29, InstallClipRegion, TRUE)
    PATCH(P_MOVESIZELAYER, 30, MoveSizeLayer, FALSE)
    PATCH(P_CREATEUPFRONTHOOKLAYER, 31, CreateUpfrontHookLayer, FALSE)
    PATCH(P_CREATEBEHINDHOOKLAYER, 32, CreateBehindHookLayer, FALSE)
    PATCH(P_SORTLAYERCR, 35, SortLayerCR, TRUE)
    PATCH(P_CHANGELAYERSHAPE, 37, ChangeLayerShape, FALSE)
    PATCH(P_CREATEUPFRONTLAYERTAGLIST, 39, CreateUpfrontLayerTagList, FALSE)
    PATCH(P_CREATEBEHINDLAYERTAGLIST, 40, CreateBehindLayerTagList, FALSE)
    PATCH(P_CHANGELAYERVISIBILITY, 41, ChangeLayerVisibility, FALSE)
}

static BOOL open_timer(void)
{
    timer_port = CreateMsgPort();
    if (timer_port == NULL)
        return FALSE;
    timer_req = (struct timerequest *)CreateIORequest(timer_port,
        sizeof(struct timerequest));
    if (timer_req == NULL)
        return FALSE;
    if (OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest *)timer_req,
        0) != 0)
    {
        DeleteIORequest((struct IORequest *)timer_req);
        timer_req = NULL;
        return FALSE;
    }
    TimerBase = timer_req->tr_node.io_Device;
    {
        struct EClockVal ev;

        eclock_freq = ReadEClock(&ev);
    }
    return TRUE;
}

static void close_timer(void)
{
    if (timer_req)
    {
        CloseDevice((struct IORequest *)timer_req);
        DeleteIORequest((struct IORequest *)timer_req);
    }
    if (timer_port)
        DeleteMsgPort(timer_port);
}

int main(void)
{
    struct RDArgs *rdargs;
    IPTR args[1] = { 0 };
    LONG i;

    rdargs = ReadArgs("ALL/S", args, NULL);
    if (rdargs == NULL)
    {
        PrintFault(IoErr(), "layerspy");
        return RETURN_FAIL;
    }
    log_all = args[0] != 0;
    FreeArgs(rdargs);

    open_timer();
    setup_patches();

    Forbid();
    for (i = 0; i < P_COUNT; ++i)
        patches[i].oldfunc = SetFunction((struct Library *)LayersBase,
            -patches[i].lvo * LIB_VECTSIZE_LAYERS, patches[i].newfunc);
    Permit();
    printf("layerspy: %ld layers.library patches installed%s; CTRL-C to "
        "remove\n", (long)P_COUNT, log_all ? " (ALL)" : "");
    fflush(stdout);
    kprintf("[LAYERSPY] installed (eclock %lu Hz)\n", (ULONG)eclock_freq);

    Wait(SIGBREAKF_CTRL_C);

    Forbid();
    for (i = 0; i < P_COUNT; ++i)
        SetFunction((struct Library *)LayersBase,
            -patches[i].lvo * LIB_VECTSIZE_LAYERS, patches[i].oldfunc);
    Permit();
    close_timer();
    printf("layerspy: patches removed\n");
    return RETURN_OK;
}
