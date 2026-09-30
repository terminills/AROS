/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Bitmap class for the amdgpu boot-mode framebuffer Gfx hidd.
*/

#include <proto/oop.h>
#include <proto/utility.h>
#include <assert.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <graphics/rastport.h>
#include <graphics/gfx.h>
#include <hidd/gfx.h>
#include <oop/oop.h>
#include <aros/symbolsets.h>
#define DEBUG 0
#include <aros/debug.h>

#include <string.h>

#include "amdgpu_gfx_intern.h"
#include "amdgpu_gfx_hidd.h"
#include "compositor.h"

#include LC_LIBDEFS_FILE

#define MNAME_ROOT(x) AmdgpuGfxBM__Root__ ## x
#define MNAME_BM(x) AmdgpuGfxBM__Hidd_BitMap__ ## x

/* aHidd_GC_* expand through HiddGCAttrBase, which <hidd/gc.h> defines as the
 * global __IHidd_GC -- never declared in a -D__OOP_NOATTRBASES__ build. Point
 * it at our own slot, the same override used for HiddCompositorAttrBase in
 * amdgpu_gfxclass.c. The interfaces[] table in amdgpu_gfx_init.c must keep the
 * same order as these indices. */
#undef HiddGCAttrBase
#define HiddGCAttrBase    XSD(cl)->attrBases[9]

/*********** BitMap::New() *************************************/
OOP_Object *MNAME_ROOT(New)(OOP_Class *cl, OOP_Object *o, struct pRoot_New *msg)
{
    EnterFunc(bug("AmdgpuGfx.BitMap::New()\n"));

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg) msg);
    if (o)
    {
        OOP_MethodID       disp_mid;
        struct AmdgpuGfxBitMapData *data;
        HIDDT_ModeID       modeid;
        OOP_Object        *sync, *pf, *dmenum;

        data = OOP_INST_DATA(cl, o);

        /* Get attr values */
        OOP_GetAttr(o, aHidd_BitMap_Display , (APTR)&data->displayhidd);
        OOP_GetAttr(o, aHidd_BitMap_PixFmt  , (APTR)&data->pixfmtobj);
        OOP_GetAttr(o, aHidd_BitMap_ModeID  , &modeid);
        OOP_GetAttr(o, aHidd_ChunkyBM_Buffer, (IPTR *)&data->VideoData);

        OOP_GetAttr(data->displayhidd, aHidd_Display_DMEnumerator, (APTR)&dmenum);
        HIDD_DMEnum_GetMode(dmenum, modeid, &sync, &pf);

        data->width        = OOP_GET(o, aHidd_BitMap_Width);
        data->height       = OOP_GET(o, aHidd_BitMap_Height);
        data->bytesperline = OOP_GET(o, aHidd_BitMap_BytesPerRow);
        data->bytesperpix  = OOP_GET(data->pixfmtobj, aHidd_PixFmt_BytesPerPixel);
        data->bpp          = OOP_GET(data->pixfmtobj, aHidd_PixFmt_BitsPerPixel);
        data->disp_width   = OOP_GET(sync, aHidd_Sync_HDisp);
        data->disp_height  = OOP_GET(sync, aHidd_Sync_VDisp);

        /* DIAGNOSTIC: what pixfmt did this displayable bitmap actually resolve to?
         * The pen/RGB/fill path packs RGB->pixel through THIS object; a mismatch
         * vs the DCN 0xAARRGGBB scanout scrambles pen colours. */
        bug("[AmdgpuGfx:BM] bitmap pixfmt: Std=%ld R %08lx<<%ld G %08lx<<%ld B %08lx<<%ld A %08lx<<%ld bpp=%ld\n",
            (long)OOP_GET(data->pixfmtobj, aHidd_PixFmt_StdPixFmt),
            (unsigned long)OOP_GET(data->pixfmtobj, aHidd_PixFmt_RedMask),   (long)OOP_GET(data->pixfmtobj, aHidd_PixFmt_RedShift),
            (unsigned long)OOP_GET(data->pixfmtobj, aHidd_PixFmt_GreenMask), (long)OOP_GET(data->pixfmtobj, aHidd_PixFmt_GreenShift),
            (unsigned long)OOP_GET(data->pixfmtobj, aHidd_PixFmt_BlueMask),  (long)OOP_GET(data->pixfmtobj, aHidd_PixFmt_BlueShift),
            (unsigned long)OOP_GET(data->pixfmtobj, aHidd_PixFmt_AlphaMask), (long)OOP_GET(data->pixfmtobj, aHidd_PixFmt_AlphaShift),
            (long)data->bpp);

        /*
         * Make a DISPLAYABLE bitmap a real VRAM surface.
         *
         * ChunkyBM has already allocated a system-RAM buffer by this point, but
         * it also accepts a replacement: setting aHidd_ChunkyBM_Buffer frees the
         * buffer it owns and clears own_buffer (gfx_chunkybitmapclass.c,
         * CBM__Root__Set). Swapping in VRAM here rather than supplying a buffer
         * at New time is deliberate -- BytesPerRow is only known AFTER the base
         * class has applied aHidd_BitMap_Align, so this is the first point at
         * which the exact size can be allocated instead of guessed. Guessing it
         * would under-allocate and corrupt the pool.
         *
         * The payoff is that ChunkyBM's drawing now writes straight into VRAM,
         * and "put this bitmap on display" becomes a scanout retarget rather
         * than a copy-forward of every changed rectangle.
         *
         * Non-displayable bitmaps stay in system RAM: they are never scanned
         * out, so VRAM would buy them nothing and the pool is finite. Failure
         * to allocate is equally fine -- the bitmap simply stays as ChunkyBM
         * made it.
         */
        if (OOP_GET(o, aHidd_BitMap_Displayable) && data->bytesperline && data->height)
        {
            APTR  vcpu = NULL;
            UQUAD vmc  = 0;
            /* AUDIT P1-5: compute the surface size in 64-bit and only VRAM-place
             * it if it fits the ULONG allocator size; an overflowing product must
             * not wrap to a small allocation that the bitmap then draws past.
             * Oversized surfaces stay in system RAM (correct, just unaccelerated). */
            UQUAD need64 = (UQUAD)data->bytesperline * (UQUAD)data->height;
            ULONG need   = (ULONG)need64;

            if (need64 <= 0xFFFFFFFFUL &&
                amdgpu_vram_alloc(&XSD(cl)->data, need, 256, &vcpu, &vmc))
            {
                struct TagItem bt[] = {
                    { aHidd_ChunkyBM_Buffer, (IPTR)vcpu },
                    { TAG_DONE,              0          }
                };

                OOP_SetAttrs(o, bt);
                data->VideoData = (UBYTE *)vcpu;
                data->vram_mc   = vmc;
                data->vram_size = need;   /* remembered for amdgpu_vram_free on Dispose (P1-2) */

                /*
                 * CLEAR IT. ChunkyBM allocates its own buffer with
                 * AllocVec(..., MEMF_CLEAR), so a bitmap has always started out
                 * zeroed; a VRAM buffer that skips that is a behaviour change,
                 * not just an optimisation.
                 *
                 * It showed up exactly as you would expect: the first boot with
                 * VRAM bitmaps scanned out Wanderer's title bar correctly (so
                 * the pixel format was right) over a flat green desktop -- that
                 * green being whatever the pool happened to contain. Anything
                 * Intuition had not yet repainted showed raw VRAM.
                 *
                 * Cleared with SDMA rather than the CPU: 8 MB through the BAR
                 * at the measured 7 MB/s would cost more than a second of
                 * blocking work per bitmap. Falls back to a CPU clear if the
                 * engine is not usable, because a slow correct bitmap beats a
                 * fast garbage one.
                 */
                {
                    struct AmdgpuDevice *ad = XSD(cl)->device;
                    BOOL cleared = FALSE;

                    /*
                     * CHUNK IT. amdgpu_sdma_fill_rect() refuses more than 64
                     * rows in one submission ("caller chunks; ring budget for
                     * one pass"), and the first version handed it all 1080 --
                     * so it returned FALSE immediately and every displayable
                     * bitmap silently took the CPU fallback instead: ~8 MB
                     * through the BAR at the measured 7115 KiB/s, over a second
                     * of blocking work per bitmap. The fallback log line was
                     * the only sign, and it read like SDMA was not up yet
                     * rather than like a caller bug.
                     */
                    if (ad) {
                        ULONG row;

                        cleared = TRUE;
                        for (row = 0; row < (ULONG)data->height; row += 64) {
                            ULONG n = (ULONG)data->height - row;

                            if (n > 64)
                                n = 64;
                            if (!amdgpu_sdma_fill_rect(ad, vmc, data->bytesperline,
                                                       row * data->bytesperline,
                                                       data->bytesperline, n, 0)) {
                                cleared = FALSE;
                                break;
                            }
                        }
                    }
                    if (!cleared) {
                        bug("[AmdgpuGfx:BitMap] SDMA clear unavailable -- clearing"
                            " %lu bytes with the CPU\n", (unsigned long)need);
                        SetMem(vcpu, 0, need);
                    }

                    /*
                     * Prove the clear reached the surface the OTG actually
                     * fetches, by reading it back BOTH ways: through the CPU
                     * aperture we just wrote, and through the MC address the
                     * scanout uses. The screen shows a flat colour where this
                     * memory should be black, and three plausible explanations
                     * have already been eliminated by measurement (scanout not
                     * latched, buffer uninitialised, stale capture). The
                     * remaining split is whether the PIXELS are wrong or the
                     * DISPLAY PATH is misreading correct pixels -- and only a
                     * read of the memory itself can separate those.
                     *
                     * If CPU and MC disagree, the pool's CPU/MC lockstep
                     * assumption is wrong and every address in it is suspect.
                     */
                    {
                        volatile ULONG *cpuv = (volatile ULONG *)vcpu;
                        ULONG viamc[4] = { 0, 0, 0, 0 };

                        if (ad)
                            amdgpu_vram_read(ad, vmc - AMDGPU_VRAM_MC_BASE, viamc, 4);

                        bug("[AmdgpuGfx:BitMap] post-clear check: via CPU "
                            "%08lx %08lx %08lx %08lx | via MC %08lx %08lx %08lx %08lx\n",
                            (unsigned long)cpuv[0], (unsigned long)cpuv[1],
                            (unsigned long)cpuv[2], (unsigned long)cpuv[3],
                            (unsigned long)viamc[0], (unsigned long)viamc[1],
                            (unsigned long)viamc[2], (unsigned long)viamc[3]);
                    }
                }

                bug("[AmdgpuGfx:BitMap] displayable %ldx%ld bm 0x%p now VRAM-resident:"
                    " cpu 0x%p mc 0x%08x%08x (%lu bytes)\n",
                    data->width, data->height, o, vcpu,
                    (ULONG)(vmc >> 32), (ULONG)vmc, (unsigned long)need);
            }
        }

        D(bug("[AmdgpuGfx:BitMap] Bitmap %ld x %ld, %u bytes per pixel, %u bytes per line\n",
              data->width, data->height, data->bytesperpix, data->bytesperline));
        D(bug("[AmdgpuGfx:BitMap] Video data at 0x%p (%u bytes)\n", data->VideoData, data->bytesperline * data->height));

        if (OOP_GET(data->pixfmtobj, aHidd_PixFmt_ColorModel) != vHidd_ColorModel_Palette)
            ReturnPtr("AmdgpuGfx.BitMap::New()", OOP_Object *, o);

        data->DAC = AllocMem(768, MEMF_ANY);
        D(bug("[AmdgpuGfx:BitMap] Palette data at 0x%p\n", data->DAC));
        if (data->DAC)
            ReturnPtr("AmdgpuGfx.BitMap::New()", OOP_Object *, o);

        disp_mid = OOP_GetMethodID(IID_Root, moRoot_Dispose);

        OOP_CoerceMethod(cl, o, (OOP_Msg) &disp_mid);
        o = NULL;
    } /* if created object */

    ReturnPtr("AmdgpuGfx.BitMap::New()", OOP_Object *, o);
}

/**********  Bitmap::Dispose()  ***********************************/
VOID MNAME_ROOT(Dispose)(OOP_Class *cl, OOP_Object *o, OOP_Msg msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);

    D(bug("[AmdgpuGfx:BitMap] Dispose(0x%p)\n", o));

    /* AUDIT P1-2: reclaim GPU resources before the object goes away, or repeated
     * bitmap create/destroy exhausts the VRAM pool and GART VA and silently
     * pushes displayable bitmaps into system RAM. */
    if (data->gart_va) {
        amdgpu_gart_unmap(XSD(cl)->device, data->gart_va, data->gart_size);
        data->gart_va   = 0;
        data->gart_size = 0;
    }
    if (data->vram_mc && data->vram_size) {
        amdgpu_vram_free(&XSD(cl)->data, data->vram_mc, data->vram_size);
        data->vram_mc   = 0;
        data->vram_size = 0;
        /* The ChunkyBM buffer pointed into this pool block; clear it so the
         * superclass does not touch freed VRAM (own_buffer was already FALSE, so
         * it will not try to free it either). */
        data->VideoData = NULL;
    }

    if (data->DAC)
        FreeMem(data->DAC, 768);

    OOP_DoSuperMethod(cl, o, msg);

    ReturnVoid("AmdgpuGfx.BitMap::Dispose");
}

/*** BitMap::Get() *******************************************/

VOID MNAME_ROOT(Get)(OOP_Class *cl, OOP_Object *o, struct pRoot_Get *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);
    ULONG              idx;

    if (msg->attrID == aAmdgpuBitMap_Self)
    {
        *msg->storage = (IPTR)o;
        return;
    }
    if (IS_BM_ATTR(msg->attrID, idx))
    {
        switch (idx)
        {
        case aoHidd_BitMap_Visible:
            *msg->storage = data->disp;
            return;
        case aoHidd_BitMap_LeftEdge:
            *msg->storage = (IPTR)data->xoffset;
            return;
        case aoHidd_BitMap_TopEdge:
            *msg->storage = (IPTR)data->yoffset;
            return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/*** BitMap::Set() *******************************************/

VOID MNAME_ROOT(Set)(OOP_Class *cl, OOP_Object *o, struct pRoot_Set *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);
    struct TagItem  *tag, *tstate, *filtered = NULL;
    struct pRoot_Set forward = *msg;
    ULONG           idx;
    LONG xoffset = data->xoffset;
    LONG yoffset = data->yoffset;
    BOOL position = FALSE;

    /* The generic BitMap Set implements offsets by copying to a framebuffer.
     * Our bitmap can BE that framebuffer: forwarding these tags corrupts the
     * backing pixels. Keep the base offsets at zero, own Get/Set, and let the
     * compositor choose either a HUBP viewport or a separate composite. */
    tstate = msg->attrList;
    while ((tag = NextTagItem(&tstate)))
        if (tag->ti_Tag == aHidd_BitMap_LeftEdge ||
            tag->ti_Tag == aHidd_BitMap_TopEdge)
            position = TRUE;
    if (position)
    {
        filtered = CloneTagItems(msg->attrList);
        if (!filtered)
        {
            bug("[amdgpu-pan] Set refused: no tag storage; bitmap unchanged\n");
            return;
        }
        forward.attrList = filtered;
    }

    tstate = forward.attrList;
    while((tag = NextTagItem(&tstate)))
    {
        if(IS_BM_ATTR(tag->ti_Tag, idx))
        {
            switch(idx)
            {
            case aoHidd_BitMap_LeftEdge:
                xoffset = (LONG)tag->ti_Data;
                tag->ti_Tag = TAG_IGNORE;
                break;
            case aoHidd_BitMap_TopEdge:
                yoffset = (LONG)tag->ti_Data;
                tag->ti_Tag = TAG_IGNORE;
                break;
            case aoHidd_BitMap_Visible:
                D(bug("[AmdgpuGfx:BitMap] Setting Visible to %d\n", tag->ti_Data));
                data->disp = tag->ti_Data;
                if (data->disp) {
                    if (data->DAC)
                        DACLoad(XSD(cl), data->DAC, 0, 256);
                }
                break;
            }
        }
    }

    OOP_DoSuperMethod(cl, o, (OOP_Msg)&forward);
    if (filtered)
        FreeTagItems(filtered);
    if (position)
        amdgpu_bitmap_position(cl, o, xoffset, yoffset);
}

/*** BitMap::SetColors() *************************************/

BOOL MNAME_BM(SetColors)(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_SetColors *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);
    struct HWData *hwdata = &XSD(cl)->data;
    ULONG xc_i, col_i;
    UBYTE p_shift;
    UWORD red, green, blue;

    D(bug("[AmdgpuGfx:BitMap] SetColors(%u, %u)\n", msg->firstColor, msg->numColors));

    if (!OOP_DoSuperMethod(cl, o, (OOP_Msg)msg)) {
        D(bug("[AmdgpuGfx:BitMap] DoSuperMethod() failed\n"));
        return FALSE;
    }

    /* AUDIT P1-7: 1 << data->bpp is undefined for bpp >= 32 (truecolor). The
     * bound only matters for a palette (LUT), so cap the shift; a 32-bit-storage
     * mode admits any 24-bit index and never trips this. */
    {
        ULONG maxcolors = (data->bpp >= 32) ? ~0UL : (1UL << data->bpp);
        if ((ULONG)(msg->firstColor + msg->numColors) > maxcolors)
            return FALSE;
    }

    if (data->DAC) {
        for ( xc_i = msg->firstColor, col_i = 0;
              col_i < msg->numColors;
              xc_i ++, col_i ++) {
            red   = msg->colors[col_i].red   >> 8;
            green = msg->colors[col_i].green >> 8;
            blue  = msg->colors[col_i].blue  >> 8;

            /* Update DAC register values */
            p_shift = 8 - hwdata->palettewidth;
            data->DAC[xc_i*3] = red >> p_shift;
            data->DAC[xc_i*3+1] = green >> p_shift;
            data->DAC[xc_i*3+2] = blue >> p_shift;
        }

        /* Upload palette to the DAC if the current bitmap is on display */
        if (data->disp)
            DACLoad(XSD(cl), data->DAC, msg->firstColor, msg->numColors);
    }
    return TRUE;
}


/*** BitMap::FillRect() **************************************/

/* Same reasoning as CopyBox: below this the submit-and-fence costs more than
   the CPU stores it replaces. Fills are cheaper per byte than copies (no read
   side), but the fixed submission cost is identical, so gate on the same
   order of magnitude and let ChunkyBM handle small rectangles. */
#define AMDGPU_FILLRECT_MIN_BYTES  (64UL * 1024UL)
#define AMDGPU_FILLRECT_ROWS       64

VOID MNAME_BM(FillRect)(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_FillRect *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);
    struct AmdgpuDevice *ad = XSD(cl)->device;
    ULONG w, h, row_bytes, done, fg, mode;

    /*
     * Accelerate only a plain opaque COPY-mode fill into a VRAM-resident
     * bitmap. Every other drawmode (XOR, AND, invert...) is a read-modify-write
     * that a constant fill cannot express, and getting that wrong paints the
     * wrong pixels rather than merely painting them slowly.
     */
    if (!ad || !data->vram_mc || !msg->gc)
    {
        OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
        return;
    }

    mode = (ULONG)OOP_GET(msg->gc, aHidd_GC_DrawMode);
    if (mode != vHidd_GC_DrawMode_Copy)
    {
        OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
        return;
    }

    /* AUDIT P1-1: SDMA CONSTANT_FILL writes the whole 32-bit pixel; it cannot
     * honour a partial GC ColorMask (that needs a read-modify-write). Require the
     * full mask over the pixel or defer to the superclass, which does. Default
     * mask is ~0 so the common case still accelerates. */
    if (((ULONG)(IPTR)OOP_GET(msg->gc, aHidd_GC_ColorMask) & 0xFFFFFFFFUL)
        != 0xFFFFFFFFUL)
    {
        OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
        return;
    }

    if (msg->maxX < msg->minX || msg->maxY < msg->minY)
        return;
    w = (ULONG)(msg->maxX - msg->minX) + 1;
    h = (ULONG)(msg->maxY - msg->minY) + 1;
    row_bytes = w * data->bytesperpix;

    if ((UQUAD)row_bytes * h < AMDGPU_FILLRECT_MIN_BYTES)
    {
        OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
        return;
    }

    /*
     * SDMA CONSTANT_FILL writes a 32-bit pattern, so it can only express a fill
     * whose pixel is exactly 4 bytes wide. At 16bpp the pen would have to be
     * duplicated into both halves AND the rectangle would have to start on an
     * even pixel for that to be correct; rather than special-case it silently,
     * hand non-32bpp fills back to the superclass.
     */
    if (data->bytesperpix != 4)
    {
        OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
        return;
    }

    fg = (ULONG)OOP_GET(msg->gc, aHidd_GC_Foreground);

    for (done = 0; done < h; done += AMDGPU_FILLRECT_ROWS)
    {
        ULONG rows = h - done;
        ULONG off;

        if (rows > AMDGPU_FILLRECT_ROWS)
            rows = AMDGPU_FILLRECT_ROWS;

        off = ((ULONG)msg->minY + done) * data->bytesperline
            + (ULONG)msg->minX * data->bytesperpix;

        if (!amdgpu_sdma_fill_rect(ad, data->vram_mc, data->bytesperline,
                                   off, row_bytes, rows, fg))
        {
            /* Hand the WHOLE rectangle back rather than leave it part-filled;
             * repainting filled rows is harmless, a torn fill is not. */
            bug("[AmdgpuGfx:BitMap] FillRect: SDMA failed at row %lu/%lu -- falling back\n",
                (unsigned long)done, (unsigned long)h);
            OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
            return;
        }
    }
}

VOID MNAME_BM(UpdateRect)(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_UpdateRect *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);
    static ULONG present_reports;
    BOOL trace_present = msg->width == 200 && msg->height == 200 &&
                         present_reports < 3;
    IPTR displayable = 0;

    if (trace_present) {
        displayable = OOP_GET(o, aHidd_BitMap_Displayable);
        bug("[amdgpu-present] UpdateRect #%lu object=%p rect=%d,%d %dx%d "
            "displayable=%ld visible=%ld compositor=%p video=%p scanout=%p "
            "same=%ld vram=%08lx:%08lx\n",
            (unsigned long)(present_reports + 1), o, msg->x, msg->y,
            msg->width, msg->height, (long)displayable, (long)data->disp,
            XSD(cl)->compositor, data->VideoData, XSD(cl)->data.framebuffer,
            (long)(data->VideoData == XSD(cl)->data.framebuffer),
            (unsigned long)(data->vram_mc >> 32), (unsigned long)data->vram_mc);
    }

    /* DisplayResource has already completed WritePixelArray before UpdateRect
     * is dispatched.  Sample the first Mesa-sized update once, through both
     * views of the destination surface, to locate the remaining invisible-
     * present failure without flooding the serial line:
     *
     *   CPU black              -> the format/conversion write did not land
     *   CPU coloured, MC black -> BAR/aperture coherency or address mismatch
     *   both coloured          -> the remaining fault is in DCN scanout
     *
     * The full CPU scan establishes that this is rendered content rather than
     * one coincidental non-zero word.  Only three words use the slow indirect
     * MM_INDEX aperture. */
    if (trace_present && present_reports == 0 && data->VideoData &&
        data->vram_mc && data->bytesperpix == 4 && msg->x >= 0 && msg->y >= 0 &&
        msg->x + msg->width <= data->width &&
        msg->y + msg->height <= data->height)
    {
        struct AmdgpuDevice *ad = XSD(cl)->device;
        ULONG nonzero_rgb = 0, varying_rgb = 0, first_rgb = 0;
        ULONG cpu_first = 0, cpu_centre = 0, cpu_last = 0;
        ULONG mc_first = 0, mc_centre = 0, mc_last = 0;
        ULONG x, y;
        UQUAD first_off, centre_off, last_off;

        first_off = (UQUAD)(ULONG)msg->y * data->bytesperline +
                    (UQUAD)(ULONG)msg->x * 4;
        centre_off = (UQUAD)((ULONG)msg->y + (ULONG)msg->height / 2) *
                     data->bytesperline +
                     (UQUAD)((ULONG)msg->x + (ULONG)msg->width / 2) * 4;
        last_off = (UQUAD)((ULONG)msg->y + (ULONG)msg->height - 1) *
                   data->bytesperline +
                   (UQUAD)((ULONG)msg->x + (ULONG)msg->width - 1) * 4;

        for (y = 0; y < (ULONG)msg->height; y++) {
            volatile ULONG *row = (volatile ULONG *)(data->VideoData +
                ((ULONG)msg->y + y) * data->bytesperline +
                (ULONG)msg->x * 4);

            for (x = 0; x < (ULONG)msg->width; x++) {
                ULONG raw = row[x];

                if (x == 0 && y == 0)
                    cpu_first = raw;
                if (x == (ULONG)msg->width / 2 &&
                    y == (ULONG)msg->height / 2)
                    cpu_centre = raw;
                if (x + 1 == (ULONG)msg->width &&
                    y + 1 == (ULONG)msg->height)
                    cpu_last = raw;
                if (raw & 0x00ffffffUL) {
                    nonzero_rgb++;
                    if (!first_rgb)
                        first_rgb = raw;
                    if ((raw & 0x00ffffffUL) !=
                        (first_rgb & 0x00ffffffUL))
                        varying_rgb++;
                }
            }
        }

        if (ad && last_off + 4 <= data->vram_size) {
            UQUAD base = data->vram_mc - AMDGPU_VRAM_MC_BASE;

            amdgpu_vram_read(ad, base + first_off, &mc_first, 1);
            amdgpu_vram_read(ad, base + centre_off, &mc_centre, 1);
            amdgpu_vram_read(ad, base + last_off, &mc_last, 1);
        }

        bug("[amdgpu-present] destination call=1 nonzero-rgb=%lu/%lu "
            "varying-rgb=%lu first-rgb=%08lx | CPU %08lx %08lx %08lx | "
            "MC %08lx %08lx %08lx\n",
            (unsigned long)nonzero_rgb,
            (unsigned long)((ULONG)msg->width * (ULONG)msg->height),
            (unsigned long)varying_rgb, (unsigned long)first_rgb,
            (unsigned long)cpu_first, (unsigned long)cpu_centre,
            (unsigned long)cpu_last, (unsigned long)mc_first,
            (unsigned long)mc_centre, (unsigned long)mc_last);
    }

    D(bug("[AmdgpuGfx:BitMap] UpdateRect(%d, %d, %d, %d), bitmap 0x%p\n", msg->x, msg->y, msg->width, msg->height, o));
    /* A displayable bitmap can be visible through the compositor without
     * being the directly scanned-out bitmap (data->disp). Match Nouveau's
     * notification contract so in-place drawing updates a split screen too.
     * Notify before taking the framebuffer lock: compositor redraw takes
     * its semaphore before CopyBox can acquire the framebuffer lock. */
    if (XSD(cl)->compositor && OOP_GET(o, aHidd_BitMap_Displayable))
    {
        struct pHidd_Compositor_BitMapRectChanged changed = {
            .mID = OOP_GetMethodID(IID_Hidd_Compositor,
                                  moHidd_Compositor_BitMapRectChanged),
            .bm = o,
            .x = msg->x,
            .y = msg->y,
            .width = msg->width,
            .height = msg->height
        };

        OOP_DoMethod(XSD(cl)->compositor, (OOP_Msg)&changed);
        if (trace_present)
            bug("[amdgpu-present] UpdateRect compositor notification returned\n");
    }

    if (data->disp) {
        LOCK_FRAMEBUFFER(XSD(cl));
        amdgpuGfxDoRefreshArea(&XSD(cl)->data, data, msg->x, msg->y, msg->x + msg->width, msg->y + msg->height);
        UNLOCK_FRAMEBUFFER(XSD(cl));
    }
    if (trace_present)
        present_reports++;
}

/* ------------------------------------------------------------------------- *
 * Complete, self-consistent 2D method set (ported from nvidiabitmap.c /
 * nouveau_bitmapclass.c). The earlier partial override set regressed the whole
 * desktop because ChunkyBM's inherited PutTemplate/PutPattern/text call back
 * through PutImage/GetImage/PutPixel on the object; overriding a *subset* with
 * behaviour that differed from ChunkyBM corrupted that inherited rendering.
 *
 * The proven fix (nvidia/nouveau): each override matches ChunkyBM's semantics
 * exactly for the case it handles and defers everything else to super, and
 * PutTemplate/PutPattern delegate to the MEM-buffer primitives (PutMemTemplate/
 * PutMemPattern) which operate on VideoData directly -- so they do NOT dispatch
 * back through PutImage/GetImage. Audited for internal PutImage/GetImage/
 * PutPixel dispatch: PutTemplate, PutPattern (handled via MEM variants here),
 * and the base JAM/COMPLEMENT paths (bypassed). PutImage/GetImage handle only
 * the Native box copy (identical to ChunkyBM's CopyMemBox path) and defer all
 * pixel-format conversions to the superclass converter.
 * ------------------------------------------------------------------------- */

static OOP_MethodID mid_CopyMemBox32, mid_PutMemTemplate32, mid_PutMemTemplate16,
                    mid_PutMemPattern32, mid_PutMemPattern16;
static BOOL amdgpu_bm_mids_ready = FALSE;

/* AUDIT P2-2: these interface method IDs were filled by a lazy unsynchronized
 * boolean, so on this 32-core box two tasks could observe a half-initialized set.
 * Double-checked locking under HW_acc: the unlocked fast-path read is safe once
 * mids_ready is TRUE because it is stored LAST, inside the lock, and x86_64 is
 * TSO (a core seeing ready=TRUE also sees the prior mid stores). The IDs are the
 * same for every bitmap, so filling them once is correct. */
static void amdgpu_bm_init_mids(struct AmdgpuGfx_staticdata *xsd)
{
    if (amdgpu_bm_mids_ready)
        return;
    ObtainSemaphore(&xsd->HW_acc);
    if (!amdgpu_bm_mids_ready)
    {
        mid_CopyMemBox32     = OOP_GetMethodID((STRPTR)IID_Hidd_BitMap, moHidd_BitMap_CopyMemBox32);
        mid_PutMemTemplate32 = OOP_GetMethodID((STRPTR)IID_Hidd_BitMap, moHidd_BitMap_PutMemTemplate32);
        mid_PutMemTemplate16 = OOP_GetMethodID((STRPTR)IID_Hidd_BitMap, moHidd_BitMap_PutMemTemplate16);
        mid_PutMemPattern32  = OOP_GetMethodID((STRPTR)IID_Hidd_BitMap, moHidd_BitMap_PutMemPattern32);
        mid_PutMemPattern16  = OOP_GetMethodID((STRPTR)IID_Hidd_BitMap, moHidd_BitMap_PutMemPattern16);
        amdgpu_bm_mids_ready = TRUE;
    }
    ReleaseSemaphore(&xsd->HW_acc);
}

/*** BitMap::PutPixel() ************************************************/
VOID MNAME_BM(PutPixel)(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_PutPixel *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);

    if (data->VideoData)
    {
        UBYTE *p = data->VideoData + (ULONG)msg->y * data->bytesperline
                 + (ULONG)msg->x * data->bytesperpix;
        switch (data->bytesperpix)
        {
        case 4: *(volatile ULONG *)p = (ULONG)msg->pixel; return;
        case 2: *(volatile UWORD *)p = (UWORD)msg->pixel; return;
        case 1: *(volatile UBYTE *)p = (UBYTE)msg->pixel; return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/*** BitMap::GetPixel() ************************************************/
HIDDT_Pixel MNAME_BM(GetPixel)(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_GetPixel *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);

    if (data->VideoData)
    {
        UBYTE *p = data->VideoData + (ULONG)msg->y * data->bytesperline
                 + (ULONG)msg->x * data->bytesperpix;
        switch (data->bytesperpix)
        {
        case 4: return (HIDDT_Pixel)(*(volatile ULONG *)p);
        case 2: return (HIDDT_Pixel)(*(volatile UWORD *)p);
        case 1: return (HIDDT_Pixel)(*(volatile UBYTE *)p);
        }
    }
    return (HIDDT_Pixel)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/* Below this size the CPU beats submitting+fencing an SDMA packet (same
 * crossover as CopyBox: the VRAM BAR is uncached, so CPU writes are ~5 MB/s,
 * but a small blit's per-submit overhead still loses). Gate conservatively. */
#define AMDGPU_IMG_DMA_MIN_BYTES  (64UL * 1024UL)

/*** BitMap::PutImage() ***********************************************
 * Native 32-bit box copy into VideoData (identical to ChunkyBM's CopyMemBox
 * path). All other pixel formats -> super (ChunkyBM's pixel-format converter). */
VOID MNAME_BM(PutImage)(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_PutImage *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);

    /* PutImage honours the GC draw mode + colour mask (ChunkyBM/planar contract,
     * e.g. gfx_planarbitmapclass.c:611). The Native and ARGB32 fast paths below
     * are raw copies that implement ONLY plain Copy with a full mask, so any
     * other draw mode or a partial mask MUST defer to the superclass, which
     * applies them -- otherwise the fast path silently ignores the ROP/mask
     * (codex TRIG-0633; the same gate FillRect already has for P1-1). */
    if ((ULONG)OOP_GET(msg->gc, aHidd_GC_DrawMode) != vHidd_GC_DrawMode_Copy ||
        ((ULONG)(IPTR)OOP_GET(msg->gc, aHidd_GC_ColorMask) & 0xFFFFFFFFUL) != 0xFFFFFFFFUL)
    {
        OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
        return;
    }

    if (msg->pixFmt == vHidd_StdPixFmt_Native && data->VideoData && data->bytesperpix == 4)
    {
        /* DMA FAST PATH: a VRAM-resident surface (data->vram_mc) is scanned out
         * directly (P1-5 scanout-retarget model), and CPU writes to it crawl
         * through the uncached BAR at ~5 MB/s. SDMA uploads it at GB/s. This is
         * a raw copy identical to CopyMemBox32, so only the Native (no format
         * conversion) path qualifies; the ARGB32 byteswap path below stays CPU.
         * Any failure falls through to the CPU box copy -- DMA must never be the
         * reason a window fails to paint. */
        ULONG row_bytes = (ULONG)msg->width * 4;

        if (data->vram_mc &&
            (UQUAD)row_bytes * (UQUAD)msg->height >= AMDGPU_IMG_DMA_MIN_BYTES &&
            amdgpu_sdma_upload_rect(XSD(cl)->device,
                (APTR)msg->pixels, msg->modulo,
                data->vram_mc + (UQUAD)msg->y * data->bytesperline + (UQUAD)msg->x * 4,
                data->bytesperline, row_bytes, (ULONG)msg->height, AMDGPU_XFER_RAW))
        {
            return;
        }

        {
            struct pHidd_BitMap_CopyMemBox32 __m = {
                0, (APTR)msg->pixels, 0, 0, (APTR)data->VideoData,
                msg->x, msg->y, msg->width, msg->height, msg->modulo, data->bytesperline
            }, *m = &__m;
            amdgpu_bm_init_mids(XSD(cl));
            m->mID = mid_CopyMemBox32;
            OOP_DoMethod(o, (OOP_Msg)m);
        }
        return;
    }
    /* ARGB32 -> native BGR032. These are NOT byte-identical (audit P0-2, codex
     * TRIG-0529): on little-endian AROS, StdPixFmt_ARGB32 is A8R8G8B8 with memory
     * bytes [A,R,G,B] (low byte = alpha), while our native scanout is B8G8R8X8
     * with memory bytes [B,G,R,X] (4th byte = PADDING, not alpha -- the driver
     * does not advertise framebuffer alpha). The correct conversion reverses the
     * channel bytes: native = byteswap(argb). The earlier "plain copy" fed the
     * decorator alpha path (WritePixelArrayAlpha -> ChunkyBM GetImage/PutImage,
     * decorator/drawfuncs.c:106,115) raw native bytes as ARGB32, so it read blue
     * as alpha and rotated channels -> black / corrupted title bars and menus.
     * (Opaque WritePixelArray never hit this: graphics converts to Native first
     * and takes the CopyMemBox path above, which is why plain-copy looked fine.)
     * Native uses CopyMemBox above; only ARGB32 hits this path. */
    if (msg->pixFmt == vHidd_StdPixFmt_ARGB32 && data->VideoData && data->bytesperpix == 4)
    {
        const UBYTE *srow = (const UBYTE *)msg->pixels;
        UBYTE *drow;
        ULONG yy;

        /* DMA FAST PATH: a VRAM-resident surface is scanned out directly, so the
         * byteswap can happen into the cached scratch and DMA to VRAM instead of
         * byteswapping straight into the uncached BAR. Same conversion as below
         * (byte-identical), no DoRefreshArea (vram_mc IS the scanout, like the
         * Native path). This is the path cybergfx WritePixelArray actually takes. */
        {
            ULONG row_bytes = (ULONG)msg->width * 4;
            if (data->vram_mc &&
                (UQUAD)row_bytes * (UQUAD)msg->height >= AMDGPU_IMG_DMA_MIN_BYTES &&
                amdgpu_sdma_upload_rect(XSD(cl)->device,
                    (APTR)msg->pixels, msg->modulo,
                    data->vram_mc + (UQUAD)msg->y * data->bytesperline + (UQUAD)msg->x * 4,
                    data->bytesperline, row_bytes, (ULONG)msg->height, AMDGPU_XFER_BSWAP))
            {
                return;
            }
        }

        drow = data->VideoData + (ULONG)msg->y * data->bytesperline
                    + (ULONG)msg->x * 4;
        for (yy = 0; yy < (ULONG)msg->height; yy++)
        {
            const UBYTE *sp = srow;
            UBYTE       *dp = drow;
            ULONG xx;
            /* srow is the caller's public ARGB32 buffer, which GHC --strict-align
             * deliberately hands over at base+1 with an odd modulo (TRIG-0692); a
             * (ULONG *) cast is alignment UB (silently OK on x86, faults elsewhere,
             * and couples any SDMA-timeout fallback to it). memcpy the 4 bytes so
             * both load and store are byte-safe on every arch; conversion is
             * byte-identical. ULONG counters also stay safe for width/height >32767. */
            for (xx = 0; xx < (ULONG)msg->width; xx++)
            {
                ULONG px;
                __builtin_memcpy(&px, sp, 4);
                px = __builtin_bswap32(px) & 0x00FFFFFFUL;   /* [A,R,G,B] -> [B,G,R,0]; X is padding, not alpha */
                __builtin_memcpy(dp, &px, 4);
                sp += 4; dp += 4;
            }
            srow += msg->modulo;
            drow += data->bytesperline;
        }
        if (data->disp)
        {
            LOCK_FRAMEBUFFER(XSD(cl));
            amdgpuGfxDoRefreshArea(&XSD(cl)->data, data,
                msg->x, msg->y, msg->x + msg->width, msg->y + msg->height);
            UNLOCK_FRAMEBUFFER(XSD(cl));
        }
        return;
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/*** BitMap::GetImage() **********************************************/
VOID MNAME_BM(GetImage)(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_GetImage *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);

    if (msg->pixFmt == vHidd_StdPixFmt_Native && data->VideoData && data->bytesperpix == 4)
    {
        /* DMA FAST PATH (download): reading a VRAM surface through the uncached
         * BAR is also ~5 MB/s on the CPU; SDMA downloads at GB/s. Raw copy, so
         * Native only; failure falls through to the CPU box copy. */
        ULONG row_bytes = (ULONG)msg->width * 4;

        if (data->vram_mc &&
            (UQUAD)row_bytes * (UQUAD)msg->height >= AMDGPU_IMG_DMA_MIN_BYTES &&
            amdgpu_sdma_download_rect(XSD(cl)->device,
                data->vram_mc + (UQUAD)msg->y * data->bytesperline + (UQUAD)msg->x * 4,
                data->bytesperline,
                (APTR)msg->pixels, msg->modulo, row_bytes, (ULONG)msg->height, AMDGPU_XFER_RAW))
        {
            return;
        }

        {
            struct pHidd_BitMap_CopyMemBox32 __m = {
                0, (APTR)data->VideoData, msg->x, msg->y, (APTR)msg->pixels,
                0, 0, msg->width, msg->height, data->bytesperline, msg->modulo
            }, *m = &__m;
            amdgpu_bm_init_mids(XSD(cl));
            m->mID = mid_CopyMemBox32;
            OOP_DoMethod(o, (OOP_Msg)m);
        }
        return;
    }
    /* Native BGR032 [B,G,R,X] -> ARGB32 [A,R,G,B], the inverse of PutImage above.
     * byteswap reverses the channels; the framebuffer's 4th byte is PADDING, so
     * force alpha = 0xFF (fully opaque) rather than leak garbage into the alpha
     * the decorator's src-over blend reads back. Without this the blend composits
     * decorator images against a wrongly-decoded, wrongly-transparent background. */
    if (msg->pixFmt == vHidd_StdPixFmt_ARGB32 && data->VideoData && data->bytesperpix == 4)
    {
        const UBYTE *srow;
        UBYTE *drow = (UBYTE *)msg->pixels;
        ULONG yy;

        /* DMA FAST PATH (download): SDMA native VRAM -> cached scratch, then the
         * byteswap+alpha-force runs CPU-side out of the scratch. Same conversion
         * as below (byte-identical). */
        {
            ULONG row_bytes = (ULONG)msg->width * 4;
            if (data->vram_mc &&
                (UQUAD)row_bytes * (UQUAD)msg->height >= AMDGPU_IMG_DMA_MIN_BYTES &&
                amdgpu_sdma_download_rect(XSD(cl)->device,
                    data->vram_mc + (UQUAD)msg->y * data->bytesperline + (UQUAD)msg->x * 4,
                    data->bytesperline,
                    (APTR)msg->pixels, msg->modulo, row_bytes, (ULONG)msg->height,
                    AMDGPU_XFER_BSWAP_ALPHA))
            {
                return;
            }
        }

        srow = data->VideoData + (ULONG)msg->y * data->bytesperline
                          + (ULONG)msg->x * 4;
        for (yy = 0; yy < (ULONG)msg->height; yy++)
        {
            const UBYTE *sp = srow;
            UBYTE       *dp = drow;
            ULONG xx;
            /* drow is the caller's public ARGB32 buffer (base+1/odd modulo under
             * GHC --strict-align, TRIG-0692) so the (ULONG *) store cast is
             * alignment UB. memcpy both ends; conversion byte-identical, and ULONG
             * counters stay safe past 32767. */
            for (xx = 0; xx < (ULONG)msg->width; xx++)
            {
                ULONG px;
                __builtin_memcpy(&px, sp, 4);
                px = (__builtin_bswap32(px) & 0xFFFFFF00UL) | 0x000000FFUL;
                __builtin_memcpy(dp, &px, 4);
                sp += 4; dp += 4;
            }
            srow += data->bytesperline;
            drow += msg->modulo;
        }
        return;
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/*** BitMap::PutTemplate() *******************************************
 * Delegate to the MEM template primitive on VideoData -- bypasses the base
 * GetImage/PutImage path. */
VOID MNAME_BM(PutTemplate)(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_PutTemplate *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);

    if (data->VideoData && (data->bytesperpix == 4 || data->bytesperpix == 2))
    {
        amdgpu_bm_init_mids(XSD(cl));
        if (data->bytesperpix == 4)
        {
            struct pHidd_BitMap_PutMemTemplate32 __m = {
                mid_PutMemTemplate32, msg->gc, msg->masktemplate, msg->modulo,
                msg->srcx, (APTR)data->VideoData, data->bytesperline,
                msg->x, msg->y, msg->width, msg->height, msg->inverttemplate
            }, *m = &__m;
            OOP_DoMethod(o, (OOP_Msg)m);
        }
        else
        {
            struct pHidd_BitMap_PutMemTemplate16 __m = {
                mid_PutMemTemplate16, msg->gc, msg->masktemplate, msg->modulo,
                msg->srcx, (APTR)data->VideoData, data->bytesperline,
                msg->x, msg->y, msg->width, msg->height, msg->inverttemplate
            }, *m = &__m;
            OOP_DoMethod(o, (OOP_Msg)m);
        }
        return;
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/*** BitMap::PutPattern() ********************************************/
VOID MNAME_BM(PutPattern)(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_PutPattern *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);

    if (data->VideoData && (data->bytesperpix == 4 || data->bytesperpix == 2))
    {
        amdgpu_bm_init_mids(XSD(cl));
        if (data->bytesperpix == 4)
        {
            struct pHidd_BitMap_PutMemPattern32 __m = {
                mid_PutMemPattern32, msg->gc, msg->pattern, msg->patternsrcx,
                msg->patternsrcy, msg->patternheight, msg->patterndepth, msg->patternlut,
                msg->invertpattern, msg->mask, msg->maskmodulo, msg->masksrcx,
                (APTR)data->VideoData, data->bytesperline, msg->x, msg->y,
                msg->width, msg->height
            }, *m = &__m;
            OOP_DoMethod(o, (OOP_Msg)m);
        }
        else
        {
            struct pHidd_BitMap_PutMemPattern16 __m = {
                mid_PutMemPattern16, msg->gc, msg->pattern, msg->patternsrcx,
                msg->patternsrcy, msg->patternheight, msg->patterndepth, msg->patternlut,
                msg->invertpattern, msg->mask, msg->maskmodulo, msg->masksrcx,
                (APTR)data->VideoData, data->bytesperline, msg->x, msg->y,
                msg->width, msg->height
            }, *m = &__m;
            OOP_DoMethod(o, (OOP_Msg)m);
        }
        return;
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/*** BitMap::Clear() ************************************************
 * Fill the whole bitmap with the GC background pixel. SDMA-accelerated for a
 * VRAM-resident bitmap, CPU fallback otherwise. */
VOID MNAME_BM(Clear)(OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_Clear *msg)
{
    struct AmdgpuGfxBitMapData *data = OOP_INST_DATA(cl, o);
    struct AmdgpuDevice *ad = XSD(cl)->device;
    ULONG bg = (ULONG)GC_BG(msg->gc);

    if (!data->VideoData || data->bytesperpix != 4)
    {
        OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
        return;
    }

    /* Accelerated whole-surface fill for a VRAM-resident bitmap. */
    if (ad && data->vram_mc)
    {
        ULONG row, h = (ULONG)data->height;
        BOOL ok = TRUE;
        for (row = 0; row < h; row += 64)
        {
            ULONG n = h - row;
            if (n > 64) n = 64;
            if (!amdgpu_sdma_fill_rect(ad, data->vram_mc, data->bytesperline,
                                       row * data->bytesperline,
                                       data->bytesperline, n, bg))
            { ok = FALSE; break; }
        }
        if (ok)
            return;
    }

    /* CPU fallback. */
    {
        ULONG  px = (ULONG)data->width;
        LONG   y;
        for (y = 0; y < data->height; y++)
        {
            ULONG *p = (ULONG *)(data->VideoData + (ULONG)y * data->bytesperline);
            ULONG  i;
            for (i = 0; i < px; i++)
                p[i] = bg;
        }
    }
}
