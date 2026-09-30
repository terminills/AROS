/*
    Copyright (C) 2010-2026, The AROS Development Team. All rights reserved.

    PORTED from workbench/hidds/nouveau/nouveau_compositor.h (ADT upstream).
    That file's own comment states it was written to be lifted into other
    drivers: "The code is generic where possible, using abstract OOP_Objects
    instead of concrete driver structures. There are places where nouveau
    specific calls are performed, however there are only few and can be
    generalized to (private) methods that should be reimplemented by child
    classes in each of the drivers."

    Measured before porting: exactly TWO driver-specific calls across the whole
    implementation -- SwitchToVideoMode and SetOffsets -- plus the includes and
    the static-data macro. Everything else is driver-agnostic.
*/


#ifndef _COMPOSITOR_INTERN_H
#define _COMPOSITOR_INTERN_H

#include "compositor.h"

#include <exec/lists.h>

struct _Rectangle
{
    WORD MinX;
    WORD MinY;
    WORD MaxX;
    WORD MaxY;
};

struct StackBitMapNode
{
    struct Node         n;
    OOP_Object *        bm;
    struct _Rectangle   screenvisiblerect;
    BOOL                isscreenvisible;
    LONG                displayedwidth;
    LONG                displayedheight;
};

struct HIDDCompositorData
{
    /* Bitmap to which all screen bitmaps are composited. Height/Width always 
       matches visible mode */
    OOP_Object              *compositedbitmap;
    
    /* Pointer to actuall screen bitmap - either compositedbitmap or topbitmap. 
       Can only be set in HIDDCompositorToggleCompositing */
    OOP_Object              *screenbitmap;

    /* Pointer to top bitmap on stack */
    OOP_Object              *topbitmap;

    HIDDT_ModeID            screenmodeid;   /* ModeID of currently visible mode */
    struct _Rectangle       screenrect;     /* Dimensions of currently visible mode */
    BOOL                    modeschanged;   /* TRUE if new top bitmap has different mode than current screenmodeid */
    /* Pending stack/geometry is not safe to redraw into the retained scanout.
       Cleared only when a subsequent switch succeeds. This does not claim
       hardware rollback or retain caller-owned bitmaps after their disposal. */
    BOOL                    display_update_failed;

    struct List             bitmapstack;
    
    struct SignalSemaphore  semaphore;
    
    OOP_Object              *gfx;           /* GFX driver object */
    OOP_Object              *display;       /* Display driver object */
    OOP_Object              *dmenum;        /* Display mode enumerator object */
    OOP_Object              *gc;            /* GC object used for drawing operations */

    /* Attr bases */
    OOP_AttrBase    pixFmtAttrBase;
    OOP_AttrBase    syncAttrBase;
    OOP_AttrBase    bitMapAttrBase;
    OOP_AttrBase    gcAttrBase;
    OOP_AttrBase    displayAttrBase;
    OOP_AttrBase    compositorAttrBase;
};

#define METHOD(base, id, name) \
  base ## __ ## id ## __ ## name (OOP_Class *cl, OOP_Object *o, struct p ## id ## _ ## name *msg)

#define BASE(lib)                   ((LIBBASETYPEPTR)(lib))

#define SD(cl)                      (&BASE(cl->UserData)->sd)

#define LOCK_COMPOSITOR_READ       { ObtainSemaphoreShared(&compdata->semaphore); }
#define LOCK_COMPOSITOR_WRITE      { ObtainSemaphore(&compdata->semaphore); }
#define UNLOCK_COMPOSITOR          { ReleaseSemaphore(&compdata->semaphore); }


/* nouveau routes this to nouveau_compat_log; amdgpu has no compat layer, so
 * it goes straight to the debug channel. Kept as nvlog() so the ported body
 * stays diffable against the original. */
#define nvlog(...) bug(__VA_ARGS__)

/* Driver hooks the compositor calls. Implemented in amdgpu_displayclass.c. */
BOOL amdgpu_compositor_switch_mode(OOP_Class *cl, OOP_Object *bm);
VOID amdgpu_compositor_set_offsets(OOP_Class *cl, OOP_Object *bm, LONG xoffset, LONG yoffset);

#endif /* _COMPOSITOR_INTERN_H */
