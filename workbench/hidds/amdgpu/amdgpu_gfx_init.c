/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: amdgpu boot-mode framebuffer Gfx Hidd display registration.

    Unlike efigfx.hidd (which registers from its own ADD2INITLIB), amdgpu.hidd
    already owns a single ADD2INITLIB (Amdgpu_Init in amdgpu_init.c) that runs
    the PCI probe + PSP silicon wake. The boot-mode display is registered from
    the END of that init via AmdgpuGfx_InitDisplay(), so opening amdgpu.hidd
    both wakes the GPU AND publishes the display.
*/

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/oop.h>

#include <exec/types.h>
#include <exec/lists.h>
#include <graphics/driver.h>
#include <graphics/gfxbase.h>
#include <hidd/gfx.h>
#include <oop/oop.h>
#include <utility/utility.h>
#include <aros/symbolsets.h>

#include "amdgpu_intern.h"       /* AmdgpuDevice/AmdgpuBase + amdgpuBase, wreg32, dmu_seg */
#include "amdgpu_gfx_intern.h"
#include "amdgpu_gfx_support.h"
#include "amdgpu_gfx_hidd.h"

#include LC_LIBDEFS_FILE

#define DEBUG 0
#include <aros/debug.h>

static void FreeAttrBases(const STRPTR *iftable, OOP_AttrBase *bases, ULONG num)
{
    ULONG i;

    for (i = 0; i < num; i++)
    {
        if (bases[i])
            OOP_ReleaseAttrBase(iftable[i]);
    }
}

static BOOL GetAttrBases(const STRPTR *iftable, OOP_AttrBase *bases, ULONG num)
{
    ULONG i;

    for (i = 0; i < num; i++)
    {
        bases[i] = OOP_ObtainAttrBase(iftable[i]);
        if (!bases[i])
        {
            FreeAttrBases(iftable, bases, i);
            return FALSE;
        }
    }

    return TRUE;
}

/* These must stay in the same order as attrBases[] entries assignment in amdgpu_gfx_intern.h */
static const STRPTR interfaces[ATTRBASES_NUM] =
{
    IID_Hidd_ChunkyBM,
    IID_Hidd_BitMap,
    IID_Hidd_Gfx,
    IID_Hidd_PixFmt,
    IID_Hidd_Sync,
    IID_Hidd,
    IID_Hidd_Display,
    IID_Hidd_DMEnum,
    IID_Hidd_Compositor,
    IID_Hidd_GC,
    IID_Hidd_BitMap_Amdgpu
};

int AmdgpuGfx_InitDisplay(struct AmdgpuHiddBase *LIBBASE)
{
    struct AmdgpuGfx_staticdata *xsd = &LIBBASE->gsd;
    struct GfxBase *GfxBase;
    ULONG err, monitor_id = ~(ULONG)0;
    int res = FALSE;

    /*
     * Bridge the display class to the silicon. The tier-1 wake loop probed the
     * GPU(s) into amdgpuBase.devices; bind the primary display-capable one (DMU
     * IP block valid) so the display/bitmap/DCN code can reach the register
     * aperture (amdgpu_wreg32), the VRAM BAR and dmu_seg[2] for scanout
     * programming. Without this the display can only adopt the GOP framebuffer.
     */
    {
        struct AmdgpuDevice *ad;
        xsd->device = NULL;
        ForeachNode(&amdgpuBase.devices, ad)
        {
            if (!xsd->device)
                xsd->device = ad;               /* first probed device = fallback */
            if (ad->ip.dmu.valid)               /* prefer the one with a display block */
            {
                xsd->device = ad;
                break;
            }
        }
        if (xsd->device)
            D(bug("[AmdgpuGfx] bound display device %04x:%04x rmmio=0x%p vram_bar=0x%p/%uMB dmu_seg2=0x%08x\n",
                  xsd->device->vendorid, xsd->device->deviceid, xsd->device->rmmio,
                  (void *)xsd->device->vram_bar_phys, (unsigned)xsd->device->vram_mb,
                  (unsigned)xsd->device->dmu_seg[2]));
        else
        {
            /* No AMD GPU in the system: leave the display to the driver that
               already owns it (efifbgfx/VESA). Registering here would take
               over the boot display and draw into a framebuffer no AMD
               scanout is showing. */
            bug("[AmdgpuGfx] no AMD GPU found - display driver not registered\n");
            return FALSE;
        }
    }

    /*
     * Open graphics.library ourselves because we will close it
     * after adding the driver.
     * Autoinit code would close it only upon driver expunge.
     */
    GfxBase = (struct GfxBase *)TaggedOpenLibrary(TAGGEDOPEN_GRAPHICS);
    if (GfxBase)
    {
        if (initAmdgpuGfxHW(&xsd->data))
        {
            /* Prepare owned storage, keeping the current boot driver's plane
             * intact. A later AMD screen selection performs the live switch. */
            amdgpuGfxVRAMHandoff(xsd);
            amdgpu_cursor_mirror_prepare(xsd);

            if (GetAttrBases(interfaces, xsd->attrBases, ATTRBASES_NUM))
            {
                xsd->basebm = OOP_FindClass(CLID_Hidd_BitMap);
                /* The compositor asks the display to CreateObject() a GC to
                 * draw with, and has to name the base class to create it from.
                 * nouveau_init.c:50 does exactly this. */
                xsd->basegc = OOP_FindClass(CLID_Hidd_GC);
                xsd->mid_Dispose = OOP_GetMethodID(IID_Root, moRoot_Dispose);
                D(bug("[AmdgpuGfx] BitMap class @ 0x%p\n", xsd->basebm));

                InitSemaphore(&xsd->framebufferlock);
                InitSemaphore(&xsd->HW_acc);
                InitSemaphore(&xsd->gallium_lock);
                xsd->cursor_update_depth = 0;
                bug("[amdgpu-pan] viewport-v1: native offsets, wrapper identity, no scanout self-copy\n");

                D(bug("[AmdgpuGfx] Init: Everything OK, installing driver\n"));

                /* Publish native modes. Current graphics.library leaves an
                 * already-open VESA public screen/default_monitor in place;
                 * registration is NOT proof of desktop migration or ownership. */
                err = AddDisplayDriver(xsd->gfxclass, NULL, DDRV_BootMode, FALSE,
                    DDRV_ResultID, (IPTR)&monitor_id, TAG_DONE);

                bug("[AmdgpuGfx] registration result=%lu monitor=%08lx; existing public screen is not migrated\n",
                    (unsigned long)err, (unsigned long)monitor_id);
                if (xsd->device) {
                    UQUAD mc = 0;
                    ULONG w = 0, h = 0;
                    BOOL master = FALSE;
                    if (amdgpu_dcn_get_scanout(xsd->device, &mc, &w, &h, &master))
                        bug("[AmdgpuGfx] post-registration scanout=%08lx:%08lx viewport=%lux%lu master=%lu (readback, not a screen-ownership claim)\n",
                            (unsigned long)(mc >> 32), (unsigned long)(ULONG)mc,
                            (unsigned long)w, (unsigned long)h, (unsigned long)master);
                }
                if (!err)
                {
                    /* expunge protection */
                    LIBBASE->library.lib_OpenCnt = 1;
                    res = TRUE;
                }
            }
        }
        CloseLibrary(&GfxBase->LibNode);
    }
    else
    {
        D(bug("[AmdgpuGfx] Failed to open graphics.library!\n"));
    }
    return res;
}
