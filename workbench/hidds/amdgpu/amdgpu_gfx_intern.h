#ifndef AMDGPU_GFX_INTERN_H
#define AMDGPU_GFX_INTERN_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: amdgpu boot-mode framebuffer Gfx private data.

    This is the OOP gfx-HIDD half of amdgpu.hidd: it adopts the firmware/GOP
    framebuffer (bootloader.resource VesaInfo) as a real AROS display driver.
    The gfx class staticdata lives inside the module libbase (AmdgpuHiddBase)
    so the genmodule classptr_field mechanism can reach it, while the separate
    global `struct AmdgpuBase amdgpuBase` (amdgpu_init.c) keeps owning the PCI
    device list and silicon-wake state.
*/

#ifndef EXEC_LIBRARIES_H
#   include <exec/libraries.h>
#endif

#ifndef OOP_OOP_H
#   include <oop/oop.h>
#endif

#ifndef EXEC_SEMAPHORES_H
#   include <exec/semaphores.h>
#endif

#include <hidd/gfx.h>

#include "amdgpu_gfx_support.h"
#include "amdgpu_cursor_mirror.h"

#define ATTRBASES_NUM 11

/* Forward decl: the display class reaches the silicon (register aperture,
   VRAM BAR, dmu_seg for DCN scanout programming) through this pointer. Full
   definition in amdgpu_intern.h. This is the bridge from the GOP-adopt display
   to a VRAM-backed, register-programmed one. */
struct AmdgpuDevice;

struct AmdgpuGfx_staticdata
{
    OOP_Class 	    	    *basebm;            /* baseclass for CreateObject */
    OOP_Class               *basegc;            /* baseclass for CreateObject (compositor GC) */

    struct AmdgpuDevice     *device;           /* primary DCN/display device, or NULL */

    OOP_Class 	    	    *gfxclass;
    OOP_Class 	    	    *displayclass;
    OOP_Class 	    	    *bmclass;
    OOP_Class               *compositorclass;
    OOP_Object      	    *gfxhidd;
    OOP_Object      	    *display;
    OOP_Object              *compositor;       /* screen composition object, or NULL */
    OOP_Object       	    *visible;		/* Currently visible bitmap */
    ULONG                   cursor_update_depth; /* display_lock protected */
    struct AmdgpuCursorMirror *cursor_mirror; /* display_lock; native probe only */
    struct HWData   	    data;
    struct SignalSemaphore  framebufferlock;
    struct SignalSemaphore  HW_acc;
    /* Lazy renderer module reference, retained by this noexpunge display
       driver so public Gallium objects never outlive their provider code. */
    struct SignalSemaphore  gallium_lock;
    struct Library         *gallium_module;
    OOP_MethodID	    mid_Dispose;
    OOP_AttrBase	    attrBases[ATTRBASES_NUM];
};

struct AmdgpuHiddBase
{
    struct Library library;
    struct AmdgpuGfx_staticdata gsd;
};

#define LOCK_FRAMEBUFFER(xsd)	ObtainSemaphore(&xsd->framebufferlock)
#define UNLOCK_FRAMEBUFFER(xsd) ReleaseSemaphore(&xsd->framebufferlock)

#define XSD(cl)	(&((struct AmdgpuHiddBase *)cl->UserData)->gsd)

#undef HiddChunkyBMAttrBase
#undef HiddBitMapAttrBase
#undef HiddGfxAttrBase
#undef HiddPixFmtAttrBase
#undef HiddSyncAttrBase
#undef HiddAttrBase
#undef HiddDisplayAttrBase
#undef HiddDMEnumAttrBase

/* These must stay in the same order as interfaces[] array in amdgpu_gfx_init.c */
#define HiddChunkyBMAttrBase	  XSD(cl)->attrBases[0]
#define HiddBitMapAttrBase	  XSD(cl)->attrBases[1]
#define HiddGfxAttrBase		  XSD(cl)->attrBases[2]
#define HiddPixFmtAttrBase	  XSD(cl)->attrBases[3]
#define HiddSyncAttrBase	  XSD(cl)->attrBases[4]
#define HiddAttrBase		  XSD(cl)->attrBases[5]
#define HiddDisplayAttrBase	  XSD(cl)->attrBases[6]
#define HiddDMEnumAttrBase	  XSD(cl)->attrBases[7]
#define aAmdgpuBitMap_Self          (XSD(cl)->attrBases[10])

/* Private, wrapper-transparent identity and position transaction. CursorFB
 * forwards Get to the native bitmap; never use its instance data as ours. */
OOP_Object *amdgpu_bitmap_native(OOP_Class *cl, OOP_Object *bm);
VOID amdgpu_bitmap_position(OOP_Class *cl, OOP_Object *bm, LONG x, LONG y);
VOID amdgpu_display_update_begin(OOP_Class *cl);
VOID amdgpu_display_update_end(OOP_Class *cl);

/* amdgpu_gfx_init.c — called from Amdgpu_Init after the silicon wake loop.
   Reads the GOP framebuffer, obtains attrbases and registers the boot-mode
   display driver (DDRV_BootMode). Returns TRUE on success. */
int AmdgpuGfx_InitDisplay(struct AmdgpuHiddBase *LIBBASE);

#endif /* AMDGPU_GFX_INTERN_H */
