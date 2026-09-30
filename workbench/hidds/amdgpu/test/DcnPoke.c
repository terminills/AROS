/*
 * DcnPoke — read/write DCN registers on the live display, from the shell.
 *
 * Exists because the hardware cursor cost this project a 35-minute
 * seed/deploy/boot cycle PER HYPOTHESIS, and most of those cycles only
 * established what was NOT wrong. With the NetShell bridge up, this turns a
 * register experiment into seconds:
 *
 *   DcnPoke REG=0x679                 # read CURSOR0_0_CURSOR_CONTROL
 *   DcnPoke REG=0x679 VAL=0x03000305  # write it, report before/after
 *   DcnPoke REG=0xcc5 SET=0x00055500  # OR these bits in (read-modify-write)
 *   DcnPoke REG=0xcc5 CLR=0x00000010  # AND-NOT these bits out
 *   DcnPoke CURSOR                    # dump the whole pipe-0 cursor block
 *
 * All offsets are dmu_seg[2]-relative dwords, matching dcn_4_1_0_offset.h and
 * every other tool here. Writes are reported with a readback, because a
 * register that silently refuses a write is exactly the failure mode that hid
 * the unmapped-cursor-aperture bug for days.
 *
 * SAFETY: this pokes the live display controller. A bad write can scramble or
 * blank the panel until reboot. That is an acceptable cost on a test rig and a
 * deliberate one -- the alternative is another 35-minute cycle per guess.
 */
#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/oop.h>
#include <hidd/pci.h>

#include <string.h>

#include "amdgpu_intern.h"

#define PLOG(...)  do { bug("[DcnPoke] " __VA_ARGS__); bug("\n"); } while (0)
#define SHELL(...) do { Printf((CONST_STRPTR)"[DcnPoke] " __VA_ARGS__); \
                        Printf((CONST_STRPTR)"\n"); } while (0)

struct AmdgpuBase amdgpuBase;

/* pipe-0 cursor block, dmu_seg[2]-relative */
static const struct { ULONG off; const char *name; } curblk[] = {
    { 0x0679, "CURSOR_CONTROL       " },
    { 0x067a, "CURSOR_SURFACE_ADDR  " },
    { 0x067b, "CURSOR_SURF_ADDR_HIGH" },
    { 0x067c, "CURSOR_SIZE          " },
    { 0x067d, "CURSOR_POSITION      " },
    { 0x067e, "CURSOR_HOT_SPOT      " },
    { 0x0680, "CURSOR_DST_OFFSET    " },
    { 0x0653, "HUBPREQ_CURSOR_SETTNG" },
    /* TTU / QoS — the fetch-scheduling side. Linux programs DCN_CUR0_TTU_CNTL0
     * with REFCYC_PER_REQ_DELIVERY / QoS_LEVEL_FIXED / QoS_RAMP_DISABLE for the
     * cursor; we never have. If the cursor's delivery budget is 0 while the
     * plane's (SURF0) is not, the cursor fetch has no schedule -- which looks
     * exactly like a correctly-programmed sprite that never composites. */
    { 0x0622, "DCN_GLOBAL_TTU_CNTL  " },
    { 0x0623, "DCN_SURF0_TTU_CNTL0  " },
    { 0x0627, "DCN_CUR0_TTU_CNTL0   " },
    { 0x0628, "DCN_CUR0_TTU_CNTL1   " },
    { 0x0cf1, "CM_CUR0_CURSOR0_CTRL " },
    { 0x0cf6, "CM_CUR0_MATRIX_MODE  " },
    { 0x0cc5, "DPP_TOP0_DPP_CONTROL " },
    { 0x0d08, "DSCL0_SCL_MODE       " },
    { 0x0d1e, "DSCL0_RECOUT_START   " },
    { 0x0d1f, "DSCL0_RECOUT_SIZE    " },
    { 0x060a, "HUBP_PRIMARY_SURF    " },
    { 0x1b43, "OTG0_OTG_CONTROL     " },
    { 0, NULL }
};

int main(void)
{
    struct AmdgpuDevice *ad;
    struct RDArgs *rd;
    IPTR args[6] = { 0, 0, 0, 0, 0, 0 };
    LONG reg = -1, val = -1, setb = -1, clrb = -1;
    BOOL dumpcur = FALSE, dumpmtrr = FALSE;

    rd = ReadArgs((CONST_STRPTR)"REG/K/N,VAL/K/N,SET/K/N,CLR/K/N,CURSOR/S,MTRR/S", args, NULL);
    if (rd) {
        if (args[0]) reg  = *(LONG *)args[0];
        if (args[1]) val  = *(LONG *)args[1];
        if (args[2]) setb = *(LONG *)args[2];
        if (args[3]) clrb = *(LONG *)args[3];
        dumpcur  = args[4] ? TRUE : FALSE;
        dumpmtrr = args[5] ? TRUE : FALSE;
        FreeArgs(rd);
    }

    /*
     * MTRR dump. The WC mapping was accepted (KrnMapGlobal -> 0) and CPU-via-BAR
     * did not move off 7115 KiB/s, so PAT alone is not deciding the memory type.
     * On x86 the effective type is the COMBINATION of MTRR and PAT, and UC wins:
     * where an MTRR marks a range UC, PAT cannot upgrade it to WC. If the BIOS
     * covers the GPU BAR with a UC MTRR -- or the default type is UC and no
     * variable range covers it -- the PAT request is simply overridden.
     */
    if (dumpmtrr) {
        ULONG lo, hi, i, vcnt;

        asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xFE));      /* IA32_MTRRCAP */
        vcnt = lo & 0xFF;
        SHELL("MTRRCAP=%08lx:%08lx  vcnt=%lu  fix=%lu  wc=%lu",
              (IPTR)hi, (IPTR)lo, (IPTR)vcnt, (IPTR)((lo >> 8) & 1), (IPTR)((lo >> 10) & 1));

        asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x2FF));     /* IA32_MTRR_DEF_TYPE */
        SHELL("DEF_TYPE=%08lx:%08lx  default=%lu (0=UC 1=WC 4=WT 5=WP 6=WB)  E=%lu FE=%lu",
              (IPTR)hi, (IPTR)lo, (IPTR)(lo & 0xFF), (IPTR)((lo >> 11) & 1), (IPTR)((lo >> 10) & 1));

        if (vcnt > 16) vcnt = 16;
        for (i = 0; i < vcnt; i++) {
            ULONG blo, bhi, mlo, mhi;
            asm volatile ("rdmsr" : "=a"(blo), "=d"(bhi) : "c"(0x200 + i * 2));
            asm volatile ("rdmsr" : "=a"(mlo), "=d"(mhi) : "c"(0x201 + i * 2));
            if (!(mlo & (1UL << 11)))
                continue;                     /* V bit clear: range unused */
            SHELL("  MTRR%lu base=%08lx%08lx type=%lu mask=%08lx%08lx",
                  (IPTR)i, (IPTR)bhi, (IPTR)(blo & ~0xFFFUL), (IPTR)(blo & 0xFF),
                  (IPTR)mhi, (IPTR)(mlo & ~0xFFFUL));
        }
        SHELL("fb aperture is 0x70_00000000 — check whether any range above covers it");
        goto out;
    }

    if (reg < 0 && !dumpcur) {
        SHELL("usage: DcnPoke REG=0x679 [VAL=x | SET=x | CLR=x] | DcnPoke CURSOR");
        return 20;
    }

    amdgpuBase.pciDeviceAB = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDevice);
    amdgpuBase.pciDriverAB = OOP_ObtainAttrBase((STRPTR)IID_Hidd_PCIDriver);
    if (!amdgpuBase.pciDeviceAB) { SHELL("FAIL: no PCI hidd"); return 20; }
    if (!amdgpu_pci_probe(&amdgpuBase)) { SHELL("FAIL: no AMD GPU"); goto out; }

    ForeachNode(&amdgpuBase.devices, ad) {
        ULONG base;

        if (!amdgpu_mmio_map(&amdgpuBase, ad)) { SHELL("FAIL: mmio map"); continue; }
        ad->vram_mb = amdgpu_get_vram_mb(ad);
        amdgpu_discovery_init(ad, ad->vram_mb);
        if (!ad->dmu_seg[2]) { SHELL("FAIL: no DMU segment"); continue; }
        base = ad->dmu_seg[2];

        if (dumpcur) {
            int i;
            SHELL("pipe-0 cursor block (dmu_seg[2]=0x%05lx):", (IPTR)base);
            for (i = 0; curblk[i].name; i++) {
                ULONG v = amdgpu_rreg32(ad, base + curblk[i].off);
                PLOG("  %s @0x%04lx = %08lx", curblk[i].name,
                     (unsigned long)curblk[i].off, (unsigned long)v);
                SHELL("  %s 0x%04lx = %08lx", curblk[i].name,
                      (IPTR)curblk[i].off, (IPTR)v);
            }
            continue;
        }

        {
            ULONG before = amdgpu_rreg32(ad, base + (ULONG)reg);
            ULONG want   = before;

            if (val  >= 0) want = (ULONG)val;
            if (setb >= 0) want = before | (ULONG)setb;
            if (clrb >= 0) want = before & ~(ULONG)clrb;

            if (want == before && val < 0 && setb < 0 && clrb < 0) {
                PLOG("reg 0x%04lx = %08lx", (unsigned long)reg, (unsigned long)before);
                SHELL("0x%04lx = %08lx", (IPTR)reg, (IPTR)before);
            } else {
                ULONG after;
                amdgpu_wreg32(ad, base + (ULONG)reg, want);
                after = amdgpu_rreg32(ad, base + (ULONG)reg);
                PLOG("reg 0x%04lx: %08lx -> wrote %08lx -> reads %08lx %s",
                     (unsigned long)reg, (unsigned long)before, (unsigned long)want,
                     (unsigned long)after, (after == want) ? "OK" : "*** REFUSED ***");
                SHELL("0x%04lx: %08lx -> %08lx -> reads %08lx %s",
                      (IPTR)reg, (IPTR)before, (IPTR)want, (IPTR)after,
                      (after == want) ? (IPTR)"OK" : (IPTR)"*** REFUSED ***");
            }
        }
    }

out:
    if (amdgpuBase.pciDeviceAB) OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDevice);
    if (amdgpuBase.pciDriverAB) OOP_ReleaseAttrBase((STRPTR)IID_Hidd_PCIDriver);
    return 0;
}
