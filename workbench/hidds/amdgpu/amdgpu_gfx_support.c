/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: amdgpu boot-mode framebuffer Gfx hardware support. The firmware
          (GOP) set the mode and handed over a linear framebuffer; the boot
          code passed its description through the KRN_VBEModeInfo/KRN_FBAddr
          tags and bootloader.resource republishes it as BL_Video. There is
          no hardware to program here - everything is reading that record and
          copying pixels. This is exactly the fb the DCN is already scanning
          out on the AMD GPU (proven by the amdgpu-mon color-bar smoke test).
*/

#define DEBUG 0
#include <aros/debug.h>
#include <aros/bootloader.h>
#include <proto/bootloader.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <proto/oop.h>
#include <hidd/pci.h>
#include <proto/kernel.h>
#include <aros/kernel.h>

#include "amdgpu_intern.h"       /* AmdgpuDevice, VRAM BAR, amdgpu_dcn_get/set_scanout */
#include "amdgpu_gfx_intern.h"
#include "amdgpu_gfx_hidd.h"

/* MC (GPU) base of the VRAM aperture on Navi (FB_BASE). The firmware GOP fb and
 * every VRAM offset are addressed as this base + offset; the CPU-visible VRAM
 * BAR maps the same VRAM starting at offset 0. Confirmed on the 9060 XT: the
 * live scanout reads back at exactly this address. */

BOOL initAmdgpuGfxHW(struct HWData *data)
{
    struct BootLoaderBase *BootLoaderBase;
    struct VesaInfo *vi;

    if (!(BootLoaderBase = OpenResource("bootloader.resource")))
        return FALSE;

    if (!(vi = (struct VesaInfo *)GetBootInfo(BL_Video)))
    {
        D(bug("[AmdgpuGfx] HwInit: no video information from the bootloader\n"));
        return FALSE;
    }

    if (!vi->FrameBuffer)
        return FALSE;

    data->width = vi->XSize;
    data->height = vi->YSize;
    data->fb_width = data->width;
    data->fb_height = data->height;
    data->bitsperpixel = data->depth = vi->BitsPerPixel;
    data->bytesperline = vi->BytesPerLine;
    if (data->depth > 8)
    {
        data->redmask = vi->Masks[VI_Red];
        data->greenmask = vi->Masks[VI_Green];
        data->bluemask = vi->Masks[VI_Blue];
        data->redshift = vi->Shifts[VI_Red];
        data->greenshift = vi->Shifts[VI_Green];
        data->blueshift = vi->Shifts[VI_Blue];
    }
    else
    {
        /* Fake masks to allow this mode to be displayed */
        data->redmask = 0xff0000;
        data->greenmask = 0xff00;
        data->bluemask = 0xff;
        data->redshift = 16;
        data->greenshift = 8;
        data->blueshift = 0;
    }
    data->framebuffer = vi->FrameBuffer;
    data->palettewidth = vi->PaletteWidth;
    if (data->palettewidth > 8)
        data->palettewidth = 8;

    if (data->depth > 24)
        data->bytesperpixel = 4;
    else if (data->depth > 16)
        data->bytesperpixel = 3;
    else if (data->depth > 8)
        data->bytesperpixel = 2;
    else
        data->bytesperpixel = 1;

    data->fbsize = data->height * data->bytesperline;

    D(bug("[AmdgpuGfx] HwInit: %ux%ux%u linear FB @ 0x%p, pitch %u\n",
          data->width, data->height, data->depth,
          data->framebuffer, data->bytesperline));
    D(bug("[AmdgpuGfx] HwInit: Masks R %08x<<%2d G %08x<<%2d B %08x<<%2d\n",
          data->redmask, data->redshift,
          data->greenmask, data->greenshift,
          data->bluemask, data->blueshift));

    /* Do NOT clear the framebuffer here: it still holds the live boot text, and
     * the VRAM handoff seeds the amdgpu-owned fb from it for a seamless flip.
     * Clearing would blank the seed to black. The display comes up showing the
     * boot state; the desktop then repaints over it. */
    /* ClearBuffer(data); */
    return TRUE;
}

/*
 * Stage 0 preparation: allocate the AMD-owned framebuffer and VRAM pool without
 * changing the live scanout, viewport or cursor. Late registration does NOT
 * migrate an already-open VESA public screen: native boot10 still reported
 * hidd.gfx.vesa after our old eager flip, so VESA painted into an orphaned buffer.
 * The validated switch_mode/Show path commits the plane when an AMD-owned screen
 * is actually selected. Merely opening this library must not steal the display.
 *
 * Every precondition is guarded: if the device/VRAM BAR/DCN state isn't exactly
 * what we understand, we return with the working GOP-adopt display untouched.
 * Same-geometry storage is seeded from the current framebuffer; a different
 * pitch starts cleared and must be painted before selection. The firmware fb is at VRAM
 * offset 0 (scanout MC == AMDGPU_VRAM_MC_BASE), making CPU-offset and MC-offset
 * identical.
 */
/*
 * amdgpu_vram_alloc -- bump-allocate VRAM out of the mapped BAR aperture.
 *
 * This exists so a DISPLAYABLE bitmap can be a real VRAM surface rather than a
 * system-RAM buffer that has to be copied forward every refresh. ChunkyBM
 * accepts a caller-supplied buffer (gfx_chunkybitmapclass.c: a supplied
 * aHidd_ChunkyBM_Buffer sets own_buffer=FALSE and is never freed), so handing
 * it a pointer from here makes all of its drawing land directly in VRAM, and
 * lets "put the bitmap on display" become a scanout retarget instead of a blit.
 *
 * A serialized bump cursor plus free list reuses retired VRAM allocations.
 * These are direct aperture addresses, not GART mappings. A displayed range
 * cannot be reused until a verified plane transaction retires it; uncertain
 * hardware state quarantines the pool until recovery.
 *
 * CPU and MC addresses come from one cursor because the aperture maps a single
 * contiguous VRAM range -- the same lockstep amdgpuGfxVRAMHandoff() relies on.
 */
static BOOL amdgpu_vram_alloc_locked(struct HWData *hw, ULONG size, ULONG align,
                       APTR *cpu, UQUAD *mc)
{
    ULONG off;
    UWORD i;

    if (!hw || !size || !hw->vram_pool_size || !hw->vram_pool_cpu)
        return FALSE;
    if (align < 256)
        align = 256;                    /* DCN surface addresses want alignment */
    if ((align & (align - 1)) || hw->vram_pool_used > ~(ULONG)0 - (align - 1))
        return FALSE;

    /* AUDIT P1-2: satisfy from a reclaimed block first (first-fit). Freed blocks
     * keep their original >=256-aligned offset, so any request with align<=that
     * offset's alignment fits at the block start; carve the remainder back. */
    for (i = 0; i < hw->vram_free_count; i++) {
        ULONG boff = hw->vram_free[i].off;
        ULONG bsz  = hw->vram_free[i].size;

        if (bsz >= size && (boff & (align - 1)) == 0) {
            if (bsz == size) {
                /* remove entry i (swap with last) */
                hw->vram_free[i] = hw->vram_free[--hw->vram_free_count];
            } else {
                hw->vram_free[i].off  = boff + size;
                hw->vram_free[i].size = bsz - size;
            }
            if (cpu) *cpu = (APTR)((IPTR)hw->vram_pool_cpu + boff);
            if (mc)  *mc  = hw->vram_pool_mc + boff;
            D(bug("[AmdgpuGfx] vram_alloc: %lu bytes REUSED at +0x%lx\n",
                  (unsigned long)size, (unsigned long)boff));
            return TRUE;
        }
    }

    off = (hw->vram_pool_used + (align - 1)) & ~(align - 1);
    if (off > hw->vram_pool_size || size > hw->vram_pool_size - off) {
        bug("[AmdgpuGfx] vram_alloc: pool exhausted (want %lu, %lu of %lu used, "
            "%u free-blocks) -- falling back to a system-RAM bitmap\n",
            (unsigned long)size, (unsigned long)hw->vram_pool_used,
            (unsigned long)hw->vram_pool_size, (unsigned)hw->vram_free_count);
        return FALSE;
    }

    if (cpu) *cpu = (APTR)((IPTR)hw->vram_pool_cpu + off);
    if (mc)  *mc  = hw->vram_pool_mc + off;
    hw->vram_pool_used = off + size;

    D(bug("[AmdgpuGfx] vram_alloc: %lu bytes at +0x%lx (cpu 0x%p mc 0x%08x%08x)"
          " -- %lu of %lu used\n",
          (unsigned long)size, (unsigned long)off,
          cpu ? *cpu : NULL,
          mc ? (ULONG)(*mc >> 32) : 0, mc ? (ULONG)*mc : 0,
          (unsigned long)hw->vram_pool_used, (unsigned long)hw->vram_pool_size));
    return TRUE;
}

static void amdgpu_vram_free_locked(struct HWData *hw, UQUAD mc, ULONG size)
{
    ULONG off, end;
    UWORD i;

    if (!hw || !size || !hw->vram_pool_size || mc < hw->vram_pool_mc)
        return;
    if (mc - hw->vram_pool_mc >= hw->vram_pool_size ||
        size > (UQUAD)hw->vram_pool_size - (mc - hw->vram_pool_mc))
        return;                         /* not one of ours */
    off = (ULONG)(mc - hw->vram_pool_mc);
    end = off + size;                  /* bounded by the pool check above */

    /* Validate before changing the list. A duplicate/overlapping free must not
     * turn a still-live allocation into reusable storage. Active scanout frees
     * are already deferred by the caller, under this same pool lock. */
    if (hw->vram_pool_used > hw->vram_pool_size ||
        end > hw->vram_pool_used || hw->vram_free_count > AMDGPU_VRAM_FREE_MAX)
        goto invalid;
    for (i = 0; i < hw->vram_free_count; i++) {
        ULONG boff = hw->vram_free[i].off;
        ULONG bsz = hw->vram_free[i].size;
        if (!bsz || boff > hw->vram_pool_used ||
            bsz > hw->vram_pool_used - boff ||
            (off < boff + bsz && boff < end))
            goto invalid;
    }

    /* Join interior neighbours too, not only blocks touching the bump tail.
     * Native VramReuseProbe-v1 freed two adjacent 8 MiB interior surfaces, but
     * a 16 MiB request still fell back to system RAM with the old free list.
     * The list is unsorted: restart after each removal to find either edge's
     * new neighbour. Each restart consumes one of at most FREE_MAX entries. */
    for (i = 0; i < hw->vram_free_count;) {
        ULONG boff = hw->vram_free[i].off;
        ULONG bend = boff + hw->vram_free[i].size;
        if (bend == off || boff == end) {
            if (bend == off) off = boff;
            else end = bend;
            hw->vram_free[i] = hw->vram_free[--hw->vram_free_count];
            i = 0;
        } else {
            i++;
        }
    }
    size = end - off;

    /* The merged range may now include the tail, avoiding a list entry. */
    if (end == hw->vram_pool_used) {
        hw->vram_pool_used = off;
        return;
    }

    if (hw->vram_free_count < AMDGPU_VRAM_FREE_MAX) {
        hw->vram_free[hw->vram_free_count].off  = off;
        hw->vram_free[hw->vram_free_count].size = size;
        hw->vram_free_count++;
    } else {
        D(bug("[AmdgpuGfx] vram_free: free list full, block +0x%lx (%lu) leaked "
              "until pool reset\n", (unsigned long)off, (unsigned long)size));
    }
    return;

invalid:
    bug("[AmdgpuGfx] vram_free: invalid/overlapping free; pool quarantined\n");
    hw->plane_faulted = TRUE;
}

static BOOL amdgpu_vram_overlaps(UQUAD a, ULONG asize, UQUAD b, ULONG bsize)
{
    if (!asize || !bsize) return FALSE;
    return a <= b ? b - a < asize : a - b < bsize;
}

BOOL amdgpu_vram_alloc(struct HWData *hw, ULONG size, ULONG align,
                       APTR *cpu, UQUAD *mc)
{
    BOOL ok = FALSE;
    if (!hw || !hw->pool_lock_ready) return FALSE;
    ObtainSemaphore(&hw->pool_lock);
    if (!hw->plane_faulted)
        ok = amdgpu_vram_alloc_locked(hw, size, align, cpu, mc);
    ReleaseSemaphore(&hw->pool_lock);
    return ok;
}

void amdgpu_vram_free(struct HWData *hw, UQUAD mc, ULONG size)
{
    if (!hw || !hw->pool_lock_ready || !size) return;
    ObtainSemaphore(&hw->pool_lock);
    if (!hw->plane_faulted && mc >= hw->vram_pool_mc &&
        mc - hw->vram_pool_mc < hw->vram_pool_size &&
        size <= (UQUAD)hw->vram_pool_size - (mc - hw->vram_pool_mc)) {
        if (amdgpu_vram_overlaps(mc, size, hw->scanout_active_mc, hw->scanout_active_size)) {
            /* OOP object disposal does not retire a hardware scanout. Keep the
             * block and remember its deferred free until a later verified flip. */
            if (!hw->scanout_release_size) {
                hw->scanout_release_mc = mc;
                hw->scanout_release_size = size;
            } else if (hw->scanout_release_mc != mc || hw->scanout_release_size != size) {
                /* Overlapping distinct frees violate the allocation contract;
                 * quarantine instead of releasing possibly displayed memory. */
                hw->plane_faulted = TRUE;
            }
        } else {
            amdgpu_vram_free_locked(hw, mc, size);
        }
    }
    ReleaseSemaphore(&hw->pool_lock);
}

void amdgpu_vram_retire_scanout_locked(struct HWData *hw, UQUAD mc, ULONG size)
{
    hw->scanout_active_mc = mc;
    hw->scanout_active_size = size;
    if (hw->scanout_release_size &&
        !amdgpu_vram_overlaps(mc, size, hw->scanout_release_mc, hw->scanout_release_size)) {
        amdgpu_vram_free_locked(hw, hw->scanout_release_mc, hw->scanout_release_size);
        hw->scanout_release_mc = 0;
        hw->scanout_release_size = 0;
    }
}

void amdgpuGfxVRAMHandoff(struct AmdgpuGfx_staticdata *xsd)
{
    struct AmdgpuDevice *ad = xsd->device;
    struct HWData *hw = &xsd->data;
    if (!hw->pool_lock_ready) {
        InitSemaphore(&hw->pool_lock);
        InitSemaphore(&hw->display_lock);
        hw->pool_lock_ready = TRUE;
    }
    UQUAD cur_mc = 0, owned_mc;
    ULONG w = 0, h = 0, gap, map_size, pool_size;
    BOOL master = FALSE;
    APTR aper_cpu, owned_cpu;
    volatile ULONG *probe;

    if (!ad || !ad->pciDriver) {
        bug("[AmdgpuGfx] handoff: no bound device -> GOP-adopt display\n");
        return;
    }
    if (!hw->framebuffer || !hw->fbsize)
        return;
    if (!amdgpu_dcn_get_scanout(ad, &cur_mc, &w, &h, &master)) {
        bug("[AmdgpuGfx] handoff: DCN scanout read failed -> GOP-adopt\n");
        return;
    }
    if (!master || cur_mc == 0) {
        bug("[AmdgpuGfx] handoff: OTG not driving a surface (mc=0x%08x%08x master=%d) -> GOP-adopt\n",
            (ULONG)(cur_mc >> 32), (ULONG)cur_mc, (int)master);
        return;
    }

    /*
     * The DCN scanout (MC cur_mc) and the GOP fb (CPU hw->framebuffer) are the
     * SAME surface: firmware CPU-mapped the scanned-out VRAM at hw->framebuffer
     * through the large GOP/BAR0 VRAM aperture (0x70_0000_0000 on the 9060 XT).
     * That is the aperture an owned fb must live in so CPU-offset and MC-offset
     * stay in lockstep. The PCI probe mis-picked the small 256 MB BAR (its 16 GB
     * BAR0 size overflowed a 32-bit read and was skipped); that BAR maps a
     * DIFFERENT VRAM region, which is what produced garbage on the first cut.
     * So map from the *fb* base (the proven aperture), keep every offset relative
     * to the live fb (no MC-base guess), and probe-write the owned region before
     * selecting an AMD screen. hw->framebuffer is the fb's physical BAR address on
     * x86_64 (identity-mapped >4 GB fb), so MapPCI extends the same aperture.
     */
    /*
     * Stage 3 (native): the OTG is ALREADY generating the panel's native raster
     * (POST/link-training set the timing + pixel clock — the survey shows OTG0 at
     * 1920x1080 while the GOP fb we adopted is only 1024x768). So there is no clock
     * work to do: prepare the AROS fb for the OTG's active area. The live HUBP
     * viewport stays untouched until switch_mode selects the new screen. Decide
     * the geometry here, BEFORE AddDisplayDriver registers the mode from hw->width/
     * height, so Intuition opens a native-sized screen. Any uncertainty falls back
     * to the working GOP geometry.
     */
    {
        struct AmdgpuDisplayMode otg;
        ULONG bpp = hw->bytesperpixel ? hw->bytesperpixel : 4;
        ULONG tgt_w = hw->width, tgt_h = hw->height, tgt_fbsize = hw->fbsize;
        BOOL adopt_native = FALSE;

        D(bug("[AmdgpuGfx] handoff: GOP-adopt scanout %ux%u (HUBP viewport)\n", w, h));

        if (amdgpu_modeset_read_timing(ad, 0, &otg) &&
            otg.w >= hw->width && otg.h >= hw->height &&
            otg.w <= 4096 && otg.h <= 2160 &&
            (otg.w != hw->width || otg.h != hw->height)) {
            tgt_w      = otg.w;
            tgt_h      = otg.h;
            tgt_fbsize = tgt_h * (tgt_w * bpp);
            adopt_native = TRUE;
            bug("[AmdgpuGfx] handoff: OTG native %ux%u > GOP %ux%u -> adopt native (no clock change)\n",
                otg.w, otg.h, hw->width, hw->height);
        }

        gap      = (tgt_fbsize + 0xFFFFFUL) & ~0xFFFFFUL;  /* owned fb, 1MB-aligned past the fb */
        /* + AUX reserve: the cursor surface lives past the visible fb and is
         * written by the CPU, so it must be INSIDE this mapping. Without it the
         * mapping ended exactly at the fb and every cursor write was discarded. */
        /*
         * Map a VRAM POOL past the owned fb and the cursor aux area, so
         * displayable bitmaps can be real VRAM surfaces (amdgpu_vram_alloc()).
         * Without it every bitmap stays in system RAM and has to be copied into
         * the scanout on every refresh.
         *
         * Sized for a handful of full-screen surfaces (1920x1080x4 is ~8 MB):
         * Intuition's screens plus the compositor's own composited bitmap. Kept
         * modest on purpose -- this is a BAR mapping, and page tables for it are
         * not free (see MMU_SPLIT_PTE_PAGE_COUNT).
         *
         * If the larger mapping fails we retry at the original size and simply
         * run without a pool: bitmaps fall back to system RAM, which is exactly
         * the behaviour that worked before. A pool is an upgrade, never a
         * prerequisite for booting.
         */
        pool_size = AMDGPU_VRAM_POOL_SIZE;
        /* Include the same rounded pool offset used below. Without rounding,
         * a native1080p pool extended94208 bytes past the requested mapping. */
        map_size  = gap + ((tgt_fbsize + AMDGPU_FB_AUX_RESERVE + 0xFFFFFUL) & ~0xFFFFFUL);

        aper_cpu = HIDD_PCIDriver_MapPCI(ad->pciDriver, (APTR)hw->framebuffer,
                                         map_size + pool_size);
        if (aper_cpu && aper_cpu != (APTR)-1) {
            map_size += pool_size;
        } else {
            bug("[AmdgpuGfx] handoff: %lu MB VRAM pool mapping refused;"
                " retrying without a pool (bitmaps stay in system RAM)\n",
                (unsigned long)(pool_size >> 20));
            pool_size = 0;
            aper_cpu  = HIDD_PCIDriver_MapPCI(ad->pciDriver, (APTR)hw->framebuffer, map_size);
        }

        /*
         * Re-map the aperture WRITE-COMBINING.
         *
         * MapPCI is generic and asks for MAP_CacheInhibit, which is correct for
         * a register aperture and catastrophic for a framebuffer: measured
         * 7115 KiB/s, one bus transaction per store. WC batches stores into
         * bursts. The request belongs HERE and not in MapPCI, because MapPCI
         * serves every PCI driver and a blanket change there would silently
         * retype every MMIO register aperture in the system.
         *
         * Best-effort: if KrnMapGlobal fails or the kernel predates
         * MAP_WriteCombine, the CacheInhibit mapping MapPCI already made stays
         * in place and the driver is merely slow, not broken.
         */
        if (aper_cpu && aper_cpu != (APTR)-1) {
            struct Library *KernelBase = OpenResource((STRPTR)"kernel.resource");

            if (KernelBase) {
                const IPTR big = 0x200000UL;
                IPTR wc_off = (IPTR)aper_cpu & (big - 1);
                int wcres = -1;

                if ((((IPTR)hw->framebuffer) & (big - 1)) == wc_off)
                    wcres = KrnMapGlobal((APTR)((IPTR)aper_cpu - wc_off),
                                         (APTR)((IPTR)hw->framebuffer - wc_off),
                                         (map_size + wc_off + big - 1) & ~(big - 1),
                                         MAP_Readable | MAP_Writable | MAP_WriteCombine);
                bug("[AmdgpuGfx] handoff: fb aperture write-combining %s "
                    "(KrnMapGlobal -> %d); fbperf CPU-via-BAR is the check\n",
                    wcres ? "REQUEST FAILED — staying uncached" : "enabled", wcres);
            }
        }
        if (!aper_cpu || aper_cpu == (APTR)-1) {
            bug("[AmdgpuGfx] handoff: fb-aperture MapPCI(0x%p, 0x%x) failed -> GOP-adopt\n",
                hw->framebuffer, map_size);
            return;
        }

        owned_cpu = (APTR)((IPTR)aper_cpu + gap);
        owned_mc  = cur_mc + gap;

        /* Prove the owned region is writable VRAM in the same aperture before the
         * flip — guards against MapPCI landing somewhere unexpected. */
        probe = (volatile ULONG *)owned_cpu;
        *probe = 0xA5A5F00DUL;
        if (*probe != 0xA5A5F00DUL) {
            bug("[AmdgpuGfx] handoff: owned VRAM not writable (cpu 0x%p) -> GOP-adopt\n", owned_cpu);
            return;
        }

        if (adopt_native) {
            /* Only prepare private storage. Reinterpreting the live GOP surface
             * with a new pitch here would corrupt the still-active VESA screen.
             * switch_mode validates/locks/programs the selected plane later. */
            ULONG i, words = tgt_fbsize / 4;
            volatile ULONG *p = (volatile ULONG *)owned_cpu;
            for (i = 0; i < words; i++)
                p[i] = 0;
            hw->width        = tgt_w;
            hw->height       = tgt_h;
            hw->bytesperline = tgt_w * bpp;
            hw->fbsize       = tgt_fbsize;
        }

        if (!adopt_native) {
            /* Same geometry: seed the owned fb with the LIVE on-screen content
             * (the firmware fb still holds the boot text) for a seamless flip. */
            CopyMem(aper_cpu, owned_cpu, hw->fbsize);
        }

        hw->framebuffer = owned_cpu;                 /* refresh now targets owned VRAM fb */
        hw->fb_mc       = owned_mc;                  /* ...and SDMA can reach it */
        hw->fb_width    = hw->width;
        hw->fb_height   = hw->height;

        /* Pool starts past the owned fb and its cursor aux area. */
        if (pool_size) {
            ULONG pool_off = (hw->fbsize + AMDGPU_FB_AUX_RESERVE + 0xFFFFFUL) & ~0xFFFFFUL;

            hw->vram_pool_cpu  = (APTR)((IPTR)owned_cpu + pool_off);
            hw->vram_pool_mc   = owned_mc + pool_off;
            hw->vram_pool_size = pool_size;
            hw->vram_pool_used = 0;
            bug("[AmdgpuGfx] handoff: VRAM pool %lu MB at cpu 0x%p mc 0x%08x%08x "
                "allocator=interior-coalesce-v1\n",
                (unsigned long)(pool_size >> 20), hw->vram_pool_cpu,
                (ULONG)(hw->vram_pool_mc >> 32), (ULONG)hw->vram_pool_mc);
        }
        hw->ad          = ad;

        bug("[AmdgpuGfx] VRAM PREPARED: existing scanout MC 0x%08x%08x; owned MC 0x%08x%08x "
            "(owned cpu 0x%p, gap 0x%x, %ux%u %s); plane/cursor changes deferred until AMD screen selection\n",
            (ULONG)(cur_mc >> 32), (ULONG)cur_mc, (ULONG)(owned_mc >> 32), (ULONG)owned_mc,
            owned_cpu, gap, hw->width, hw->height, adopt_native ? "NATIVE" : "GOP");
    }

    /* The advertised display contract uses CursorFB software cursors. Do not
     * enable the diagnostic white-block hardware sprite over another driver. */
}

static BOOL amdgpu_framebuffer_valid(const struct HWData *hw)
{
    return hw && !hw->plane_faulted && hw->framebuffer && hw->fb_width && hw->fb_height &&
        hw->bytesperpixel >= 1 && hw->bytesperpixel <= 4 &&
        (UQUAD)hw->fb_width * hw->bytesperpixel <= hw->bytesperline &&
        (UQUAD)hw->fb_height * hw->bytesperline <= hw->fbsize;
}

/* Copy the (possibly partial) bitmap buffer to the visible framebuffer. */
void amdgpuGfxDoRefreshArea(struct HWData *hwdata, struct AmdgpuGfxBitMapData *data,
                      LONG x1, LONG y1, LONG x2, LONG y2)
{
    UBYTE *src, *dst;
    ULONG srcmod, dstmod;
    LONG y, w, h, sx, sy;
    QUAD left, top, right, bottom, limit;

    if (!amdgpu_framebuffer_valid(hwdata) || !data || !data->VideoData ||
        !data->width || !data->height || !data->bytesperpix ||
        data->bytesperpix > 4 ||
        (UQUAD)data->width * data->bytesperpix > data->bytesperline ||
        (UQUAD)data->height * data->bytesperline > (UQUAD)~(ULONG)0 ||
        x2 <= x1 || y2 <= y1)
        return;

    /* Direct scanout is already current. Test backing identity BEFORE adding
     * screen offsets: a shifted source/destination pair in this same buffer
     * is not a refresh, it destroys the panned bitmap (cold22/cold23). */
    if (data->VideoData == hwdata->framebuffer &&
        data->bytesperline == hwdata->bytesperline &&
        data->bytesperpix == hwdata->bytesperpixel)
        return;

    /* Clip in wide coordinates before making either pointer. Published raster
     * dimensions may exceed the selected framebuffer, and a scrolled source
     * also has its own storage boundary. Neither is permission to copy outside
     * the other. This validates advertised extents, not object lifetime. */
    left = (QUAD)x1 + data->xoffset;
    top = (QUAD)y1 + data->yoffset;
    right = (QUAD)x2 + data->xoffset;
    bottom = (QUAD)y2 + data->yoffset;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (left < data->xoffset) left = data->xoffset;
    if (top < data->yoffset) top = data->yoffset;
    if (right > hwdata->fb_width) right = hwdata->fb_width;
    if (bottom > hwdata->fb_height) bottom = hwdata->fb_height;
    if (right > data->disp_width) right = data->disp_width;
    if (bottom > data->disp_height) bottom = data->disp_height;
    limit = (QUAD)data->xoffset + data->width;
    if (right > limit) right = limit;
    limit = (QUAD)data->yoffset + data->height;
    if (bottom > limit) bottom = limit;
    if (right <= left || bottom <= top ||
        right > 0x7fffffff || bottom > 0x7fffffff ||
        right - left > 0x7fffffff / data->bytesperpix)
        return;
    x1 = (LONG)left; y1 = (LONG)top;
    x2 = (LONG)right; y2 = (LONG)bottom;

    w = x2 - x1;
    h = y2 - y1;
    if (left - data->xoffset > 0x7fffffff ||
        top - data->yoffset > 0x7fffffff)
        return;
    sx = (LONG)(left - data->xoffset);
    sy = (LONG)(top - data->yoffset);
    w *= data->bytesperpix;

    srcmod = data->bytesperline;
    dstmod = hwdata->bytesperline;
    src = data->VideoData + (UQUAD)sy * srcmod + (UQUAD)sx * data->bytesperpix;
    dst = (UBYTE *)hwdata->framebuffer + (UQUAD)y1 * dstmod +
        (UQUAD)x1 * hwdata->bytesperpixel;

    if (src == dst && srcmod == dstmod &&
        data->bytesperpix == hwdata->bytesperpixel)
        return; /* direct scanout already contains these pixels; no self-DMA */

    /*
     * DEPTH CONVERSION. The driver now publishes a 16-bit pixel format as well
     * as the native one, so a bitmap can be RGB565 while the scanout stays
     * 32-bit. Every path below this point is a raw byte copy -- it would move
     * width*2 source bytes into a 32-bit destination and produce garbage, which
     * is worse than not offering the depth at all.
     *
     * Convert here and return. Deliberately CPU-side and unaccelerated: SDMA
     * COPY cannot reformat pixels, so an accelerated version needs either a
     * format-converting blit or a real modeset that makes the scanout 16-bit.
     * Correctness first; this path only runs on non-native screenmodes.
     */
    if (data->bytesperpix != hwdata->bytesperpixel)
    {
        ULONG px = w / (data->bytesperpix ? data->bytesperpix : 1);
        LONG  yy;

        /* A refresh must not reprogram the live plane or replace its stride.
         * Real depth selection belongs to the locked compositor modeset, with
         * format, pitch, surface address and buffer lifetime committed together.
         * Keep this converter only for a bitmap whose format differs from the
         * currently selected scanout; do not silently perform a partial modeset.
         * See ME_DISPATCH_STATUS.md, refresh-format isolation.
         */
        if (data->bytesperpix == 2 && hwdata->bytesperpixel == 4)
        {
            for (yy = 0; yy < h; yy++)
            {
                const UWORD *sp = (const UWORD *)(src + (ULONG)yy * srcmod);
                ULONG       *dp = (ULONG *)(dst + (ULONG)yy * dstmod);
                ULONG        xx;

                for (xx = 0; xx < px; xx++)
                {
                    UWORD v = sp[xx];
                    /* RGB565 -> 0x00RRGGBB, replicating the high bits into the
                     * low ones so full-scale input maps to full-scale output
                     * (0x1f -> 0xff, not 0xf8). */
                    ULONG r = (ULONG)((v >> 11) & 0x1f);
                    ULONG g = (ULONG)((v >>  5) & 0x3f);
                    ULONG b = (ULONG)( v        & 0x1f);

                    dp[xx] = (((r << 3) | (r >> 2)) << 16)
                           | (((g << 2) | (g >> 4)) <<  8)
                           |  ((b << 3) | (b >> 2));
                }
            }
            return;
        }

        {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                bug("[AmdgpuGfx] refresh: unsupported depth conversion %lu -> %lu bytes/pixel"
                    " — screen will be wrong; add a converter or do not publish that format\n",
                    (unsigned long)data->bytesperpix,
                    (unsigned long)hwdata->bytesperpixel);
            }
            return;     /* refuse rather than scribble a mis-sized copy */
        }
    }

    /*
     * Refresh-rate instrumentation. The SDMA path is confirmed active (no
     * fallback warning, bitmap GART-mapped once) yet the desktop still feels
     * slow, so the assumption that the RAM->VRAM transfer dominates is wrong and
     * needs testing rather than defending.
     *
     * Every bug() line already carries a high-resolution timestamp, so logging
     * one line per N refreshes gives the refresh rate and the rect sizes for
     * free — no timer.device, no allocation, nothing that can perturb what is
     * being measured. If the deltas show refreshes are rare, the cost is in
     * AROS's software rendering into the bitmap and not in this function at all.
     */
    {
        static ULONG nref = 0, rows_acc = 0, full_acc = 0;

        nref++;
        rows_acc += (ULONG)h;
        if (h >= 1000)
            full_acc++;
        if ((nref & 31) == 0)
            bug("[AmdgpuGfx] refresh stats: n=%lu rows=%lu fullscreen=%lu last=%ldx%ld\n",
                (unsigned long)nref, (unsigned long)rows_acc,
                (unsigned long)full_acc, (long)(w / (data->bytesperpix ? data->bytesperpix : 4)),
                (long)h);
    }

    /* One-shot instrumentation: does Intuition actually refresh amdgpu's bitmap
     * after the VRAM handoff, and to which framebuffer? (owned VRAM vs the old
     * GOP fb). Logs the first few refreshes then goes quiet. */
    {
        static int nlog = 0;
        if (nlog < 5) {
            nlog++;
            bug("[AmdgpuGfx] refresh #%d dst=0x%p (hwdata->framebuffer=0x%p) src=0x%p rect %ld,%ld..%ld,%ld\n",
                nlog, dst, hwdata->framebuffer, data->VideoData,
                (long)x1, (long)y1, (long)x2, (long)y2);
        }
    }

    /*
     * SDMA fast path. The destination here is the SCANOUT, which lives behind a
     * PCI BAR that is mapped UNCACHED (core_InitMMU builds every identity PDE
     * with pwt=0/pcd=0/pat=0; AROS programs neither IA32_PAT nor MTRRs, and the
     * default PAT has no write-combining entry). Measured on this DUT, sysmem to
     * VRAM at 256 KiB: CPU through the BAR 5146 KiB/s versus SDMA 3232318 KiB/s
     * — 628x. At 5 MB/s a full 1920x1080x32 update is ~1.6 seconds, which is
     * the slowness this is here to remove.
     *
     * Chunked to the ring budget (64 scanlines per submission). Falls back to
     * the CPU loop on ANY failure — a slow screen is bad, a screen that stops
     * updating is worse, so this is never allowed to be fatal.
     */
    /*
     * SIZE GATE — measured, not assumed.
     *
     * Refresh-rate instrumentation showed the cost is PER SUBMISSION, not per
     * byte. Two consecutive 32-refresh windows:
     *     n=64..96    3492 rows copied   delta 1.167e8 ticks
     *     n=96..128    221 rows copied   delta 1.058e8 ticks
     * 16x fewer rows, essentially the same wall time. And the rects the desktop
     * actually issues are tiny — 16x16, 5x1, 1x3, 12x1 — with only ~15% of
     * refreshes full-screen.
     *
     * So an SDMA submission (ring re-arm, packet upload through the UNCACHED
     * VRAM aperture, fence round trip) costs the same whether it moves 12 bytes
     * or 8 MB. Putting one behind every 1x3 rect made small refreshes far slower
     * than the CPU path they replaced, which is why the desktop still felt slow
     * after the "628x faster" change.
     *
     * Both earlier results were right about their own case: SDMA wins hugely on
     * bulk (the CPU crawls at 5 MB/s through the uncached BAR) and loses badly
     * on tiny rects. The desktop is mostly tiny rects.
     *
     * Threshold: CPU-via-BAR runs at ~5 MB/s, so it clears ~2 KB in the time one
     * SDMA submission costs. 8 KB is a deliberately conservative crossover —
     * comfortably above the noise, and still routing every full-screen and
     * large-window update to the engine. The remaining fixed cost is worth
     * attacking separately (the ring lives in VRAM, so packet upload is itself
     * uncached MMIO), and lowering it should lower this threshold.
     */
    if (hwdata->ad && hwdata->fb_mc && data->VideoData &&
        ((ULONG)w * (ULONG)h) >= 8192)
    {
        ULONG total = data->bytesperline * data->height;
        ULONG srcoff = (ULONG)((UQUAD)sy * srcmod + (UQUAD)sx * data->bytesperpix);
        ULONG dstoff = (ULONG)((UQUAD)y1 * dstmod + (UQUAD)x1 * hwdata->bytesperpixel);
        LONG  done = 0;
        BOOL  ok = TRUE;

        while (done < h && ok) {
            LONG chunk = h - done;

            if (chunk > 64) chunk = 64;
            ok = amdgpu_sdma_blit_rect(hwdata->ad, &data->gart_va, &data->gart_size,
                                       data->VideoData, total,
                                       srcmod, srcoff + (ULONG)done * srcmod,
                                       hwdata->fb_mc,
                                       dstmod, dstoff + (ULONG)done * dstmod,
                                       (ULONG)w, (ULONG)chunk);
            done += chunk;
        }
        if (ok)
            return;

        {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                bug("[AmdgpuGfx] SDMA refresh unavailable — falling back to CPU blits "
                    "through the uncached BAR (expect ~5 MB/s)\n");
            }
        }
    }

    if ((srcmod != dstmod) || (srcmod != (ULONG)w))
    {
        for (y = 0; y < h; y++)
        {
            CopyMem(src, dst, w);
            src += srcmod;
            dst += dstmod;
        }
    }
    else
    {
        CopyMem(src, dst, (ULONG)w * (ULONG)h);
    }
}

/* The GOP framebuffer is truecolor: no hardware palette to load. */
void DACLoad(struct AmdgpuGfx_staticdata *xsd, UBYTE *DAC, unsigned char first, int num)
{
    (void)xsd; (void)DAC; (void)first; (void)num;
}

void ClearBuffer(struct HWData *data)
{
    if (!amdgpu_framebuffer_valid(data))
        return;

    /*
     * SetMem across the framebuffer is a CPU memset through the UNCACHED BAR.
     * Measured: CPU-via-BAR runs at 7115 KiB/s, so clearing 1920x1080x4 costs
     * ~1.2 SECONDS of blocking work every time a screen is cleared. SDMA moves
     * the same bytes at 3223789 KiB/s.
     *
     * Fill in <=64-row chunks because the ring budget is one pass per submit
     * (5 dwords per row), matching how the scanout blit chunks.
     */
    if (data->ad && data->fb_mc) {
        ULONG rows_done = 0, ok = 1;

        while (rows_done < data->fb_height && ok) {
            ULONG chunk = data->fb_height - rows_done;
            if (chunk > 64) chunk = 64;
            ok = amdgpu_sdma_fill_rect(data->ad, data->fb_mc,
                                       data->bytesperline,
                                       rows_done * data->bytesperline,
                                       data->fb_width * data->bytesperpixel,
                                       chunk, 0);
            rows_done += chunk;
        }
        if (ok)
            return;

        {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                bug("[AmdgpuGfx] SDMA clear unavailable — falling back to CPU memset "
                    "through the uncached BAR (~1.2s for a 1080p screen)\n");
            }
        }
    }

    SetMem(data->framebuffer, 0, data->fb_height * data->bytesperline);
}
