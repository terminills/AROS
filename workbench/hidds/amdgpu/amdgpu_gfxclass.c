/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Gfx root class for the amdgpu boot-mode framebuffer.

    NOTE: the root gfx class shares the module basename "Amdgpu" (amdgpu.conf
    root config), so its OOP methods are Amdgpu__Root__*. The nested Display
    and BitMap classes use their own basenames (AmdgpuGfxDisplay / AmdgpuGfxBM).
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
#include <oop/oop.h>
#include <clib/alib_protos.h>
#include <string.h>

#define DEBUG 0
#include <aros/debug.h>

#include "amdgpu_gfx_intern.h"
#include "amdgpu_compositor.h"

/* compositor.h defines this as the global __IHidd_Compositor, which the
 * -D__OOP_NOATTRBASES__ build never declares. Point it at our own slot
 * instead, the same override nouveau_hiddclass.c:40 uses. */
#undef HiddCompositorAttrBase
#define HiddCompositorAttrBase    XSD(cl)->attrBases[8]
/* aHidd_GC_* need HiddGCAttrBase; in a -D__OOP_NOATTRBASES__ build it is not the
 * global __IHidd_GC. Point it at our own slot (attrBases[9], same index as
 * amdgpu_bitmapclass.c) so CopyBox can read GC DrawMode/ColorMask. cl is in
 * scope in the CopyBox method. */
#undef HiddGCAttrBase
#define HiddGCAttrBase    XSD(cl)->attrBases[9]
#include "amdgpu_gfx_hidd.h"
#include "amdgpu_gfx_support.h"

#include LC_LIBDEFS_FILE

AROS_INTH1(ResetHandler, struct HWData *, hwdata)
{
    AROS_INTFUNC_INIT

    ClearBuffer(hwdata);

    return FALSE;

    AROS_INTFUNC_EXIT
}

OOP_Object *Amdgpu__Root__New(OOP_Class *cl, OOP_Object *o, struct pRoot_New *msg)
{
    struct TagItem msgNewTags[] =
    {
        { aHidd_Name            , (IPTR)"amdgpu.hidd"                },
        { aHidd_HardwareName    , (IPTR)"AMD GPU Boot Framebuffer"   },
        { aHidd_ProducerName    , (IPTR)"AMD"                        },
        {TAG_MORE, 0UL}
    };
    struct pRoot_New msgNew;

    EnterFunc(bug("AmdgpuGfx::New()\n"));

    /* There is one firmware framebuffer; refuse a second driver object */
    if (XSD(cl)->gfxhidd)
        return NULL;

    if ((msgNewTags[3].ti_Data = (IPTR)msg->attrList) == 0)
        msgNewTags[3].ti_Tag = TAG_DONE;

    msgNew.mID = msg->mID;
    msgNew.attrList = msgNewTags;

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&msgNew);
    if (o)
    {
        struct AmdgpuGfxHiddData *data = OOP_INST_DATA(cl, o);
        struct TagItem displaytags[] =
        {
            { aHidd_Display_GfxHidd, (IPTR)o },
            { TAG_DONE,              0       }
        };

        D(bug("Got object from super\n"));

        XSD(cl)->display = OOP_NewObject(XSD(cl)->displayclass, NULL, displaytags);
        if (XSD(cl)->display)
        {
            D(bug("[AmdgpuGfx:Driver] %s: display @ 0x%p\n", __func__, XSD(cl)->display));
            XSD(cl)->gfxhidd = o;

            /*
             * Create the compositor. This driver reports COMPF_ABOVE in its
             * ModeProperties, which tells graphics.library the driver composes
             * for itself -- graphics_display.c then skips DF_SoftComposit and
             * never instantiates the system compositor for us. So if we do not
             * build one here, nothing composes at all. nouveau creates its own
             * the same way, from Gfx::New (nouveau_hiddclass.c:684).
             *
             * Non-fatal: a driver object that works without composition is far
             * better than no display at all, so a failure here is logged and
             * the compositor is left NULL. Every call site checks it.
             */
            {
                struct TagItem comptags[] =
                {
                    { aHidd_Compositor_DisplayHidd, (IPTR)XSD(cl)->display },
                    { TAG_DONE,                     0                      }
                };

                XSD(cl)->compositor = OOP_NewObject(XSD(cl)->compositorclass, NULL, comptags);
                if (XSD(cl)->compositor)
                    D(bug("[AmdgpuGfx:Driver] %s: compositor @ 0x%p\n", __func__, XSD(cl)->compositor));
                else
                    bug("[AmdgpuGfx:Driver] %s: compositor creation FAILED -- "
                        "screen composition unavailable\n", __func__);
            }

            data->ResetInterrupt.is_Node.ln_Name = cl->ClassNode.ln_Name;
            data->ResetInterrupt.is_Code = (VOID_FUNC)ResetHandler;
            data->ResetInterrupt.is_Data = &XSD(cl)->data;
            AddResetCallback(&data->ResetInterrupt);
        }
        else
        {
            OOP_MethodID dispose_mid = XSD(cl)->mid_Dispose;
            OOP_CoerceMethod(cl, o, (OOP_Msg)&dispose_mid);
            o = NULL;
        }
    }
    ReturnPtr("AmdgpuGfx::New", OOP_Object *, o);
}


/*********  Gfx::CopyBox()  ************************************/

/* Below this many bytes the GPU loses to the CPU: submitting and fencing an
 * SDMA packet costs more than memcpy'ing a small rectangle. The measured
 * crossover on this part is 64 KiB..1 MiB, so gate conservatively and let the
 * superclass handle everything smaller. An ungated accelerator is slower than
 * no accelerator. */
#define AMDGPU_COPYBOX_MIN_BYTES  (64UL * 1024UL)

/* Rows per SDMA submission; amdgpu_sdma_copy_rect_vram() takes at most 64. */
#define AMDGPU_COPYBOX_ROWS       64
#define AMDGPU_COPYBOX_DN_MIN_BYTES 4096UL

/* Is this bitmap's storage inside our VRAM pool? Asked by ADDRESS, never by
 * class: the bitmap handed to us may be wrapped (CursorFB wraps displayable
 * bitmaps whenever the driver reports no hardware sprite), and a wrapper
 * forwards Get to the real bitmap. Two bugs on this branch came from trusting
 * a bitmap's class; this asks the only question that matters. */
static BOOL amdgpu_bm_vram(OOP_Class *cl, OOP_Object *bm, UQUAD *mc, ULONG *stride)
{
    struct HWData *hw = &XSD(cl)->data;
    IPTR  buf = 0, bpr = 0, hgt = 0;
    UQUAD off, span;

    if (!bm || !hw->vram_pool_size)
        return FALSE;

    OOP_GetAttr(bm, aHidd_ChunkyBM_Buffer, &buf);
    OOP_GetAttr(bm, aHidd_BitMap_BytesPerRow, &bpr);
    OOP_GetAttr(bm, aHidd_BitMap_Height, &hgt);
    if (!buf || !bpr || !hgt)
        return FALSE;

    /* AUDIT P1-5: the START pointer being in the pool is not enough -- the WHOLE
     * bitmap (height rows at stride) must lie inside the pool, or an SDMA copy/
     * fill would run off the end. Validate the complete range in 64-bit. */
    if (buf < (IPTR)hw->vram_pool_cpu ||
        buf >= (IPTR)hw->vram_pool_cpu + hw->vram_pool_size)
        return FALSE;
    off  = (UQUAD)(buf - (IPTR)hw->vram_pool_cpu);
    span = (UQUAD)bpr * (UQUAD)hgt;
    if (off + span > (UQUAD)hw->vram_pool_size)
        return FALSE;

    *mc     = hw->vram_pool_mc + off;
    *stride = (ULONG)bpr;
    return TRUE;
}

VOID Amdgpu__Hidd_Gfx__CopyBox(OOP_Class *cl, OOP_Object *o, struct pHidd_Gfx_CopyBox *msg)
{
    UQUAD  src_mc = 0, dst_mc = 0;
    ULONG  src_stride = 0, dst_stride = 0, bytes_pp = 0, row_bytes, done;
    OOP_Object *access_bm[2];
    ULONG held = 0, access_count;
    BOOL copied = FALSE, download = FALSE, bounce = FALSE;
    UBYTE *dst_cpu = NULL;
    OOP_MethodID release_mid;

    /*
     * Accelerate only VRAM -> VRAM. This is exactly the case that VRAM-resident
     * bitmaps created: between two system-RAM bitmaps an SDMA copy is a
     * PESSIMISATION, because the source would have to be GART-mapped and the
     * submission costs more than the memcpy it replaces.
     */
    if (!msg->width || !msg->height)
        return;

    /* AUDIT P0-4: the SDMA blit is a raw byte copy. It can ONLY substitute for a
     * Copy-draw-mode, full-colour-mask, identical-source/dest-format blit. XOR/
     * AND/invert minterms, partial masks and cross-format copies must go to the
     * superclass, which honours those semantics -- otherwise the fast path paints
     * the wrong pixels. (Non-overlap is validated further down.) */
    if (msg->gc)
    {
        if ((ULONG)OOP_GET(msg->gc, aHidd_GC_DrawMode) != vHidd_GC_DrawMode_Copy)
        {
            OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
            return;
        }
        if (((ULONG)(IPTR)OOP_GET(msg->gc, aHidd_GC_ColorMask) & 0xFFFFFFFFUL)
            != 0xFFFFFFFFUL)
        {
            OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
            return;
        }
    }
    {
        OOP_Object *spf = (OOP_Object *)OOP_GET(msg->src,  aHidd_BitMap_PixFmt);
        OOP_Object *dpf = (OOP_Object *)OOP_GET(msg->dest, aHidd_BitMap_PixFmt);
        if (!spf || !dpf ||
            OOP_GET(spf, aHidd_PixFmt_StdPixFmt) != OOP_GET(dpf, aHidd_PixFmt_StdPixFmt))
        {
            OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
            return;
        }
    }
    {
        static ULONG nrej = 0;
        BOOL sv = amdgpu_bm_vram(cl, msg->src,  &src_mc, &src_stride);
        BOOL dv = amdgpu_bm_vram(cl, msg->dest, &dst_mc, &dst_stride);

        if (sv && !dv)
        {
            IPTR dbuf = 0, dbpr = 0, dhgt = 0;

            OOP_GetAttr(msg->dest, aHidd_ChunkyBM_Buffer, &dbuf);
            OOP_GetAttr(msg->dest, aHidd_BitMap_BytesPerRow, &dbpr);
            OOP_GetAttr(msg->dest, aHidd_BitMap_Height, &dhgt);
            if (dbuf && dbpr && dhgt)
            {
                download   = TRUE;
                dst_cpu    = (UBYTE *)dbuf;
                dst_stride = (ULONG)dbpr;
            }
        }
        if (!download && (!sv || !dv))
        {
            /* Zero accelerated CopyBox calls could mean "nothing ever issues
             * one" or "the gate rejects them all", and those need opposite
             * fixes. Say which. */
            if (++nrej <= 8)
                bug("[AmdgpuGfx] CopyBox REJECT #%lu: src_vram=%d dst_vram=%d %ux%u\n",
                    (unsigned long)nrej, (int)sv, (int)dv, msg->width, msg->height);
            OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
            return;
        }
    }

    {
        OOP_Object *pf = (OOP_Object *)OOP_GET(msg->src, aHidd_BitMap_PixFmt);
        bytes_pp = pf ? (ULONG)OOP_GET(pf, aHidd_PixFmt_BytesPerPixel) : 0;
    }
    if (!bytes_pp)
    {
        OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
        return;
    }
    row_bytes = (ULONG)msg->width * bytes_pp;

    if (!download && src_mc == dst_mc)
    {
        LONG sx2 = msg->srcX + (LONG)msg->width - 1, sy2 = msg->srcY + (LONG)msg->height - 1;
        LONG dx2 = msg->destX + (LONG)msg->width - 1, dy2 = msg->destY + (LONG)msg->height - 1;

        bounce = !(sx2 < msg->destX || dx2 < msg->srcX || sy2 < msg->destY || dy2 < msg->srcY);
    }

    /* AUDIT P1-5: total in 64-bit so a large box cannot wrap the size gate. */
    if ((UQUAD)row_bytes * (UQUAD)msg->height <
        ((download || bounce) ? AMDGPU_COPYBOX_DN_MIN_BYTES : AMDGPU_COPYBOX_MIN_BYTES))
    {
        static ULONG nsmall = 0;
        if (++nsmall <= 8)
            bug("[AmdgpuGfx] CopyBox too small #%lu: %ux%u = %lu bytes (gate %lu)\n",
                (unsigned long)nsmall, msg->width, msg->height,
                (unsigned long)((UQUAD)row_bytes * (UQUAD)msg->height),
                (unsigned long)AMDGPU_COPYBOX_MIN_BYTES);
        OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
        return;
    }

    /* CopyBox bypasses the bitmap methods which normally hide the software
     * pointer. Keep BOTH operands' public direct-access brackets across every
     * SDMA submission and its fence: source pixels must not contain the cursor,
     * and destination save-under must be refreshed only after the copy. The
     * objects can be CursorFB wrappers, so never unwrap/coerce this dispatch.
     * Our VRAM gate limits this to one display's cursor semaphore; order the
     * two objects consistently and acquire an identical object only once.
     * Release everything before falling back to the superclass.
     */
    access_bm[0] = msg->src;
    access_bm[1] = msg->dest;
    access_count = msg->src == msg->dest ? 1 : 2;
    if ((IPTR)access_bm[0] > (IPTR)access_bm[1])
    {
        OOP_Object *tmp = access_bm[0];
        access_bm[0] = access_bm[1];
        access_bm[1] = tmp;
    }
    release_mid = OOP_GetMethodID(IID_Hidd_BitMap, moHidd_BitMap_ReleaseDirectAccess);
    while (held < access_count)
    {
        UBYTE *address = NULL;
        ULONG width = 0, height = 0, bank = 0, mem = 0;
        OOP_Object *bm = access_bm[held];
        struct pHidd_BitMap_ObtainDirectAccess access = {
            OOP_GetMethodID(IID_Hidd_BitMap, moHidd_BitMap_ObtainDirectAccess),
            &address, &width, &height, &bank, &mem
        };
        UQUAD mc = bm == msg->src ? src_mc : dst_mc;
        ULONG stride = bm == msg->src ? src_stride : dst_stride;

        if (!OOP_DoMethod(bm, (OOP_Msg)&access))
            goto release_access;
        held++;
        /* Only linear, unchanged storage is safe for the addresses already
         * checked above. A successful obtain must still be released if its
         * layout cannot service this rectangle. Check both roles for a
         * same-bitmap copy, including negative coordinates before conversion.
         */
        if (!address ||
            address != ((download && bm == msg->dest) ? dst_cpu :
                        (UBYTE *)XSD(cl)->data.vram_pool_cpu +
                        (mc - XSD(cl)->data.vram_pool_mc)) ||
            bank < (UQUAD)stride * height || mem < (UQUAD)stride * height ||
            (bm == msg->src && (msg->srcX < 0 || msg->srcY < 0 ||
                (UQUAD)msg->srcX + msg->width > width ||
                (UQUAD)msg->srcY + msg->height > height)) ||
            (bm == msg->dest && (msg->destX < 0 || msg->destY < 0 ||
                (UQUAD)msg->destX + msg->width > width ||
                (UQUAD)msg->destY + msg->height > height)))
            goto release_access;
    }

    {
        static ULONG ncopy = 0;
        if (++ncopy <= 12)
            bug("[AmdgpuGfx] CopyBox #%lu (%d,%d)->(%d,%d) %ux%u bpp=%lu "
                "src(mc=%08lx%08lx stride=%lu) dst(mc=%08lx%08lx stride=%lu)\n",
                (unsigned long)ncopy, msg->srcX, msg->srcY, msg->destX, msg->destY,
                msg->width, msg->height, (unsigned long)bytes_pp,
                (ULONG)(src_mc >> 32), (ULONG)src_mc, (unsigned long)src_stride,
                (ULONG)(dst_mc >> 32), (ULONG)dst_mc, (unsigned long)dst_stride);
    }

    if (bounce)
    {
        UBYTE *tmp = AllocVec((IPTR)row_bytes * msg->height, MEMF_ANY);
        UQUAD d_off = (UQUAD)msg->destY * dst_stride + (UQUAD)msg->destX * bytes_pp;

        if (!tmp || !amdgpu_sdma_download_rect(XSD(cl)->device,
                src_mc + (UQUAD)msg->srcY * src_stride + (UQUAD)msg->srcX * bytes_pp,
                src_stride, tmp, row_bytes, row_bytes, msg->height, AMDGPU_XFER_RAW))
        {
            FreeVec(tmp);
            goto release_access;
        }
        if (!amdgpu_sdma_upload_rect(XSD(cl)->device, tmp, row_bytes, dst_mc + d_off,
                                     dst_stride, row_bytes, msg->height, AMDGPU_XFER_RAW))
        {
            UBYTE *d = (UBYTE *)XSD(cl)->data.vram_pool_cpu +
                       (dst_mc - XSD(cl)->data.vram_pool_mc) + d_off;

            for (done = 0; done < msg->height; done++)
                CopyMem(tmp + (IPTR)done * row_bytes, d + (IPTR)done * dst_stride, row_bytes);
        }
        FreeVec(tmp);
        copied = TRUE;
        goto release_access;
    }

    if (download)
    {
        if (!amdgpu_sdma_download_rect(XSD(cl)->device,
                src_mc + (UQUAD)msg->srcY * src_stride + (UQUAD)msg->srcX * bytes_pp,
                src_stride,
                dst_cpu + (UQUAD)msg->destY * dst_stride + (UQUAD)msg->destX * bytes_pp,
                dst_stride, row_bytes, msg->height, AMDGPU_XFER_RAW))
            goto release_access;
        copied = TRUE;
        goto release_access;
    }

    for (done = 0; done < msg->height; done += AMDGPU_COPYBOX_ROWS)
    {
        ULONG rows = msg->height - done;
        ULONG s_off, d_off;

        if (rows > AMDGPU_COPYBOX_ROWS)
            rows = AMDGPU_COPYBOX_ROWS;

        s_off = ((ULONG)msg->srcY  + done) * src_stride + (ULONG)msg->srcX  * bytes_pp;
        d_off = ((ULONG)msg->destY + done) * dst_stride + (ULONG)msg->destX * bytes_pp;

        if (!amdgpu_sdma_copy_rect_vram(XSD(cl)->device,
                                        src_mc, src_stride, s_off,
                                        dst_mc, dst_stride, d_off,
                                        row_bytes, rows))
        {
            /* Partial failure mid-rectangle: the remaining rows are still
             * unwritten, so hand the WHOLE operation to the superclass rather
             * than leave a half-copied box. Redoing the copied rows is
             * harmless; leaving torn ones is not. */
            bug("[AmdgpuGfx] CopyBox: SDMA failed at row %lu/%u -- falling back\n",
                (unsigned long)done, msg->height);
            goto release_access;
        }
    }
    copied = TRUE;

release_access:
    while (held)
    {
        /* OOP_DoMethod evaluates its object argument twice. Never put the
         * release-index decrement in that argument: the same-bitmap case
         * would underflow and dispatch through an out-of-bounds object. */
        OOP_Object *bm = access_bm[held - 1];
        held--;
        OOP_DoMethod(bm, (OOP_Msg)&release_mid);
    }
    if (!copied)
        OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    else
    {
        static ULONG nguarded = 0;
        if (++nguarded <= 12)
            bug("[AmdgpuGfx] CopyBox cursor-guard-v1 complete #%lu: %ux%u rows=%lu\n",
                (unsigned long)nguarded, msg->width, msg->height,
                (unsigned long)msg->height);
    }
}

VOID Amdgpu__Root__Dispose(OOP_Class *cl, OOP_Object *o, OOP_Msg msg)
{
    struct AmdgpuGfxHiddData *data = OOP_INST_DATA(cl, o);

    RemResetCallback(&data->ResetInterrupt);
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    XSD(cl)->gfxhidd = NULL;
}

VOID Amdgpu__Root__Get(OOP_Class *cl, OOP_Object *o, struct pRoot_Get *msg)
{
    ULONG idx;

    if (IS_GFX_ATTR(msg->attrID, idx))
    {
        switch (idx)
        {
            case aoHidd_Gfx_NoFrameBuffer:
                /* MUST stay TRUE. FALSE puts graphics.library in
                 * vHidd_FrameBuffer_Direct (gfx_hiddclass.c get_fbmode), whose
                 * contract requires bm != fb and Show() to copy the new bitmap
                 * INTO a separate framebuffer. This driver's Show()
                 * (amdgpu_displayclass.c) only flips aHidd_BitMap_Visible and
                 * returns msg->bitMap, so display setup rejects the driver and
                 * AROS falls back to Headless. Measured 2026-08-19: with FALSE
                 * the boot log has ZERO Show(), ZERO refreshes and ZERO
                 * SetCursorPos, vs 1/10/5 on the working baseline.
                 * Revisit only together with a Show() that does the copy. */
                *msg->storage = TRUE;
                return;

            case aoHidd_Gfx_SupportsHWCursor:
                /*
                 * FALSE until the DCN sprite actually composites. Claiming a
                 * hardware cursor makes AROS STOP drawing the software pointer
                 * (see the DIPF_IS_SPRITES note in amdgpu_displayclass.c), so
                 * advertising one we cannot draw costs the user their pointer
                 * entirely -- which is exactly what happened: the software
                 * cursor was visible before this was set, and nothing was
                 * visible after.
                 *
                 * A broken hardware cursor is strictly worse than none. Set
                 * this back to TRUE only once a sprite is confirmed ON SCREEN.
                 */
                *msg->storage = FALSE;
                return;

            case aoHidd_Gfx_DisplayDefault:
                *msg->storage = (IPTR)XSD(cl)->display;
                return;

            case aoHidd_Gfx_MemoryAttribs:
                if (msg->storage)
                    amdgpu_gfx_memory_tags(&XSD(cl)->data, (struct TagItem *)msg->storage);
                return;

            case aoHidd_Gfx_SensorAttribs:
                if (msg->storage)
                    amdgpu_gfx_sensor_tags(XSD(cl)->device, (struct TagItem *)msg->storage);
                return;

            case aoHidd_Gfx_InfoAttribs:
                if (msg->storage)
                    amdgpu_gfx_info_tags(XSD(cl)->device, (struct TagItem *)msg->storage,
                                         VERSION_NUMBER, REVISION_NUMBER);
                return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}
