/*
 * FbPattern — paint a test pattern into the ACTIVE scanout and see if it lands.
 *
 * FbProbe derives where the display is fetching from and can read it back, but
 * reading 0x00eeeeee there is only CONSISTENT with the desktop grey -- any grey
 * buffer reads the same. Writing is the conclusive test:
 *
 *   pattern APPEARS on the panel  -> that address IS the live framebuffer, and
 *                                    the CPU<->MC derivation is proven end to end.
 *   pattern does NOT appear       -> we are looking at the wrong buffer, and every
 *                                    conclusion built on "I wrote to the
 *                                    framebuffer" has to be thrown out.
 *
 * The address is DERIVED from hardware every run, never hardcoded:
 *   MMHUB FB_LOC_BASE            -> where VRAM starts in MC space
 *   HUBP PRIMARY_SURFACE_ADDRESS -> what the display is actually fetching
 *   ad->vram_bar_phys            -> the CPU aperture over that VRAM
 *   CPU = vram_bar_phys + (scanout_mc - vram_start)
 *
 *   FbPattern              # 8 colour bars + a magenta box, into the active fb
 *   FbPattern AT/K/N       # paint at a VRAM OFFSET instead of the scanout, e.g.
 *                          # AT=0x7f0000 for the cursor surface. This is an
 *                          # OFFSET from the start of VRAM, not an absolute MC
 *                          # address: ReadArgs /N yields a 32-bit LONG and MC
 *                          # addresses are 64-bit (0x80_00ff0000), so an
 *                          # absolute value would silently truncate.
 *   FbPattern CLEAR/S      # repaint desktop grey instead of the pattern
 *   FbPattern SHOW/S       # with AT=, briefly POINT THE SCANOUT at that address
 *                          # (~10s) then restore it.
 *
 * SHOW answers the one thing writing alone cannot: whether the display ENGINE
 * can fetch from a region, as opposed to whether the CPU can write to it. The
 * cursor surface sits inside VRAM but OUTSIDE the visible framebuffer, and it
 * has only ever been proven CPU-writable. Painting bars there and then pointing
 * the scanout at it makes the engine read it:
 *   bars appear -> the engine fetches that region fine; cursor addressing is
 *                  cleared and the fault is in cursor compositing itself.
 *   black/frozen -> the engine cannot fetch there, which is exactly why a
 *                  perfectly-programmed cursor draws nothing.
 * The original address is restored automatically, so a failed test costs a few
 * seconds of scrambled screen rather than a reboot.
 *
 * The pattern is deliberately unmistakable -- saturated primaries in vertical
 * bars, nothing the desktop would ever draw -- so "did it appear?" needs no
 * interpretation. Source width and pitch come from HUBP.  DSCL RECOUT is the
 * output size and is not a valid stride when firmware is scaling a smaller GOP
 * surface (for example 1024x768 -> 1920x1080).
 */
#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/oop.h>
#include <hidd/pci.h>

#include <string.h>

#include "amdgpu_intern.h"

#define PLOG(...)  do { bug("[FbPattern] " __VA_ARGS__); bug("\n"); } while (0)
#define SHELL(...) do { Printf((CONST_STRPTR)"[FbPattern] " __VA_ARGS__); \
                        Printf((CONST_STRPTR)"\n"); } while (0)

struct AmdgpuBase amdgpuBase;

/* dmu_seg[2]-relative, pipe 0 */
#define R_HUBP_SURF         0x060a
#define R_HUBP_SURF_HIGH    0x060b
#define R_HUBP_VIEWPORT_DIM 0x05eb
#define R_HUBP_SURF_PITCH   0x0607
#define R_DSCL_RECOUT_SIZE  0x0d1f
#define R_OTG_CONTROL       0x1b43
/* MMHUB, relative to ad->ip.mmhub.base */
#define MMHUB_FB_LOC_BASE   0x0554

static ULONG rd(struct AmdgpuDevice *ad, ULONG off)
{
    return amdgpu_rreg32(ad, ad->dmu_seg[2] + off);
}

int main(void)
{
    struct AmdgpuDevice *ad;
    struct RDArgs *rd_args;
    IPTR args[3] = { 0, 0, 0 };
    UQUAD at_mc = 0;
    BOOL  clear = FALSE, show = FALSE;

    rd_args = ReadArgs((CONST_STRPTR)"AT/K/N,CLEAR/S,SHOW/S", args, NULL);
    if (rd_args) {
        if (args[0]) at_mc = (UQUAD)(ULONG)*(LONG *)args[0];
        clear = args[1] ? TRUE : FALSE;
        show  = args[2] ? TRUE : FALSE;
        FreeArgs(rd_args);
    }

    PLOG("=== framebuffer write test ===");
    SHELL("=== framebuffer write test ===");

    amdgpuBase.pciDeviceAB = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDevice);
    amdgpuBase.pciDriverAB = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDriver);
    if (!amdgpuBase.pciDeviceAB) { SHELL("FAIL: no PCI hidd"); return 20; }
    if (!amdgpu_pci_probe(&amdgpuBase)) { SHELL("FAIL: no AMD GPU"); goto out; }

    ForeachNode(&amdgpuBase.devices, ad) {
        ULONG fb_base, w, pitch, pitch_pixels, output_w, viewport, otg;
        UQUAD vram_start, scan, off, cpu;
        ULONG salo, sahi;

        if (!amdgpu_mmio_map(&amdgpuBase, ad)) { SHELL("FAIL: mmio map"); continue; }
        ad->vram_mb = amdgpu_get_vram_mb(ad);
        amdgpu_discovery_init(ad, ad->vram_mb);
        if (!ad->dmu_seg[2] || !ad->ip.mmhub.valid) { SHELL("FAIL: no DMU/MMHUB"); continue; }

        fb_base    = amdgpu_rreg32(ad, ad->ip.mmhub.base + MMHUB_FB_LOC_BASE);
        vram_start = ((UQUAD)(fb_base & 0x00FFFFFFUL)) << 24;

        salo = rd(ad, R_HUBP_SURF);
        sahi = rd(ad, R_HUBP_SURF_HIGH);
        otg  = rd(ad, R_OTG_CONTROL);
        scan = ((UQUAD)sahi << 32) | (UQUAD)salo;

        /* AT= overrides the target so an arbitrary region (e.g. the cursor
         * surface at a DIFFERENT address than the scanout) can be tested for
         * reachability, but the scanout is still reported for comparison. */
        if (at_mc) {
            /* AT is an offset from VRAM start, not an absolute MC address --
             * see the header. Rebuild the absolute address here so the rest of
             * the code (and SHOW) works in MC terms as before. */
            UQUAD abs_mc = vram_start + at_mc;
            PLOG("scanout is MC 0x%08lx%08lx; painting at VRAM offset 0x%08lx"
                 " = MC 0x%08lx%08lx instead",
                 (unsigned long)(scan >> 32), (unsigned long)(scan & 0xFFFFFFFFUL),
                 (unsigned long)at_mc,
                 (unsigned long)(abs_mc >> 32), (unsigned long)(abs_mc & 0xFFFFFFFFUL));
            scan = abs_mc;
        }

        if (!(otg & 1UL))
            PLOG("WARNING: pipe0 OTG_MASTER_EN=0 — this pipe is not driving a panel");

        if (scan < vram_start) { SHELL("FAIL: target below the VRAM window"); continue; }
        off = scan - vram_start;
        cpu = (UQUAD)ad->vram_bar_phys + off;

        viewport    = rd(ad, R_HUBP_VIEWPORT_DIM);
        w           = viewport & 0xFFFFUL;
        pitch_pixels = (rd(ad, R_HUBP_SURF_PITCH) & 0xFFFFUL) + 1;
        output_w    = rd(ad, R_DSCL_RECOUT_SIZE) & 0xFFFFUL;
        pitch       = pitch_pixels * 4;
        if (!w || !pitch_pixels || pitch_pixels < w) {
            SHELL("FAIL: invalid HUBP source width/pitch — refusing to guess");
            continue;
        }

        PLOG("VRAM starts MC 0x%08lx%08lx | BAR cpu 0x%08lx%08lx size %lu MB",
             (unsigned long)(vram_start >> 32), (unsigned long)(vram_start & 0xFFFFFFFFUL),
             (unsigned long)((UQUAD)ad->vram_bar_phys >> 32),
             (unsigned long)((UQUAD)ad->vram_bar_phys & 0xFFFFFFFFUL),
             (unsigned long)((UQUAD)ad->vram_bar_size >> 20));
        PLOG("target MC 0x%08lx%08lx -> VRAM off 0x%08lx%08lx -> CPU 0x%08lx%08lx (source w %lu, pitch %lu px/%lu bytes, output w %lu)",
             (unsigned long)(scan >> 32), (unsigned long)(scan & 0xFFFFFFFFUL),
             (unsigned long)(off >> 32), (unsigned long)(off & 0xFFFFFFFFUL),
             (unsigned long)(cpu >> 32), (unsigned long)(cpu & 0xFFFFFFFFUL),
             (unsigned long)w, (unsigned long)pitch_pixels,
             (unsigned long)pitch, (unsigned long)output_w);

        {
            /* 8 saturated bars, 200 rows tall, starting 100 rows down. Nothing
             * the desktop draws looks like this, so the result needs no
             * interpretation. */
            static const ULONG bar[8] = {
                0x00FF0000UL, 0x0000FF00UL, 0x000000FFUL, 0x00FFFF00UL,
                0x00FF00FFUL, 0x0000FFFFUL, 0x00FFFFFFUL, 0x00000000UL
            };
            ULONG y0 = 100, rows = 200, yy, xx;
            ULONG bw = w / 8;
            UQUAD last = off + (UQUAD)(y0 + rows) * pitch;

            if (last > (UQUAD)ad->vram_bar_size) {
                PLOG("REFUSING: pattern would run past the BAR window (%lu MB)",
                     (unsigned long)((UQUAD)ad->vram_bar_size >> 20));
                SHELL("FAIL: pattern would fall outside the BAR");
                continue;
            }

            for (yy = 0; yy < rows; yy++) {
                volatile ULONG *row = (volatile ULONG *)(IPTR)(cpu + (UQUAD)(y0 + yy) * pitch);
                for (xx = 0; xx < w; xx++)
                    row[xx] = clear ? 0x00EEEEEEUL : bar[bw ? (xx / bw) & 7 : 0];
            }
            /* The framebuffer BAR is remapped WC by the installed driver.
             * A volatile store orders the compiler, not the processor's WC
             * buffers; drain them before judging what DCN can fetch. */
            __asm__ __volatile__("sfence" ::: "memory");

            /* Prove the writes reached the VRAM aperture before using the
             * panel as the discriminator.  A posted/WC write failure and a
             * DCN fetch failure look identical on HDMI, but not here. */
            {
                volatile ULONG *first = (volatile ULONG *)(IPTR)
                    (cpu + (UQUAD)y0 * pitch);
                volatile ULONG *middle = (volatile ULONG *)(IPTR)
                    (cpu + (UQUAD)(y0 + rows / 2) * pitch);
                volatile ULONG *last_row = (volatile ULONG *)(IPTR)
                    (cpu + (UQUAD)(y0 + rows - 1) * pitch);
                ULONG expect0 = clear ? 0x00EEEEEEUL : bar[0];
                ULONG expect4 = clear ? 0x00EEEEEEUL : bar[4];
                ULONG got0 = first[0];
                ULONG got4 = middle[bw * 4];
                ULONG got7 = last_row[w - 1];

                PLOG("VRAM readback first=%08lx middle-bar4=%08lx last=%08lx"
                     " expect=%08lx/%08lx/%08lx => %s",
                     (unsigned long)got0, (unsigned long)got4,
                     (unsigned long)got7, (unsigned long)expect0,
                     (unsigned long)expect4,
                     (unsigned long)(clear ? 0x00EEEEEEUL : bar[7]),
                     got0 == expect0 && got4 == expect4 &&
                     got7 == (clear ? 0x00EEEEEEUL : bar[7]) ?
                     "VRAM-WRITE-PASS" : "VRAM-WRITE-FAIL");
            }

            PLOG("*** %s %lu rows x %lu px at y=%lu through CPU 0x%08lx%08lx ***",
                 clear ? "CLEARED" : "PAINTED", (unsigned long)rows, (unsigned long)w,
                 (unsigned long)y0,
                 (unsigned long)(cpu >> 32), (unsigned long)(cpu & 0xFFFFFFFFUL));
            SHELL("%s 8 colour bars across y=100..300.", clear ? "CLEARED" : "PAINTED");
            SHELL("  VISIBLE  -> this IS the live framebuffer, derivation proven.");
            SHELL("  NOT seen -> wrong buffer; anything built on 'I wrote to the fb' is void.");

            if (show && at_mc) {
                /* Make the ENGINE read the region, not just the CPU. Restore
                 * unconditionally afterwards -- leaving the scanout pointed
                 * somewhere odd would cost a reboot to undo. */
                ULONG olo = salo, ohi = sahi;
                PLOG("SHOW: pointing scanout at 0x%08lx%08lx for ~10s (was %08lx:%08lx)",
                     (unsigned long)(at_mc >> 32), (unsigned long)(at_mc & 0xFFFFFFFFUL),
                     (unsigned long)ohi, (unsigned long)olo);
                SHELL("SHOW: scanout -> %08lx:%08lx for ~10s. Bars = the engine CAN fetch"
                      " there. Black/frozen = it CANNOT, and that is the cursor bug.",
                      (IPTR)(at_mc >> 32), (IPTR)(at_mc & 0xFFFFFFFFUL));

                amdgpu_wreg32(ad, ad->dmu_seg[2] + R_HUBP_SURF_HIGH,
                              (ULONG)(at_mc >> 32));
                amdgpu_wreg32(ad, ad->dmu_seg[2] + R_HUBP_SURF,
                              (ULONG)(at_mc & 0xFFFFFFFFUL));
                Delay(500);                     /* ~10s at 50 ticks/s */
                amdgpu_wreg32(ad, ad->dmu_seg[2] + R_HUBP_SURF_HIGH, ohi);
                amdgpu_wreg32(ad, ad->dmu_seg[2] + R_HUBP_SURF,      olo);

                PLOG("SHOW: scanout restored to %08lx:%08lx (readback %08lx:%08lx)",
                     (unsigned long)ohi, (unsigned long)olo,
                     (unsigned long)rd(ad, R_HUBP_SURF_HIGH),
                     (unsigned long)rd(ad, R_HUBP_SURF));
                SHELL("SHOW: scanout restored.");
            }
        }
    }

out:
    if (amdgpuBase.pciDeviceAB) OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDevice);
    if (amdgpuBase.pciDriverAB) OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDriver);
    return 0;
}
