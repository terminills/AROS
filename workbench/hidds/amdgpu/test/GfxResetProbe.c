/*
 * GfxResetProbe — standalone GFX-reset lever tester for the RX 9060 XT (GFX12).
 *
 * The GC/GFX block boots write-blocked; the SMU-debug mode1 reset amdgpu.hidd
 * ships is display-SAFE, which is the tell it never actually resets GC (claude-3
 * + terminills, JOB-0084). This probe reuses the driver's own probe/mmio/
 * discovery to find the 9060 XT and resolve its register segments, then runs ONE
 * reset lever selected by argument and reports the DECISIVE oracle — the WRITE
 * TEST (GFX_IMU_RLC_RAM_ADDR_HIGH must STICK) plus GFX_IMU_GFX_RESET_CTRL leaving
 * 0x30. Build once; run per-lever (pushed over the RNDIS link) = seconds/theory.
 * The lever that makes the write STICK is the real GC reset -> fold into
 * amdgpu_reset.c.
 *
 *   GfxResetProbe            # baseline WRITE TEST, no reset
 *   GfxResetProbe mode1      # existing SMU-debug mode1 (expected: still blocked)
 *   GfxResetProbe grbm       # GRBM_SOFT_RESET on GC/RLC/CP
 *
 * More levers (FLR via config space, mode2, NBIF NO_SOFT_RESET F0 clear, +/-
 * ordering) get added as the search proceeds — small edits, fast rebuilds.
 */
#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/oop.h>
#include <hidd/pci.h>

#include <string.h>

#include "amdgpu_intern.h"
#include "asic_reg/nbif_6_3_1_excerpt.h"   /* RSMU index/data + F2 strap (derive F0) */

/* Serial (COM1) detail + Shell output so the operator sees it on the DUT. */
#define PLOG(...)  do { bug("[GfxResetProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define SHELL(...) do { Printf((CONST_STRPTR)"[GfxResetProbe] " __VA_ARGS__); \
                        Printf((CONST_STRPTR)"\n"); } while (0)

/* amdgpu_pci_probe() populates this; standalone progs define it themselves. */
struct AmdgpuBase amdgpuBase;

/* GC register access: BASE_IDX 0 = gc_seg[0] (GRBM etc.), BASE_IDX 1 = gc_seg[1]
 * (IMU/RLC/CP control). Same split the driver's static gc0_r/gc1_r use. */
#define GC0(ad, off)      amdgpu_rreg32((ad), (ad)->gc_seg[0] + (off))
#define GC0W(ad, off, v)  amdgpu_wreg32((ad), (ad)->gc_seg[0] + (off), (v))
#define GC1(ad, off)      amdgpu_rreg32((ad), (ad)->gc_seg[1] + (off))
#define GC1W(ad, off, v)  amdgpu_wreg32((ad), (ad)->gc_seg[1] + (off), (v))

#define regGFX_IMU_GFX_RESET_CTRL     0x40bc   /* gc_seg[1] */
#define regGFX_IMU_RLC_RAM_ADDR_HIGH  0x40ad   /* gc_seg[1] — the write-test reg */
#define regGRBM_STATUS                0x0da4   /* gc_seg[0] */
#define regGRBM_SOFT_RESET            0x0da8   /* gc_seg[0] — offset to VERIFY */
#define regGFX_IMU_SCRATCH_1          0x4069   /* gc_seg[1] — 0xbabeface when the IMU fw is live */

static void pspin(ULONG loops) { volatile ULONG i = 0; while (i < loops) i++; }

/* The decisive oracle. RLC_RAM_ADDR_HIGH has ~11 valid bits, so a good write
 * reads back masked (0xDEADBEEF & 0x7ff), not exact. */
static void write_test(struct AmdgpuDevice *ad, const char *tag)
{
    ULONG rb, rc, grbm;
    BOOL sticks, outofreset;

    GC1W(ad, regGFX_IMU_RLC_RAM_ADDR_HIGH, 0xDEADBEEFUL);
    rb   = GC1(ad, regGFX_IMU_RLC_RAM_ADDR_HIGH);
    rc   = GC1(ad, regGFX_IMU_GFX_RESET_CTRL);
    grbm = GC0(ad, regGRBM_STATUS);
    sticks     = ((rb & 0x7ffUL) == (0xDEADBEEFUL & 0x7ffUL));
    outofreset = ((rc & 0x1fUL) == 0x1fUL);

    PLOG("%-7s WRITE-TEST: rlc_ram_hi=0x%08lx %s | RESET_CTRL=0x%08lx %s | GRBM_STATUS=0x%08lx",
         tag, (unsigned long)rb, sticks ? "*** STICKS ***" : "blocked",
         (unsigned long)rc, outofreset ? "0x1f OUT-OF-RESET" : "held(0x30)",
         (unsigned long)grbm);
    SHELL("%-7s WRITE %s | RESET_CTRL=0x%08lx %s",
          tag, sticks ? "STICKS <-- LEVER!" : "blocked",
          (IPTR)rc, outofreset ? "(0x1f)" : "(held)");
}

/* PCI config-space accessors via the Hidd_PCIDevice methods (same pattern the
 * driver uses). Needed for a PCIe Function Level Reset — an out-of-GC-domain
 * reset that power-cycles the whole function, which register levers can't do
 * while GC is unpowered. */
static UBYTE cfg_r8(OOP_Object *dev, UWORD reg)
{
    struct pHidd_PCIDevice_ReadConfigByte m = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice, moHidd_PCIDevice_ReadConfigByte), .reg = reg };
    return (UBYTE)OOP_DoMethod(dev, (OOP_Msg)&m);
}
static UWORD cfg_r16(OOP_Object *dev, UWORD reg)
{
    struct pHidd_PCIDevice_ReadConfigWord m = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice, moHidd_PCIDevice_ReadConfigWord), .reg = reg };
    return (UWORD)OOP_DoMethod(dev, (OOP_Msg)&m);
}
static void cfg_w16(OOP_Object *dev, UWORD reg, UWORD val)
{
    struct pHidd_PCIDevice_WriteConfigWord m = {
        .mID = OOP_GetMethodID(IID_Hidd_PCIDevice, moHidd_PCIDevice_WriteConfigWord), .reg = reg, .val = val };
    OOP_DoMethod(dev, (OOP_Msg)&m);
}

/* Lever: PCIe Function Level Reset. Walk the cap list to the PCIe Express cap
 * (0x10), then set Initiate FLR (bit 15 of Device Control @ cap+0x08). This
 * resets the function from OUTSIDE the GC power domain — the direction the
 * WRITE-TEST said we need. NOTE: FLR may drop the PCIe link / crash the DUT;
 * NetShell auto-relaunch (rescue Startup-Network) recovers the shell on reboot. */
static void lever_flr(struct AmdgpuDevice *ad)
{
    OOP_Object *dev = ad->pciDevice;
    UWORD sta;
    UBYTE cap;
    int guard = 0;

    if (!dev) { PLOG("flr: no pciDevice"); return; }
    sta = cfg_r16(dev, 0x06);                 /* PCI Status */
    if (!(sta & 0x10)) { PLOG("flr: no cap list (status=0x%04x)", (unsigned)sta); return; }
    cap = (UBYTE)(cfg_r8(dev, 0x34) & 0xFC);  /* Cap Pointer */
    while (cap && guard++ < 48) {
        UBYTE id = cfg_r8(dev, cap);
        if (id == 0x10) {                     /* PCI Express capability */
            UWORD devcap = cfg_r16(dev, cap + 0x04);
            UWORD devctl = cfg_r16(dev, cap + 0x08);
            PLOG("flr: PCIe cap@0x%02x DevCap=0x%04x (FLR-capable=%d) DevCtl=0x%04x",
                 (unsigned)cap, (unsigned)devcap, (devcap >> 28) & 1, (unsigned)devctl);
            SHELL("flr: PCIe cap@0x%02lx FLR-capable=%ld -> issuing FLR",
                  (IPTR)cap, (LONG)((devcap >> 28) & 1));
            cfg_w16(dev, cap + 0x08, (UWORD)(devctl | 0x8000)); /* Initiate FLR */
            pspin(60000000);                  /* >=100ms settle per PCIe spec */
            PLOG("flr: FLR issued (DevCtl bit15), settled");
            return;
        }
        cap = (UBYTE)(cfg_r8(dev, cap + 1) & 0xFC);
    }
    PLOG("flr: no PCIe Express cap found");
}


/* Lever: PCI power-state cycle D0 -> D3hot -> D0.
 *
 * The leading hypothesis after everything else was eliminated. Every Linux
 * capture of the healthy GFX_IMU_GFX_RESET_CTRL 0x00 -> 0x30 -> 0x7F walk was
 * taken across rmmod -> modprobe. Measured on the DUT: after rmmod every GC
 * register reads 0xFFFFFFFF, i.e. the card is POWERED DOWN in D3. modprobe then
 * returns it to D0. So the release may simply be the POWER-STATE transition
 * making the PSP/IMU re-boot from IFWI — which our card never does, because the
 * GOP hands it over already in D0 and AROS never power-cycles it.
 *
 * Distinct from codex's earlier PM probe (JOB-0084 2026-08-15 12:51Z): that one
 * checked pci_pm_reset() ELIGIBILITY, found PMCSR=0x0008 (D0, NO_SOFT_RESET=1),
 * correctly concluded Linux would reject D3hot->D0 as a *reset*, and therefore
 * ISSUED NO TRANSITION. Whether the transition re-inits the PSP/IMU is a
 * different question and is still unmeasured. */
static void lever_d3d0(struct AmdgpuDevice *ad)
{
    OOP_Object *dev = ad->pciDevice;
    UWORD sta, pmcsr;
    UBYTE cap;
    int guard = 0;

    if (!dev) { PLOG("d3d0: no pciDevice"); return; }
    sta = cfg_r16(dev, 0x06);
    if (!(sta & 0x10)) { PLOG("d3d0: no cap list (status=0x%04x)", (unsigned)sta); return; }

    cap = (UBYTE)(cfg_r8(dev, 0x34) & 0xFC);
    while (cap && guard++ < 48) {
        if (cfg_r8(dev, cap) == 0x01) {          /* PCI Power Management cap */
            UWORD pmc = cfg_r16(dev, cap + 0x02);
            pmcsr = cfg_r16(dev, cap + 0x04);
            PLOG("d3d0: PM cap@0x%02x PMC=0x%04x PMCSR=0x%04x (state D%u, NO_SOFT_RESET=%u)",
                 (unsigned)cap, (unsigned)pmc, (unsigned)pmcsr,
                 (unsigned)(pmcsr & 3), (unsigned)((pmcsr >> 3) & 1));
            PLOG("d3d0: RESET_CTRL before = 0x%08lx  imu_s1=0x%08lx",
                 (unsigned long)GC1(ad, regGFX_IMU_GFX_RESET_CTRL),
                 (unsigned long)GC1(ad, regGFX_IMU_SCRATCH_1));
            SHELL("d3d0: PMCSR=0x%04lx -> entering D3hot", (IPTR)pmcsr);

            /* D0 -> D3hot */
            cfg_w16(dev, cap + 0x04, (UWORD)((pmcsr & ~3u) | 3u));
            pspin(30000000);                      /* >= spec 10ms in D3hot */
            PLOG("d3d0: in D3hot, PMCSR=0x%04x", (unsigned)cfg_r16(dev, cap + 0x04));

            /* D3hot -> D0 */
            pmcsr = cfg_r16(dev, cap + 0x04);
            cfg_w16(dev, cap + 0x04, (UWORD)(pmcsr & ~3u));
            pspin(60000000);                      /* >= spec 10ms D3hot->D0 recovery */
            PLOG("d3d0: back in D0, PMCSR=0x%04x", (unsigned)cfg_r16(dev, cap + 0x04));

            PLOG("d3d0: RESET_CTRL after  = 0x%08lx  imu_s1=0x%08lx",
                 (unsigned long)GC1(ad, regGFX_IMU_GFX_RESET_CTRL),
                 (unsigned long)GC1(ad, regGFX_IMU_SCRATCH_1));
            return;
        }
        cap = (UBYTE)(cfg_r8(dev, cap + 1) & 0xFC);
    }
    PLOG("d3d0: no PCI PM capability found");
}

/* Lever: broad GRBM soft-reset of the GFX sub-blocks (CP/RLC/GFX). Bits are a
 * first cut (refined by the search); assert, settle, deassert, settle. */
static void lever_grbm(struct AmdgpuDevice *ad)
{
    ULONG before = GC0(ad, regGRBM_SOFT_RESET);
    PLOG("lever grbm: GRBM_SOFT_RESET before=0x%08lx -> assert broad GFX reset", (unsigned long)before);
    GC0W(ad, regGRBM_SOFT_RESET, 0x00010001UL);   /* placeholder mask — refine per search */
    pspin(4000000);
    GC0W(ad, regGRBM_SOFT_RESET, 0x0UL);
    pspin(20000000);
    PLOG("lever grbm: GRBM_SOFT_RESET after=0x%08lx", (unsigned long)GC0(ad, regGRBM_SOFT_RESET));
}

/* NBIF strap access via the PF0 RSMU index/data aperture (mirrors amdgpu_nbio.c).
 * byte_address = (nbio_seg[5] + strap_dw) << 2. */
static ULONG nbif_rr(struct AmdgpuDevice *ad, ULONG strap_dw)
{
    ULONG idx = ad->nbio_seg[1] + NBIF_6_3_1_PF0_RSMU_INDEX_DW;
    ULONG dat = ad->nbio_seg[1] + NBIF_6_3_1_PF0_RSMU_DATA_DW;
    ULONG ba  = (ad->nbio_seg[5] + strap_dw) << 2;
    amdgpu_wreg32(ad, idx, ba); (void)amdgpu_rreg32(ad, idx);
    return amdgpu_rreg32(ad, dat);
}
static void nbif_wr(struct AmdgpuDevice *ad, ULONG strap_dw, ULONG val)
{
    ULONG idx = ad->nbio_seg[1] + NBIF_6_3_1_PF0_RSMU_INDEX_DW;
    ULONG dat = ad->nbio_seg[1] + NBIF_6_3_1_PF0_RSMU_DATA_DW;
    ULONG ba  = (ad->nbio_seg[5] + strap_dw) << 2;
    amdgpu_wreg32(ad, idx, ba); (void)amdgpu_rreg32(ad, idx);
    amdgpu_wreg32(ad, dat, val); (void)amdgpu_rreg32(ad, dat);
}

/* Dump STRAP2 registers at the F2 offset (0xd102, known) and strided candidates
 * below it, so the EPF0/EPF1/EPF2 per-function NO_SOFT_RESET bits can be located
 * empirically -> the DEV0_F0 strap that gates whether a reset actually resets
 * GFX (GFX is function 0). bit7 is NO_SOFT_RESET on F2; watch it across functions. */
static void lever_nbifdump(struct AmdgpuDevice *ad)
{
    /* F0/F1/F2 at 0x80 and 0x100 dw strides below/at the F2 offset — a first net;
     * widen next iteration if none look like straps. */
    ULONG f2 = NBIF_6_3_1_RCC_DEV0_EPF2_STRAP2_DW;
    ULONG cands[8];
    int i;
    cands[0] = f2 - 0x200; cands[1] = f2 - 0x180; cands[2] = f2 - 0x100;
    cands[3] = f2 - 0x080; cands[4] = f2 - 0x002; cands[5] = f2 - 0x001;
    cands[6] = f2;         cands[7] = f2 + 0x001;

    if (!ad->nbio_seg[1] || !ad->nbio_seg[5]) {
        PLOG("nbifdump: nbio_seg[1/5] unavailable");
        SHELL("nbifdump: nbio_seg[1/5] unavailable");
        return;
    }
    PLOG("nbifdump: nbio_seg[1]=0x%05lx nbio_seg[5]=0x%05lx; F2 STRAP2 @dw 0x%lx NO_SOFT_RESET_F2=0x%lx",
         (unsigned long)ad->nbio_seg[1], (unsigned long)ad->nbio_seg[5],
         (unsigned long)f2, (unsigned long)NBIF_6_3_1_STRAP_NO_SOFT_RESET_DEV0_F2);
    SHELL("nbifdump: nbio_seg[1]=0x%05lx nbio_seg[5]=0x%05lx F2@dw=0x%lx",
          (IPTR)(ULONG)ad->nbio_seg[1], (IPTR)(ULONG)ad->nbio_seg[5], (IPTR)(ULONG)f2);
    for (i = 0; i < 8; i++) {
        ULONG v = nbif_rr(ad, cands[i]);
        PLOG("  STRAP2[dw 0x%04lx] = 0x%08lx (bit7=%ld)%s",
             (unsigned long)cands[i], (unsigned long)v, (LONG)((v >> 7) & 1),
             (cands[i] == f2) ? "  <- F2 (known)" : "");
        SHELL("  STRAP2[dw 0x%04lx] = 0x%08lx (bit7=%ld)%s",
              (IPTR)(ULONG)cands[i], (IPTR)(ULONG)v, (IPTR)(LONG)((v >> 7) & 1),
              (cands[i] == f2) ? "  <- F2 (known)" : "");
    }
}

/* Clear NO_SOFT_RESET on DEV0 function 0 (GFX/display/compute) so a subsequent
 * reset actually cycles GFX. The shipping amdgpu_nbio_hw_init only clears F2,
 * which the dump shows is already 0 — F0 (0xd002) is still 1, exempting GFX from
 * every reset. Returns TRUE if the strap was set and got cleared. */
static BOOL clear_f0_strap(struct AmdgpuDevice *ad)
{
    ULONG before, after;
    if (!ad->nbio_seg[1] || !ad->nbio_seg[5]) {
        PLOG("f0strap: nbio_seg[1/5] unavailable"); SHELL("f0strap: nbio_seg unavailable");
        return FALSE;
    }
    before = nbif_rr(ad, NBIF_6_3_1_RCC_DEV0_EPF0_STRAP2_DW);
    nbif_wr(ad, NBIF_6_3_1_RCC_DEV0_EPF0_STRAP2_DW,
            before & ~NBIF_6_3_1_STRAP_NO_SOFT_RESET_DEV0_F0);
    after = nbif_rr(ad, NBIF_6_3_1_RCC_DEV0_EPF0_STRAP2_DW);
    PLOG("f0strap: EPF0 before=0x%08lx after=0x%08lx (NO_SOFT_RESET %s->%s)",
         (unsigned long)before, (unsigned long)after,
         (before & 0x80) ? "1" : "0", (after & 0x80) ? "1" : "0");
    SHELL("f0strap: EPF0 before=0x%08lx after=0x%08lx (bit7 %ld->%ld)",
          (IPTR)(ULONG)before, (IPTR)(ULONG)after,
          (IPTR)(LONG)((before >> 7) & 1), (IPTR)(LONG)((after >> 7) & 1));
    return (before & NBIF_6_3_1_STRAP_NO_SOFT_RESET_DEV0_F0) &&
           !(after & NBIF_6_3_1_STRAP_NO_SOFT_RESET_DEV0_F0);
}

/* ---------------------------------------------------------------------------
 * SMU levers. The reset-lever sweep is CLOSED NEGATIVE: none/grbm/flr/mode1 —
 * and f0clear+mode1, which demonstrably DOES reach function 0 (it drops the DCN
 * scanout) — all leave GRBM_STATUS=0 with GFX writes blocked. No reset reachable
 * from outside the GC power domain powers GC. That leaves the PMFW: on discrete
 * Navi44 the SMU owns the GFX rail, and Linux turns it on with
 * EnableAllSmuFeatures during smu_smc_hw_setup(), between PSP autoload and GFX
 * entry — which is exactly why its gfx_v12_0_wait_for_rlc_autoload_complete()
 * returns in ~3us while ours spins forever on BOOTLOAD_COMPLETE=0.
 *
 * The opcode is NOT in doubt: smu_v14_0_2_ppsmc.h is byte-identical between
 * mainline v7.1 (the tree contemporary with the Ubuntu kernel that drives this
 * exact card) and ROCm therock-7.14, and both define EnableAllSmuFeatures=0x6.
 * The PMFW's GetDriverIfVersion 0x33-vs-driver-0x2E delta does not select a
 * different message table — smu_cmn_check_fw_version() only dev_info_once()s it
 * and returns 0. So the earlier "wedge" was a mailbox/timing effect, not a wrong
 * opcode; running it HERE (post-boot, one message, recoverable by reboot) is how
 * we find out without taking the boot path down with it.
 */
/* Offsets + BASE_IDX verified against Linux gc_12_0_0_offset.h. */
#define regCP_STAT                    0x0f40   /* gc_seg[0] */
#define regRLC_RLCS_BOOTLOAD_STATUS   0x4e7c   /* gc_seg[1] */
#define regGFX_IMU_CORE_CTRL          0x40b6   /* gc_seg[1] */
#define regGFX_IMU_SCRATCH_10         0x4072   /* gc_seg[1] */
#define regGFX_IMU_SCRATCH_0          0x4068   /* gc_seg[1] — IMU fw version */
#define regRLC_CNTL                   0x4c00   /* gc_seg[1] */
/* The RLC bootloader pointers. On the PSP path the PSP is supposed to program
 * these so the IMU knows where to boot the RLC from; if they read back zero the
 * PSP accepted AUTOLOAD_RLC without ever arming the bootloader, which would
 * explain an autoload that is accepted but never completes. */
#define regGFX_IMU_RLC_BOOTLOADER_ADDR_HI 0x5f81  /* gc_seg[1] */
#define regGFX_IMU_RLC_BOOTLOADER_ADDR_LO 0x5f82  /* gc_seg[1] */
#define regGFX_IMU_RLC_BOOTLOADER_SIZE    0x5f83  /* gc_seg[1] */

/* RLC_GPM_STAT and RLC_RLCS_GPM_STAT alias the same 0x4e6c/BASE_IDX 1 in
 * gc_12_0_0_offset.h. Bits 1/2 are the only DIRECT read of whether the GFX rail
 * and the GFX clock are actually up — every other signal we have (RESET_CTRL,
 * SCRATCH_10, CP_STAT) is downstream of them. */
#define regRLC_RLCS_GPM_STAT          0x4e6c   /* gc_seg[1] */
#define GPM_GFX_POWER_STATUS_BIT      1
#define GPM_GFX_CLOCK_STATUS_BIT      2
#define GPM_RLC_BUSY_BIT              0
#define GPM_GFX_PIPELINE_PWR_BIT      4
#define GPM_RLC_EXEC_ROM_CODE_BIT     0x16
#define GPM_PG_ERROR_STATUS_BIT       0x18

#define PPSMC_GetSmuVersion              0x2
#define PPSMC_GetDriverIfVersion         0x3
#define PPSMC_SetAllowedFeaturesMaskLow  0x4
#define PPSMC_SetAllowedFeaturesMaskHigh 0x5
#define PPSMC_EnableAllSmuFeatures       0x6
#define PPSMC_GetRunningSmuFeaturesLow   0xC
#define PPSMC_GetRunningSmuFeaturesHigh  0xD
/* Never-tried levers, all present in smu_v14_0_2_ppsmc.h. GFXOFF is the one
 * mechanism that parks GFX exactly where we observe it: rail down, RESET_CTRL
 * stuck with HARD/EA/UTCL2/SDMA still asserted. Linux never sends DisallowGfxOff
 * at init because its own bookkeeping starts with GFXOFF disallowed — it never
 * inherits a GOP-initialised card the way we do. */
#define PPSMC_AllowGfxOff                0x28
#define PPSMC_DisallowGfxOff             0x29
#define PPSMC_GetMinDpmFreq              0x1D
#define PPSMC_GetMaxDpmFreq              0x1E
#define PPSMC_GetAllRunningSmuFeatures   0x54
#define PPSMC_GetSvi3Voltage             0x55
#define PPCLK_GFXCLK                     0      /* PPCLK_e, smu14_driver_if_v14_0.h */

#define FEATURE_DPM_GFXCLK_BIT  1    /* low word  */
#define FEATURE_GFX_IMU_BIT    36    /* high word, bit 36-32=4 */

/* FEATURE_PWR_DOMAIN_e — the parameter EnableAllSmuFeatures actually takes.
 * See the citation in amdgpu_smu.c; 0=ALL is what has been stalling this PMFW,
 * 4=GFX is what smu_v14_0_2_enable_gfx_features() sends. */
#define FEATURE_PWR_ALL   0
#define FEATURE_PWR_GFX   4

/* The GFX-side oracle for the SMU levers: if the PMFW powers the rail, CP_STAT /
 * BOOTLOAD_STATUS / RESET_CTRL must move — GRBM_STATUS leaving 0 is the tell. */
static void gfx_state(struct AmdgpuDevice *ad, const char *tag)
{
    ULONG cp    = GC0(ad, regCP_STAT);
    ULONG boot  = GC1(ad, regRLC_RLCS_BOOTLOAD_STATUS);
    ULONG rc    = GC1(ad, regGFX_IMU_GFX_RESET_CTRL);
    ULONG core  = GC1(ad, regGFX_IMU_CORE_CTRL);
    ULONG scr10 = GC1(ad, regGFX_IMU_SCRATCH_10);
    ULONG grbm  = GC0(ad, regGRBM_STATUS);

    PLOG("%-6s GFX: CP_STAT=0x%08lx BOOTLOAD=0x%08lx (COMPLETE=%lu) RESET_CTRL=0x%08lx "
         "CORE_CTRL=0x%08lx SCRATCH_10=0x%08lx GRBM_STATUS=0x%08lx",
         tag, (unsigned long)cp, (unsigned long)boot, (unsigned long)(boot & 1),
         (unsigned long)rc, (unsigned long)core, (unsigned long)scr10,
         (unsigned long)grbm);
    {
        ULONG s0 = GC1(ad, regGFX_IMU_SCRATCH_0);
        ULONG s1 = GC1(ad, regGFX_IMU_SCRATCH_1);
        ULONG bl_hi = GC1(ad, regGFX_IMU_RLC_BOOTLOADER_ADDR_HI);
        ULONG bl_lo = GC1(ad, regGFX_IMU_RLC_BOOTLOADER_ADDR_LO);
        ULONG bl_sz = GC1(ad, regGFX_IMU_RLC_BOOTLOADER_SIZE);
        ULONG rlc   = GC1(ad, regRLC_CNTL);

        /* SCRATCH_1 alternates b000b000 <-> babeface between consecutive reads
         * while the IMU is running its poll loop, so neither value alone means
         * "stopped" — only a value that never changes across reads does. */
        PLOG("%-6s IMU: SCRATCH_0=0x%08lx SCRATCH_1=0x%08lx (%s) RLC_CNTL=0x%08lx",
             tag, (unsigned long)s0, (unsigned long)s1,
             (s1 == 0xbabefaceUL) ? "IMU FW LIVE (babeface phase)" :
             (s1 == 0xb000b000UL) ? "IMU FW LIVE (b000b000 phase)" : "imu fw silent",
             (unsigned long)rlc);
        /* This used to print "ARMED by PSP" for any non-zero read, which made it
         * announce ARMED on every run of this job — the registers read all-ones,
         * i.e. UNREADABLE (wrong offset, or an unpowered/unimplemented block),
         * which is the opposite of armed. All-ones is now called out as such. */
        PLOG("%-6s RLC BOOTLOADER: addr=0x%08lx%08lx size=0x%08lx -> %s",
             tag, (unsigned long)bl_hi, (unsigned long)bl_lo, (unsigned long)bl_sz,
             (bl_hi == 0xFFFFFFFFUL && bl_lo == 0xFFFFFFFFUL && bl_sz == 0xFFFFFFFFUL)
                 ? "*** ALL-ONES: register UNREADABLE, tells us nothing ***"
                 : (bl_hi || bl_lo || bl_sz) ? "armed (non-zero)"
                                             : "*** NOT ARMED (all zero) ***");
        SHELL("%-6s CP=0x%lx BOOT=0x%lx GRBM=0x%lx | imu_s1=0x%lx bl=%08lx:%08lx sz=0x%lx",
              tag, (IPTR)cp, (IPTR)boot, (IPTR)grbm, (IPTR)s1,
              (IPTR)bl_hi, (IPTR)bl_lo, (IPTR)bl_sz);
    }
}

/* Read-only SMU state: safe to run first, cannot wedge anything. */
static void smu_info(struct AmdgpuDevice *ad)
{
    ULONG ver = 0, ifv = 0, flo = 0, fhi = 0, st;

    st  = amdgpu_smu_send_msg(ad, PPSMC_GetSmuVersion, 0, &ver);
    PLOG("smu: GetSmuVersion resp=0x%08lx ver=0x%08lx (%lu.%lu.%lu)%s",
         (unsigned long)st, (unsigned long)ver,
         (unsigned long)((ver >> 16) & 0xFF), (unsigned long)((ver >> 8) & 0xFF),
         (unsigned long)(ver & 0xFF), (st != 1) ? "  <-- MAILBOX NOT OK" : "");
    st  = amdgpu_smu_send_msg(ad, PPSMC_GetDriverIfVersion, 0, &ifv);
    PLOG("smu: GetDriverIfVersion resp=0x%08lx if=0x%08lx (Linux driver constant 0x2E; "
         "a delta here is informational only)", (unsigned long)st, (unsigned long)ifv);
    amdgpu_smu_send_msg(ad, PPSMC_GetRunningSmuFeaturesLow,  0, &flo);
    amdgpu_smu_send_msg(ad, PPSMC_GetRunningSmuFeaturesHigh, 0, &fhi);
    PLOG("smu: RunningFeatures=%08lx:%08lx  DPM_GFXCLK(b1)=%s GFX_IMU(b36)=%s",
         (unsigned long)fhi, (unsigned long)flo,
         (flo & (1UL << FEATURE_DPM_GFXCLK_BIT)) ? "ON" : "off",
         (fhi & (1UL << (FEATURE_GFX_IMU_BIT - 32))) ? "ON" : "off");
    SHELL("smu ver=0x%lx if=0x%lx running=%08lx:%08lx",
          (IPTR)ver, (IPTR)ifv, (IPTR)fhi, (IPTR)flo);
}

/* One decoded SMU metrics sample: current clocks, activity, power,
 * temperatures and fan, as the driver reads them (TABLE_SMU_METRICS). */
static void smu_metrics(struct AmdgpuDevice *ad)
{
    struct AmdgpuSmuMetrics m;

    if (!amdgpu_smu_read_metrics(ad, &m)) {
        SHELL("metrics: SMU did not deliver the table");
        return;
    }
    SHELL("metrics #%lu: GFXCLK cur %lu avg %lu MHz | UCLK %lu | SOCCLK %lu | FCLK %lu",
          (IPTR)m.counter, (IPTR)m.gfxclk_cur, (IPTR)m.gfxclk_avg,
          (IPTR)m.uclk_cur, (IPTR)m.socclk_cur, (IPTR)m.fclk_cur);
    SHELL("metrics: GFX busy %lu%% mem busy %lu%% | socket %lu W board %lu W",
          (IPTR)m.gfx_activity, (IPTR)m.uclk_activity,
          (IPTR)m.socket_power, (IPTR)m.board_power);
    SHELL("metrics: edge %lu C hotspot %lu C mem %lu C | fan %lu%% %lu rpm",
          (IPTR)m.temp_edge, (IPTR)m.temp_hotspot, (IPTR)m.temp_mem,
          (IPTR)m.fan_pwm, (IPTR)m.fan_rpm);
}

/* MP1 mailbox registers, same mapping the driver uses (C2PMSG_N dword = 0x40+N). */
#define MP1_C2PMSG_66  (0x40 + 66)   /* message   */
#define MP1_C2PMSG_82  (0x40 + 82)   /* parameter / return value */
#define MP1_C2PMSG_90  (0x40 + 90)   /* response  */

/* Snapshot the whole live C2PMSG bank. Both EnableAllSmuFeatures attempts —
 * FEATURE_PWR_ALL(0) and FEATURE_PWR_GFX(4) — leave C2PMSG_90 at 0 forever,
 * and in the smu_cmn protocol resp==0 means COMMAND IN PROGRESS. So the PMFW
 * accepted the command and is executing it indefinitely, not rejecting it: it
 * is hung inside its own handler. Rather than guess which prerequisite it is
 * blocked on, diff the register bank across the stall — PMFW firmware parks
 * assert/state codes in these scratch registers, so any dword that moves names
 * the thing it is stuck on. */
static void c2p_scan(struct AmdgpuDevice *ad, ULONG *out, const char *tag)
{
    ULONG base = amdgpu_smu_base(ad);
    ULONG n;

    PLOG("C2PMSG scan [%s] @0x%05lx (non-zero only):", tag, (unsigned long)base);
    for (n = 0; n < 128; n++) {
        ULONG v = amdgpu_rreg32(ad, base + 0x40 + n);
        out[n] = v;
        if (v != 0 && v != 0xFFFFFFFFUL)
            PLOG("  [%s] C2PMSG_%-3lu = 0x%08lx", tag, (unsigned long)n,
                 (unsigned long)v);
    }
}

static void c2p_diff(const ULONG *before, const ULONG *after)
{
    ULONG n, moved = 0;

    for (n = 0; n < 128; n++) {
        if (before[n] != after[n]) {
            moved++;
            PLOG("*** C2PMSG_%-3lu CHANGED 0x%08lx -> 0x%08lx ***",
                 (unsigned long)n, (unsigned long)before[n], (unsigned long)after[n]);
            SHELL("C2PMSG_%lu 0x%lx -> 0x%lx", (IPTR)n, (IPTR)before[n], (IPTR)after[n]);
        }
    }
    PLOG("*** C2PMSG diff across the stall: %lu register(s) moved ***",
         (unsigned long)moved);
    SHELL("C2PMSG diff: %lu moved", (IPTR)moved);
}

/* Observe the stall instead of theorising about it: scan the bank, fire the
 * message by hand (no bounded helper), poll the response for far longer than
 * the driver's 8s to settle "hung" vs merely "slow", then re-scan and diff. */
static void lever_smu_dump(struct AmdgpuDevice *ad, ULONG pwr)
{
    static ULONG before[128], after[128];
    ULONG base = amdgpu_smu_base(ad);
    ULONG resp = 0, t, ticks = 0;

    smu_info(ad);
    c2p_scan(ad, before, "pre");

    PLOG("*** firing EnableAllSmuFeatures(0x6) pwr=%lu BY HAND — long poll ***",
         (unsigned long)pwr);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_66, 0);
    (void)amdgpu_rreg32(ad, base + MP1_C2PMSG_66);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_90, 0);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_82, pwr);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_66, 0x6);
    PLOG("  readback msg=0x%08lx arg=0x%08lx resp=0x%08lx",
         (unsigned long)amdgpu_rreg32(ad, base + MP1_C2PMSG_66),
         (unsigned long)amdgpu_rreg32(ad, base + MP1_C2PMSG_82),
         (unsigned long)amdgpu_rreg32(ad, base + MP1_C2PMSG_90));

    /* ~10x the driver's window. If it is merely slow, this catches it; if this
     * also expires, "hung in handler" is established, not assumed. */
    for (t = 0; t < 80000000UL; t++) {
        resp = amdgpu_rreg32(ad, base + MP1_C2PMSG_90);
        if (resp != 0)
            break;
        pspin(50);
        if ((t & 0xFFFFFFUL) == 0) {
            ticks++;
            PLOG("  ...still 0 after %lu polls (GRBM_STATUS=0x%08lx)",
                 (unsigned long)t, (unsigned long)GC0(ad, regGRBM_STATUS));
        }
    }
    PLOG("*** long poll ended: resp=0x%08lx after %lu polls (%s) ***",
         (unsigned long)resp, (unsigned long)t,
         resp ? "ANSWERED" : "NEVER ANSWERED — PMFW hung in handler");
    SHELL("long poll resp=0x%lx after %lu (%s)", (IPTR)resp, (IPTR)t,
          resp ? "ANSWERED" : "HUNG");

    c2p_scan(ad, after, "post");
    c2p_diff(before, after);
}

/* Watch GFX_IMU_GFX_RESET_CTRL *through* a mode1, printing every distinct value.
 *
 * The Linux oracle shows a working card's register walks 0xFFFFFFFF (block
 * unreachable, i.e. actually held in reset) -> 0x30 -> 0x7F. We have only ever
 * sampled before and after our own mode1 and seen 0x30 both times, which does
 * not distinguish "the reset works but the release never happens" from "our
 * mode1 never takes the GC domain down at all and 0x30 is untouched GOP-era
 * state". Sending the debug message by hand and polling straight through the
 * window separates those two. */
#define MP1_C2PMSG_53  (0x40 + 53)   /* debug param */
#define MP1_C2PMSG_75  (0x40 + 75)   /* debug msg   */
#define MP1_C2PMSG_54  (0x40 + 54)   /* debug resp  */
#define DEBUGSMC_Mode1Reset 2

static void lever_mode1watch(struct AmdgpuDevice *ad)
{
    ULONG base = amdgpu_smu_base(ad);
    ULONG last = 0xDEADDEADUL, v, i, seen = 0;

    PLOG("mode1watch: RESET_CTRL before = 0x%08lx",
         (unsigned long)GC1(ad, regGFX_IMU_GFX_RESET_CTRL));
    SHELL("mode1watch: before=0x%lx",
          (IPTR)GC1(ad, regGFX_IMU_GFX_RESET_CTRL));

    /* smu_msg_v1_send_debug_msg: param, msg, clear resp. Fire-and-forget. */
    amdgpu_wreg32(ad, base + MP1_C2PMSG_53, 0);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_75, DEBUGSMC_Mode1Reset);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_54, 0);

    /* Poll straight through the reset window. No settle delay on purpose: the
     * whole point is to catch the transient Linux shows. */
    for (i = 0; i < 3000000UL; i++) {
        v = GC1(ad, regGFX_IMU_GFX_RESET_CTRL);
        if (v != last) {
            PLOG("mode1watch: [%8lu] RESET_CTRL -> 0x%08lx", (unsigned long)i,
                 (unsigned long)v);
            SHELL("  [%lu] -> 0x%08lx", (IPTR)i, (IPTR)v);
            last = v;
            if (++seen > 24)
                break;      /* plenty; do not flood the link */
        }
    }
    PLOG("mode1watch: final RESET_CTRL=0x%08lx after %lu polls, %lu distinct value(s)",
         (unsigned long)GC1(ad, regGFX_IMU_GFX_RESET_CTRL),
         (unsigned long)i, (unsigned long)seen);
    SHELL("mode1watch final=0x%08lx (%lu distinct)",
          (IPTR)GC1(ad, regGFX_IMU_GFX_RESET_CTRL), (IPTR)seen);
}

/* Generic single-message sender — walk the PPSMC message space one at a time.
 *
 * Budget: message 0x6 kills the PMFW outright (afterwards even GetSmuVersion
 * returns nothing, and mode1 does NOT revive it), so a HANGING message costs a
 * reboot. A message that ANSWERS costs nothing, so the whole neighbourhood can
 * be walked inside one boot until something hangs. Reports the response, the
 * returned value, and any PMFW-side register movement.
 *
 * The point of walking it: if the feature-control family answers except 0x6,
 * then EnableSmuFeaturesLow/High (0x8/0x9) may be able to turn on the GFX
 * feature bits directly by mask — a way around EnableAllSmuFeatures rather than
 * a diagnosis of it.
 */
static void lever_smu_msg(struct AmdgpuDevice *ad, ULONG msg, ULONG arg)
{
    static ULONG before[128], after[128];
    ULONG base = amdgpu_smu_base(ad);
    ULONG resp = 0, t;

    c2p_scan(ad, before, "pre");

    PLOG("*** sending msg=0x%02lx arg=0x%08lx (long poll) ***",
         (unsigned long)msg, (unsigned long)arg);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_66, 0);
    (void)amdgpu_rreg32(ad, base + MP1_C2PMSG_66);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_90, 0);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_82, arg);
    amdgpu_wreg32(ad, base + MP1_C2PMSG_66, msg);

    for (t = 0; t < 20000000UL; t++) {
        resp = amdgpu_rreg32(ad, base + MP1_C2PMSG_90);
        if (resp != 0)
            break;
        pspin(50);
    }
    PLOG("*** msg=0x%02lx arg=0x%08lx -> resp=0x%08lx val=0x%08lx after %lu polls (%s) ***",
         (unsigned long)msg, (unsigned long)arg, (unsigned long)resp,
         (unsigned long)amdgpu_rreg32(ad, base + MP1_C2PMSG_82),
         (unsigned long)t,
         !resp ? "HUNG — this one costs a reboot" :
         (resp == 1) ? "OK" : "error code");
    SHELL("msg=0x%02lx arg=0x%lx -> resp=0x%lx val=0x%lx (%s)",
          (IPTR)msg, (IPTR)arg, (IPTR)resp,
          (IPTR)amdgpu_rreg32(ad, base + MP1_C2PMSG_82),
          !resp ? "HUNG" : (resp == 1) ? "OK" : "ERR");

    c2p_scan(ad, after, "post");
    c2p_diff(before, after);
}

/* THE lever. mask=TRUE also sends SetAllowedFeaturesMask first (the !SCPM path);
 * Linux skips those two under SCPM, so the default is enable-only. */
static void lever_smu_enable(struct AmdgpuDevice *ad, BOOL mask)
{
    ULONG val = 0, st, flo = 0, fhi = 0;

    smu_info(ad);

    if (mask) {
        st = amdgpu_smu_send_msg(ad, PPSMC_SetAllowedFeaturesMaskHigh, 0xFFFFFFFFUL, &val);
        PLOG("smu: SetAllowedFeaturesMaskHigh(0xFFFFFFFF) resp=0x%08lx", (unsigned long)st);
        st = amdgpu_smu_send_msg(ad, PPSMC_SetAllowedFeaturesMaskLow, 0xFFFFFFFFUL, &val);
        PLOG("smu: SetAllowedFeaturesMaskLow(0xFFFFFFFF) resp=0x%08lx", (unsigned long)st);
    }

    PLOG("smu: *** sending EnableAllSmuFeatures (0x6) — heavy op, mailbox poll is seconds ***");
    st = amdgpu_smu_send_msg(ad, PPSMC_EnableAllSmuFeatures, 0, &val);
    PLOG("smu: *** EnableAllSmuFeatures resp=0x%08lx val=0x%08lx ***",
         (unsigned long)st, (unsigned long)val);
    SHELL("EnableAllSmuFeatures resp=0x%lx", (IPTR)st);

    amdgpu_smu_send_msg(ad, PPSMC_GetRunningSmuFeaturesLow,  0, &flo);
    amdgpu_smu_send_msg(ad, PPSMC_GetRunningSmuFeaturesHigh, 0, &fhi);
    PLOG("smu: post-enable RunningFeatures=%08lx:%08lx  DPM_GFXCLK(b1)=%s GFX_IMU(b36)=%s -> "
         "GFX rail %s", (unsigned long)fhi, (unsigned long)flo,
         (flo & (1UL << FEATURE_DPM_GFXCLK_BIT)) ? "ON" : "off",
         (fhi & (1UL << (FEATURE_GFX_IMU_BIT - 32))) ? "ON" : "off",
         (flo || fhi) ? "SOME FEATURES ON" : "STILL UNPOWERED (features=0)");
    SHELL("post-enable running=%08lx:%08lx", (IPTR)fhi, (IPTR)flo);

    /* Give the rail + IMU a moment to come up before the GFX oracle re-reads.
     * Kept short enough that the NetShell socket drain still catches the AFTER
     * lines; the COM1 capture holds the full detail either way. */
    pspin(5000000);
}

/* Decode the ONE register that says, directly, whether the GFX rail and the GFX
 * clock are live. Everything this job has been reading — RESET_CTRL stuck at
 * 0x30, SCRATCH_10 parked at 7, CP_STAT, BOOTLOAD_STATUS — is downstream of
 * these two bits, so a frozen IMU is ambiguous until they are read: it says
 * nothing about whether the IMU is BROKEN or simply UNPOWERED.
 *
 * Caveat recorded up front: RLC_RLCS_GPM_STAT is maintained by RLC firmware,
 * and our RLC never completes autoload. A zero here therefore does NOT prove
 * the rail is down — it may just mean nobody is updating the register. Only a
 * NON-zero reading is strong evidence, and the SMU queries below are the
 * independent oracle that does not depend on the RLC running at all. */
static void gpm_decode(struct AmdgpuDevice *ad, const char *tag)
{
    ULONG g = GC1(ad, regRLC_RLCS_GPM_STAT);

    PLOG("%-6s GPM_STAT=0x%08lx  GFX_POWER=%s GFX_CLOCK=%s PIPELINE_PWR=%s "
         "RLC_BUSY=%s EXEC_ROM=%s PG_ERROR=%s", tag, (unsigned long)g,
         (g & (1UL << GPM_GFX_POWER_STATUS_BIT))  ? "UP"   : "down",
         (g & (1UL << GPM_GFX_CLOCK_STATUS_BIT))  ? "UP"   : "down",
         (g & (1UL << GPM_GFX_PIPELINE_PWR_BIT))  ? "UP"   : "down",
         (g & (1UL << GPM_RLC_BUSY_BIT))          ? "busy" : "idle",
         (g & (1UL << GPM_RLC_EXEC_ROM_CODE_BIT)) ? "ROM"  : "-",
         (g & (1UL << GPM_PG_ERROR_STATUS_BIT))   ? "*** PG ERROR ***" : "-");
    SHELL("%-6s gpm=0x%lx pwr=%s clk=%s", tag, (IPTR)g,
          (g & (1UL << GPM_GFX_POWER_STATUS_BIT)) ? "UP" : "down",
          (g & (1UL << GPM_GFX_CLOCK_STATUS_BIT)) ? "UP" : "down");
}

/* Ask PMFW directly what it thinks the GFX rail is doing, then try the one
 * mechanism that would park GFX in exactly the state we observe: GFXOFF.
 *
 * Why GFXOFF is a live hypothesis and not just another guess: the IMU has
 * released GRBM and DFLL (RESET_CTRL 0x30) and stopped short of releasing
 * HARD/EA/UTCL2/SDMA (0x7F). Releasing DFLL is the IMU starting the GFX clock
 * generator; the very next thing it must do is wait for that clock to lock,
 * which cannot happen without GFX voltage. If PMFW has GFX gated off — which is
 * exactly what a GOP-initialised card would do, since the GOP drives DCN and
 * has no use for GFX — the IMU waits forever at that point. That is a
 * mechanism, and it predicts the frozen SCRATCH_10=7 we measured.
 *
 * Read-only queries run FIRST so a wedge from DisallowGfxOff cannot cost us the
 * measurement. */
static void lever_gfxpower(struct AmdgpuDevice *ad)
{
    ULONG st, val, lo = 0, hi = 0, i;

    gfx_state(ad, "BEFORE");
    gpm_decode(ad, "BEFORE");

    /* GetAllRunningSmuFeatures (0x54) — never tried. If it answers where the
     * Low/High pair reports zero, the pair was the wrong question. */
    val = 0;
    st = amdgpu_smu_send_msg(ad, PPSMC_GetAllRunningSmuFeatures, 0, &val);
    PLOG("smu: GetAllRunningSmuFeatures(0x54) resp=0x%08lx val=0x%08lx",
         (unsigned long)st, (unsigned long)val);

    /* GetSvi3Voltage (0x55) — a DIRECT VRM readout, and the only oracle here
     * that does not depend on the RLC or the IMU running. Linux never calls it
     * on 14.0.2 so the plane argument is undocumented; walk the first few. */
    for (i = 0; i < 3; i++) {
        val = 0;
        st = amdgpu_smu_send_msg(ad, PPSMC_GetSvi3Voltage, i, &val);
        PLOG("smu: GetSvi3Voltage(plane=%lu) resp=0x%08lx val=0x%08lx",
             (unsigned long)i, (unsigned long)st, (unsigned long)val);
    }

    /* Does PMFW have a GFXCLK DPM table at all? If GetMin/MaxDpmFreq(GFXCLK)
     * are rejected, DPM never came up for GFX and the rail is not coming on. */
    val = 0;
    st = amdgpu_smu_send_msg(ad, PPSMC_GetMinDpmFreq, PPCLK_GFXCLK, &val);
    PLOG("smu: GetMinDpmFreq(GFXCLK) resp=0x%08lx val=%lu MHz",
         (unsigned long)st, (unsigned long)val);
    val = 0;
    st = amdgpu_smu_send_msg(ad, PPSMC_GetMaxDpmFreq, PPCLK_GFXCLK, &val);
    PLOG("smu: GetMaxDpmFreq(GFXCLK) resp=0x%08lx val=%lu MHz",
         (unsigned long)st, (unsigned long)val);

    /* Now the actual lever. */
    val = 0;
    st = amdgpu_smu_send_msg(ad, PPSMC_DisallowGfxOff, 0, &val);
    PLOG("smu: *** DisallowGfxOff(0x29) resp=0x%08lx val=0x%08lx ***",
         (unsigned long)st, (unsigned long)val);
    SHELL("DisallowGfxOff resp=0x%lx", (IPTR)st);

    pspin(2000000);
    gpm_decode(ad, "POST29");
    gfx_state(ad, "POST29");

    amdgpu_smu_send_msg(ad, PPSMC_GetRunningSmuFeaturesLow,  0, &lo);
    amdgpu_smu_send_msg(ad, PPSMC_GetRunningSmuFeaturesHigh, 0, &hi);
    PLOG("smu: post-DisallowGfxOff RunningFeatures=%08lx:%08lx",
         (unsigned long)hi, (unsigned long)lo);
}

int main(void)
{
    struct AmdgpuDevice *ad;
    UBYTE lever[32] = "none";
    struct RDArgs *rd;
    IPTR args[4] = { 0, 0, 0, 0 };
    ULONG pwr = FEATURE_PWR_GFX;   /* default: power the GFX block, not ALL */
    ULONG msg = 0, arg = 0;        /* for the generic smumsg walker */

    rd = ReadArgs((CONST_STRPTR)"LEVER,PWR/N,MSG/N,ARG/N", args, NULL);
    if (rd) {
        if (args[0]) { strncpy((char *)lever, (char *)args[0], sizeof(lever) - 1); }
        if (args[1]) { pwr = (ULONG)*(LONG *)args[1]; }
        if (args[2]) { msg = (ULONG)*(LONG *)args[2]; }
        if (args[3]) { arg = (ULONG)*(LONG *)args[3]; }
        FreeArgs(rd);
    }

    PLOG("=== GFX-reset lever probe (lever=%s pwr=%lu msg=0x%02lx arg=0x%08lx) ===",
         (char *)lever, (unsigned long)pwr, (unsigned long)msg, (unsigned long)arg);
    SHELL("=== GFX-reset lever probe (lever=%s pwr=%lu msg=0x%02lx) ===",
          (char *)lever, (IPTR)pwr, (IPTR)msg);

    amdgpuBase.pciDeviceAB = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDevice);
    amdgpuBase.pciDriverAB = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDriver);
    if (!amdgpuBase.pciDeviceAB) { PLOG("no PCI hidd"); SHELL("FAIL: no PCI hidd"); return 20; }

    if (!amdgpu_pci_probe(&amdgpuBase)) { PLOG("no AMD GPU found"); SHELL("FAIL: no AMD GPU"); goto out; }

    ForeachNode(&amdgpuBase.devices, ad) {
        PLOG("device %04x:%04x", ad->vendorid, ad->deviceid);
        if (!amdgpu_mmio_map(&amdgpuBase, ad)) { PLOG("mmio map failed"); continue; }
        ad->vram_mb = amdgpu_get_vram_mb(ad);
        amdgpu_discovery_init(ad, ad->vram_mb);
        PLOG("gc_seg[0]=0x%05lx gc_seg[1]=0x%05lx smu_base=0x%05x vram=%luMB",
             (unsigned long)ad->gc_seg[0], (unsigned long)ad->gc_seg[1],
             (unsigned)ad->ip.smu.base, (unsigned long)ad->vram_mb);

        /* Read-only levers must stay read-only: write_test() stores 0xDEADBEEF
         * into GFX_IMU_RLC_RAM_ADDR_HIGH, which was harmless while GC sat in
         * reset during bring-up but not on a live GPU whose RLC is running. */
        if (strcmp((char *)lever, "metrics") && strcmp((char *)lever, "smuinfo")) {
            write_test(ad, "BEFORE");
            gfx_state(ad, "BEFORE");
        }

        if      (!strcmp((char *)lever, "mode1")) amdgpu_mode1_reset(ad);
        else if (!strcmp((char *)lever, "smuinfo"))  smu_info(ad);
        else if (!strcmp((char *)lever, "metrics"))  smu_metrics(ad);
        else if (!strcmp((char *)lever, "smuen"))    lever_smu_enable(ad, FALSE);
        else if (!strcmp((char *)lever, "smumask"))  lever_smu_enable(ad, TRUE);
        /* The faithful one: full Linux smu_smc_hw_setup() prefix + enable.
         * smuen/smumask omit the driver/tools DRAM table registration, which
         * Linux requires before it will reach EnableAllSmuFeatures.
         * PWR selects the FEATURE_PWR_DOMAIN_e arg — default 4 = FEATURE_PWR_GFX,
         * the value smu_v14_0_2_enable_gfx_features() uses to power just the GFX
         * block. Pass PWR=0 for FEATURE_PWR_ALL (what smu_smc_hw_setup sends, and
         * what stalls this PMFW). One attempt per boot: a stall wedges the
         * mailbox for every later message. */
        else if (!strcmp((char *)lever, "smudump"))  lever_smu_dump(ad, pwr);
        else if (!strcmp((char *)lever, "mode1watch")) lever_mode1watch(ad);
        /* Run the VBIOS ASIC_Init command table (Linux amdgpu_device_asic_init).
         * The oracle says this is what walks RESET_CTRL 0x30 -> 0x7F. Running it
         * here tests the hypothesis with no reboot and no boot-path change. */
        else if (!strcmp((char *)lever, "asicinit")) amdgpu_atom_asic_init(ad);
        else if (!strcmp((char *)lever, "d3d0"))     lever_d3d0(ad);
        else if (!strcmp((char *)lever, "smumsg"))   lever_smu_msg(ad, msg, arg);
        else if (!strcmp((char *)lever, "gfxpower")) lever_gfxpower(ad);
        else if (!strcmp((char *)lever, "smufull")) { smu_info(ad);
                                                      amdgpu_smu_full_setup(ad, pwr);
                                                      smu_info(ad);
                                                      pspin(5000000); }
        else if (!strcmp((char *)lever, "grbm"))  lever_grbm(ad);
        else if (!strcmp((char *)lever, "flr"))   lever_flr(ad);
        else if (!strcmp((char *)lever, "nbifdump")) lever_nbifdump(ad);
        else if (!strcmp((char *)lever, "f0clear")) clear_f0_strap(ad);
        else if (!strcmp((char *)lever, "f0mode1")) { clear_f0_strap(ad); amdgpu_mode1_reset(ad); }
        else if (!strcmp((char *)lever, "f0flr"))   { clear_f0_strap(ad); lever_flr(ad); }
        else if (!strcmp((char *)lever, "f0grbm"))  { clear_f0_strap(ad); lever_grbm(ad); }
        /* cpring: call the CP_RB0 graphics-ring bring-up DIRECTLY. On a booted DUT
         * the rail is already powered + autoloaded (BEFORE GFX shows BOOTLOAD
         * COMPLETE=1), so we skip gfx_v12_wait_psp_autoload — on a re-attach its
         * exit condition (CP_STAT==0) never trips and it floods. This runs the ring
         * setup + PM4 WRITE_DATA fence. CP output is on serial (bug()). */
        else if (!strcmp((char *)lever, "cpring")) gfx_v12_cp_gfx_ring_bringup(ad);
        /* fullinit: run the COMPLETE boot IP ladder (nbio->gmc->psp->smu->gfx->
         * sdma->dcn) via amdgpu_device_init — the same sequence Amdgpu_Init runs
         * at boot. This powers the GFX rail (PSP autoload + SMU feature-enable)
         * BEFORE the CP/MEC/MES bring-up, so the MES SET_HW_RESOURCES probe sees
         * the real boot-path state (not the "contaminated" bare re-attach). All
         * output is on serial (bug()): the PKT dump + MES version + SET_HW result. */
        else if (!strcmp((char *)lever, "fullinit")) amdgpu_device_init(&amdgpuBase, ad);
        /* gartcp: on a UEFI-warm re-attach the PSP is already up, so our PSP block
         * skips its fw-load — and with it amdgpu_gart_enable() — leaving no GART, so
         * cpring bails "no GART ring buffer". Here we force-build the GART arena+table
         * (VRAM MC base 0x80_00000000, derived by the PSP block from MMHUB FB_LOCATION)
         * and THEN run the CP/MES bring-up directly (bypassing the CP_STAT==0 autoload
         * wait that never trips on a warm CP). This lets the MES SET_HW_RESOURCES probe
         * (packet dump + MES version) run in the fast loop without an ISO boot. */
        else if (!strcmp((char *)lever, "gartcp")) {
            BOOL g = amdgpu_gart_enable(ad, 0x8000000000ULL);
            PLOG("gartcp: amdgpu_gart_enable -> %s (gart_ready=%d)", g ? "OK" : "FAIL", (int)ad->gart_ready);
            gfx_v12_cp_gfx_ring_bringup(ad);
        }
        else                                      PLOG("lever 'none' — baseline only");

        if (strcmp((char *)lever, "none") != 0 &&
            strcmp((char *)lever, "metrics") && strcmp((char *)lever, "smuinfo")) {
            write_test(ad, "AFTER");
            gfx_state(ad, "AFTER");
        }
    }

out:
    if (amdgpuBase.pciDeviceAB) OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDevice);
    if (amdgpuBase.pciDriverAB) OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDriver);
    PLOG("done");
    SHELL("done");
    return 0;
}
