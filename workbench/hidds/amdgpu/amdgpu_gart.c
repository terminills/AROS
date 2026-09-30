/*
 * amdgpu.hidd — PCIE GART (VMID0) enable for GFX12 / Navi44 (RX 9060 XT).
 *
 * WHY THIS EXISTS
 * ---------------
 * The PSP GFX-firmware autoload set (RLC / CP-PFP/ME/MEC / MES) rejected our
 * LOAD_IP_FW with status 0xffff0006 while address-tolerant SMC/IMU accepted the
 * same VRAM-staged buffers. A live kprobe on Linux amdgpu's psp_cmd_submit_buf
 * on THIS exact card (kernel 7.0) proved the difference is the ADDRESS DOMAIN:
 * Linux stages every PSP firmware buffer in *system RAM*, maps it through a 512 MiB
 * PCIE GART (aperture 0x0-0x1FFFFFFF, page-table in VRAM), and hands the PSP a
 * GART VA (0x700000..0xEEB000). We handed raw VRAM MC addresses (0x80_00000000+);
 * the PSP resolves the autoload set's source through the GART, so ours parsed to
 * nothing. Firmware was ruled out — our gc_12_0_0_toc.bin is byte-identical to the
 * DUT's and every fw size matches. See qemu-test/linux-psp-trace/
 * GMC12_GART_ENABLE_SPEC.md + evidence-gart-confirmed (JOB-0084).
 *
 * WHAT THIS DOES
 * --------------
 * Faithful reimplementation of the pre-PSP gmc_v12_0 / mmhub_v4_1_0 GART enable:
 *   1. Allocate a contiguous-ish system-RAM staging arena; get its bus address(es)
 *      via CachePreDMA (the AROS DMA-address primitive, same path okm uses).
 *   2. Build a flat 1-level GART page table (131072 x 8-byte GFX12 PTEs = 1 MiB)
 *      in VRAM, mapping GART VAs -> the arena's physical pages (VALID|SYSTEM|
 *      SNOOPED|R/W|EXE|MTYPE_UC|IS_PTE), rest invalid (0).
 *   3. Program the MMHUB VMID0 GART/aperture/default-page/TLB/L2/context state.
 *      MP0/PSP clients ride MMHUB; Linux does not enable GFXHUB until gfx_v12_0.
 *   4. Flush: VRAM readback + MMHUB engine-17 VMID0 invalidate.
 * The PSP GFX-fw path (amdgpu_psp.c) then stages into the arena and passes GART VAs.
 *
 * DISPLAY SAFETY: only GART-aperture VAs (0x0-0x1FFFFFFF) route through our page
 * table; the live GOP scanout framebuffer sits at a VRAM MC address (0x80_...),
 * inside the system/FB aperture, so it bypasses the GART untouched. (Same class of
 * GMC re-touch as the display-safe MODE1 reset, 23cce639e4.)
 *
 * All register dword offsets below are from gc_12_0_0_offset.h / mmhub_4_1_0_offset.h
 * (BASE_IDX 0), verified against the Linux tree. amdgpu_rreg32/_wreg32 take a DWORD
 * offset into the MMIO aperture; GFXHUB regs add gc_seg[0], MMHUB regs add mmhub.base.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/semaphores.h>

#include "amdgpu_intern.h"
#include "amdgpu_gart.h"

/* CachePreDMA flags (not exported in a public header; drivers define locally — see
 * workbench/hidds/okm/okm_fwsec.c and rom/devs/nvme). */
#ifndef DMA_ReadFromRAM
#define DMA_ReadFromRAM (1L << 0)   /* DMA goes RAM -> device */
#endif
#ifndef DMA_Continue
#define DMA_Continue    (1L << 1)   /* continuation of a split PreDMA request */
#endif

/* ---- GART geometry (matches Linux gmc_v12_0 on Navi44) ---- */
#define GART_APERTURE_SIZE   ((UQUAD)512 << 20)          /* 0x20000000, 512 MiB */
#define GART_START           ((UQUAD)0)
#define GART_END             (GART_APERTURE_SIZE - 1)    /* 0x1FFFFFFF */
#define GART_PAGE_SHIFT      12
#define GART_PAGE_SIZE       (1UL << GART_PAGE_SHIFT)    /* 4096 */
#define GART_NUM_PAGES       (ULONG)(GART_APERTURE_SIZE >> GART_PAGE_SHIFT) /* 131072 */
#define GART_TABLE_SIZE      (GART_NUM_PAGES * 8)        /* 0x100000, 1 MiB */

/* Table lives in VRAM at a fixed offset clear of the framebuffer (low VRAM), the
 * PSP ring/cmd/fence (256-280 MiB), and the top-of-VRAM PSP TMR (~15.9 GiB). */
#define GART_TABLE_VRAM_OFF  ((UQUAD)512 << 20)          /* 0x20000000 */

/* System-RAM fw staging arena, GART-mapped starting at this GART VA. 32 MiB holds
 * the full sequentially-staged GFX fw set with headroom. */
#define GART_ARENA_SIZE      (32UL << 20)                /* 0x2000000 */
#define GART_FW_VA           ((UQUAD)1 << 20)            /* 0x100000: leave page 0 unmapped */

/* ---- GFX12 PTE bit layout (gmc_v12_0.c:450-468, amdgpu_vm.h) ---- */
#define PTE_ADDR_MASK        0x0000FFFFFFFFF000ULL       /* phys page addr bits 47:12 */
/* VALID(0)|SYSTEM(1)|SNOOPED(2)|EXE(4)|READ(5)|WRITE(6)|MTYPE_UC(3<<54)|IS_PTE(63) */
#define PTE_SYS_FLAGS        0x80C0000000000077ULL
/* BAR-mapped local VRAM is WC, not cached system RAM. Start uncached at the
 * GPU as well: VALID|EXE|READ|WRITE|MTYPE_UC|IS_PTE, no SYSTEM or SNOOPED. */
#define PTE_VRAM_FLAGS       0x80C0000000000071ULL

struct GartMapping {
    struct Node node;
    struct AmdgpuDevice *ad;
    UQUAD va;
    ULONG size, first_idx, npages;
};

struct AmdgpuGartRange {
    struct AmdgpuDevice *ad;
    ULONG first_idx, npages;
    BOOL attempted, bound;
};

/* Serializes dynamic VA reservation, PTE updates, and exact unmap ownership.
 * No BO/pool lock is acquired here. Only normal task-context callers may map.
 * Firmware table setup runs serially in the device ladder, before publication.
 */
static struct SignalSemaphore gart_map_lock;
static struct List gart_mappings;
static BOOL gart_service_ready;

void amdgpu_gart_service_init(void)
{
    if (gart_service_ready) return;
    InitSemaphore(&gart_map_lock);
    NEWLIST(&gart_mappings);
    /* Called once at module init, never lazily from concurrent map callers. */
    gart_service_ready = TRUE;
    bug("[amdgpu-gart] map-guards-v1: serialized maps, exact unmaps, no VA reuse\n");
}

/* ---- GFXHUB register dword offsets (add gc_seg[0]) ---- */
#define regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32   0x168f
#define regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32   0x1690
#define regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32  0x16af
#define regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32  0x16b0
#define regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32    0x16cf
#define regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32    0x16d0
#define regGCMC_VM_AGP_TOP                           0x1616
#define regGCMC_VM_AGP_BOT                           0x1617
#define regGCMC_VM_AGP_BASE                          0x1618
#define regGCMC_VM_SYSTEM_APERTURE_LOW_ADDR          0x1619
#define regGCMC_VM_SYSTEM_APERTURE_HIGH_ADDR         0x161a
#define regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB  0x15a8
#define regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB  0x15a9
#define regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32 0x15d4
#define regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32 0x15d5
#define regGCMC_VM_MX_L1_TLB_CNTL                    0x161b
#define regGCVM_L2_CNTL                              0x15c4
#define regGCVM_L2_CNTL2                             0x15c5
#define regGCVM_L2_CNTL3                             0x15c6
#define regGCVM_L2_CNTL4                             0x15dd
#define regGCVM_L2_CNTL5                             0x15e3
#define regGCVM_CONTEXT0_CNTL                        0x1624
#define regGCVM_INVALIDATE_ENG0_REQ                  0x1647
#define regGCVM_INVALIDATE_ENG0_ACK                  0x1659

/* ---- MMHUB (mmhub_4_1_0) register dword offsets (add ip.mmhub.base) ---- */
#define regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32   0x05cf
#define regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32   0x05d0
#define regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32  0x05ef
#define regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32  0x05f0
#define regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32    0x060f
#define regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32    0x0610
#define regMMMC_VM_AGP_TOP                           0x0556
#define regMMMC_VM_AGP_BOT                           0x0557
#define regMMMC_VM_AGP_BASE                          0x0558
#define regMMMC_VM_SYSTEM_APERTURE_LOW_ADDR          0x0559
#define regMMMC_VM_SYSTEM_APERTURE_HIGH_ADDR         0x055a
#define regMMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB  0x04c8
#define regMMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB  0x04c9
#define regMMMC_VM_MX_L1_TLB_CNTL                    0x055b
#define regMMVM_L2_CNTL                              0x04e4
#define regMMVM_L2_CNTL2                             0x04e5
#define regMMVM_L2_CNTL3                             0x04e6
#define regMMVM_L2_PROTECTION_FAULT_CNTL2            0x04ed
#define regMMVM_L2_PROTECTION_FAULT_STATUS_LO32      0x04f0
#define regMMVM_L2_PROTECTION_FAULT_STATUS_HI32      0x04f1
#define regMMVM_L2_PROTECTION_FAULT_ADDR_LO32        0x04f2
#define regMMVM_L2_PROTECTION_FAULT_ADDR_HI32        0x04f3
#define regMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32 0x04f4
#define regMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32 0x04f5
#define regMMMC_VM_FB_OFFSET                          0x04c7
#define regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32  0x04f7
#define regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32  0x04f8
#define regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32 0x04f9
#define regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32 0x04fa
#define regMMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32     0x04fb
#define regMMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32     0x04fc
#define regMMVM_L2_CNTL4                             0x04fd
#define regMMVM_L2_CNTL5                             0x0503
#define regMMVM_CONTEXT0_CNTL                        0x0564
#define regMMVM_INVALIDATE_ENG0_SEM                  0x0575
#define regMMVM_INVALIDATE_ENG0_REQ                  0x0587
#define regMMVM_INVALIDATE_ENG0_ACK                  0x0599
#define regMMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32      0x05ab
#define regMMVM_INVALIDATE_ENG0_ADDR_RANGE_HI32      0x05ac

/* Engine 17 is the GART/VMID0 invalidation engine; eng_distance = 1 dword. */
#define GART_INVAL_ENG       17
/* PER_VMID(0)=1 | INVALIDATE L2_PTES(19)|PDE0(20)|PDE1(21)|PDE2(22)|L1_PTES(23) */
#define GART_INVAL_REQ       0x00F80001UL

static inline void gwr(struct AmdgpuDevice *ad, ULONG base, ULONG off, ULONG val)
{ amdgpu_wreg32(ad, base + off, val); }
static inline ULONG grd(struct AmdgpuDevice *ad, ULONG base, ULONG off)
{ return amdgpu_rreg32(ad, base + off); }

void amdgpu_gart_dump_fault(struct AmdgpuDevice *ad, const char *tag)
{
    ULONG base = ad->ip.mmhub.base;
    ULONG status_lo, status_hi, addr_lo, addr_hi;
    UQUAD fault_va;

    if (!ad->ip.mmhub.valid || !base)
        return;

    status_lo = grd(ad, base, regMMVM_L2_PROTECTION_FAULT_STATUS_LO32);
    status_hi = grd(ad, base, regMMVM_L2_PROTECTION_FAULT_STATUS_HI32);
    addr_lo = grd(ad, base, regMMVM_L2_PROTECTION_FAULT_ADDR_LO32);
    addr_hi = grd(ad, base, regMMVM_L2_PROTECTION_FAULT_ADDR_HI32);
    fault_va = (((UQUAD)(addr_hi & 0xFUL) << 32) | addr_lo) << 12;

    bug("[amdgpu-gart] FAULT %-14s status=0x%08lx%08lx addr=0x%08lx%08lx "
        "va=0x%08lx%08lx cid=0x%02lx vmid=%lu rw=%lu walker=%lu perm=0x%lx map=%lu more=%lu\n",
        tag ? tag : "(none)",
        (unsigned long)status_hi, (unsigned long)status_lo,
        (unsigned long)addr_hi, (unsigned long)addr_lo,
        (unsigned long)(fault_va >> 32), (unsigned long)fault_va,
        (unsigned long)((status_lo >> 9) & 0x1FF),
        (unsigned long)((status_lo >> 20) & 0xF),
        (unsigned long)((status_lo >> 18) & 1),
        (unsigned long)((status_lo >> 1) & 7),
        (unsigned long)((status_lo >> 4) & 0xF),
        (unsigned long)((status_lo >> 8) & 1),
        (unsigned long)(status_lo & 1));
}

/* Program one hub's VMID0/GART aperture + TLB + L2 + context registers.
 * Linux's non-translate_further path sets BANK_SELECT=9 and
 * L2_CACHE_BIGK_FRAGMENT_SIZE=6 on both hubs, yielding 0x80130009. */
static void gart_program_hub(struct AmdgpuDevice *ad, ULONG base, BOOL mmhub,
                             UQUAD pt_base, UQUAD sys_low, UQUAD sys_high,
                             UQUAD default_mc, UQUAD dummy_phys)
{
    ULONG pt_lo = (ULONG)(pt_base & 0xFFFFFFFFUL);
    ULONG pt_hi = (ULONG)(pt_base >> 32);
    ULONG end_lo = (ULONG)(GART_END >> 12);       /* 0x1FFFF */
    ULONG l2_cntl3 = 0x80130009UL;

    if (!mmhub) {
        /* Step A — GART aperture regs */
        gwr(ad, base, regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, pt_lo);
        gwr(ad, base, regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, pt_hi);
        gwr(ad, base, regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, 0);
        gwr(ad, base, regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, 0);
        gwr(ad, base, regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, end_lo);
        gwr(ad, base, regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, 0);
        /* Step B — system aperture (AGP disabled: BOT>TOP) */
        gwr(ad, base, regGCMC_VM_AGP_BASE, 0);
        gwr(ad, base, regGCMC_VM_AGP_BOT, 0x00FFFFFFUL);
        gwr(ad, base, regGCMC_VM_AGP_TOP, 0);
        gwr(ad, base, regGCMC_VM_SYSTEM_APERTURE_LOW_ADDR, (ULONG)sys_low);
        gwr(ad, base, regGCMC_VM_SYSTEM_APERTURE_HIGH_ADDR, (ULONG)sys_high);
        gwr(ad, base, regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB, (ULONG)(default_mc >> 12));
        gwr(ad, base, regGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB, (ULONG)(default_mc >> 44));
        gwr(ad, base, regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, (ULONG)(dummy_phys >> 12));
        gwr(ad, base, regGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, (ULONG)(dummy_phys >> 44));
        /* Step C — L1 TLB */
        gwr(ad, base, regGCMC_VM_MX_L1_TLB_CNTL, 0x1859);
        /* Step D — L2 cache */
        gwr(ad, base, regGCVM_L2_CNTL,  0x00080801UL);
        gwr(ad, base, regGCVM_L2_CNTL2, 0x00000003UL);
        gwr(ad, base, regGCVM_L2_CNTL3, l2_cntl3);
        gwr(ad, base, regGCVM_L2_CNTL4, 0x000000C1UL);
        gwr(ad, base, regGCVM_L2_CNTL5, 0x00003FE0UL);
        /* Step E — enable VMID0 context (DEPTH=0, flat 1-level GART) */
        gwr(ad, base, regGCVM_CONTEXT0_CNTL, 0x00000001UL);
    } else {
        ULONG tmp;

        gwr(ad, base, regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, pt_lo);
        gwr(ad, base, regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, pt_hi);
        gwr(ad, base, regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, 0);
        gwr(ad, base, regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, 0);
        gwr(ad, base, regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, end_lo);
        gwr(ad, base, regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, 0);
        gwr(ad, base, regMMMC_VM_AGP_BASE, 0);
        gwr(ad, base, regMMMC_VM_AGP_BOT, 0x00FFFFFFUL);
        gwr(ad, base, regMMMC_VM_AGP_TOP, 0);
        gwr(ad, base, regMMMC_VM_SYSTEM_APERTURE_LOW_ADDR, (ULONG)sys_low);
        gwr(ad, base, regMMMC_VM_SYSTEM_APERTURE_HIGH_ADDR, (ULONG)sys_high);
        gwr(ad, base, regMMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB,
            (ULONG)(default_mc >> 12));
        gwr(ad, base, regMMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB,
            (ULONG)(default_mc >> 44));
        gwr(ad, base, regMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32,
            (ULONG)(dummy_phys >> 12));
        gwr(ad, base, regMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32,
            (ULONG)(dummy_phys >> 44));
        tmp = grd(ad, base, regMMVM_L2_PROTECTION_FAULT_CNTL2);
        gwr(ad, base, regMMVM_L2_PROTECTION_FAULT_CNTL2, tmp | 0x00040000UL);
        gwr(ad, base, regMMMC_VM_MX_L1_TLB_CNTL, 0x1859);
        gwr(ad, base, regMMVM_L2_CNTL,  0x00080801UL);
        tmp = grd(ad, base, regMMVM_L2_CNTL2);
        gwr(ad, base, regMMVM_L2_CNTL2, tmp | 0x00000003UL);
        gwr(ad, base, regMMVM_L2_CNTL3, l2_cntl3);
        /* Linux starts from 0xc1 then clears both VMC_TAP_*_PHYSICAL bits. */
        gwr(ad, base, regMMVM_L2_CNTL4, 0x00000001UL);
        gwr(ad, base, regMMVM_L2_CNTL5, 0x00003FE0UL);
        gwr(ad, base, regMMVM_CONTEXT0_CNTL, 0x00000001UL);

        /* Disable context-1 identity translation exactly as mmhub_v4_1_0 does. */
        gwr(ad, base, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32,
            0xFFFFFFFFUL);
        gwr(ad, base, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32,
            0x0000000FUL);
        gwr(ad, base, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32, 0);
        gwr(ad, base, regMMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32, 0);
        gwr(ad, base, regMMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32, 0);
        gwr(ad, base, regMMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32, 0);
    }
}

/* Linux gives every MMHUB invalidation engine the full address range before any
 * flush request. Address-range registers are two dwords apart per engine. */
static void gart_program_mmhub_invalidation(struct AmdgpuDevice *ad, ULONG base)
{
    ULONG i;
    for (i = 0; i < 18; i++) {
        gwr(ad, base, regMMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32 + i * 2,
            0xFFFFFFFFUL);
        gwr(ad, base, regMMVM_INVALIDATE_ENG0_ADDR_RANGE_HI32 + i * 2,
            0x0000001FUL);
    }
}

/* Engine-17 VMID0 invalidate. GFXHUB skips the semaphore; MMHUB acquires/releases
 * it (mmhub_v4_1_0 flush). Poll ACK bit0 (VMID0) with a bounded spin. */
/*
 * Historical bring-up: MMHUB acked promptly while GFXHUB did not. Physical
 * cold42 now records GFXHUB ACKs after wake, but ACK observation alone is not
 * a retirement/coherency proof for reused VAs. Keep the bounded fallback and
 * monotonic allocation until reuse has its own native acceptance.
 *
 * Offsets, engine stride and the request value were all checked against Linux
 * and are correct: eng_distance is 1 on both hubs, engine 17 is the GART engine,
 * and gfxhub_v12_0_get_invalidate_req() for vmid 0 / flush_type 0 encodes to
 * exactly the 0x00F80001 used here (PER_VMID bit 0, L2_PTES/PDE0/PDE1/PDE2/L1
 * bits 19..23). What differs is the ACCESS PATH: gmc_v12_0_flush_vm_hub() reaches
 * GFXHUB through WREG32_RLC_NO_KIQ / RREG32_RLC_NO_KIQ, i.e. RLC-mediated
 * register access, because direct GC writes can be dropped when the RLC is
 * managing the block. Direct MMIO requests may therefore be dropped before
 * wake. Linux also skips the GFXHUB flush entirely when
 * !adev->gfx.is_poweron.
 *
 * So this used to spin 1000000 times per GFXHUB flush — seconds of wall time on
 * every gart_unmap, twice, for an ack that cannot arrive. Now it is bounded and
 * warns ONCE.
 *
 * WHY VA REUSE REMAINS DISABLED:
 * amdgpu_gart_map() is a BUMP allocator, so every mapping gets a fresh VA that
 * has never been translated before and therefore cannot have a stale TLB entry.
 * This avoids reuse of historical GFXHUB translations. The moment a free list lands
 * and VAs get REUSED, a stale GFXHUB translation becomes reachable and this
 * can become data corruption. Do not add VA reuse before validating invalidation
 * and retirement. Fresh VA allocation does not retire outstanding GPU accesses;
 * each caller must complete those before releasing its backing memory.
 */
static BOOL gart_flush_hub(struct AmdgpuDevice *ad, ULONG base, BOOL mmhub)
{
    static BOOL gfxhub_warned = FALSE;
    ULONG req_off = (mmhub ? regMMVM_INVALIDATE_ENG0_REQ : regGCVM_INVALIDATE_ENG0_REQ) + GART_INVAL_ENG;
    ULONG ack_off = (mmhub ? regMMVM_INVALIDATE_ENG0_ACK : regGCVM_INVALIDATE_ENG0_ACK) + GART_INVAL_ENG;
    ULONG sem_off = regMMVM_INVALIDATE_ENG0_SEM + GART_INVAL_ENG;
    ULONG spins   = mmhub ? 100000 : 2000;   /* bound the pre-wake/no-ACK path */
    ULONG i, ack = 0;
    BOOL acquired = !mmhub;

    if (mmhub) {
        for (i = 0; i < 100000; i++) {
            if (grd(ad, base, sem_off) & 1) { acquired = TRUE; break; }
        }
    }
    gwr(ad, base, req_off, GART_INVAL_REQ);
    for (i = 0; i < spins; i++) {
        ack = grd(ad, base, ack_off);
        if (ack & 1) break;
    }
    if (mmhub)
        gwr(ad, base, sem_off, 0);

    if (!mmhub && !(ack & 1)) {
        if (!gfxhub_warned) {
            gfxhub_warned = TRUE;
            bug("[amdgpu-gart] GFXHUB invalidate did not ACK (Linux uses "
                "RLC-mediated register access). Bounded to %lu spins and warned "
                "once. VAs are never reused; caller still owns GPU retirement. "
                "Do NOT add a GART free list without native coherency proof.\n",
                (unsigned long)spins);
        }
        return FALSE;
    }
    D(bug("[amdgpu-gart] %s flush: ack=0x%08lx after %lu spins\n",
          mmhub ? "MMHUB" : "GFXHUB", (unsigned long)ack, (unsigned long)i));
    return acquired && (ack & 1);
}

/* Build the GART page table in a host buffer, mapping the arena's physical pages,
 * then write the whole 1 MiB table into VRAM in one pass. Returns TRUE on success. */
static BOOL gart_build_table(struct AmdgpuDevice *ad)
{
    ULONG *tbl;
    ULONG  off, gpage = 0;
    ULONG  base_idx = (ULONG)(GART_FW_VA >> GART_PAGE_SHIFT);   /* 256 */

    tbl = AllocMem(GART_TABLE_SIZE, MEMF_ANY | MEMF_CLEAR);     /* zero = invalid PTEs */
    if (!tbl) {
        bug("[amdgpu-gart] table AllocMem(0x%lx) failed\n", (unsigned long)GART_TABLE_SIZE);
        return FALSE;
    }

    /* Walk the arena in contiguous physical runs; fill a valid system-RAM PTE per page. */
    off = 0;
    while (off < ad->gart_arena_size) {
        ULONG len = ad->gart_arena_size - off;
        APTR  phys = CachePreDMA(ad->gart_arena + off, &len,
                                 DMA_ReadFromRAM | (off ? DMA_Continue : 0));
        UQUAD run_phys = (UQUAD)(IPTR)phys;
        ULONG p, npages = len >> GART_PAGE_SHIFT;

        if (off == 0)
            ad->gart_arena_phys = (IPTR)phys;
        for (p = 0; p < npages; p++, gpage++) {
            UQUAD page_phys = run_phys + ((UQUAD)p << GART_PAGE_SHIFT);
            UQUAD pte = (page_phys & PTE_ADDR_MASK) | PTE_SYS_FLAGS;
            ULONG idx = base_idx + gpage;
            tbl[idx * 2]     = (ULONG)(pte & 0xFFFFFFFFUL);
            tbl[idx * 2 + 1] = (ULONG)(pte >> 32);
        }
        if (!len) break;
        off += len;
    }

    bug("[amdgpu-gart] arena %lu MiB phys=0x%08lx%08lx (%s) mapped %lu pages @GART VA 0x%08lx\n",
        (unsigned long)(ad->gart_arena_size >> 20),
        (unsigned long)(((UQUAD)ad->gart_arena_phys) >> 32),
        (unsigned long)((IPTR)ad->gart_arena_phys & 0xFFFFFFFFUL),
        ((IPTR)ad->gart_arena_phys & (GART_PAGE_SIZE - 1)) ? "!! MISALIGNED" : "4K-aligned",
        (unsigned long)gpage, (unsigned long)GART_FW_VA);

    /* One-shot write of the whole table into VRAM via the BIF aperture. */
    amdgpu_vram_write(ad, GART_TABLE_VRAM_OFF, tbl, GART_TABLE_SIZE / 4);
    FreeMem(tbl, GART_TABLE_SIZE);

    /* Readback flush: force the BIF write path to land before the GPU walks it. */
    { ULONG fl; amdgpu_vram_read(ad, GART_TABLE_VRAM_OFF + (UQUAD)base_idx * 8, &fl, 1); (void)fl; }
    return TRUE;
}

/* Enable the PCIE GART for VMID0 and stand up the system-RAM fw staging arena.
 * Idempotent: allocates once, (re)programs the hub registers every call (so it
 * survives a MODE1 reset that re-inits the GMC). vram_start = VRAM MC base. */
/*
 * GART allocator: map arbitrary system-RAM buffers so the GPU can reach them.
 *
 * Until now the only system memory the GPU could touch was the single 32 MB
 * arena this file allocates for PSP firmware staging, mapped once at a fixed VA.
 * That is enough for firmware and useless for graphics: an AROS BitMap is an
 * ordinary AllocMem buffer somewhere else entirely, and a blit needs it mapped.
 *
 * The aperture is 512 MB / 131072 pages and the arena occupies only the first
 * 8192 of them, so allocation starts above the arena and hands out the rest.
 *
 * Deliberately a BUMP allocator: VA is never reclaimed, so amdgpu_gart_unmap()
 * invalidates the PTEs but does not return the address range. That is honest for
 * the current consumer (a small number of long-lived bitmap mappings) and wrong
 * for a workload that maps and unmaps continuously; a free list is the obvious
 * next step and is not written yet. Running out is reported, not silently
 * wrapped onto live mappings.
 *
 * Two details that are easy to get wrong and are handled here:
 *   - The buffer need NOT be page-aligned. The containing pages are mapped and
 *     the returned VA carries the same in-page offset, so callers can map a
 *     bitmap wherever AllocMem put it.
 *   - A virtually contiguous AROS buffer is NOT necessarily physically
 *     contiguous, so this walks it in physical runs via CachePreDMA exactly as
 *     gart_build_table() does, rather than assuming one base address.
 */
static void gart_write_pte(struct AmdgpuDevice *ad, ULONG idx, UQUAD pte)
{
    ULONG dw[2];

    dw[0] = (ULONG)(pte & 0xFFFFFFFFUL);
    dw[1] = (ULONG)(pte >> 32);
    amdgpu_vram_write(ad, GART_TABLE_VRAM_OFF + ((UQUAD)idx * 8), dw, 2);
}

/* Invalidate both hubs after a page-table edit. MMHUB alone is not enough:
 * SDMA and the GFX rings translate through GFXHUB, so a stale GFXHUB TLB is
 * exactly the silent-no-op failure mode that cost a boot earlier. */
static BOOL gart_invalidate_all(struct AmdgpuDevice *ad)
{
    BOOL mm = FALSE, gfx = FALSE;
    if (ad->ip.mmhub.valid && ad->ip.mmhub.base)
        mm = gart_flush_hub(ad, ad->ip.mmhub.base, TRUE);
    if (ad->gc_seg[0])
        gfx = gart_flush_hub(ad, ad->gc_seg[0], FALSE);
    return mm && gfx;
}

/* Public-service reservations share the bump cursor with all legacy private
 * mappings. No allocator can overlap the firmware arena or another consumer.
 * requested=0 selects an aligned fresh address; exact requests below the
 * cursor fail even if an old range there has been released. */
struct AmdgpuGartRange *amdgpu_gart_reserve(struct AmdgpuDevice *ad,
    ULONG size, ULONG alignment, UQUAD requested, UQUAD *address)
{
    struct AmdgpuGartRange *r;
    UQUAD first, minimum, end;
    if (!address) return NULL;
    *address = 0;
    if (!gart_service_ready || !ad || !size || (size & (GART_PAGE_SIZE - 1)) ||
        alignment < GART_PAGE_SIZE || (alignment & (alignment - 1)) ||
        requested >= GART_APERTURE_SIZE || (requested & (alignment - 1)))
        return NULL;
    r = AllocMem(sizeof(*r), MEMF_ANY | MEMF_CLEAR);
    if (!r) return NULL;
    ObtainSemaphore(&gart_map_lock);
    if (!ad->gart_ready) goto fail;
    minimum = GART_FW_VA + ad->gart_arena_size;
    first = (UQUAD)ad->gart_next_page << GART_PAGE_SHIFT;
    if (first < minimum) first = minimum;
    first = (first + alignment - 1) & ~((UQUAD)alignment - 1);
    if (requested) {
        if (requested < first) goto fail;
        first = requested;
    }
    end = first + size;
    if (end > GART_APERTURE_SIZE) goto fail;
    r->ad = ad;
    r->first_idx = (ULONG)(first >> GART_PAGE_SHIFT);
    r->npages = size >> GART_PAGE_SHIFT;
    ad->gart_next_page = (ULONG)(end >> GART_PAGE_SHIFT);
    *address = first;
    ReleaseSemaphore(&gart_map_lock);
    return r;
fail:
    ReleaseSemaphore(&gart_map_lock);
    FreeMem(r, sizeof(*r));
    return NULL;
}

static BOOL gart_pte_matches(struct AmdgpuDevice *ad, ULONG idx, UQUAD want)
{
    ULONG dw[2];
    amdgpu_vram_read(ad, GART_TABLE_VRAM_OFF + (UQUAD)idx * 8, dw, 2);
    return (((UQUAD)dw[1] << 32) | dw[0]) == want;
}

BOOL amdgpu_gart_bind_range(struct AmdgpuDevice *ad,
    struct AmdgpuGartRange *r, APTR cpu)
{
    ULONG i;
    BOOL ok = FALSE;
    if (!r || r->ad != ad || !cpu || ((IPTR)cpu & (GART_PAGE_SIZE - 1)))
        return FALSE;
    ObtainSemaphore(&gart_map_lock);
    if (!ad->gart_ready || r->attempted) goto out;
    r->attempted = r->bound = TRUE; /* Retain backing even on partial failure. */
    for (i = 0; i < r->npages; i++) {
        ULONG len = GART_PAGE_SIZE;
        APTR phys = CachePreDMA((UBYTE *)cpu + ((IPTR)i << GART_PAGE_SHIFT),
            &len, DMA_ReadFromRAM | (i ? DMA_Continue : 0));
        UQUAD page = (UQUAD)(IPTR)phys;
        if (!phys || len < GART_PAGE_SIZE || (page & ~PTE_ADDR_MASK)) goto out;
        gart_write_pte(ad, r->first_idx + i, page | PTE_SYS_FLAGS);
        if (!gart_pte_matches(ad, r->first_idx + i, page | PTE_SYS_FLAGS)) goto out;
    }
    ok = gart_invalidate_all(ad);
out:
    ReleaseSemaphore(&gart_map_lock);
    bug("[amdgpu-va] PTE bind idx=%lu pages=%lu readback+both-hub-ack=%s\n",
        (unsigned long)r->first_idx, (unsigned long)r->npages, ok ? "PASS" : "FAIL-pinned");
    return ok;
}

BOOL amdgpu_gart_bind_vram_range(struct AmdgpuDevice *ad,
    struct AmdgpuGartRange *r, UQUAD payload_mc)
{
    UQUAD vram_start, vram_bytes, offset, physical, bytes;
    ULONG i, fb_offset;
    BOOL ok = FALSE;
    if (!r || r->ad != ad || !ad || (payload_mc & (GART_PAGE_SIZE - 1)))
        return FALSE;
    ObtainSemaphore(&gart_map_lock);
    if (!ad->gart_ready || r->attempted || !ad->ip.mmhub.valid ||
        !ad->ip.mmhub.base || ad->gart_table_mc < GART_TABLE_VRAM_OFF) goto out;
    vram_start = ad->gart_table_mc - GART_TABLE_VRAM_OFF;
    vram_bytes = (UQUAD)ad->vram_mb << 20;
    bytes = (UQUAD)r->npages << GART_PAGE_SHIFT;
    if (!bytes || payload_mc < vram_start) goto out;
    offset = payload_mc - vram_start;
    if (offset >= vram_bytes || bytes > vram_bytes - offset) goto out;
    /* Linux amdgpu_vm_bo_update/bo_update_mapping supplies VRAM resource
     * offsets plus vm_manager.vram_base_offset, NOT the CPU BAR or MC window
     * address. Match the same FB_OFFSET conversion already used for our root.
     * The BO service supplies only its own aligned payload's MC address. */
    fb_offset = grd(ad, ad->ip.mmhub.base, regMMMC_VM_FB_OFFSET);
    if (fb_offset == 0xffffffffUL) goto out;
    physical = ((UQUAD)fb_offset << 24) + offset;
    if ((physical & ~PTE_ADDR_MASK) || bytes - GART_PAGE_SIZE > PTE_ADDR_MASK - physical)
        goto out;
    r->attempted = r->bound = TRUE; /* Partial PTE updates retain backing. */
    __sync_synchronize(); /* Drain caller's WC payload stores before exposure. */
    for (i = 0; i < r->npages; i++) {
        UQUAD pte = (physical + ((UQUAD)i << GART_PAGE_SHIFT)) | PTE_VRAM_FLAGS;
        gart_write_pte(ad, r->first_idx + i, pte);
        if (!gart_pte_matches(ad, r->first_idx + i, pte)) goto out;
    }
    ok = gart_invalidate_all(ad);
out:
    ReleaseSemaphore(&gart_map_lock);
    bug("[amdgpu-va] VRAM-PTE-v1 idx=%lu pages=%lu non-SYSTEM-UC readback+both-hub-ack=%s\n",
        (unsigned long)r->first_idx, (unsigned long)r->npages, ok ? "PASS" : "FAIL-pinned");
    return ok;
}

BOOL amdgpu_gart_unbind_range(struct AmdgpuDevice *ad,
    struct AmdgpuGartRange *r)
{
    ULONG i;
    BOOL ok = TRUE;
    if (!r || r->ad != ad) return FALSE;
    ObtainSemaphore(&gart_map_lock);
    if (!ad->gart_ready || !r->bound) { ok = FALSE; goto out; }
    for (i = 0; i < r->npages; i++) {
        gart_write_pte(ad, r->first_idx + i, 0);
        if (!gart_pte_matches(ad, r->first_idx + i, 0)) ok = FALSE;
    }
    if (!gart_invalidate_all(ad)) ok = FALSE;
    if (ok) r->bound = FALSE;
out:
    ReleaseSemaphore(&gart_map_lock);
    bug("[amdgpu-va] PTE unbind idx=%lu pages=%lu readback+both-hub-ack=%s; no VA reuse\n",
        (unsigned long)r->first_idx, (unsigned long)r->npages, ok ? "PASS" : "FAIL-pinned");
    return ok;
}

void amdgpu_gart_release_range(struct AmdgpuGartRange *r)
{
    /* Only the BO service owns these private records. It must hold its lock
     * and release all bindings/GPU refs first. The VA remains quarantined. */
    if (r && !r->bound) FreeMem(r, sizeof(*r));
}

UQUAD amdgpu_gart_map(struct AmdgpuDevice *ad, APTR buf, ULONG size)
{
    ULONG page_off = (ULONG)((IPTR)buf & (GART_PAGE_SIZE - 1));
    ULONG npages, first_idx, gpage = 0, i;
    UQUAD total, arena_end, va;
    struct GartMapping *mapping;
    const char *failure = "invalid physical page";

    if (!gart_service_ready || !ad || !buf || !size ||
        (IPTR)buf > ~(IPTR)0 - (size - 1))
        return 0;

    total = (UQUAD)page_off + size;
    if (total > GART_APERTURE_SIZE) return 0;
    npages = (ULONG)((total + GART_PAGE_SIZE - 1) >> GART_PAGE_SHIFT);
    mapping = AllocMem(sizeof(*mapping), MEMF_ANY | MEMF_CLEAR);
    if (!mapping) return 0;

    ObtainSemaphore(&gart_map_lock);
    if (!ad->gart_ready) goto reject;

    /* First allocation starts above the entire firmware arena. Use wide
     * arithmetic and check before narrowing, including rounded-up end pages. */
    arena_end = (GART_FW_VA + ad->gart_arena_size + GART_PAGE_SIZE - 1)
                >> GART_PAGE_SHIFT;
    if (arena_end >= GART_NUM_PAGES) goto reject;
    if (!ad->gart_next_page)
        ad->gart_next_page = (ULONG)arena_end;

    if (ad->gart_next_page < arena_end ||
        ad->gart_next_page > GART_NUM_PAGES ||
        npages > GART_NUM_PAGES - ad->gart_next_page) {
        bug("[amdgpu-gart] map: out of GART pages (next=%lu need=%lu limit=%lu)\n",
            (unsigned long)ad->gart_next_page, (unsigned long)npages,
            (unsigned long)GART_NUM_PAGES);
        goto reject;
    }

    first_idx = ad->gart_next_page;
    /* Reserve before any PTE write. Even a failed transaction quarantines its
     * VA range: never hand a possibly cached translation to another buffer. */
    ad->gart_next_page += npages;

    /* Resolve one page at a time, and NEVER ask CachePreDMA about memory the
     * caller does not own.
     *
     * The first cut walked from `base` — the page-aligned address BELOW buf —
     * for npages*4096 bytes, which reaches up to 4095 bytes before buf AND up to
     * 4095 bytes past buf+size. Both are foreign allocations. That is wrong on
     * its face (CachePreDMA's contract is about the caller's own buffer) and it
     * is the leading suspect for the heap corruption that showed up as a GP
     * fault in CloseDevice() after the benchmark mapped and unmapped 512 pages.
     *
     * So: query only inside [buf, buf+size). For the first page that means
     * asking about buf itself and subtracting the in-page offset to get the
     * page's physical base; for every later page the page start is already
     * inside the buffer and can be asked about directly.
     *
     * Page-at-a-time rather than walking contiguous runs. Mapping is not a hot
     * path (a BitMap maps once at creation), and the run optimisation is what
     * made the previous version reach outside the buffer in the first place.
     * Correctness first; the run walk can come back later if mapping ever shows
     * up in a profile. */
    for (gpage = 0; gpage < npages; gpage++) {
        ULONG within = (gpage << GART_PAGE_SHIFT);
        ULONG offset = gpage ? 0 : page_off;
        ULONG boff = gpage ? within - page_off : 0;
        APTR  q;
        ULONG len, needed;
        UQUAD page_phys;

        needed = size - boff;
        if (needed > GART_PAGE_SIZE - offset)
            needed = GART_PAGE_SIZE - offset;
        len = needed;
        q = CachePreDMA((APTR)((IPTR)buf + boff), &len,
                       DMA_ReadFromRAM | (gpage ? DMA_Continue : 0));
        if (!q || len < needed) {
            failure = "short/failed CachePreDMA";
            goto fail;
        }
        /* A 4K PTE cannot describe a sub-page discontinuity or silently drop
         * high physical address bits. The DMA address must preserve offset. */
        if (((UQUAD)(IPTR)q & (GART_PAGE_SIZE - 1)) != offset)
            goto fail;
        page_phys = (UQUAD)(IPTR)q - offset;
        if (page_phys & ~PTE_ADDR_MASK) goto fail;

        gart_write_pte(ad, first_idx + gpage,
                       page_phys | PTE_SYS_FLAGS);
    }

    va = ((UQUAD)first_idx << GART_PAGE_SHIFT) + page_off;

    /* Force the PTE writes through the BIF path before the GPU walks them, then
     * drop stale translations in both hubs. */
    { ULONG fl; amdgpu_vram_read(ad, GART_TABLE_VRAM_OFF + ((UQUAD)first_idx * 8), &fl, 1); (void)fl; }
    gart_invalidate_all(ad);

    mapping->ad = ad;
    mapping->va = va;
    mapping->size = size;
    mapping->first_idx = first_idx;
    mapping->npages = npages;
    AddTail(&gart_mappings, &mapping->node);
    ReleaseSemaphore(&gart_map_lock);

    bug("[amdgpu-gart] map: cpu %p size %lu -> VA 0x%08lx%08lx (%lu pages @idx %lu)\n",
        buf, (unsigned long)size,
        (unsigned long)(va >> 32), (unsigned long)(ULONG)va,
        (unsigned long)npages, (unsigned long)first_idx);
    return va;

fail:
    for (i = 0; i < gpage; i++)
        gart_write_pte(ad, first_idx + i, 0);
    if (gpage) {
        ULONG fl;
        amdgpu_vram_read(ad, GART_TABLE_VRAM_OFF + ((UQUAD)first_idx * 8), &fl, 1);
        gart_invalidate_all(ad);
    }
    bug("[amdgpu-gart] map: %s at page %lu/%lu cpu %p; range quarantined\n",
        failure, (unsigned long)gpage, (unsigned long)npages, buf);
reject:
    ReleaseSemaphore(&gart_map_lock);
    FreeMem(mapping, sizeof(*mapping));
    return 0;
}

void amdgpu_gart_unmap(struct AmdgpuDevice *ad, UQUAD va, ULONG size)
{
    struct GartMapping *mapping;
    ULONG i, fl;

    if (!gart_service_ready || !ad || !va || !size)
        return;

    ObtainSemaphore(&gart_map_lock);
    if (ad->gart_ready) {
        ForeachNode(&gart_mappings, mapping) {
            if (mapping->ad != ad || mapping->va != va || mapping->size != size)
                continue;
            /* Only the exact successful mapping owns these bounded entries.
             * The caller must already have retired every GPU use of its buffer. */
            for (i = 0; i < mapping->npages; i++)
                gart_write_pte(ad, mapping->first_idx + i, 0);
            amdgpu_vram_read(ad, GART_TABLE_VRAM_OFF +
                            ((UQUAD)mapping->first_idx * 8), &fl, 1);
            gart_invalidate_all(ad);
            Remove(&mapping->node);
            ReleaseSemaphore(&gart_map_lock);
            FreeMem(mapping, sizeof(*mapping));
            return;
        }
    }
    ReleaseSemaphore(&gart_map_lock);
    bug("[amdgpu-gart] unmap rejected: no exact live mapping VA=0x%08lx%08lx size=%lu\n",
        (unsigned long)(va >> 32), (unsigned long)(ULONG)va, (unsigned long)size);
}

/*
 * Program GFXHUB's VMID0 page table — AFTER the GFX rail is powered.
 *
 * amdgpu_gart_enable() deliberately programs MMHUB only, because it runs during
 * PSP init when the GFX block is still dark, and this file already documented
 * the consequence: "GFXHUB is initialized later by gfx_v12_0 after the GFX block
 * is powered, so pre-PSP writes there drop." The diagnostic printed it every
 * boot — GFXHUB ctx0_base_lo=0x00000000 — and it was harmless while nothing used
 * GFXHUB, since MP0/PSP clients ride MMHUB.
 *
 * SDMA does NOT ride MMHUB. It translates through GFXHUB, so with GFXHUB's page
 * table left at zero a system-memory copy retires cleanly and moves nothing:
 * measured exactly that way, 256/256 dwords untouched in both directions with no
 * GART fault raised, because there was no translation to fault on.
 *
 * This is only callable now. Before the SMU feature-enable landed the GFX rail
 * was off and these writes would have gone into a dead power island — which is
 * precisely why the original code did not attempt them.
 *
 * Everything is derived from state amdgpu_gart_enable() already stored, so there
 * is no second source of truth to drift: vram_start comes back out of
 * gart_table_mc, and the page table itself is reused as-is.
 */
void amdgpu_gart_enable_gfxhub(struct AmdgpuDevice *ad)
{
    UQUAD vram_start, vram_base_offset, pt_base, fb_start, fb_end;
    UQUAD sys_low, sys_high, default_mc, dummy_phys;
    ULONG base_lo, cnt;

    if (!ad->gart_ready || !ad->gart_table_mc || !ad->gc_seg[0]) {
        bug("[amdgpu-gart] gfxhub: GART not ready (ready=%d table=0x%08lx%08lx gc0=0x%05lx)"
            " — skipping\n", (int)ad->gart_ready,
            (unsigned long)(ad->gart_table_mc >> 32),
            (unsigned long)(ULONG)ad->gart_table_mc,
            (unsigned long)ad->gc_seg[0]);
        return;
    }

    vram_start       = ad->gart_table_mc - GART_TABLE_VRAM_OFF;
    vram_base_offset = (UQUAD)grd(ad, ad->ip.mmhub.base, regMMMC_VM_FB_OFFSET) << 24;
    pt_base          = vram_base_offset + (ad->gart_table_mc - vram_start);
    pt_base         |= 0x1;                     /* AMDGPU_PTE_VALID */
    fb_start         = vram_start;
    fb_end           = vram_start + ((UQUAD)ad->vram_mb << 20) - 1;
    sys_low          = fb_start >> 18;
    sys_high         = fb_end   >> 18;
    default_mc       = fb_start;
    dummy_phys       = (UQUAD)ad->gart_arena_phys;

    gart_program_hub(ad, ad->gc_seg[0], FALSE, pt_base, sys_low, sys_high,
                     default_mc, dummy_phys);
    gart_flush_hub(ad, ad->gc_seg[0], FALSE);

    /* Read back rather than assume: a zero here means the writes still are not
     * landing, which is a different problem from a bad translation. */
    base_lo = grd(ad, ad->gc_seg[0], regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32);
    cnt     = grd(ad, ad->gc_seg[0], regGCVM_CONTEXT0_CNTL);
    bug("[amdgpu-gart] *** GFXHUB VMID0 %s: ctx0_base_lo=0x%08lx cnt=0x%08lx "
        "(want base_lo=0x%08lx cnt bit0=1) ***\n",
        (base_lo && (cnt & 1)) ? "ENABLED" : "STILL DEAD",
        (unsigned long)base_lo, (unsigned long)cnt,
        (unsigned long)(ULONG)(pt_base & 0xFFFFFFFFUL));
    amdgpu_gart_dump_fault(ad, "gfxhub-enable");
}

BOOL amdgpu_gart_capacity(struct AmdgpuDevice *ad, UQUAD *total, UQUAD *remaining)
{
    UQUAD first, next;
    BOOL ok = FALSE;
    if (!total || !remaining) return FALSE;
    *total = *remaining = 0;
    if (!gart_service_ready || !ad) return FALSE;
    ObtainSemaphore(&gart_map_lock);
    first = (GART_FW_VA + ad->gart_arena_size + GART_PAGE_SIZE - 1) >> GART_PAGE_SHIFT;
    next = ad->gart_next_page ? ad->gart_next_page : first;
    if (ad->gart_ready && first < GART_NUM_PAGES && next >= first && next <= GART_NUM_PAGES) {
        *total = (GART_NUM_PAGES - first) << GART_PAGE_SHIFT;
        *remaining = (GART_NUM_PAGES - next) << GART_PAGE_SHIFT;
        ok = TRUE;
    }
    ReleaseSemaphore(&gart_map_lock);
    return ok;
}

BOOL amdgpu_gart_enable(struct AmdgpuDevice *ad, UQUAD vram_start)
{
    UQUAD pt_mc, pt_base, vram_base_offset;
    UQUAD fb_start, fb_end, sys_low, sys_high, default_mc, dummy_phys;

    if (!ad->ip.mmhub.valid || !ad->ip.mmhub.base) {
        bug("[amdgpu-gart] MMHUB base unknown — cannot route PSP GTT; GART disabled\n");
        return FALSE;
    }

    /* One-time: PAGE-ALIGNED system-RAM arena + VRAM page table. AllocMem only
     * guarantees 8-byte alignment, but a GART PTE addresses a 4KB page (bits 47:12),
     * so the arena MUST be 4KB-aligned — otherwise every GART VA points into the
     * wrong page. (This was the first-cut bug: arena phys ...b20 masked to ...000,
     * so the PSP read 0xb20 bytes before each fw and rejected it.) Over-allocate one
     * page and round the base up; keep the raw pointer for cleanup. */
    if (!ad->gart_arena) {
        IPTR raw;
        ad->gart_arena_raw = AllocMem(GART_ARENA_SIZE + GART_PAGE_SIZE, MEMF_ANY | MEMF_CLEAR);
        if (!ad->gart_arena_raw) {
            bug("[amdgpu-gart] arena AllocMem(0x%lx) failed — GART disabled\n",
                (unsigned long)(GART_ARENA_SIZE + GART_PAGE_SIZE));
            return FALSE;
        }
        raw = (IPTR)ad->gart_arena_raw;
        ad->gart_arena = (UBYTE *)((raw + (GART_PAGE_SIZE - 1)) & ~(IPTR)(GART_PAGE_SIZE - 1));
        ad->gart_arena_size = GART_ARENA_SIZE;
        ad->gart_fw_va = GART_FW_VA;
        ad->gart_table_mc = vram_start + GART_TABLE_VRAM_OFF;

        if (!gart_build_table(ad)) {
            FreeMem(ad->gart_arena_raw, GART_ARENA_SIZE + GART_PAGE_SIZE);
            ad->gart_arena_raw = NULL;
            ad->gart_arena = NULL;
            return FALSE;
        }
    }

    pt_mc      = ad->gart_table_mc;
    /* gmc_v12_0_get_vm_pde() does not put a VRAM MC address in VMID0's root
     * register. It converts the VRAM BO to MMHUB physical form by subtracting
     * gmc.vram_start and adding vm_manager.vram_base_offset. On this Navi44 the
     * latter is MMMC_VM_FB_OFFSET << 24 (currently zero). */
    vram_base_offset = (UQUAD)grd(ad, ad->ip.mmhub.base, regMMMC_VM_FB_OFFSET) << 24;
    pt_base    = vram_base_offset + (pt_mc - vram_start);
    pt_base   |= 0x1;                              /* AMDGPU_PTE_VALID */
    fb_start   = vram_start;
    fb_end     = vram_start + ((UQUAD)ad->vram_mb << 20) - 1;
    sys_low    = fb_start >> 18;
    sys_high   = fb_end >> 18;
    default_mc = fb_start;                        /* harmless VRAM default */
    dummy_phys = (UQUAD)ad->gart_arena_phys;      /* L2 fault default = a valid mapped page */

    bug("[amdgpu-gart] enable: table MC=0x%08lx%08lx VMID0-base=0x%08lx%08lx "
        "(FB_OFFSET=0x%08lx%08lx) "
        "FB[0x%08lx%08lx..0x%08lx%08lx] sys[0x%lx..0x%lx]\n",
        (unsigned long)(pt_mc >> 32), (unsigned long)(pt_mc & 0xFFFFFFFFUL),
        (unsigned long)(pt_base >> 32), (unsigned long)(pt_base & 0xFFFFFFFFUL),
        (unsigned long)(vram_base_offset >> 32),
        (unsigned long)(vram_base_offset & 0xFFFFFFFFUL),
        (unsigned long)(fb_start >> 32), (unsigned long)(fb_start & 0xFFFFFFFFUL),
        (unsigned long)(fb_end >> 32), (unsigned long)(fb_end & 0xFFFFFFFFUL),
        (unsigned long)sys_low, (unsigned long)sys_high);

    /* Linux gmc_v12_0 enables only MMHUB before PSP. GFXHUB is initialized later
     * by gfx_v12_0 after the GFX block is powered, so pre-PSP writes there drop. */
    gart_program_hub(ad, ad->ip.mmhub.base, TRUE, pt_base, sys_low, sys_high,
                     default_mc, dummy_phys);
    gart_program_mmhub_invalidation(ad, ad->ip.mmhub.base);
    gart_flush_hub(ad, ad->ip.mmhub.base, TRUE);
    amdgpu_gart_dump_fault(ad, "after-enable");

    /* DIAGNOSTIC: did the programming actually take? Read back the context base +
     * enable on BOTH hubs, and the first mapped PTE from the VRAM table. This tells
     * us WHY a GART VA may not resolve for the PSP: either MMHUB rejected the
     * VMID0 state or the VRAM page-table write did not land. GFXHUB readback is
     * observation only here because Linux does not initialize it until gfx_v12_0. */
    {
        ULONG mm_lo = 0, mm_hi = 0, mm_cnt = 0, gc_lo = 0, gc_cnt = 0;
        ULONG pte_lo = 0, pte_hi = 0;
        UQUAD expected_pte;
        UQUAD pte_idx = GART_FW_VA >> GART_PAGE_SHIFT;   /* first mapped entry */
        mm_lo  = grd(ad, ad->ip.mmhub.base, regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32);
        mm_hi  = grd(ad, ad->ip.mmhub.base, regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32);
        mm_cnt = grd(ad, ad->ip.mmhub.base, regMMVM_CONTEXT0_CNTL);
        if (ad->gc_seg[0]) {
            gc_lo  = grd(ad, ad->gc_seg[0], regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32);
            gc_cnt = grd(ad, ad->gc_seg[0], regGCVM_CONTEXT0_CNTL);
        }
        amdgpu_vram_read(ad, GART_TABLE_VRAM_OFF + pte_idx * 8, &pte_lo, 1);
        amdgpu_vram_read(ad, GART_TABLE_VRAM_OFF + pte_idx * 8 + 4, &pte_hi, 1);
        expected_pte = ((UQUAD)ad->gart_arena_phys & PTE_ADDR_MASK) | PTE_SYS_FLAGS;
        bug("[amdgpu-gart] DIAG MMHUB ctx0_base=0x%08lx%08lx cnt=0x%08lx (want base_lo=0x%08lx cnt bit0=1)\n",
            (unsigned long)mm_hi, (unsigned long)mm_lo, (unsigned long)mm_cnt,
            (unsigned long)(pt_base & 0xFFFFFFFFUL));
        bug("[amdgpu-gart] DIAG GFXHUB ctx0_base_lo=0x%08lx cnt=0x%08lx (0 => hub dead/write-blocked)\n",
            (unsigned long)gc_lo, (unsigned long)gc_cnt);
        bug("[amdgpu-gart] DIAG PTE[0x%08lx]=0x%08lx%08lx (want arena_phys|0x77, ...%03lx77)\n",
            (unsigned long)GART_FW_VA, (unsigned long)pte_hi, (unsigned long)pte_lo,
            (unsigned long)((IPTR)ad->gart_arena_phys >> 12 & 0xFFF));

        /* MP0 is an MMHUB client and LOAD_TOC runs before Linux enables GFXHUB.
         * Advertise the mapping only when MMHUB accepted VMID0 and the first PTE
         * survived the VRAM write/readback path. */
        ad->gart_ready =
            mm_lo == (ULONG)(pt_base & 0xFFFFFFFFUL) &&
            mm_hi == (ULONG)(pt_base >> 32) &&
            (mm_cnt & 1) != 0 &&
            pte_lo == (ULONG)(expected_pte & 0xFFFFFFFFUL) &&
            pte_hi == (ULONG)(expected_pte >> 32);
        if (!ad->gart_ready)
            bug("[amdgpu-gart] MMHUB/PTE readiness failed — PSP fw stays on VRAM\n");
    }

    bug("[amdgpu-gart] *** GART %s *** fw staging %s system RAM @ GART VA 0x%08lx\n",
        ad->gart_ready ? "READY" : "NOT READY",
        ad->gart_ready ? "uses" : "will not use",
        (unsigned long)GART_FW_VA);
    return TRUE;
}
