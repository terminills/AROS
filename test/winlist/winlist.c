/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: List the windows and layers of the front screen (or a named one).

    Prints Intuition's window list and, front to back, the layer list of the
    screen, so a picture that stays on the screen after CloseWindow() can be
    told apart from a window that is still open: a ghost with no layer is
    stale pixels, a ghost with a layer is a window somebody never closed.

    winlist SCREEN/K,FRONT/S,CLIPRECTS/S,BRIEF/S,LOOP/N,TICKS/N
      CLIPRECTS   also print every layer's ClipRect list (bounds, the layer
                  obscuring it, its backing bitmap) and, for each window, the
                  pixel the screen bitmap holds 10,20 inside the window's
                  border, so a window that "does not paint" can be told
                  apart from a display that does not show what was painted
      BRIEF       one line per layer (time of day, bounds, visible region,
                  cliprect counts) for sampling the layer state in a loop
      LOOP n      take n samples, TICKS t (default 5) apart, printing only
                  the samples that differ from the previous one, to catch
                  the moment another program's window changes the layers
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include <proto/cybergraphics.h>

#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <cybergraphx/cybergraphics.h>

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static BOOL cliprects, brief;

/* Same clock as Chromium's log lines, so the two can be lined up. */
static void print_time(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    printf("%02ld:%02ld:%02ld.%03ld", (long)(tv.tv_sec / 3600) % 24,
        (long)(tv.tv_sec / 60) % 60, (long)tv.tv_sec % 60,
        (long)tv.tv_usec / 1000);
}

/* One line per layer into buf; the same text for the same layer state, so
   a sampling loop can print only the changes. */
static void brief_screen(struct Screen *s, char *buf, size_t len)
{
    struct Layer *l;
    size_t used = 0;

    buf[0] = '\0';
    for (l = s->LayerInfo.top_layer; l != NULL && used < len; l = l->back)
    {
        struct Window *lw = (struct Window *)l->Window;
        struct ClipRect *cr;
        LONG n = 0, hidden = 0;

        for (cr = l->ClipRect; cr != NULL; cr = cr->Next, ++n)
            if (cr->lobs)
                ++hidden;
        used += snprintf(buf + used, len - used,
            "  layer %p %ld,%ld-%ld,%ld%s vis %ld,%ld-%ld,%ld cr %ld/%ld "
            "hidden \"%s\"\n", l, (long)l->bounds.MinX, (long)l->bounds.MinY,
            (long)l->bounds.MaxX, (long)l->bounds.MaxY,
            IS_VISIBLE(l) ? "" : " HIDDEN",
            (long)l->VisibleRegion->bounds.MinX,
            (long)l->VisibleRegion->bounds.MinY,
            (long)l->VisibleRegion->bounds.MaxX,
            (long)l->VisibleRegion->bounds.MaxY, (long)hidden, (long)n,
            lw && lw->Title ? (char *)lw->Title : "");
    }
}

static void print_region(const char *name, struct Region *r)
{
    struct RegionRectangle *rr;
    LONG n = 0;

    if (r == NULL)
    {
        printf("    %s NULL\n", name);
        return;
    }
    for (rr = r->RegionRectangle; rr != NULL; rr = rr->Next)
        ++n;
    printf("    %s bounds %ld,%ld-%ld,%ld %ld rects\n", name,
        (long)r->bounds.MinX, (long)r->bounds.MinY, (long)r->bounds.MaxX,
        (long)r->bounds.MaxY, (long)n);
}

static void list_cliprects(struct Layer *l)
{
    struct ClipRect *cr;
    LONG n;

    print_region("shape", l->shape);
    print_region("visibleshape", l->visibleshape);
    print_region("VisibleRegion", l->VisibleRegion);
    print_region("shaperegion", l->shaperegion);
    for (cr = l->ClipRect, n = 0; cr != NULL; cr = cr->Next, ++n)
        printf("    cliprect %ld,%ld-%ld,%ld lobs %p bitmap %p flags 0x%lx\n",
            (long)cr->bounds.MinX, (long)cr->bounds.MinY,
            (long)cr->bounds.MaxX, (long)cr->bounds.MaxY, cr->lobs,
            cr->BitMap, (unsigned long)cr->Flags);
    printf("    %ld cliprects, damage %p\n", (long)n, l->DamageList);
}

/* What the screen bitmap holds inside the window, read past the layers. */
static ULONG screen_pixel(struct Screen *s, struct Window *w)
{
    ULONG pixel = 0;
    LONG x = w->LeftEdge + w->BorderLeft + 10;
    LONG y = w->TopEdge + w->BorderTop + 20;

    ReadPixelArray(&pixel, 0, 0, sizeof(pixel), &s->RastPort, x, y, 1, 1,
        RECTFMT_ARGB);
    return pixel;
}

static void list_screen(struct Screen *s)
{
    struct Window *w;
    struct Layer *l;
    LONG n;

    printf("screen %p \"%s\" %ldx%ld first window %p\n", s,
        s->Title ? (char *)s->Title : "", (long)s->Width, (long)s->Height,
        s->FirstWindow);

    for (w = s->FirstWindow, n = 0; w != NULL; w = w->NextWindow, ++n)
    {
        struct Layer *wl = w->WLayer;

        printf("  window %p \"%s\" at %ld,%ld %ldx%ld flags 0x%08lx "
            "idcmp 0x%08lx port %p layer %p%s\n", w,
            w->Title ? (char *)w->Title : "", (long)w->LeftEdge,
            (long)w->TopEdge, (long)w->Width, (long)w->Height,
            (unsigned long)w->Flags, (unsigned long)w->IDCMPFlags,
            w->UserPort, wl, wl && IS_VISIBLE(wl) ? "" : " HIDDEN");
        if (cliprects)
            printf("    screen pixel 10,20 inside = 0x%08lx\n",
                (unsigned long)screen_pixel(s, w));
    }
    printf("  %ld windows\n", (long)n);

    for (l = s->LayerInfo.top_layer, n = 0; l != NULL; l = l->back, ++n)
    {
        struct Window *lw = (struct Window *)l->Window;

        printf("  layer %p %ld,%ld-%ld,%ld flags 0x%04lx pri %ld%s window %p "
            "\"%s\"\n", l, (long)l->bounds.MinX, (long)l->bounds.MinY,
            (long)l->bounds.MaxX, (long)l->bounds.MaxY,
            (unsigned long)l->Flags, (long)l->priority,
            IS_VISIBLE(l) ? "" : " HIDDEN", lw,
            lw && lw->Title ? (char *)lw->Title : "");
        if (cliprects)
            list_cliprects(l);
    }
    printf("  %ld layers\n", (long)n);
}

static char sample[2][8192];

int main(void)
{
    struct RDArgs *rdargs;
    IPTR args[6] = { 0, 0, 0, 0, 0, 0 };
    struct Screen *s;
    ULONG ilock;
    LONG loop = 1, ticks = 5, i, cur = 0;

    rdargs = ReadArgs("SCREEN/K,FRONT/S,CLIPRECTS/S,BRIEF/S,LOOP/N,TICKS/N",
        args, NULL);
    if (rdargs == NULL)
    {
        PrintFault(IoErr(), "winlist");
        return RETURN_FAIL;
    }
    cliprects = args[2] != 0;
    brief = args[3] != 0;
    if (args[4])
    {
        loop = *(LONG *)args[4];
        brief = TRUE;
    }
    if (args[5])
        ticks = *(LONG *)args[5];

    for (i = 0; i < loop; ++i)
    {
        if (i > 0)
            Delay(ticks);
        ilock = LockIBase(0);
        for (s = IntuitionBase->FirstScreen; s != NULL; s = s->NextScreen)
        {
            if (args[0] != 0 && (s->Title == NULL
                || strcmp((char *)s->Title, (char *)args[0]) != 0))
                continue;
            if (brief)
                brief_screen(s, sample[cur], sizeof(sample[cur]));
            else
                list_screen(s);
            if (args[0] != 0 || args[1] != 0)
                break;
        }
        UnlockIBase(ilock);
        if (brief && (i == 0 || strcmp(sample[0], sample[1]) != 0))
        {
            print_time();
            printf(" sample %ld\n%s", (long)i, sample[cur]);
            fflush(stdout);
        }
        cur ^= 1;
    }

    FreeArgs(rdargs);
    return RETURN_OK;
}
