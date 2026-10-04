/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Display class for the amdgpu boot-mode framebuffer.
*/

#include <aros/asmcall.h>
#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <aros/symbolsets.h>
#include <devices/inputevent.h>
#include <exec/alerts.h>
#include <exec/memory.h>
#include <hidd/hidd.h>
#include <hidd/gfx.h>
#include <hidd/gallium.h>
#include <oop/oop.h>
#include <clib/alib_protos.h>
#include <string.h>

#define DEBUG 0
#include <aros/debug.h>

#ifndef __OOP_NOATTRBASES__
#define __OOP_NOATTRBASES__
#endif

#include "amdgpu_intern.h"       /* AmdgpuDevice, AmdgpuDisplayMode, amdgpu_display_probe_modes */
#include "amdgpu_gfx_intern.h"
#include "amdgpu_compositor.h"
#include "amdgpu_gfx_hidd.h"
#include "amdgpu_gfx_support.h"
#include "amdgpu_plane.h"
#include "amdgpu_cs.h"

#include LC_LIBDEFS_FILE

/* Build one aHidd_Sync_* tag array (AllocVec'd, persists for the driver's life)
 * from a probed mode's CVT-computed timing. The "AMDGPU:%hx%v" description makes
 * graphics.library fill each mode's own WxH, so one format string serves all. */
static struct TagItem *amdgpu_build_sync(OOP_Class *cl, struct AmdgpuDisplayMode *m)
{
    struct TagItem *s = AllocVec(sizeof(struct TagItem) * 16, MEMF_PUBLIC | MEMF_CLEAR);
    LONG j = 0;

    if (!s)
        return NULL;
    s[j].ti_Tag = aHidd_Sync_PixelClock;  s[j++].ti_Data = (IPTR)m->pixelclock * 1000;
    s[j].ti_Tag = aHidd_Sync_HDisp;       s[j++].ti_Data = m->w;
    s[j].ti_Tag = aHidd_Sync_HSyncStart;  s[j++].ti_Data = m->hsync_start;
    s[j].ti_Tag = aHidd_Sync_HSyncEnd;    s[j++].ti_Data = m->hsync_end;
    s[j].ti_Tag = aHidd_Sync_HTotal;      s[j++].ti_Data = m->htotal;
    s[j].ti_Tag = aHidd_Sync_HMin;        s[j++].ti_Data = m->w;
    s[j].ti_Tag = aHidd_Sync_HMax;        s[j++].ti_Data = 4096;   /* bitmap range, not display timing (matches NV) */
    s[j].ti_Tag = aHidd_Sync_VDisp;       s[j++].ti_Data = m->h;
    s[j].ti_Tag = aHidd_Sync_VSyncStart;  s[j++].ti_Data = m->vsync_start;
    s[j].ti_Tag = aHidd_Sync_VSyncEnd;    s[j++].ti_Data = m->vsync_end;
    s[j].ti_Tag = aHidd_Sync_VTotal;      s[j++].ti_Data = m->vtotal;
    s[j].ti_Tag = aHidd_Sync_VMin;        s[j++].ti_Data = m->h;
    s[j].ti_Tag = aHidd_Sync_VMax;        s[j++].ti_Data = 4096;   /* bitmap range, not display timing (matches NV) */
    /* Per-mode description WITH the refresh rate (Nick: modes need supported
     * frequencies, like the NV: modes). %h/%v stay as graphics.library codes for
     * WxH; the "@<hz>" is appended literally. amdgpu deliberately avoids sprintf
     * (stdc.library not up at this residentpri), so build the number by hand. */
    {
        STRPTR desc = AllocVec(32, MEMF_PUBLIC | MEMF_CLEAR);
        if (desc) {
            /* The nominal rate the mode was asked for, as NV: modes show it;
               the rate derived from the timing rounds to nearest otherwise
               (CVT rounds the pixel clock down, so 60 Hz came out 59.9x
               and was printed as 59). */
            ULONG vt = (ULONG)m->htotal * (ULONG)m->vtotal;
            ULONG hz = m->refresh ? (ULONG)m->refresh :
                       vt ? (ULONG)(((UQUAD)m->pixelclock * 1000UL + vt / 2) / vt) : 0;
            STRPTR d = desc;
            const char *p = "AMDGPU:%hx%v@";
            while (*p) *d++ = *p++;
            if (hz == 0) {
                *d++ = '0';
            } else {
                char tmp[12]; int n = 0;
                while (hz) { tmp[n++] = (char)('0' + (hz % 10)); hz /= 10; }
                while (n) *d++ = tmp[--n];
            }
            *d = '\0';
            s[j].ti_Tag = aHidd_Sync_Description; s[j++].ti_Data = (IPTR)desc;
        } else {
            s[j].ti_Tag = aHidd_Sync_Description; s[j++].ti_Data = (IPTR)"AMDGPU:%hx%v";
        }
    }
    s[j].ti_Tag = TAG_DONE;               s[j++].ti_Data = 0UL;
    return s;
}

OOP_Object *AmdgpuGfxDisplay__Root__New(OOP_Class *cl, OOP_Object *o, struct pRoot_New *msg)
{
    struct TagItem pftags[] =
    {
        {aHidd_PixFmt_RedShift,     0}, /*  0 */
        {aHidd_PixFmt_GreenShift,   0}, /*  1 */
        {aHidd_PixFmt_BlueShift,    0}, /*  2 */
        {aHidd_PixFmt_AlphaShift,   0}, /*  3 */
        {aHidd_PixFmt_RedMask,      0}, /*  4 */
        {aHidd_PixFmt_GreenMask,    0}, /*  5 */
        {aHidd_PixFmt_BlueMask,     0}, /*  6 */
        {aHidd_PixFmt_AlphaMask,    0}, /*  7 */
        {aHidd_PixFmt_ColorModel,   0}, /*  8 */
        {aHidd_PixFmt_Depth,        0}, /*  9 */
        {aHidd_PixFmt_BytesPerPixel,0}, /* 10 */
        {aHidd_PixFmt_BitsPerPixel, 0}, /* 11 */
        {aHidd_PixFmt_StdPixFmt,    vHidd_StdPixFmt_Native}, /* 12 */
        {aHidd_PixFmt_CLUTShift,    0}, /* 13 */
        {aHidd_PixFmt_CLUTMask,     0}, /* 14 */
        {aHidd_PixFmt_BitMapType,   vHidd_BitMapType_Chunky}, /* 15 */
        {TAG_DONE, 0UL }
    };
    /*
     * Second published depth. nouveau publishes a CROSS PRODUCT of syncs x pixel
     * formats (nouveau_hiddclass.c repeats aHidd_DMEnum_PixFmtTags), which is
     * where depth choice in ScreenMode comes from. We published exactly one
     * format derived from whatever the GOP left in the scanout, so the prefs
     * list only ever offered 24-bit.
     *
     * RGB565 is a fixed layout, unlike the native entry above which is filled
     * in from live hardware state. The scanout stays 32-bit; UpdateRect
     * converts on the way out, so this costs no modeset -- which the driver
     * cannot do yet anyway.
     */
    struct TagItem pftags_16[] =
    {
        {aHidd_PixFmt_RedShift,     16                      }, /*  0 */
        {aHidd_PixFmt_GreenShift,   21                      }, /*  1 */
        {aHidd_PixFmt_BlueShift,    27                      }, /*  2 */
        {aHidd_PixFmt_AlphaShift,   0                       }, /*  3 */
        {aHidd_PixFmt_RedMask,      0x0000f800              }, /*  4 */
        {aHidd_PixFmt_GreenMask,    0x000007e0              }, /*  5 */
        {aHidd_PixFmt_BlueMask,     0x0000001f              }, /*  6 */
        {aHidd_PixFmt_AlphaMask,    0x00000000              }, /*  7 */
        {aHidd_PixFmt_ColorModel,   vHidd_ColorModel_TrueColor}, /* 8 */
        {aHidd_PixFmt_Depth,        16                      }, /*  9 */
        {aHidd_PixFmt_BytesPerPixel,2                       }, /* 10 */
        {aHidd_PixFmt_BitsPerPixel, 16                      }, /* 11 */
        {aHidd_PixFmt_StdPixFmt,    vHidd_StdPixFmt_RGB16_LE}, /* 12 */
        {aHidd_PixFmt_CLUTShift,    0                       }, /* 13 */
        {aHidd_PixFmt_CLUTMask,     0                       }, /* 14 */
        {aHidd_PixFmt_BitMapType,   vHidd_BitMapType_Chunky }, /* 15 */
        {TAG_DONE, 0UL }
    };
    struct TagItem sync_mode[] =
    {
        {aHidd_Sync_PixelClock,         0                       },
        {aHidd_Sync_HTotal,             0                       },
        {aHidd_Sync_HDisp,              0                       },
        {aHidd_Sync_VDisp,              0                       },
        {aHidd_Sync_HMax,               16384                   },
        {aHidd_Sync_VMax,               16384                   },
        {aHidd_Sync_Description,        (IPTR)"AMDGPU:%hx%v"     },
        {TAG_DONE,                      0UL                     }
    };
    /* [0]=PixFmtTags, [1]=native SyncTags, [2..]=probed-mode SyncTags, then
     * TAG_DONE. Assembled at runtime once the EDID probe returns. */
    /* 3 leading entries now (two PixFmtTags + the native SyncTags), then one
     * SyncTags per probed mode, then TAG_DONE. Sized to match the loop bound
     * below -- they must move together or this overruns the stack. */
    struct TagItem modetags[3 + AMDGPU_DISPLAY_MAX_MODES + 1];
    struct TagItem dispTags[] =
    {
        {aHidd_Display_ModeTags, (IPTR)modetags},
        {TAG_MORE, 0UL}
    };
    struct pRoot_New newdispMsg;

    D(bug("[AmdgpuGfx:Display] %s()\n", __func__));

    /* Do not allow more than one object instance to be created */
    if (XSD(cl)->display)
        return NULL;

    pftags[0].ti_Data = XSD(cl)->data.redshift;
    pftags[1].ti_Data = XSD(cl)->data.greenshift;
    pftags[2].ti_Data = XSD(cl)->data.blueshift;
    pftags[4].ti_Data = XSD(cl)->data.redmask;
    pftags[5].ti_Data = XSD(cl)->data.greenmask;
    pftags[6].ti_Data = XSD(cl)->data.bluemask;
    /* The pixfmt (channel masks/shifts) comes straight from HwInit's VesaInfo
     * copy. It is already correct for this GOP -- verified on hardware: R mask
     * 0xff0000 shift 8, G 0xff00 shift 16, B 0xff shift 24 = the AROS
     * 0xAARRGGBB layout that matches the DCN ARGB8888 scanout. A prior "forced
     * ARGB8888" override here was therefore a no-op and has been removed. The
     * one-line report below records the published pixfmt in the boot log. */
    bug("[AmdgpuGfx:Display] PixFmt from HwInit: R %08lx<<%ld G %08lx<<%ld B %08lx<<%ld (depth %ld)\n",
        (unsigned long)XSD(cl)->data.redmask,   (long)XSD(cl)->data.redshift,
        (unsigned long)XSD(cl)->data.greenmask, (long)XSD(cl)->data.greenshift,
        (unsigned long)XSD(cl)->data.bluemask,  (long)XSD(cl)->data.blueshift,
        (long)XSD(cl)->data.depth);
    pftags[8].ti_Data = (XSD(cl)->data.depth > 8) ? vHidd_ColorModel_TrueColor : vHidd_ColorModel_Palette;
    pftags[9].ti_Data = (XSD(cl)->data.depth > 24) ? 24 : XSD(cl)->data.depth;
    pftags[10].ti_Data = XSD(cl)->data.bytesperpixel;
    pftags[11].ti_Data = (XSD(cl)->data.bitsperpixel > 24) ? 24 : XSD(cl)->data.bitsperpixel;
    /* A shift by the width of the operand is undefined in C, and x86 masks the
     * count to 5 bits, so depth 32 has always produced 0 here. Keep that value
     * explicitly; the mask only matters for palette (<= 8-bit) modes. */
    pftags[14].ti_Data = (XSD(cl)->data.depth >= 32) ? 0
                       : ((1UL << XSD(cl)->data.depth) - 1);

    sync_mode[0].ti_Data = 60 * XSD(cl)->data.width * XSD(cl)->data.height;
    sync_mode[1].ti_Data = XSD(cl)->data.width;
    sync_mode[2].ti_Data = XSD(cl)->data.width;
    sync_mode[3].ti_Data = XSD(cl)->data.height;

    /* Keep the canonical driver prefix.  graphics.library inserts the selected
     * depth before @refresh, yielding e.g. AMDGPU:1920x1080x24@60 within the
     * 32-byte NameInfo limit.  The connector is intentionally omitted because
     * the full driver+connector+mode tuple cannot fit that ABI field reliably. */
    sync_mode[6].ti_Data = (IPTR)"AMDGPU:%hx%v@60";

    /* Assemble the published mode list: the native (adopted/panel) mode FIRST so
     * a first boot with no ScreenMode prefs stays native, then the EDID-probed
     * modes. The native mode is driven today (the OTG already runs it); selecting
     * a non-native one needs the per-mode modeset (OTG timing + DCCG DTO) — the
     * next increment. The EDID path (GPIO-DDC) is proven on hardware (15 modes). */
    {
        struct AmdgpuDevice *ad = XSD(cl)->device;
        struct AmdgpuDisplayMode probed[AMDGPU_DISPLAY_MAX_MODES];
        char clabel[16];
        ULONG nprobed = ad ? amdgpu_display_probe_modes(ad, probed,
                             AMDGPU_DISPLAY_MAX_MODES, clabel, sizeof(clabel)) : 0;
        ULONG nw = XSD(cl)->data.width, nh = XSD(cl)->data.height;
        ULONG mi = 0, i, published = 0;

        /* Both depths, then the syncs. AROS takes the cross product, so every
         * published mode becomes available at 24-bit AND 16-bit. */
        modetags[mi].ti_Tag = aHidd_DMEnum_PixFmtTags; modetags[mi++].ti_Data = (IPTR)pftags;
        modetags[mi].ti_Tag = aHidd_DMEnum_PixFmtTags; modetags[mi++].ti_Data = (IPTR)pftags_16;
        modetags[mi].ti_Tag = aHidd_DMEnum_SyncTags;   modetags[mi++].ti_Data = (IPTR)sync_mode;

        for (i = 0; i < nprobed && mi < (3 + AMDGPU_DISPLAY_MAX_MODES); i++)
        {
            struct TagItem *s;
            if (probed[i].w == nw && probed[i].h == nh)
                continue;                       /* native already published first */
            /*
             * Only publish what we can actually program. Without a per-mode
             * modeset we cannot retime the OTG, so a mode is realised by
             * centring it inside the existing raster
             * (amdgpu_compositor_switch_mode -> set_viewport + set_recout).
             * That works for modes that FIT; a larger one would be refused at
             * switch time, leaving the OTG scanning the old raster out of a
             * differently-strided bitmap -- i.e. garbage on screen. Publishing
             * a mode we would then refuse is worse than not offering it.
             */
            if (probed[i].w > nw || probed[i].h > nh)
            {
                D(bug("[AmdgpuGfx:Display] skipping %lux%lu: exceeds raster %lux%lu\n",
                      (unsigned long)probed[i].w, (unsigned long)probed[i].h,
                      (unsigned long)nw, (unsigned long)nh));
                continue;
            }
            s = amdgpu_build_sync(cl, &probed[i]);
            if (!s)
                continue;
            modetags[mi].ti_Tag = aHidd_DMEnum_SyncTags;
            modetags[mi++].ti_Data = (IPTR)s;
            published++;
        }
        modetags[mi].ti_Tag = TAG_DONE;
        modetags[mi].ti_Data = 0UL;

        bug("[AmdgpuGfx:Display] EDID probe -> %lu mode(s)%s%s ; published native %lux%lu + %lu more\n",
            (unsigned long)nprobed, nprobed ? " on " : "", nprobed ? clabel : "",
            (unsigned long)nw, (unsigned long)nh, (unsigned long)published);
    }

    if ((dispTags[1].ti_Data = (IPTR)msg->attrList) == 0)
        dispTags[1].ti_Tag = TAG_DONE;

    newdispMsg.mID = msg->mID;
    newdispMsg.attrList = dispTags;

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&newdispMsg);

    D(bug("[AmdgpuGfx:Display] %s: obj @ 0x%p\n", __func__, o));
    return o;
}

VOID AmdgpuGfxDisplay__Root__Get(OOP_Class *cl, OOP_Object *o, struct pRoot_Get *msg)
{
    ULONG idx;

    Hidd_Switch (msg->attrID, idx)
    {
    case aoHidd_Name:
        *msg->storage = (IPTR)"AMD GPU Display";
        return;
    }

    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/*********  Display::ShowViewPorts()  **************************/

/*
 * This is the link that actually drives the compositor.
 *
 * graphics.library calls ShowViewPorts to hand the driver the current stack of
 * visible viewports. Returning TRUE tells it the driver handled the stack
 * itself; returning FALSE (or not implementing the method at all, which is
 * where amdgpu was) makes it fall back to showing a single bitmap -- so only
 * the frontmost screen is ever displayed and screen dragging does nothing.
 *
 * nouveau_displayclass.c:352 is the same forwarder.
 */
ULONG AmdgpuGfxDisplay__Hidd_Display__ShowViewPorts(OOP_Class *cl, OOP_Object *o,
    struct pHidd_Display_ShowViewPorts *msg)
{
    struct pHidd_Compositor_BitMapStackChanged bscmsg =
    {
        .mID  = OOP_GetMethodID(IID_Hidd_Compositor, moHidd_Compositor_BitMapStackChanged),
        .data = msg->Data
    };

    /* No compositor means we cannot honour the stack. Say so rather than
     * claiming success -- graphics.library then keeps its own fallback. */
    if (!XSD(cl)->compositor)
        return FALSE;

    D(bug("[AmdgpuGfxDisplay] ShowViewPorts: top bitmap %p\n",
          msg->Data ? msg->Data->Bitmap : NULL));

    OOP_DoMethod(XSD(cl)->compositor, (OOP_Msg)&bscmsg);

    return TRUE;
}

/*********  Display::CreateObject()  ***************************/

OOP_Object *AmdgpuGfxDisplay__Hidd_Display__CreateObject(OOP_Class *cl, OOP_Object *o, struct pHidd_Display_CreateObject *msg)
{
    OOP_Object      *object = NULL;

    D(bug("[AmdgpuGfx:Display] %s()\n", __func__));

    /* Match nouveau's display-to-Gallium dispatch, but load our separate
     * renderer only on demand. gallium.hidd need not exist when the display
     * was registered; therefore resolve its base class at request time.
     * Cold80 qualified the installed provider's public pixels and teardown.
     * This hook does not change bitmap, compositor or cursor ownership. */
    OOP_Class *basegallium = OOP_FindClass(CLID_Hidd_Gallium);
    if (basegallium && msg->cl == basegallium)
    {
        struct AmdgpuGfx_staticdata *xsd = XSD(cl);
        if (!gfx_v12_managed_ready(xsd->device))
            return NULL;

        ObtainSemaphore(&xsd->gallium_lock);
        if (!xsd->gallium_module)
            xsd->gallium_module = OpenLibrary("radeonsi_native.hidd", 26);
        if (xsd->gallium_module)
            object = OOP_NewObject(NULL, "hidd.gallium.radeonsi_native", msg->attrList);
        ReleaseSemaphore(&xsd->gallium_lock);

        /* The provider validates the Gallium ABI and native device. NULL
         * leaves the existing gallium.library fallback policy in charge. */
        bug("[amdgpu-gallium] display request provider=radeonsi_native object=%p; "
            "renderer and pixels require client verification\n", object);
        return object;
    }

    if (msg->cl == XSD(cl)->basebm)
    {
        BOOL displayable;
        struct TagItem tags[2] =
        {
            {TAG_IGNORE, 0                  },
            {TAG_MORE  , (IPTR)msg->attrList}
        };
        struct pHidd_Display_CreateObject p;

        displayable = GetTagData(aHidd_BitMap_Displayable, FALSE, msg->attrList);

        /* One line per bitmap request, so the class decision is visible instead
         * of inferred. Which bitmap the compositor ends up handing back to us
         * is the open question; guessing it has already cost a boot. */
        bug("[AmdgpuGfx:Display] CreateObject: disp=%ld composit=%ld friend=0x%p"
            " modeid=0x%lx %ldx%ld\n",
            (long)displayable,
            (long)GetTagData(aHidd_BitMap_Compositable, FALSE, msg->attrList),
            (APTR)GetTagData(aHidd_BitMap_Friend, 0, msg->attrList),
            (unsigned long)GetTagData(aHidd_BitMap_ModeID, 0, msg->attrList),
            (long)GetTagData(aHidd_BitMap_Width, 0, msg->attrList),
            (long)GetTagData(aHidd_BitMap_Height, 0, msg->attrList));
        if (displayable)
        {
            /* Only displayable bitmaps are bitmaps of our class */
            tags[0].ti_Tag  = aHidd_BitMap_ClassPtr;
            tags[0].ti_Data = (IPTR)XSD(cl)->bmclass;
        }
        else
        {
            /* Non-displayable friends of our bitmaps are plain chunky bitmaps */
            OOP_Object *friend = (OOP_Object *)GetTagData(aHidd_BitMap_Friend, 0, msg->attrList);

            if (friend && (OOP_OCLASS(friend) == XSD(cl)->bmclass))
            {
                tags[0].ti_Tag  = aHidd_BitMap_ClassID;
                tags[0].ti_Data = (IPTR)CLID_Hidd_ChunkyBM;
            }
        }

        p.mID = msg->mID;
        p.cl = msg->cl;
        p.attrList = tags;

        object = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&p);
        bug("[AmdgpuGfx:Display] CreateObject -> bm 0x%p class 0x%p (ours 0x%p)\n",
            object, object ? OOP_OCLASS(object) : NULL, XSD(cl)->bmclass);
    }
    else
        object = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);

    ReturnPtr("AmdgpuGfx.Display::CreateObject", OOP_Object *, object);
}

/*
 * The two driver-specific hooks the ported compositor needs. Everything else in
 * amdgpu_compositorclass.c is driver-agnostic; these are the calls its author
 * flagged as "should be reimplemented by child classes in each of the drivers".
 */

/*
 * amdgpu_compositor_switch_mode — make this bitmap the one being scanned out.
 *
 * nouveau's equivalent issues a full DRM modeset. amdgpu cannot change the
 * raster yet (that needs OTG timing plus a DCCG/PLL pixel clock), but it can do
 * everything short of it, which covers every mode this driver publishes:
 *   - point the scanout at the bitmap's VRAM base
 *   - size the HUBP fetch rectangle to the bitmap
 *   - place the output rectangle, centred, on the existing raster
 *   - match the surface pixel format to the bitmap's depth
 *
 * Preflight leaves the plane untouched. The transaction engine returns success
 * only after the new surface is in use. Verified rollback retains the old
 * software tuple; an uncertain result quarantines pool storage until recovery.
 */
static BOOL amdgpu_select_plane(OOP_Class *cl, OOP_Object *bm)
{
    /*
     * Reach everything through OOP attributes, NOT through OOP_INST_DATA/XSD on
     * the bitmap's own class.
     *
     * The first cut of this hook did the latter, and the first boot with the
     * compositor live proved why that is wrong:
     *
     *   [amdgpu-comp] switch_mode: 100078984x1 exceeds the raster 0x0
     *
     * garbage geometry and a 0x0 raster, because the bitmap the compositor
     * hands us is not necessarily OUR bitmap class -- so both
     * OOP_INST_DATA(OOP_OCLASS(bm), bm) and XSD(OOP_OCLASS(bm)) were reading at
     * the wrong offsets. nouveau_displayclass.c:111 does it the class-agnostic
     * way instead, and that is what this now mirrors: ModeID from the bitmap,
     * geometry from the Sync object, depth from the PixFmt object -- all via
     * OOP_GetAttr, which does not care what class the bitmap is.
     *
     * `cl` is handed in rather than derived from bm for the same reason. The
     * caller passes OOP_OCLASS(compdata->gfx), which is provably an amdgpu
     * class (the port already relies on that for ->basebm), so XSD(cl) and the
     * aHidd_* attr bases -- which all expand through XSD(cl)->attrBases[] --
     * are valid. Deriving a class from the bitmap is precisely what failed.
     */
    OOP_Object *display = NULL, *sync = NULL, *pf = NULL, *dmenum = NULL;
    OOP_Object *bitmap_pf;
    struct AmdgpuGfx_staticdata *xsd;
    struct HWData *hw;
    HIDDT_ModeID modeid = vHidd_ModeID_Invalid;
    IPTR e = 0;
    ULONG w, h, bpp, rx, ry, stride, source_x, source_y;
    IPTR width_attr, height_attr, bpp_attr, row_attr, bm_width, bm_height;
    IPTR leftedge = 0, topedge = 0;
    UQUAD offset, span;
    UQUAD new_mc = 0;
    IPTR  new_bufptr = 0;

    if (!bm)
        return FALSE;

    OOP_GetAttr(bm, aHidd_BitMap_Display, &e);
    display = (OOP_Object *)e;
    if (!display)
        return FALSE;

    /* `cl` is the amdgpu Gfx class, handed in by the caller, so XSD() and every
     * aHidd_* attr base (which expand through XSD(cl)->attrBases[]) are valid. */
    xsd = XSD(cl);
    if (!xsd || !xsd->device)
        return FALSE;
    hw = &xsd->data;

    e = 0;
    OOP_GetAttr(display, aHidd_Display_DMEnumerator, &e);
    dmenum = (OOP_Object *)e;
    if (!dmenum)
        return FALSE;

    OOP_GetAttr(bm, aHidd_BitMap_ModeID, &modeid);
    if (modeid == vHidd_ModeID_Invalid)
    {
        bug("[amdgpu-comp] switch_mode: bitmap %p has no ModeID\n", bm);
        return FALSE;
    }

    if (!HIDD_DMEnum_GetMode(dmenum, modeid, &sync, &pf) || !sync || !pf)
    {
        bug("[amdgpu-comp] switch_mode: no sync/pixfmt for ModeID 0x%lx\n",
            (unsigned long)modeid);
        return FALSE;
    }

    width_attr  = OOP_GET(sync, aHidd_Sync_HDisp);
    height_attr = OOP_GET(sync, aHidd_Sync_VDisp);
    bpp_attr    = OOP_GET(pf, aHidd_PixFmt_BytesPerPixel);
    if (!width_attr || width_attr > 0x3fff ||
        !height_attr || height_attr > 0x3fff ||
        (bpp_attr != 2 && bpp_attr != 4))
        return FALSE;
    w = (ULONG)width_attr;
    h = (ULONG)height_attr;
    bpp = (ULONG)bpp_attr;

    /* The sync dimensions are NOT storage stride. Read through the wrapper
     * attributes, validate the complete advertised bitmap storage, and reject
     * a format mismatch before touching MMIO. Do not truncate wide attributes
     * before checking them. Pool bounds are necessary, not an allocation
     * lifetime/ownership guarantee; the compositor must retain the bitmap. */
    bm_width = OOP_GET(bm, aHidd_BitMap_Width);
    bm_height = OOP_GET(bm, aHidd_BitMap_Height);
    row_attr = OOP_GET(bm, aHidd_BitMap_BytesPerRow);
    bitmap_pf = (OOP_Object *)OOP_GET(bm, aHidd_BitMap_PixFmt);
    if (!bitmap_pf || OOP_GET(bitmap_pf, aHidd_PixFmt_BytesPerPixel) != bpp_attr ||
        bm_width < width_attr || bm_width > 0x10000 ||
        bm_height < height_attr || bm_height > (IPTR)~(ULONG)0 ||
        !row_attr || row_attr > 0x10000UL * bpp || row_attr % bpp ||
        row_attr / bpp < bm_width)
        return FALSE;
    stride = (ULONG)(row_attr / bpp);
    span = (UQUAD)row_attr * (UQUAD)bm_height;
    OOP_GetAttr(bm, aHidd_BitMap_LeftEdge, &leftedge);
    OOP_GetAttr(bm, aHidd_BitMap_TopEdge, &topedge);
    /* Positive Y is a compositor operation, never an out-of-bounds hardware
     * viewport. Negate only after bounded signed validation (including MIN). */
    if ((SIPTR)leftedge > 0 || (SIPTR)topedge > 0 ||
        (SIPTR)leftedge < -65535 || (SIPTR)topedge < -65535)
        return FALSE;
    source_x = (ULONG)-(SIPTR)leftedge;
    source_y = (ULONG)-(SIPTR)topedge;
    if ((UQUAD)source_x + w > bm_width ||
        (UQUAD)source_y + h > bm_height)
        return FALSE;

    /* Centre on the raster. Without a per-mode modeset we cannot retime the
     * OTG, so a mode larger than the raster is refused rather than programmed
     * as a bad rectangle. amdgpu_displayclass.c filters the published mode list
     * to what fits, so this should not normally trigger. */
    if (!hw->width || !hw->height || w > hw->width || h > hw->height) {
        bug("[amdgpu-comp] switch_mode: %lux%lu vs raster %lux%lu — "
            "needs a real modeset (OTG timing + PLL), refusing\n",
            (unsigned long)w, (unsigned long)h,
            (unsigned long)hw->width, (unsigned long)hw->height);
        return FALSE;
    }
    rx = (hw->width  - w) / 2;
    ry = (hw->height - h) / 2;

    /*
     * TRIG-0634: resolve + validate the new surface's VRAM residency BEFORE any
     * live register write. Identify the surface by WHERE ITS PIXELS LIVE (the
     * CursorFB wrapper forwards aHidd_ChunkyBM_Buffer to the real bitmap). If the
     * pixels are not in our VRAM pool we cannot install a scanout address -- a
     * displayable alloc that fell back to system RAM lands here -- so FAIL NOW,
     * before touching format/viewport/recout and before the compositor disposes
     * anything, so the OLD surface keeps scanning out and is retained (no
     * use-after-free of a freed/reused VRAM block DCN is still reading).
     */
    {
        IPTR bufptr = 0;
        OOP_GetAttr(bm, aHidd_ChunkyBM_Buffer, &bufptr);
        if (!(hw->vram_pool_size && hw->vram_pool_cpu && bufptr &&
              bufptr >= (IPTR)hw->vram_pool_cpu))
        {
            bug("[amdgpu-comp] switch_mode: bitmap %p buffer 0x%p NOT in VRAM pool "
                "(0x%p +0x%lx) -- refusing retarget, keeping old scanout resident\n",
                bm, (APTR)bufptr, hw->vram_pool_cpu, (unsigned long)hw->vram_pool_size);
            return FALSE;
        }
        offset = (UQUAD)(bufptr - (IPTR)hw->vram_pool_cpu);
        if (offset >= hw->vram_pool_size ||
            span > (UQUAD)hw->vram_pool_size - offset ||
            span - 1 > (UQUAD)(~(IPTR)0 - bufptr) ||
            offset > ~(UQUAD)0 - hw->vram_pool_mc)
            return FALSE;
        new_bufptr = bufptr;
        new_mc = hw->vram_pool_mc + offset;
        /* Our bitmap allocator guarantees 256-byte-aligned bases. Keep that
         * contract here; the DCN4.1 address high register has only 16 bits,
         * so the complete surface must also fit its 48-bit MC address range. */
        if ((new_mc & 0xff) || new_mc > 0xffffffffffffULL ||
            span - 1 > 0xffffffffffffULL - new_mc)
            return FALSE;
    }

    /*
     * Everything from here to the unlock is a PLANE UPDATE of a live,
     * scanning-out pipe: pixel format, fetch rectangle, scaler rectangle and
     * surface address. DCN double-buffers these and latches them together on
     * VUPDATE, so they must be bracketed by the OTG update lock. Writing them
     * unlocked lets the hardware latch a half-written configuration mid-frame.
     *
     * This is not precautionary. Unlocked, the first 1920x1080 -> 800x600
     * screenmode change took the machine down: the viewport write reported OK,
     * the recout write read back 0xffffffff, and the platform reset itself.
     *
     * Serialize software callers, then require the hardware lock. A failed
     * acquisition must not be followed by plane writes or by an unlock of a
     * transaction we do not own. The lock helper cancels its own timed-out
     * request while the device still responds.
     */
    LOCK_FRAMEBUFFER(xsd);
    if (!hw->pool_lock_ready) {
        UNLOCK_FRAMEBUFFER(xsd);
        return FALSE;
    }
    ObtainSemaphore(&hw->pool_lock);
    {
        struct AmdgpuPlane plane = {
            new_mc, w, h, stride, bpp, hw->width, hw->height,
            source_x, source_y, (ULONG)bm_height
        };
        enum AmdgpuPlaneResult result = AMDGPU_PLANE_UNCERTAIN;
        if (!hw->plane_faulted)
            result = amdgpu_plane_apply(xsd->device, &plane);
        if (result != AMDGPU_PLANE_ACTIVE) {
            if (result == AMDGPU_PLANE_UNCERTAIN)
                hw->plane_faulted = TRUE;
            bug("[amdgpu-comp] plane not committed: %s; old software state retained\n",
                hw->plane_faulted ? "UNCERTAIN, pool quarantined until recovery" : "old plane preserved");
            ReleaseSemaphore(&hw->pool_lock);
            UNLOCK_FRAMEBUFFER(xsd);
            return FALSE;
        }
    }

    /*
     * Put the bitmap ON DISPLAY: point the scanout at it.
     *
     * Identify it by WHERE ITS PIXELS LIVE, not by its class. The class test
     * that was here failed on hardware, and the reason is instructive: the base
     * Display class wraps a displayable bitmap in a CursorFB bitmap whenever the
     * driver reports no hardware sprite (gfx_displayclass.c: create_cursorfb
     * under display_NeedSoftCursor), so what the compositor hands back is the
     * WRAPPER, whose class is not ours and whose realbm is only reachable via
     * cursorfb_realbitmap() -- a gfx-internal helper needing a csd we do not
     * have. amdgpu asks for that wrapper today, because SupportsHWCursor is
     * FALSE while the DCN sprite is parked.
     *
     * But CursorFB forwards every Get to the real bitmap, so asking for the
     * chunky buffer returns the REAL pixel pointer through the wrapper. If that
     * pointer lies inside our VRAM pool then the surface is ours and its MC
     * address follows from the offset, because CPU and MC stay in lockstep
     * across the aperture.
     *
     * This is deliberately wrapper-agnostic: it asks the only question that
     * actually matters -- "are these pixels in my VRAM?" -- and so keeps working
     * through any present or future wrapper that forwards attributes, and
     * through the HW-cursor case where no wrapper exists at all.
     */
    /* The transaction has acknowledged the new in-use address. Only now may
     * a disposed old scanout allocation re-enter the pool's free list. */
    amdgpu_vram_retire_scanout_locked(hw, new_mc, (ULONG)span);
    hw->framebuffer = (APTR)new_bufptr;   /* keep CPU/MC views in step */
    hw->fb_mc       = new_mc;
    hw->fb_width   = w;
    hw->fb_height  = h;
    hw->bytesperline = (ULONG)row_attr;
    hw->bytesperpixel = bpp;
    hw->fbsize     = (ULONG)span; /* preflight bounded this by the ULONG pool */
    bug("[amdgpu-comp] switch_mode: scanout -> bitmap %p buffer 0x%p mc 0x%08x%08x\n",
        bm, (APTR)new_bufptr, (ULONG)(new_mc >> 32), (ULONG)new_mc);
    bug("[amdgpu-pan] viewport-v1 selected origin=%lu,%lu backing=%lux%lu stride=%lu; base unchanged by origin\n",
        (unsigned long)source_x, (unsigned long)source_y,
        (unsigned long)bm_width, (unsigned long)bm_height, (unsigned long)stride);

    bug("[amdgpu-comp] switch_mode: %lux%lu @%lubpp centred at (%lu,%lu) on %lux%lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)(bpp * 8),
        (unsigned long)rx, (unsigned long)ry,
        (unsigned long)hw->width, (unsigned long)hw->height);

    ReleaseSemaphore(&hw->pool_lock);
    UNLOCK_FRAMEBUFFER(xsd);
    return TRUE;
}

/* BEGIN AMDGPU display publication
 * display_lock spans hardware selection and logical publication. The base
 * software cursor takes its own semaphore and can call bitmap refresh, so no
 * OOP cursor/Visible operation may run while framebufferlock or pool_lock is
 * held. Hide the pointer during retargeting: CursorFB Visible callbacks and
 * the base Show both select a target, and neither may back up a drawn cursor.
 */
static void amdgpu_cursor_visibility(OOP_Class *cl, OOP_Object *display, BOOL visible)
{
    struct pHidd_Display_SetCursorVisible msg = {
        .mID = OOP_GetMethodID(IID_Hidd_Display, moHidd_Display_SetCursorVisible),
        .visible = visible
    };
    OOP_DoSuperMethod(cl, display, (OOP_Msg)&msg);
    amdgpu_cursor_mirror_update(XSD(cl), visible);
}

/* Nestable under the compositor write lock. Keep the software pointer hidden
 * across offset changes, candidate preparation, plane selection AND rollback.
 * A nested switch must not expose new coordinates with the old cursor backup. */
VOID amdgpu_display_update_begin(OOP_Class *cl)
{
    struct AmdgpuGfx_staticdata *xsd = XSD(cl);
    ObtainSemaphore(&xsd->data.display_lock);
    if (xsd->cursor_update_depth++ == 0 && xsd->display)
        amdgpu_cursor_visibility(xsd->displayclass, xsd->display, FALSE);
}

VOID amdgpu_display_update_end(OOP_Class *cl)
{
    struct AmdgpuGfx_staticdata *xsd = XSD(cl);
    if (--xsd->cursor_update_depth == 0 && xsd->display && !xsd->data.plane_faulted)
        amdgpu_cursor_visibility(xsd->displayclass, xsd->display,
                                xsd->data.cursor_requested_visible);
    ReleaseSemaphore(&xsd->data.display_lock);
}

static void amdgpu_publish_bitmap(OOP_Class *cl, OOP_Object *display,
                                   OOP_Object *bitmap)
{
    struct AmdgpuGfx_staticdata *xsd = XSD(cl);
    struct TagItem tags[] = {
        { aHidd_BitMap_Visible, FALSE }, { TAG_DONE, 0 }
    };
    struct pHidd_Display_Show msg = {
        .mID = OOP_GetMethodID(IID_Hidd_Display, moHidd_Display_Show),
        .bitMap = bitmap, .flags = 0
    };

    if (xsd->visible)
        OOP_SetAttrs(xsd->visible, tags);
    if (bitmap) {
        tags[0].ti_Data = TRUE;
        OOP_SetAttrs(bitmap, tags);
    }
    /* Also covers unwrapped bitmaps. Coerce only to our Display superclass,
       not through the driver Show (which would recurse into plane selection). */
    OOP_DoSuperMethod(cl, display, (OOP_Msg)&msg);
    xsd->visible = bitmap;
}

BOOL amdgpu_compositor_switch_mode(OOP_Class *cl, OOP_Object *bm)
{
    struct AmdgpuGfx_staticdata *xsd = XSD(cl);
    BOOL selected;
    if (!xsd || !xsd->data.pool_lock_ready || !xsd->display ||
        !xsd->displayclass || !bm ||
        (OOP_Object *)OOP_GET(bm, aHidd_BitMap_Display) != xsd->display)
        return FALSE;
    amdgpu_display_update_begin(cl);
    selected = amdgpu_select_plane(cl, bm);
    if (selected)
        amdgpu_publish_bitmap(xsd->displayclass, xsd->display, bm);
    amdgpu_display_update_end(cl);
    return selected;
}

OOP_Object *AmdgpuGfxDisplay__Hidd_Display__Show(OOP_Class *cl, OOP_Object *o,
                                               struct pHidd_Display_Show *msg)
{
    struct AmdgpuGfx_staticdata *xsd = XSD(cl);
    OOP_Object *shown;
    if (!xsd->data.pool_lock_ready || o != xsd->display)
        return NULL;
    ObtainSemaphore(&xsd->data.display_lock);
    if (msg->bitMap) {
        /* Exec semaphores are recursive for the owning task. */
        amdgpu_compositor_switch_mode(cl, msg->bitMap);
    } else {
        amdgpu_cursor_visibility(cl, o, FALSE);
        amdgpu_publish_bitmap(cl, o, NULL);
        LOCK_FRAMEBUFFER(xsd);
        /* Blank only a plane this driver has actually selected. Physical
           storage remains pinned until a verified subsequent plane switch. */
        if (xsd->data.scanout_active_size)
            ClearBuffer(&xsd->data);
        UNLOCK_FRAMEBUFFER(xsd);
    }
    shown = xsd->visible;
    ReleaseSemaphore(&xsd->data.display_lock);
    return shown;
}
/* END AMDGPU display publication */

/* CursorFB forwards this private Get, unlike instance-data casts on wrappers.
 * Only our native class may supply the direct instance used by the setter. */
OOP_Object *amdgpu_bitmap_native(OOP_Class *cl, OOP_Object *bm)
{
    IPTR native = 0;
    if (bm)
        OOP_GetAttr(bm, aAmdgpuBitMap_Self, &native);
    if (!native || OOP_OCLASS((OOP_Object *)native) != XSD(cl)->bmclass)
        return NULL;
    return (OOP_Object *)native;
}

/* Nouveau's internal setter contract: no validation, notification or copying.
 * Caller holds the compositor/display transaction. OOP_SetAttrs here would
 * recursively enter the public bitmap position transaction. */
VOID amdgpu_compositor_set_offsets(OOP_Class *cl, OOP_Object *bm, LONG xoffset, LONG yoffset)
{
    struct AmdgpuGfxBitMapData *data;
    bm = amdgpu_bitmap_native(cl, bm);
    if (!bm)
        return;
    data = OOP_INST_DATA(XSD(cl)->bmclass, bm);
    data->xoffset = xoffset;
    data->yoffset = yoffset;
}

/* Report the panel-native mode as this display's nominal size. Intuition's
 * SetDisplayDefaults (intuition_misc.c) queries the default monitor's
 * NominalDimensions for the default screen when there are no ScreenMode prefs;
 * the base gfx display class returns a small default, which is why first boot
 * came up 640x480 once the full mode list was published. Returning native makes
 * first boot with no prefs come up at the panel mode (user: "first boot with no
 * prefs should be whatever the panel is"). Mirrors vmwaresvga/headlessgfx, which
 * override this same method. The full published list stays user-selectable. */
VOID AmdgpuGfxDisplay__Hidd_Display__NominalDimensions(OOP_Class *cl, OOP_Object *o,
    struct pHidd_Display_NominalDimensions *msg)
{
    struct AmdgpuGfx_staticdata *data = XSD(cl);

    if (msg->width)
        *(msg->width)  = data->data.width;
    if (msg->height)
        *(msg->height) = data->data.height;
    if (msg->depth)
        *(msg->depth)  = (data->data.depth > 24) ? 24 : data->data.depth;

    bug("[AmdgpuGfx:Display] NominalDimensions -> %ux%ux%u (no-prefs default)\n",
        data->data.width, data->data.height,
        (data->data.depth > 24) ? 24 : data->data.depth);
}

/*
 * ---- Hardware cursor: the Workbench pointer, not a private sprite ----
 *
 * amdgpu_cursor.c originally programmed DCN with its own test image. That was a
 * hardware sprite AROS knew nothing about: Wanderer kept drawing its software
 * pointer and the driver was never asked for a cursor at all. terminills named
 * it — that is the GOP's mouse, not the Workbench one.
 *
 * A hardware cursor only becomes THE cursor when two things are true:
 *   1. the display class implements SetCursorShape / SetCursorPos /
 *      SetCursorVisible, so AROS drives the sprite, and
 *   2. ModeProperties advertises DIPF_IS_SPRITES, which is how AROS learns it
 *      can stop compositing a software pointer.
 * Missing either one leaves the software cursor in place. nouveau does exactly
 * this pair (nouveau.conf declares all four methods); amdgpu declared none of
 * them, which is why every register-level fix to the sprite changed nothing
 * visible.
 */
static struct HIDD_ModeProperties amdgpu_modeprops =
{
    /*
     * DIPF_IS_SPRITES is DISABLED until the sprite is confirmed on screen.
     * Per the note above, this flag is how AROS learns it may stop compositing
     * a software pointer -- so setting it while the DCN cursor draws nothing
     * leaves the user with NO pointer at all. Restore it (and
     * aoHidd_Gfx_SupportsHWCursor in amdgpu_gfxclass.c) together, only after a
     * cursor is actually visible.
     *
     * NumHWSprites must ALSO be 0 while DIPF_IS_SPRITES is off: advertising 1 HW
     * sprite with no sprite flag/types is an inconsistent ModeProperties contract
     * (GHC Hidd_Display.ModeProperties FAIL, codex TRIG-0575). Restore both to
     * 1/DIPF_IS_SPRITES together only when the DCN sprite is proven on-screen.
     */
    0,
    0,
    COMPF_ABOVE
};

ULONG AmdgpuGfxDisplay__Hidd_Display__ModeProperties(OOP_Class *cl, OOP_Object *o,
    struct pHidd_Display_ModeProperties *msg)
{
    ULONG len = msg->propsLen;

    if (len > sizeof(amdgpu_modeprops))
        len = sizeof(amdgpu_modeprops);
    CopyMem(&amdgpu_modeprops, msg->props, len);
    return len;
}

/* CURSOR OWNERSHIP (audit P0-3). While SupportsHWCursor is FALSE the base Display
 * class wraps the framebuffer in a software CursorFB and its own SetCursor* draw
 * the pointer by alpha-compositing an ARGB32 shape onto the display bitmap
 * (gfx_displayclass.c cursor_Draw -> GetImage/PutImage ARGB32, now that the
 * format contract is fixed). AMDGPU previously overrode all three methods and
 * sent them ONLY to the DCN hardware sprite, so the software pointer was never
 * updated and -- with the DCN sprite not yet visibly proven -- no pointer showed
 * at all. Delegate to the superclass so exactly ONE cursor owner (the software
 * pointer) is active. The DCN path in amdgpu_cursor.c is retained but dormant;
 * re-enable it together with DIPF_IS_SPRITES (ModeProperties) and
 * aoHidd_Gfx_SupportsHWCursor (amdgpu_gfxclass.c) only after the sprite is
 * confirmed on-screen across shape/alpha/hotspot/clip/mode-switch tests. */
BOOL AmdgpuGfxDisplay__Hidd_Display__SetCursorShape(OOP_Class *cl, OOP_Object *o,
    struct pHidd_Display_SetCursorShape *msg)
{
    BOOL result;
    struct HWData *hw = &XSD(cl)->data;
    if (!hw->pool_lock_ready) return FALSE;
    ObtainSemaphore(&hw->display_lock);
    result = (BOOL)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (result && XSD(cl)->cursor_mirror) {
        struct AmdgpuCursorMirror *cursor = XSD(cl)->cursor_mirror;
        IPTR width = 0, height = 0;
        if (msg->shape) {
            OOP_GetAttr(msg->shape, aHidd_BitMap_Width, &width);
            OOP_GetAttr(msg->shape, aHidd_BitMap_Height, &height);
        }
        cursor->valid = width && height && width <= AMDGPU_MIRROR_DIM &&
                        height <= AMDGPU_MIRROR_DIM;
        if (cursor->valid) {
            cursor->width = width;
            cursor->height = height;
            cursor->xoffset = msg->xoffset;
            cursor->yoffset = msg->yoffset;
            /* AROS BGRA32 on little endian yields native 0xAARRGGBB words.
             * Fixed64-pixel pitch matches the private DCN surface, not width. */
            HIDD_BM_GetImage(msg->shape, (UBYTE *)cursor->argb,
                AMDGPU_MIRROR_DIM * 4, 0, 0, width, height,
                vHidd_StdPixFmt_BGRA32);
            cursor->dirty = TRUE;
        }
        amdgpu_cursor_mirror_update(XSD(cl), hw->cursor_requested_visible);
    }
    ReleaseSemaphore(&hw->display_lock);
    return result;
}

/* HW-cursor re-enable note (when SupportsHWCursor + DIPF_IS_SPRITES go true):
 * the DCN sprite wants ARGB in memory with alpha in the TOP byte, which on LE
 * AROS is vHidd_StdPixFmt_BGRA32 (NOT ARGB32, whose alpha is the low byte -> DCN
 * reads blue as alpha -> transparent), and GetImage's modulo must be width*4 to
 * match amdgpu_cursor_set_shape()'s argb[y*w+x] packing. Register programming
 * lives in amdgpu_cursor.c. */

BOOL AmdgpuGfxDisplay__Hidd_Display__SetCursorPos(OOP_Class *cl, OOP_Object *o,
    struct pHidd_Display_SetCursorPos *msg)
{
    BOOL result;
    struct HWData *hw = &XSD(cl)->data;
    if (!hw->pool_lock_ready) return FALSE;
    ObtainSemaphore(&hw->display_lock);
    result = (BOOL)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (XSD(cl)->cursor_mirror) {
        XSD(cl)->cursor_mirror->x = msg->x;
        XSD(cl)->cursor_mirror->y = msg->y;
        amdgpu_cursor_mirror_update(XSD(cl), hw->cursor_requested_visible);
    }
    ReleaseSemaphore(&hw->display_lock);
    return result;
}

VOID AmdgpuGfxDisplay__Hidd_Display__SetCursorVisible(OOP_Class *cl, OOP_Object *o,
    struct pHidd_Display_SetCursorVisible *msg)
{
    struct HWData *hw = &XSD(cl)->data;
    if (!hw->pool_lock_ready) return;
    ObtainSemaphore(&hw->display_lock);
    hw->cursor_requested_visible = msg->visible;
    amdgpu_cursor_visibility(cl, o, !hw->plane_faulted && msg->visible && XSD(cl)->visible);
    ReleaseSemaphore(&hw->display_lock);
}
