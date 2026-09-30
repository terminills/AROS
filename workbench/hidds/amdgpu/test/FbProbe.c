/*
 * FbProbe — report the LIVE display state straight from DCN, for all four pipes.
 *
 * Written because "which framebuffer are we actually on?" could not be answered
 * from the driver's own logs: those prove the panel shows A buffer that receives
 * our writes, not WHICH VRAM address is being scanned out. This asks the
 * hardware instead of inferring, and it runs on demand -- push it with HttpGet
 * and run it from the netshell, seconds per question instead of a 35-minute
 * seed/deploy/boot cycle per hypothesis.
 *
 * For every pipe it reports the whole chain a pixel travels:
 *
 *   OTG      OTG_MASTER_EN            is this pipe driving a panel at all?
 *   HUBP     PRIMARY_SURFACE_ADDRESS  which VRAM address is being scanned out
 *   DSCL     SCL_MODE / RECOUT        the scaler, and on DCN4 the CURSOR's
 *                                     destination rect -- cursor composition
 *                                     moved AFTER the scaler this generation
 *   DPP      DPP_CLOCK_ENABLE         is the pixel pipe even clocked?
 *   CURSOR   CONTROL/POS/ADDR/SIZE    the cursor's own state
 *   CM_CUR0  CUR0_ENABLE / MODE       the DPP-side cursor enable
 *
 *   FbProbe            # survey every pipe
 *   FbProbe PIPE/N     # one pipe only
 *   FbProbe MARK/S     # WRITE a magenta square at (100,100) through the DERIVED
 *                      # CPU address of the active scanout.
 *
 * MARK is the only conclusive proof of the mapping. Reading the derived address
 * and getting 0x00eeeeee is merely CONSISTENT with the desktop grey -- any grey
 * buffer reads the same. If a magenta square appears on the panel, the derived
 * CPU address IS the memory being displayed, proven. If it does not, the
 * mapping is wrong no matter how plausible the arithmetic looked.
 *
 * Offsets verified against dcn_4_1_0_offset.h; all are BASE_IDX 2 (dmu_seg[2])
 * except OTG which is also BASE_IDX 2. Per-pipe strides measured from the
 * header, not assumed: HUBPREQ/CURSOR 0xDC, DSCL/DPP_TOP 0x16B, OTG 0x80.
 */
#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/oop.h>
#include <hidd/pci.h>

#include <string.h>

#include "amdgpu_intern.h"

#define PLOG(...)  do { bug("[FbProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define SHELL(...) do { Printf((CONST_STRPTR)"[FbProbe] " __VA_ARGS__); \
                        Printf((CONST_STRPTR)"\n"); } while (0)

/* amdgpu_pci_probe() populates this; standalone progs define it themselves. */
struct AmdgpuBase amdgpuBase;

/* pipe 0 offsets, dmu_seg[2]-relative */
#define R_OTG_CONTROL       0x1b43
#define R_HUBP_SURF         0x060a
#define R_HUBP_SURF_HIGH    0x060b
#define R_DSCL_SCL_MODE     0x0d08
#define R_DSCL_RECOUT_START 0x0d1e
#define R_DSCL_RECOUT_SIZE  0x0d1f
#define R_DPP_CONTROL       0x0cc5
#define R_CUR_CONTROL       0x0679
#define R_CUR_SURF          0x067a
#define R_CUR_SURF_HIGH     0x067b
#define R_CUR_SIZE          0x067c
#define R_CUR_POSITION      0x067d
#define R_CM_CUR0_CONTROL   0x0cf1

/* MMHUB, dword offsets within ad->ip.mmhub.base (BASE_IDX 0). FB base/top are
 * the MC address in 24-bit (16 MiB) units -- this is what DEFINES where VRAM
 * lives in MC space, so the CPU<->MC mapping can be DERIVED here instead of
 * assumed from the fact that the desktop happens to be visible. */
#define MMHUB_FB_LOC_BASE      0x0554
#define MMHUB_FB_LOC_TOP       0x0555
#define MMHUB_FB_OFFSET_R      0x04c7

/* per-pipe strides, measured from the header */
#define S_HUBP   0x0DC   /* HUBPREQ0 0x060a -> HUBPREQ1 0x06e6, CURSOR0_0/0_1 too */
#define S_DSCL   0x16B   /* DSCL0 0x0d08 -> DSCL1 0x0e73, DPP_TOP0/1 too */
#define S_OTG    0x080   /* OTG0 0x1b43 -> OTG1 0x1bc3 */

static ULONG rd(struct AmdgpuDevice *ad, ULONG off)
{
    return amdgpu_rreg32(ad, ad->dmu_seg[2] + off);
}

static void dump_pipe(struct AmdgpuDevice *ad, ULONG p)
{
    ULONG otg  = rd(ad, R_OTG_CONTROL       + p * S_OTG);
    ULONG salo = rd(ad, R_HUBP_SURF         + p * S_HUBP);
    ULONG sahi = rd(ad, R_HUBP_SURF_HIGH    + p * S_HUBP);
    ULONG scl  = rd(ad, R_DSCL_SCL_MODE     + p * S_DSCL);
    ULONG rcst = rd(ad, R_DSCL_RECOUT_START + p * S_DSCL);
    ULONG rcsz = rd(ad, R_DSCL_RECOUT_SIZE  + p * S_DSCL);
    ULONG dpp  = rd(ad, R_DPP_CONTROL       + p * S_DSCL);
    ULONG cctl = rd(ad, R_CUR_CONTROL       + p * S_HUBP);
    ULONG cslo = rd(ad, R_CUR_SURF          + p * S_HUBP);
    ULONG cshi = rd(ad, R_CUR_SURF_HIGH     + p * S_HUBP);
    ULONG csz  = rd(ad, R_CUR_SIZE          + p * S_HUBP);
    ULONG cpos = rd(ad, R_CUR_POSITION      + p * S_HUBP);
    ULONG cmc  = rd(ad, R_CM_CUR0_CONTROL   + p * S_DSCL);

    ULONG live = otg & 1UL;

    PLOG("pipe%lu %s", (unsigned long)p, live ? "LIVE (OTG_MASTER_EN=1)" : "dark");
    PLOG("  OTG_CONTROL   =%08lx master_en=%lu", (unsigned long)otg, (unsigned long)live);
    PLOG("  SCANOUT ADDR  =%08lx:%08lx  <-- the framebuffer this pipe displays",
         (unsigned long)sahi, (unsigned long)salo);
    PLOG("  DSCL SCL_MODE =%08lx  RECOUT start=(%lu,%lu) size=%lux%lu",
         (unsigned long)scl,
         (unsigned long)(rcst & 0xFFFFUL), (unsigned long)((rcst >> 16) & 0xFFFFUL),
         (unsigned long)(rcsz & 0xFFFFUL), (unsigned long)((rcsz >> 16) & 0xFFFFUL));
    PLOG("  DPP_CONTROL   =%08lx clk_en=%lu", (unsigned long)dpp,
         (unsigned long)((dpp >> 4) & 1UL));
    PLOG("  CURSOR CONTROL=%08lx en=%lu req_mode=%lu mode=%lu",
         (unsigned long)cctl, (unsigned long)(cctl & 1UL),
         (unsigned long)((cctl >> 2) & 1UL), (unsigned long)((cctl >> 8) & 7UL));
    PLOG("  CURSOR ADDR   =%08lx:%08lx size=%lux%lu pos=(%lu,%lu)",
         (unsigned long)cshi, (unsigned long)cslo,
         (unsigned long)((csz >> 16) & 0xFFFFUL), (unsigned long)(csz & 0xFFFFUL),
         (unsigned long)((cpos >> 15) & 0x7FFFUL), (unsigned long)(cpos & 0x7FFFUL));
    PLOG("  CM_CUR0       =%08lx cur0_en=%lu mode=%lu pending=%lu",
         (unsigned long)cmc, (unsigned long)(cmc & 1UL),
         (unsigned long)((cmc >> 4) & 7UL), (unsigned long)((cmc >> 16) & 1UL));

    /* Shell gets the one-line verdict; serial keeps the full detail. */
    if (live)
        SHELL("pipe%lu LIVE  fb=%08lx:%08lx  recout=%lux%lu  dppclk=%lu  cur_en=%lu cur@(%lu,%lu) cur_fb=%08lx:%08lx",
              (IPTR)p, (IPTR)sahi, (IPTR)salo,
              (IPTR)(rcsz & 0xFFFFUL), (IPTR)((rcsz >> 16) & 0xFFFFUL),
              (IPTR)((dpp >> 4) & 1UL), (IPTR)(cctl & 1UL),
              (IPTR)((cpos >> 15) & 0x7FFFUL), (IPTR)(cpos & 0x7FFFUL),
              (IPTR)cshi, (IPTR)cslo);
    else
        SHELL("pipe%lu dark", (IPTR)p);
}

int main(void)
{
    struct AmdgpuDevice *ad;
    struct RDArgs *rd_args;
    IPTR args[2] = { 0, 0 };
    LONG only = -1;
    BOOL mark = FALSE;

    rd_args = ReadArgs((CONST_STRPTR)"PIPE/N,MARK/S", args, NULL);
    if (rd_args) {
        if (args[0]) only = (LONG)*(LONG *)args[0];
        mark = args[1] ? TRUE : FALSE;
        FreeArgs(rd_args);
    }

    PLOG("=== live display survey ===");
    SHELL("=== live display survey ===");

    amdgpuBase.pciDeviceAB = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDevice);
    amdgpuBase.pciDriverAB = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDriver);
    if (!amdgpuBase.pciDeviceAB) { SHELL("FAIL: no PCI hidd"); return 20; }

    if (!amdgpu_pci_probe(&amdgpuBase)) { SHELL("FAIL: no AMD GPU"); goto out; }

    ForeachNode(&amdgpuBase.devices, ad) {
        ULONG p;

        PLOG("device %04x:%04x", ad->vendorid, ad->deviceid);
        if (!amdgpu_mmio_map(&amdgpuBase, ad)) { SHELL("FAIL: mmio map"); continue; }
        ad->vram_mb = amdgpu_get_vram_mb(ad);
        amdgpu_discovery_init(ad, ad->vram_mb);

        if (!ad->dmu_seg[2]) {
            PLOG("no DMU segment — display block not discovered");
            SHELL("FAIL: no DMU segment");
            continue;
        }
        PLOG("dmu_seg[2]=0x%05lx vram=%luMB",
             (unsigned long)ad->dmu_seg[2], (unsigned long)ad->vram_mb);
        SHELL("%04x:%04x dmu_seg[2]=0x%05lx",
              (IPTR)ad->vendorid, (IPTR)ad->deviceid, (IPTR)ad->dmu_seg[2]);

        /*
         * THE question this probe exists for: which memory is actually being
         * scanned out, and does our CPU pointer address it? Everything else in
         * this driver infers that from "the desktop is visible", which only
         * proves the panel shows A buffer that receives our writes -- not WHICH
         * VRAM address. Derive it from the hardware instead.
         */
        if (ad->ip.mmhub.valid) {
            ULONG mb = ad->ip.mmhub.base;
            ULONG fb_base = amdgpu_rreg32(ad, mb + MMHUB_FB_LOC_BASE);
            ULONG fb_top  = amdgpu_rreg32(ad, mb + MMHUB_FB_LOC_TOP);
            ULONG fb_off  = amdgpu_rreg32(ad, mb + MMHUB_FB_OFFSET_R);
            UQUAD vram_start = ((UQUAD)(fb_base & 0x00FFFFFFUL)) << 24;
            UQUAD vram_end   = (((UQUAD)(fb_top & 0x00FFFFFFUL)) << 24) | 0x00FFFFFFUL;
            ULONG salo = rd(ad, R_HUBP_SURF);
            ULONG sahi = rd(ad, R_HUBP_SURF_HIGH);
            UQUAD scan = ((UQUAD)sahi << 32) | (UQUAD)salo;

            PLOG("VRAM MC window [0x%08lx%08lx .. 0x%08lx%08lx] fb_offset=0x%08lx",
                 (unsigned long)(vram_start >> 32), (unsigned long)(vram_start & 0xFFFFFFFFUL),
                 (unsigned long)(vram_end >> 32), (unsigned long)(vram_end & 0xFFFFFFFFUL),
                 (unsigned long)fb_off);
            PLOG("VRAM CPU aperture (BAR) phys=0x%08lx%08lx size=%lu MB",
                 (unsigned long)((UQUAD)ad->vram_bar_phys >> 32),
                 (unsigned long)((UQUAD)ad->vram_bar_phys & 0xFFFFFFFFUL),
                 (unsigned long)((UQUAD)ad->vram_bar_size >> 20));

            if (scan >= vram_start && scan <= vram_end) {
                UQUAD off = scan - vram_start;
                UQUAD cpu = (UQUAD)ad->vram_bar_phys + off;
                PLOG("*** ACTIVE SCANOUT: MC 0x%08lx%08lx = VRAM offset 0x%08lx%08lx"
                     " -> CPU 0x%08lx%08lx ***",
                     (unsigned long)(scan >> 32), (unsigned long)(scan & 0xFFFFFFFFUL),
                     (unsigned long)(off >> 32), (unsigned long)(off & 0xFFFFFFFFUL),
                     (unsigned long)(cpu >> 32), (unsigned long)(cpu & 0xFFFFFFFFUL));
                SHELL("ACTIVE FB: MC=%08lx:%08lx offset=%08lx:%08lx CPU=%08lx:%08lx",
                      (IPTR)(scan >> 32), (IPTR)(scan & 0xFFFFFFFFUL),
                      (IPTR)(off >> 32), (IPTR)(off & 0xFFFFFFFFUL),
                      (IPTR)(cpu >> 32), (IPTR)(cpu & 0xFFFFFFFFUL));
                if (off + 16 <= (UQUAD)ad->vram_bar_size) {
                    volatile ULONG *fp = (volatile ULONG *)(IPTR)cpu;
                    PLOG("    first dwords through the BAR at that CPU address:"
                         " %08lx %08lx %08lx %08lx",
                         (unsigned long)fp[0], (unsigned long)fp[1],
                         (unsigned long)fp[2], (unsigned long)fp[3]);
                    PLOG("    NOTE: reading grey here is CONSISTENT with the desktop"
                         " but proves nothing on its own — run 'FbProbe MARK' to write");

                    if (mark) {
                        /* Width from the live RECOUT, not assumed: this is the
                         * scaler's output rect and therefore the real line
                         * length being scanned out. */
                        ULONG rcsz  = rd(ad, R_DSCL_RECOUT_SIZE);
                        ULONG w     = rcsz & 0xFFFFUL;
                        ULONG pitch = w * 4;
                        ULONG mx = 100, my = 100, ms = 64, yy, xx, painted = 0;

                        if (w == 0) {
                            PLOG("    MARK: recout width is 0 — refusing to guess a pitch");
                            SHELL("MARK aborted: recout width 0");
                        } else if ((UQUAD)(off + (my + ms) * pitch) > (UQUAD)ad->vram_bar_size) {
                            PLOG("    MARK: square would fall outside the BAR — refusing");
                            SHELL("MARK aborted: outside BAR");
                        } else {
                            for (yy = 0; yy < ms; yy++) {
                                volatile ULONG *row =
                                    (volatile ULONG *)(IPTR)(cpu + (UQUAD)(my + yy) * pitch);
                                for (xx = 0; xx < ms; xx++) { row[mx + xx] = 0x00FF00FFUL; painted++; }
                            }
                            PLOG("    *** MARK: painted %lu magenta pixels at (%lu,%lu)"
                                 " via CPU 0x%08lx%08lx, pitch %lu (recout w=%lu) ***",
                                 (unsigned long)painted, (unsigned long)mx, (unsigned long)my,
                                 (unsigned long)(cpu >> 32), (unsigned long)(cpu & 0xFFFFFFFFUL),
                                 (unsigned long)pitch, (unsigned long)w);
                            SHELL("MARK: magenta 64x64 at (100,100) — if you SEE it, the derived"
                                  " CPU address IS the active framebuffer");
                        }
                    }
                } else {
                    PLOG("    NOTE: scanout offset is OUTSIDE the mapped BAR window"
                         " (size %lu MB) — the CPU cannot see what is being displayed",
                         (unsigned long)((UQUAD)ad->vram_bar_size >> 20));
                    SHELL("WARNING: scanout is outside the CPU BAR window");
                }
            } else {
                PLOG("*** ACTIVE SCANOUT MC 0x%08lx%08lx IS OUTSIDE THE VRAM WINDOW —"
                     " the display is NOT fed from local VRAM ***",
                     (unsigned long)(scan >> 32), (unsigned long)(scan & 0xFFFFFFFFUL));
                SHELL("WARNING: scanout MC=%08lx:%08lx is OUTSIDE VRAM",
                      (IPTR)(scan >> 32), (IPTR)(scan & 0xFFFFFFFFUL));
            }
        }

        for (p = 0; p < 4; p++) {
            if (only >= 0 && (LONG)p != only)
                continue;
            dump_pipe(ad, p);
        }
    }

out:
    if (amdgpuBase.pciDeviceAB) OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDevice);
    if (amdgpuBase.pciDriverAB) OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDriver);
    return 0;
}
