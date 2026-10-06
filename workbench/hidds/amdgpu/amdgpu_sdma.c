/*
 * amdgpu.hidd — SDMA (System DMA) bring-up for GFX12 / SDMA 7.x.
 *
 * WHY THIS BLOCK EXISTS NOW, AND NOT BEFORE
 * -----------------------------------------
 * The ip ladder carried { "sdma", NULL } as an honest "not yet" for the whole
 * bring-up, and it could not have been anything else: SDMA_RESETB is bit 3 of
 * GFX_IMU_GFX_RESET_CTRL, one of the four bits held LOW while that register sat
 * at 0x30. The copy engine was physically in reset. Once the SMU feature-enable
 * was actually called from the boot path (bbce14b16e) the register reaches 0x7F
 * and SDMA comes out of reset along with HARD/EA/UTCL2 — so this is the first
 * moment the block has been addressable at all.
 *
 * WHY SDMA IS THE RIGHT NEXT RUNG
 * -------------------------------
 * It is the smallest engine on the part that can prove COMMAND SUBMISSION works,
 * which is the step that converts "powered and out of reset" into "working".
 * Everything measured so far is passive state: RESET_CTRL, BOOTLOAD_COMPLETE,
 * GPM_STAT, GRBM_STATUS. None of it shows the GPU executing anything we wrote.
 * CP_STAT has read 0 on every boot — idle, not working.
 *
 * SDMA needs no MES, no MQD, no shaders and no compute state: program a ring,
 * write a packet, watch the read pointer move. It is also the engine that does
 * accelerated 2D blits, so this is the foundation for the BitMap class rather
 * than a throwaway diagnostic.
 *
 * ADDRESSING (the part that is easy to get wrong)
 * ----------------------------------------------
 * SDMA registers are NOT a separate aperture. Linux sdma_v7_0_get_reg_offset()
 * resolves them against the GC segments:
 *   - 0x5880..0x589a  (the "HYP_DEC" range, e.g. SDMA0_MCU_CNTL 0x588e) -> gc_seg[1]
 *   - everything else (the QUEUE0 block, 0x0080..0x009b)                -> gc_seg[0]
 *   - instance 1 adds SDMA1_REG_OFFSET 0x600 in the normal range.
 * That maps directly onto the GC0/GC1 accessors this driver already uses.
 *
 * The ring lives in VRAM and is addressed by MC address (VRAM MC base + offset),
 * the same convention the PSP and SMU paths already use here. That is not an
 * assumption: the SMU demonstrably DMA'd its PPTable into VRAM at MC
 * 0x00000080:24000000 and we read the content back, so an on-chip engine
 * reaching VRAM by MC address is established on this card.
 *
 * CURRENT SCOPE (the history above describes initial bring-up).
 * Ring 0 of instance 0, no doorbell (MMIO write pointer), no interrupts, no
 * scheduler, VMID0. A serialized continuous ring serves copy/fill and image
 * staging with an independent sequence fence. Native copy/fill and wrap tests
 * are required; a register readback or CPU-fallback pixel pass is insufficient.
 * Unretired DMA has fail-stop containment, not a verified reset/recovery path.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/semaphores.h>
#include <devices/timer.h>
#include <proto/timer.h>

#include "amdgpu_intern.h"

/* AUDIT P0-5: the SDMA ring lives at one fixed VRAM address and submissions
 * share its producer counter and completion slot, so two tasks issuing work
 * concurrently would corrupt each other's ring state. Serialize all submissions
 * through one lock. It lives here with the ring (single engine, single ring),
 * is the innermost lock taken, and is initialized in amdgpu_sdma_hw_init before
 * the first submission. */
static struct SignalSemaphore sdma_submit_lock;
static BOOL sdma_lock_ready = FALSE;

/* --- register offsets, verified against gc_12_0_0_offset.h -------------- */
/* QUEUE0 block: gc_seg[0]-relative */
#define SDMA0_QUEUE0_RB_CNTL              0x0080
#define SDMA0_QUEUE0_RB_BASE              0x0081
#define SDMA0_QUEUE0_RB_BASE_HI           0x0082
#define SDMA0_QUEUE0_RB_RPTR              0x0083
#define SDMA0_QUEUE0_RB_RPTR_HI           0x0084
#define SDMA0_QUEUE0_RB_WPTR              0x0085
#define SDMA0_QUEUE0_RB_WPTR_HI           0x0086
#define SDMA0_QUEUE0_RB_RPTR_ADDR_LO      0x0087
#define SDMA0_QUEUE0_RB_RPTR_ADDR_HI      0x0088
#define SDMA0_QUEUE0_IB_CNTL              0x0089
#define SDMA0_QUEUE0_DOORBELL             0x008f
#define SDMA0_QUEUE0_RB_WPTR_POLL_ADDR_LO 0x0098
#define SDMA0_QUEUE0_RB_WPTR_POLL_ADDR_HI 0x0099
#define SDMA0_QUEUE0_MINOR_PTR_UPDATE     0x009b
/* HYP_DEC range: gc_seg[1]-relative */
#define SDMA0_MCU_CNTL                    0x588e

#define SDMA1_REG_OFFSET                  0x600

/* RB_CNTL field shifts (gc_12_0_0_sh_mask.h) */
#define RB_CNTL_RB_ENABLE_SHIFT             0
#define RB_CNTL_RB_SIZE_SHIFT               1
#define RB_CNTL_WPTR_POLL_ENABLE_SHIFT      8
#define RB_CNTL_RB_SWAP_ENABLE_SHIFT        9
#define RB_CNTL_MCU_WPTR_POLL_ENABLE_SHIFT  11
#define RB_CNTL_RPTR_WRITEBACK_ENABLE_SHIFT 12
#define RB_CNTL_RPTR_WRITEBACK_TIMER_SHIFT  16
#define RB_CNTL_RB_PRIV_SHIFT               23

#define SDMA_MCU_CNTL_HALT_SHIFT            0

/* SDMA packet opcodes (vega10_sdma_pkt_open.h — stable across generations) */
#define SDMA_OP_NOP    0
#define SDMA_OP_COPY   1
#define SDMA_OP_FENCE  5
/* CONSTANT_FILL: on SDMA7 (RDNA4/GFX12) the header bit that AROS previously set
 * as sw[17:16]=2 is wrong -- AMD Linux 6.16 sdma_v7_0.c (:56-60, :1817-1827)
 * redefines bit16 as COMPRESS and emits {0x0001000b, dst_lo, dst_hi, value,
 * byte_count-1}. The old sw=2 encoding (0x0002000b) sets bit17 with COMPRESS=0,
 * which the DUT's constant-fill self-test/GHC FillRect rejected (codex TRIG-0548).
 * Count stays BYTES minus one, same convention as COPY (Linux uses the same
 * units, so the packet body is unchanged). Mesa's 0x8000000b (FILLSIZE at
 * bits31:30) is a DISTINCT uncompressed encoding with different count units --
 * not used here. */
#define SDMA_OP_CONST_FILL         11
#define SDMA_FILL_HDR_COMPRESS_SHIFT 16   /* bit16 = COMPRESS(1) on SDMA7 */
#define SDMA_FENCE_MTYPE_UC  0x3   /* uncached, as sdma_v7_0_ring_emit_fence uses */

/* COPY_LINEAR header fields, sdma_v7_1_0_pkt_open.h (the GFX12 packet header):
 * op 0..7, sub_op 8..15, tmz bit 18, cpv bit 19. sdma_v7_0_emit_copy_buffer()
 * sets CPV(1) unconditionally on this generation. */
#define SDMA_SUBOP_COPY_LINEAR   0
#define SDMA_SUBOP_COPY_LINEAR_SUB_WIND 4   /* sdma_v7_1_0_pkt_open.h */
#define SDMA_COPY_HDR_SUBOP_SHIFT 8
#define SDMA_COPY_HDR_TMZ_SHIFT   18
#define SDMA_COPY_HDR_CPV_SHIFT   19

/* VRAM layout for this block. Kept clear of the regions already spoken for:
 * PSP 264MiB, autoload 320MiB, GART 512MiB, SMU driver table 576MiB,
 * SMU tools 640MiB. */
#define SDMA_VRAM_MC_BASE      0x8000000000ULL
#define SDMA_RING_VRAM_OFF     ((UQUAD)704 << 20)   /* ring buffer          */
#define SDMA_RPTR_WB_VRAM_OFF  ((UQUAD)705 << 20)   /* rptr writeback dword */
#define SDMA_FENCE_VRAM_OFF    ((UQUAD)706 << 20)   /* fence target dword   */
#define SDMA_BLIT_SRC_VRAM_OFF ((UQUAD)707 << 20)   /* blit source          */
#define SDMA_BLIT_DST_VRAM_OFF ((UQUAD)708 << 20)   /* blit destination     */

#define SDMA_BLIT_DWORDS       256                  /* 1 KiB test blit */

/* --- 2D sub-window blit test surface ----------------------------------- */
/* A real BitMap blit is a RECTANGLE between two surfaces that have a pitch,
 * not a flat byte range, so this is the packet the display path actually
 * needs. 64x16 at 4 bytes/pixel = 4 KiB per surface. */
#define SDMA_SW_WIDTH    64      /* surface width  in pixels (== pitch)     */
#define SDMA_SW_HEIGHT   16      /* surface height in pixels                */
#define SDMA_SW_BPP      4       /* bytes per pixel; elementsize = log2()=2 */
#define SDMA_SW_ELEMSIZE 2

/* The rectangle: deliberately NOT at the origin and NOT the full width, so a
 * packet that ignores offsets or pitch produces a visibly wrong picture rather
 * than an accidentally-correct one. */
#define SDMA_SW_SRC_X    8
#define SDMA_SW_SRC_Y    4
#define SDMA_SW_DST_X    32
#define SDMA_SW_DST_Y    2
#define SDMA_SW_RECT_W   16
#define SDMA_SW_RECT_H   8

/* SUBWIN packet field shifts (sdma_v7_1_0_pkt_open.h). NOTE the pitch shift is
 * the one field where the AMD-generated header and Mesa DISAGREE: the header
 * says 13, but Mesa's ac_cmdbuf_sdma.c uses 16 for SDMA >= 7.0 —
 *   uint32_t pitch_shift = (sdma_ip_version >= SDMA_7_0 || ... < SDMA_4_0) ? 16 : 13;
 * Rather than pick an authority, the test below runs the blit with BOTH and
 * reports which one the silicon actually accepts. */
#define SDMA_SW_PITCH_SHIFT_MESA   16
#define SDMA_SW_PITCH_SHIFT_HEADER 13
#define SDMA_SW_HDR_ELEMSIZE_SHIFT 29

#define SDMA_RING_DWORDS       1024                 /* 4 KiB */
#define SDMA_RING_FETCH_DWORDS 16                   /* SDMA7 ring fetch: 64 bytes */
#define SDMA_RING_LOG2_DWORDS  10                   /* order_base_2(1024)   */

#define SDMA_FENCE_MAGIC       0x5D3AF00DUL

/* Single-device/ring ownership matches the existing fence and scratch storage.
 * Keep the hardware's cumulative BYTE pointer, not a modulo ring position.
 * Only the VRAM array index wraps. No per-submission HALT or pointer reset.
 */
static struct AmdgpuDevice *sdma_ring_owner;
static UQUAD sdma_producer;
static ULONG sdma_sequence;
static BOOL sdma_ring_ready;
static BOOL sdma_ring_failed;
static BOOL sdma_subwin32_ready;

static BOOL sdma_submit_and_wait_locked(struct AmdgpuDevice *ad, ULONG inst,
                                        const ULONG *pkt, ULONG dwords);

static ULONG sdma_q_read(struct AmdgpuDevice *ad, ULONG inst, ULONG off)
{
    return amdgpu_rreg32(ad, ad->gc_seg[0] + off + (inst ? SDMA1_REG_OFFSET : 0));
}

static void sdma_q_write(struct AmdgpuDevice *ad, ULONG inst, ULONG off, ULONG val)
{
    amdgpu_wreg32(ad, ad->gc_seg[0] + off + (inst ? SDMA1_REG_OFFSET : 0), val);
}

/* Take the engine out of halt. Linux clears SDMA0_MCU_CNTL.HALT to enable. */
static void sdma_set_halt(struct AmdgpuDevice *ad, ULONG inst, BOOL halt)
{
    ULONG reg = ad->gc_seg[1] + SDMA0_MCU_CNTL + (inst ? 0x20 : 0);
    ULONG v   = amdgpu_rreg32(ad, reg);

    if (halt) v |=  (1UL << SDMA_MCU_CNTL_HALT_SHIFT);
    else      v &= ~(1UL << SDMA_MCU_CNTL_HALT_SHIFT);
    amdgpu_wreg32(ad, reg, v);
}

/*
 * Fence in SYSTEM RAM, not VRAM.
 *
 * The completion fence started in VRAM, which meant every poll of the wait loop
 * was an amdgpu_vram_read() — an MMIO transaction through the BIF aperture.
 * That is a fixed per-submission cost paid thousands of times per blit, and the
 * benchmark showed it dominating: the copy engine measured 19x SLOWER than the
 * CPU at 4 KiB while beating it at 1 MiB, which is the signature of a large
 * fixed overhead rather than a slow engine.
 *
 * Now that the GART can map arbitrary buffers, the fence lives in system RAM and
 * the wait loop is a plain cached load. x86 snoops, so no explicit invalidate is
 * needed per poll; the pointer is read through a volatile so the compiler cannot
 * hoist it out of the loop.
 *
 * This is the cheap half of the fix. The expensive half — a real interrupt
 * instead of any polling at all — still wants the ih block, which is still a
 * NULL stub. Falls back to the VRAM fence if the GART mapping is unavailable,
 * so this cannot make the driver less able to boot.
 */
static volatile ULONG *sdma_fence_cpu;   /* NULL => use the VRAM fence */
static UQUAD           sdma_fence_va;

static void sdma_fence_init(struct AmdgpuDevice *ad)
{
    static ULONG *raw;

    if (sdma_fence_cpu || !ad->gart_ready)
        return;
    raw = AllocMem(4096, MEMF_ANY | MEMF_CLEAR);
    if (!raw)
        return;
    sdma_fence_va = amdgpu_gart_map(ad, raw, 4096);
    if (!sdma_fence_va) {
        FreeMem(raw, 4096);
        return;
    }
    sdma_fence_cpu = (volatile ULONG *)raw;
    bug("[amdgpu-sdma] fence moved to system RAM: cpu %p VA 0x%08lx%08lx "
        "(poll is now a cached load, not an MMIO read)\n", raw,
        (unsigned long)(sdma_fence_va >> 32), (unsigned long)(ULONG)sdma_fence_va);
}

static UQUAD sdma_fence_addr(void)
{
    return sdma_fence_cpu ? sdma_fence_va
                          : (SDMA_VRAM_MC_BASE + SDMA_FENCE_VRAM_OFF);
}

/* Clear the fence slot wherever it currently lives. */
static void sdma_fence_clear(struct AmdgpuDevice *ad)
{
    ULONG zero = 0;

    if (sdma_fence_cpu) *sdma_fence_cpu = 0;
    else                amdgpu_vram_write(ad, SDMA_FENCE_VRAM_OFF, &zero, 1);
}

/* The cached fence path must not turn a CPU iteration count into a device
 * deadline. Keep one timer reference for the resident ring's lifetime, like
 * its persistent fence/scratch allocations; no timer allocation in a blit.
 * The timer is opened before any common-path submission. Failure disables
 * that path before it can publish DMA work.
 */
static struct MsgPort *sdma_clock_port;
static struct timerequest *sdma_clock_request;
static ULONG sdma_clock_frequency;

static UQUAD sdma_clock_ticks(void)
{
    struct Device *TimerBase = sdma_clock_request->tr_node.io_Device;
    struct EClockVal time;
    ReadEClock(&time);
    return ((UQUAD)time.ev_hi << 32) | time.ev_lo;
}

static BOOL sdma_clock_init(void)
{
    struct Device *TimerBase = NULL;
    struct EClockVal time;

    if (sdma_clock_frequency) return TRUE;
    sdma_clock_port = CreateMsgPort();
    if (sdma_clock_port)
        sdma_clock_request = (struct timerequest *)CreateIORequest(
            sdma_clock_port, sizeof(*sdma_clock_request));
    if (!sdma_clock_request || OpenDevice((CONST_STRPTR)"timer.device",
            UNIT_MICROHZ, (struct IORequest *)sdma_clock_request, 0)) goto fail;
    TimerBase = sdma_clock_request->tr_node.io_Device;
    sdma_clock_frequency = ReadEClock(&time);
    if (sdma_clock_frequency) {
        bug("[amdgpu-sdma] completion deadline-v1 / fetch-align-v1: EClock=%lu Hz, 200ms; ring fetch=16 dwords; legacy 200000-poll boundary measured separately\n",
            (unsigned long)sdma_clock_frequency);
        return TRUE;
    }
fail:
    if (TimerBase) CloseDevice((struct IORequest *)sdma_clock_request);
    if (sdma_clock_request) DeleteIORequest((struct IORequest *)sdma_clock_request);
    if (sdma_clock_port) DeleteMsgPort(sdma_clock_port);
    sdma_clock_request = NULL;
    sdma_clock_port = NULL;
    bug("[amdgpu-sdma] completion clock unavailable; no common-path DMA submissions\n");
    return FALSE;
}

/*
 * Bring up instance 0's gfx ring and prove it retires a packet.
 *
 * Returns TRUE when the ring is running AND a submitted fence was observed to
 * complete. Anything less is reported as the specific thing that failed — this
 * block never claims a ring works because the register writes stuck.
 */
static BOOL sdma_ring_bringup(struct AmdgpuDevice *ad, ULONG inst)
{
    const UQUAD ring_mc  = SDMA_VRAM_MC_BASE + SDMA_RING_VRAM_OFF;
    const UQUAD rptr_mc  = SDMA_VRAM_MC_BASE + SDMA_RPTR_WB_VRAM_OFF;
    const UQUAD fence_mc = sdma_fence_addr();
    ULONG rb_cntl, rptr0, rptr1, i;
    ULONG pkt[8], zero[1] = { 0 };
    ULONG fence_val = 0, rptr_wb = 0;
    BOOL  retired = FALSE;

    if (inst || sdma_ring_owner)
        return FALSE; /* never reinitialize a resident or another device's ring */
    sdma_ring_owner = ad;
    sdma_producer = 0;
    sdma_sequence = 0;
    sdma_ring_ready = FALSE;
    sdma_ring_failed = FALSE;

    /* Clear the ring, the rptr writeback slot and the fence target so nothing
     * we read afterwards can be stale data from a previous boot. */
    for (i = 0; i < SDMA_RING_DWORDS; i++)
        amdgpu_vram_write(ad, SDMA_RING_VRAM_OFF + (i * 4), zero, 1);
    amdgpu_vram_write(ad, SDMA_RPTR_WB_VRAM_OFF, zero, 1);
    amdgpu_vram_write(ad, SDMA_RPTR_WB_VRAM_OFF + 4, zero, 1);
    sdma_fence_clear(ad);
    /* Independent completion sequence; callers retain their diagnostic magic
     * fence at +0, but it can never satisfy the common submission waiter. */
    if (sdma_fence_cpu) sdma_fence_cpu[2] = 0;
    else amdgpu_vram_write(ad, SDMA_FENCE_VRAM_OFF + 8, zero, 1);

    /* Halt while reprogramming, exactly as Linux does around gfx_resume. */
    sdma_set_halt(ad, inst, TRUE);

    /* Ring disabled while we set it up. */
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_CNTL, 0);

    /* Only initial setup rewinds pointers. Linux requires MINOR_PTR_UPDATE
     * around a decreasing WPTR, not around each normal forward commit. */
    sdma_q_write(ad, inst, SDMA0_QUEUE0_MINOR_PTR_UPDATE, 1);
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_RPTR,    0);
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_RPTR_HI, 0);
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_WPTR,    0);
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_WPTR_HI, 0);
    sdma_q_write(ad, inst, SDMA0_QUEUE0_MINOR_PTR_UPDATE, 0);

    /* rptr writeback target. Low bits must be clear (Linux masks 0xFFFFFFFC). */
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_RPTR_ADDR_LO,
                 (ULONG)(rptr_mc & 0xFFFFFFFCUL));
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_RPTR_ADDR_HI,
                 (ULONG)(rptr_mc >> 32));

    /* Ring base is >> 8 / >> 40, i.e. 256-byte granular. */
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_BASE,    (ULONG)(ring_mc >> 8));
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_BASE_HI, (ULONG)(ring_mc >> 40));

    /* No doorbell in this increment: the write pointer is driven by MMIO, which
     * removes the doorbell aperture as a variable while we are establishing
     * whether submission works at all. */
    sdma_q_write(ad, inst, SDMA0_QUEUE0_DOORBELL, 0);

    rb_cntl  = (ULONG)SDMA_RING_LOG2_DWORDS << RB_CNTL_RB_SIZE_SHIFT;
    rb_cntl |= 1UL << RB_CNTL_RPTR_WRITEBACK_ENABLE_SHIFT;
    rb_cntl |= 1UL << RB_CNTL_RB_PRIV_SHIFT;
    rb_cntl |= 1UL << RB_CNTL_RB_ENABLE_SHIFT;
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_CNTL, rb_cntl);

    /* Release the engine, then confirm the programming actually stuck before
     * blaming a submission failure on the ring. */
    sdma_set_halt(ad, inst, FALSE);

    {
        ULONG rb_rd   = sdma_q_read(ad, inst, SDMA0_QUEUE0_RB_CNTL);
        ULONG base_rd = sdma_q_read(ad, inst, SDMA0_QUEUE0_RB_BASE);

        bug("[amdgpu-sdma] inst%lu ring @MC %08lx:%08lx  RB_CNTL wrote=%08lx read=%08lx  "
            "RB_BASE wrote=%08lx read=%08lx\n",
            (unsigned long)inst,
            (unsigned long)(ULONG)(ring_mc >> 32), (unsigned long)(ULONG)ring_mc,
            (unsigned long)rb_cntl, (unsigned long)rb_rd,
            (unsigned long)(ULONG)(ring_mc >> 8), (unsigned long)base_rd);

        if (rb_rd == 0xFFFFFFFFUL || base_rd == 0xFFFFFFFFUL) {
            bug("[amdgpu-sdma] inst%lu registers read all-ones — block not addressable\n",
                (unsigned long)inst);
            return FALSE;
        }
        if (base_rd != (ULONG)(ring_mc >> 8)) {
            bug("[amdgpu-sdma] inst%lu RB_BASE did not stick — SDMA still write-blocked\n",
                (unsigned long)inst);
            return FALSE;
        }
    }

    rptr0 = sdma_q_read(ad, inst, SDMA0_QUEUE0_RB_RPTR);

    /* Diagnostic fence plus four NOPs. The common submitter adds its separate
     * sequence fence and pads the physical fetch to 16 dwords. */
    pkt[0] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[1] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[2] = (ULONG)(fence_mc >> 32);
    pkt[3] = SDMA_FENCE_MAGIC;
    pkt[4] = (ULONG)SDMA_OP_NOP;
    pkt[5] = (ULONG)SDMA_OP_NOP;
    pkt[6] = (ULONG)SDMA_OP_NOP;
    pkt[7] = (ULONG)SDMA_OP_NOP;
    sdma_ring_ready = TRUE;
    bug("[amdgpu-sdma] continuous-ring-v1: 64-bit byte counters, 16-dword fetch alignment, independent sequence fence; unretired timeout retains caller until reboot\n");
    retired = sdma_submit_and_wait_locked(ad, inst, pkt, 8);
    fence_val = sdma_fence_cpu ? *sdma_fence_cpu : 0;
    if (!sdma_fence_cpu)
        amdgpu_vram_read(ad, SDMA_FENCE_VRAM_OFF, &fence_val, 1);

    rptr1 = sdma_q_read(ad, inst, SDMA0_QUEUE0_RB_RPTR);
    amdgpu_vram_read(ad, SDMA_RPTR_WB_VRAM_OFF, &rptr_wb, 1);

    bug("[amdgpu-sdma] inst%lu RPTR %08lx -> %08lx (writeback=%08lx) fence=%08lx seq=%lu\n", (unsigned long)inst,
        (unsigned long)rptr0, (unsigned long)rptr1, (unsigned long)rptr_wb,
        (unsigned long)fence_val, (unsigned long)sdma_sequence);

    if (retired) {
        bug("[amdgpu-sdma] *** inst%lu FIRST SUBMISSION RETIRED — the GPU executed a "
            "packet we wrote. Engine is WORKING, not merely powered. ***\n",
            (unsigned long)inst);
        return TRUE;
    }

    /* Distinguish the two failure shapes rather than reporting a bare timeout:
     * a moving read pointer means the engine fetched our packets and the fence
     * write is what failed (an addressing problem); a stationary one means it
     * never fetched at all (a ring/enable problem). */
    if (rptr1 != rptr0)
        bug("[amdgpu-sdma] inst%lu FETCHED the ring (RPTR moved) but the fence never "
            "landed — packet or fence-address problem, not a ring problem\n",
            (unsigned long)inst);
    else
        bug("[amdgpu-sdma] inst%lu RPTR never moved — engine did not fetch; ring enable "
            "or ucode not running\n", (unsigned long)inst);
    return FALSE;
}

/*
 * The actual blit. This is the point of the whole block: a linear VRAM->VRAM
 * copy issued to the engine, verified by content rather than by a status bit.
 *
 * Verification is deliberately hostile to false positives. The destination is
 * pre-filled with a DIFFERENT pattern than the source, so a copy that silently
 * does nothing leaves the destination readable and WRONG rather than plausibly
 * zero; and the source pattern is position-dependent (index-derived), so a copy
 * that transfers the right number of bytes from the wrong place, or splats one
 * value, fails the check instead of passing it. Every mismatching dword is
 * counted and the first one is printed with both values.
 *
 * Uses the same continuous ring and independent completion as normal drawing.
 */
static BOOL sdma_blit_test(struct AmdgpuDevice *ad, ULONG inst)
{
    const UQUAD src_mc   = SDMA_VRAM_MC_BASE + SDMA_BLIT_SRC_VRAM_OFF;
    const UQUAD dst_mc   = SDMA_VRAM_MC_BASE + SDMA_BLIT_DST_VRAM_OFF;
    const UQUAD fence_mc = sdma_fence_addr();
    const ULONG bytes    = SDMA_BLIT_DWORDS * 4;
    ULONG pkt[16], v, i, mism = 0, got = 0, want = 0;
    ULONG first_bad = 0xFFFFFFFFUL;   /* sentinel: ULONG is 32-bit here, so ~0UL
                                       * (64-bit) would never compare equal */
    ULONG fence_val = 0, rptr0, rptr1;
    BOOL  retired = FALSE;

    /* Source: position-dependent so a wrong-address or splat copy cannot pass.
     * Destination: a different constant so "did nothing" is distinguishable
     * from "copied correctly". */
    for (i = 0; i < SDMA_BLIT_DWORDS; i++) {
        v = 0xB1000000UL | (i * 0x00010001UL);
        amdgpu_vram_write(ad, SDMA_BLIT_SRC_VRAM_OFF + (i * 4), &v, 1);
        v = 0xDEAD0000UL | i;
        amdgpu_vram_write(ad, SDMA_BLIT_DST_VRAM_OFF + (i * 4), &v, 1);
    }
    sdma_fence_clear(ad);

    /* Confirm the pre-fill actually landed before trusting any later compare —
     * otherwise a broken VRAM window would look like a broken blit. */
    amdgpu_vram_read(ad, SDMA_BLIT_SRC_VRAM_OFF, &got, 1);
    amdgpu_vram_read(ad, SDMA_BLIT_DST_VRAM_OFF, &want, 1);
    if (got != 0xB1000000UL || want != 0xDEAD0000UL) {
        bug("[amdgpu-sdma] blit pre-fill did not land (src=%08lx dst=%08lx) — VRAM window "
            "problem, not a blit result\n", (unsigned long)got, (unsigned long)want);
        return FALSE;
    }

    rptr0 = sdma_q_read(ad, inst, SDMA0_QUEUE0_RB_RPTR);

    /* COPY_LINEAR, then a FENCE so completion has an explicit signal rather than
     * being inferred from the destination changing. */
    pkt[0] = (ULONG)SDMA_OP_COPY
           | ((ULONG)SDMA_SUBOP_COPY_LINEAR << SDMA_COPY_HDR_SUBOP_SHIFT)
           | (1UL << SDMA_COPY_HDR_CPV_SHIFT);
    pkt[1] = bytes - 1;                       /* count is bytes-1 */
    pkt[2] = 0;                               /* no endian swap */
    pkt[3] = (ULONG)(src_mc & 0xFFFFFFFFUL);
    pkt[4] = (ULONG)(src_mc >> 32);
    pkt[5] = (ULONG)(dst_mc & 0xFFFFFFFFUL);
    pkt[6] = (ULONG)(dst_mc >> 32);
    pkt[7] = 0;                               /* no DCC/compression */
    pkt[8]  = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[9]  = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[10] = (ULONG)(fence_mc >> 32);
    pkt[11] = SDMA_FENCE_MAGIC;
    pkt[12] = (ULONG)SDMA_OP_NOP;
    pkt[13] = (ULONG)SDMA_OP_NOP;
    pkt[14] = (ULONG)SDMA_OP_NOP;
    pkt[15] = (ULONG)SDMA_OP_NOP;

    bug("[amdgpu-sdma] inst%lu BLIT: COPY_LINEAR %lu bytes  src MC %08lx:%08lx -> "
        "dst MC %08lx:%08lx\n", (unsigned long)inst, (unsigned long)bytes,
        (unsigned long)(ULONG)(src_mc >> 32), (unsigned long)(ULONG)src_mc,
        (unsigned long)(ULONG)(dst_mc >> 32), (unsigned long)(ULONG)dst_mc);

    retired = sdma_submit_and_wait_locked(ad, inst, pkt, 16);
    fence_val = sdma_fence_cpu ? *sdma_fence_cpu : 0;
    if (!sdma_fence_cpu)
        amdgpu_vram_read(ad, SDMA_FENCE_VRAM_OFF, &fence_val, 1);
    rptr1 = sdma_q_read(ad, inst, SDMA0_QUEUE0_RB_RPTR);

    /* Compare content regardless of whether the fence landed — a blit that
     * worked with a fence that did not is a different bug from a blit that
     * never ran, and the destination is what actually matters. */
    for (i = 0; i < SDMA_BLIT_DWORDS; i++) {
        ULONG expect = 0xB1000000UL | (i * 0x00010001UL);
        amdgpu_vram_read(ad, SDMA_BLIT_DST_VRAM_OFF + (i * 4), &v, 1);
        if (v != expect) {
            if (first_bad == 0xFFFFFFFFUL) { first_bad = i; got = v; want = expect; }
            mism++;
        }
    }

    bug("[amdgpu-sdma] inst%lu blit result: RPTR %08lx -> %08lx  fence=%08lx (%s) "
        "mismatched dwords %lu/%lu\n", (unsigned long)inst,
        (unsigned long)rptr0, (unsigned long)rptr1, (unsigned long)fence_val,
        retired ? "retired" : "TIMEOUT", (unsigned long)mism,
        (unsigned long)SDMA_BLIT_DWORDS);

    if (retired && mism == 0) {
        bug("[amdgpu-sdma] *** inst%lu ACCELERATED BLIT VERIFIED — %lu bytes copied "
            "VRAM->VRAM by the GPU, all %lu dwords match a position-dependent "
            "pattern ***\n", (unsigned long)inst, (unsigned long)bytes,
            (unsigned long)SDMA_BLIT_DWORDS);
        return TRUE;
    }

    bug("[amdgpu-sdma] inst%lu blit MISMATCH: first bad dword %lu got=%08lx want=%08lx%s\n",
        (unsigned long)inst, (unsigned long)first_bad,
        (unsigned long)got, (unsigned long)want,
        (mism == SDMA_BLIT_DWORDS) ? "  (ALL dwords wrong — copy did not run)" : "");
    return FALSE;
}

/* Coherent 64-bit register snapshot across a possible low-word rollover. */
static UQUAD sdma_consumer(struct AmdgpuDevice *ad)
{
    ULONG hi, lo, again, tries;
    for (tries = 0; tries < 8; tries++) {
        hi = sdma_q_read(ad, 0, SDMA0_QUEUE0_RB_RPTR_HI);
        lo = sdma_q_read(ad, 0, SDMA0_QUEUE0_RB_RPTR);
        again = sdma_q_read(ad, 0, SDMA0_QUEUE0_RB_RPTR_HI);
        if (hi == again) return ((UQUAD)hi << 32) | lo;
    }
    return ~(UQUAD)0; /* cannot be mistaken for available ring capacity */
}

static ULONG sdma_completed_sequence(struct AmdgpuDevice *ad)
{
    ULONG v;
    if (sdma_fence_cpu) return sdma_fence_cpu[2];
    amdgpu_vram_read(ad, SDMA_FENCE_VRAM_OFF + 8, &v, 1);
    return v;
}

/* Write a logical ring span, splitting at the physical 4KiB boundary. The
 * producer counter itself never wraps at that boundary. Capacity is checked
 * for the entire payload + independent fence + padding before any write. */
static void sdma_ring_store(struct AmdgpuDevice *ad, UQUAD byte_pos,
                            const ULONG *src, ULONG count)
{
    ULONG offset = (ULONG)(byte_pos >> 2) & (SDMA_RING_DWORDS - 1);
    ULONG first = SDMA_RING_DWORDS - offset;
    if (first > count) first = count;
    amdgpu_vram_write(ad, SDMA_RING_VRAM_OFF + offset * 4, src, first);
    if (first < count)
        amdgpu_vram_write(ad, SDMA_RING_VRAM_OFF, src + first, count - first);
}

/* A BOOL failure normally permits CPU fallback and freeing/unmapping caller
 * storage. After publishing DMA that is unsafe until its sequence fence lands.
 * We do not have a verified SDMA reset/abort contract yet. Retain the innermost
 * submission lock and caller stack/buffer ownership; park this task until a
 * physical reboot. Do not spin, rewind, release scratch, or race CPU fallback.
 * This is explicit fail-stop containment, NOT production reset recovery. */
static void sdma_unretired_stop(void)
{
    sdma_ring_failed = TRUE;
    bug("[amdgpu-sdma] DMA UNRETIRED: retaining submission/caller ownership; no CPU fallback, physical reboot required\n");
    for (;;) Wait(0);
}

/* One synchronous submission on a persistent ring. The private fence at +8
 * follows all caller packets, so stale caller magic at +0 is never completion.
 * Reference: Linux sdma_v7_0_ring_set_wptr and amdgpu_ring_commit (f377d0025eb0).
 */
static BOOL sdma_submit_and_wait_locked(struct AmdgpuDevice *ad, ULONG inst,
                                 const ULONG *pkt, ULONG dwords)
{
    const UQUAD completion_mc = sdma_fence_addr() + 8;
    ULONG i, sequence, fence[4];
    UQUAD started, legacy_elapsed, elapsed, deadline;
    UQUAD consumer, next, used;
    ULONG padding, ring_dwords, nops[SDMA_RING_FETCH_DWORDS];
    static ULONG late_retirements;

    if (!sdma_clock_frequency || !sdma_ring_ready || sdma_ring_failed ||
        ad != sdma_ring_owner || inst || !pkt || !dwords ||
        dwords > SDMA_RING_DWORDS - SDMA_RING_FETCH_DWORDS)
        return FALSE;

    /* Linux sdma_v7_0_ring_funcs.align_mask is 0xf: ring_commit pads to the
     * engine's 64-byte fetch size before publishing WPTR. Several callers
     * provide only 8-dword alignment (the 64-row copy is 520 dwords). Pad the
     * ring itself, not the caller's fixed-size packet array. No caller bytes
     * beyond dwords are read, and the complete fetch stays inside the ring.
     */
    padding = (SDMA_RING_FETCH_DWORDS -
               ((dwords + 4) & (SDMA_RING_FETCH_DWORDS - 1))) &
              (SDMA_RING_FETCH_DWORDS - 1);
    ring_dwords = dwords + 4 + padding;
    /* Leave one fetch unit unused to avoid a modulo full/empty ambiguity. */
    if (ring_dwords > SDMA_RING_DWORDS - SDMA_RING_FETCH_DWORDS)
        return FALSE;
    for (i = 0; i < padding; i++)
        nops[i] = SDMA_OP_NOP;

    /* Every previous data operation has retired its private sequence before
     * releasing this lock; only trailing NOPs may remain in the ring. Still
     * honor RPTR capacity before overwriting those bytes. No new DMA has been
     * published on this failure path, so a normal FALSE is safe here. */
    started = sdma_clock_ticks();
    deadline = ((UQUAD)sdma_clock_frequency + 4) / 5;
    for (i = 0; i < 100000; i++) {
        consumer = sdma_consumer(ad);
        used = sdma_producer - consumer;
        if (consumer <= sdma_producer &&
            used <= (SDMA_RING_DWORDS - SDMA_RING_FETCH_DWORDS - ring_dwords) * 4)
            break;
        if (sdma_clock_ticks() - started >= deadline) break;
    }
    if (consumer > sdma_producer ||
        used > (SDMA_RING_DWORDS - SDMA_RING_FETCH_DWORDS - ring_dwords) * 4) {
        sdma_ring_failed = TRUE;
        bug("[amdgpu-sdma] continuous-ring-v1 capacity fault BEFORE publish; prior data retired, queue disabled for this boot; RPTR=%08lx:%08lx producer=%08lx:%08lx\n",
            (unsigned long)(consumer >> 32), (unsigned long)(ULONG)consumer,
            (unsigned long)(sdma_producer >> 32), (unsigned long)(ULONG)sdma_producer);
        return FALSE;
    }

    sequence = ++sdma_sequence;
    if (!sequence) sequence = ++sdma_sequence;
    fence[0] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    fence[1] = (ULONG)completion_mc;
    fence[2] = (ULONG)(completion_mc >> 32);
    fence[3] = sequence;
    sdma_ring_store(ad, sdma_producer, pkt, dwords);
    sdma_ring_store(ad, sdma_producer + dwords * 4, fence, 4);
    if (padding)
        sdma_ring_store(ad, sdma_producer + (dwords + 4) * 4, nops, padding);

    /* Order CPU source/scratch and ring stores before the MMIO publication.
     * Drain posted aperture writes with an MMIO read on this device as well. */
    __sync_synchronize();
    (void)sdma_q_read(ad, inst, SDMA0_QUEUE0_RB_CNTL);
    next = sdma_producer + ring_dwords * 4;
    sdma_producer = next;

    started = sdma_clock_ticks();
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_WPTR, (ULONG)next);
    sdma_q_write(ad, inst, SDMA0_QUEUE0_RB_WPTR_HI, (ULONG)(next >> 32));

    for (i = 0; i < 200000; i++)
        if (sdma_completed_sequence(ad) == sequence)
            goto retired;

    /* Preserve the old fast poll as a measured boundary, not a timeout. Do
     * not print while waiting: serial latency would perturb the experiment.
     * A separate finite cap also bounds a stopped clock; expiry does not prove
     * engine quiescence and therefore enters fail-stop containment below.
     */
    legacy_elapsed = sdma_clock_ticks() - started;
    elapsed = legacy_elapsed;
    for (i = 0; elapsed < deadline && i < 16777216; i++) {
        if (sdma_completed_sequence(ad) == sequence) {
            elapsed = sdma_clock_ticks() - started;
            if (++late_retirements <= 8 ||
                !(late_retirements & (late_retirements - 1)))
                bug("[amdgpu-sdma] deadline-v1 late retirement #%lu inst%lu dwords=%lu ring_dwords=%lu legacy_us=%lu completed_us=%lu\n",
                    (unsigned long)late_retirements, (unsigned long)inst,
                    (unsigned long)dwords,
                    (unsigned long)ring_dwords,
                    (unsigned long)(legacy_elapsed * 1000000 / sdma_clock_frequency),
                    (unsigned long)(elapsed * 1000000 / sdma_clock_frequency));
            goto retired;
        }
        elapsed = sdma_clock_ticks() - started;
    }

    /* Preserve exact failure state before parking the submitting task. */
    {
        ULONG rptr = sdma_q_read(ad, inst, SDMA0_QUEUE0_RB_RPTR);
        ULONG wptr = sdma_q_read(ad, inst, SDMA0_QUEUE0_RB_WPTR);
        ULONG rc   = sdma_q_read(ad, inst, SDMA0_QUEUE0_RB_CNTL);
        bug("[amdgpu-sdma] submit TIMEOUT continuous-ring-v1 inst%lu dwords=%lu ring_dwords=%lu RPTR=%08lx WPTR=%08lx RB_CNTL=%08lx legacy_us=%lu elapsed_us=%lu wait_cap=%lu\n",
            (unsigned long)inst, (unsigned long)dwords,
            (unsigned long)ring_dwords,
            (unsigned long)rptr, (unsigned long)wptr, (unsigned long)rc,
            (unsigned long)(legacy_elapsed * 1000000 / sdma_clock_frequency),
            (unsigned long)(elapsed * 1000000 / sdma_clock_frequency),
            (unsigned long)(i == 16777216));
        bug("[amdgpu-sdma] sequence wanted=%lu observed=%lu producer=%08lx:%08lx STATUS_REG=%08lx\n",
            (unsigned long)sequence, (unsigned long)sdma_completed_sequence(ad),
            (unsigned long)(next >> 32), (unsigned long)(ULONG)next,
            (unsigned long)sdma_q_read(ad, inst, 0x0024));
    }
    sdma_unretired_stop();
    return FALSE; /* unreachable: never authorize fallback on unretired DMA */

retired:
    __sync_synchronize(); /* completion precedes CPU readback/scratch reuse */
    if (sequence <= 8 || !(sequence & (sequence - 1)))
        bug("[amdgpu-sdma] continuous-ring-v1 retired seq=%lu producer=%08lx:%08lx wraps=%lu dwords=%lu ring_dwords=%lu\n",
            (unsigned long)sequence, (unsigned long)(next >> 32),
            (unsigned long)(ULONG)next, (unsigned long)(next / (SDMA_RING_DWORDS * 4)),
            (unsigned long)dwords, (unsigned long)ring_dwords);
    return TRUE;
}

/* Serializing wrapper (audit P0-5). Every ring submission funnels through here.
 * The lock is skipped only before amdgpu_sdma_hw_init has initialized it (single-
 * threaded bring-up), so there is never contention on an uninitialized lock. */
static BOOL sdma_submit_and_wait(struct AmdgpuDevice *ad, ULONG inst,
                                 const ULONG *pkt, ULONG dwords)
{
    BOOL ok;

    if (sdma_lock_ready)
        ObtainSemaphore(&sdma_submit_lock);
    amdgpu_busy_begin(ad, AMDGPU_ENGINE_SDMA);
    ok = sdma_submit_and_wait_locked(ad, inst, pkt, dwords);
    amdgpu_busy_end(ad, AMDGPU_ENGINE_SDMA);
    if (sdma_lock_ready)
        ReleaseSemaphore(&sdma_submit_lock);
    return ok;
}

/*
 * 2D SUB-WINDOW blit: copy a RECTANGLE between two pitched surfaces.
 *
 * This is the packet a BitMap blit actually needs — COPY_LINEAR moves a flat
 * byte range, which is only useful when the copy happens to be full-width and
 * contiguous. Here the rectangle is deliberately offset in BOTH surfaces and
 * narrower than the pitch, so a packet that ignores offsets, or that treats
 * pitch as width, produces a visibly wrong picture instead of an accidentally
 * correct one.
 *
 * Layout from Mesa ac_cmdbuf_sdma.c (13 dwords), which is the authority here
 * because it demonstrably runs on this generation:
 *   0  header | log2(bpp) << 29
 *   1  src lo   2  src hi
 *   3  src_x | src_y << 16
 *   4  src_z | (src_pitch - 1) << pitch_shift
 *   5  src_slice_pitch - 1
 *   6  dst lo   7  dst hi
 *   8  dst_x | dst_y << 16
 *   9  dst_z | (dst_pitch - 1) << pitch_shift
 *   10 dst_slice_pitch - 1
 *   11 (w - 1) | (h - 1) << 16
 *   12 (d - 1)
 * Offsets, pitches and rect dimensions are in ELEMENTS (pixels), not bytes.
 *
 * `pitch_shift` is passed in because the AMD header and Mesa disagree about it
 * on this generation; the caller runs both and lets the silicon decide.
 *
 * Verification checks the WHOLE destination surface, not just the rectangle:
 * pixels inside the rect must equal the corresponding source pixels, and every
 * pixel OUTSIDE it must still hold its original fill. A blit that copies the
 * right pixels but also scribbles elsewhere fails.
 */
static BOOL sdma_subwin_blit(struct AmdgpuDevice *ad, ULONG inst,
                             ULONG pitch_shift, ULONG elemsize,
                             ULONG sx0, ULONG sy0, ULONG dx0, ULONG dy0,
                             ULONG rw, ULONG rh, const char *tag)
{
    const UQUAD src_mc   = SDMA_VRAM_MC_BASE + SDMA_BLIT_SRC_VRAM_OFF;
    const UQUAD dst_mc   = SDMA_VRAM_MC_BASE + SDMA_BLIT_DST_VRAM_OFF;
    const UQUAD fence_mc = sdma_fence_addr();
    const ULONG slice    = SDMA_SW_WIDTH * SDMA_SW_HEIGHT;   /* elements */
    ULONG pkt[20], v, x, y;
    ULONG inside_bad = 0, outside_bad = 0;
    ULONG first_bad_x = 0, first_bad_y = 0, got = 0, want = 0;
    BOOL  have_first = FALSE, retired;

    /* Fill both surfaces. Source is position-dependent in BOTH axes so a blit
     * that lands on the wrong row or column cannot pass; destination is a
     * distinct pattern so "did nothing" is distinguishable from "copied". */
    for (y = 0; y < SDMA_SW_HEIGHT; y++) {
        for (x = 0; x < SDMA_SW_WIDTH; x++) {
            ULONG off = (y * SDMA_SW_WIDTH + x) * SDMA_SW_BPP;
            v = 0x5A000000UL | (y << 8) | x;
            amdgpu_vram_write(ad, SDMA_BLIT_SRC_VRAM_OFF + off, &v, 1);
            v = 0xC7000000UL | (y << 8) | x;
            amdgpu_vram_write(ad, SDMA_BLIT_DST_VRAM_OFF + off, &v, 1);
        }
    }

    pkt[0]  = (ULONG)SDMA_OP_COPY
            | ((ULONG)SDMA_SUBOP_COPY_LINEAR_SUB_WIND << SDMA_COPY_HDR_SUBOP_SHIFT)
            | (1UL << SDMA_COPY_HDR_CPV_SHIFT)
            | (elemsize << SDMA_SW_HDR_ELEMSIZE_SHIFT);
    pkt[1]  = (ULONG)(src_mc & 0xFFFFFFFFUL);
    pkt[2]  = (ULONG)(src_mc >> 32);
    pkt[3]  = (ULONG)sx0 | ((ULONG)sy0 << 16);
    pkt[4]  = 0 | ((ULONG)(SDMA_SW_WIDTH - 1) << pitch_shift);
    pkt[5]  = slice - 1;
    pkt[6]  = (ULONG)(dst_mc & 0xFFFFFFFFUL);
    pkt[7]  = (ULONG)(dst_mc >> 32);
    pkt[8]  = (ULONG)dx0 | ((ULONG)dy0 << 16);
    pkt[9]  = 0 | ((ULONG)(SDMA_SW_WIDTH - 1) << pitch_shift);
    pkt[10] = slice - 1;
    pkt[11] = (ULONG)(rw - 1) | ((ULONG)(rh - 1) << 16);
    pkt[12] = 0;                                  /* depth 1 -> (d - 1) = 0 */
    pkt[13] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[14] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[15] = (ULONG)(fence_mc >> 32);
    pkt[16] = SDMA_FENCE_MAGIC;
    pkt[17] = (ULONG)SDMA_OP_NOP;
    pkt[18] = (ULONG)SDMA_OP_NOP;
    pkt[19] = (ULONG)SDMA_OP_NOP;

    retired = sdma_submit_and_wait(ad, inst, pkt, 20);

    for (y = 0; y < SDMA_SW_HEIGHT; y++) {
        for (x = 0; x < SDMA_SW_WIDTH; x++) {
            ULONG off = (y * SDMA_SW_WIDTH + x) * SDMA_SW_BPP;
            BOOL in_rect = (x >= dx0 && x < dx0 + rw &&
                            y >= dy0 && y < dy0 + rh);
            ULONG expect;

            if (in_rect) {
                ULONG sx = (x - dx0) + sx0;
                ULONG sy = (y - dy0) + sy0;
                expect = 0x5A000000UL | (sy << 8) | sx;
            } else {
                expect = 0xC7000000UL | (y << 8) | x;
            }

            amdgpu_vram_read(ad, SDMA_BLIT_DST_VRAM_OFF + off, &v, 1);
            if (v != expect) {
                if (in_rect) inside_bad++; else outside_bad++;
                if (!have_first) {
                    have_first = TRUE;
                    first_bad_x = x; first_bad_y = y; got = v; want = expect;
                }
            }
        }
    }

    bug("[amdgpu-sdma] SUBWIN[%s ps=%lu es=%lu]: %s  rect %lux%lu (%lu,%lu)->(%lu,%lu)  "
        "bad inside=%lu outside=%lu\n", tag, (unsigned long)pitch_shift,
        (unsigned long)elemsize,
        retired ? "retired" : "TIMEOUT",
        (unsigned long)rw, (unsigned long)rh,
        (unsigned long)sx0, (unsigned long)sy0,
        (unsigned long)dx0, (unsigned long)dy0,
        (unsigned long)inside_bad, (unsigned long)outside_bad);

    if (retired && inside_bad == 0 && outside_bad == 0) {
        bug("[amdgpu-sdma] *** SUBWIN[%s] 2D RECT BLIT VERIFIED — every pixel in the "
            "rectangle copied from the right source pixel, and nothing outside it "
            "was touched ***\n", tag);
        return TRUE;
    }

    if (have_first)
        bug("[amdgpu-sdma] SUBWIN[%s] first bad pixel (%lu,%lu) got=%08lx want=%08lx\n",
            tag, (unsigned long)first_bad_x, (unsigned long)first_bad_y,
            (unsigned long)got, (unsigned long)want);

    /* Full dump of the FIRST destination row and the source row it should have
     * come from. The per-row summary showed a repeating pattern that fits
     * "source elements arrive in duplicated pairs", but that was INFERRED from
     * three sampled positions; printing every element either confirms it or
     * kills it. Source values encode their own (x,y), so each printed word says
     * which source element actually landed there. */
    {
        ULONG n = (rw < 16) ? rw : 16;
        char  line[240];
        ULONG p = 0, k;

        for (k = 0; k < n; k++) {
            ULONG off = (dy0 * SDMA_SW_WIDTH + dx0 + k) * SDMA_SW_BPP;
            amdgpu_vram_read(ad, SDMA_BLIT_DST_VRAM_OFF + off, &v, 1);
            /* print just the source (y,x) the value encodes */
            line[p++] = ' ';
            line[p++] = "0123456789abcdef"[(v >> 12) & 0xF];
            line[p++] = "0123456789abcdef"[(v >> 8)  & 0xF];
            line[p++] = ':';
            line[p++] = "0123456789abcdef"[(v >> 4)  & 0xF];
            line[p++] = "0123456789abcdef"[v & 0xF];
        }
        line[p] = 0;
        bug("[amdgpu-sdma] SUBWIN[%s] dst row %lu landed (srcY:srcX):%s\n",
            tag, (unsigned long)dy0, line);

        p = 0;
        for (k = 0; k < n; k++) {
            ULONG off = (sy0 * SDMA_SW_WIDTH + sx0 + k) * SDMA_SW_BPP;
            amdgpu_vram_read(ad, SDMA_BLIT_SRC_VRAM_OFF + off, &v, 1);
            line[p++] = ' ';
            line[p++] = "0123456789abcdef"[(v >> 12) & 0xF];
            line[p++] = "0123456789abcdef"[(v >> 8)  & 0xF];
            line[p++] = ':';
            line[p++] = "0123456789abcdef"[(v >> 4)  & 0xF];
            line[p++] = "0123456789abcdef"[v & 0xF];
        }
        line[p] = 0;
        bug("[amdgpu-sdma] SUBWIN[%s] src row %lu expected      :%s\n",
            tag, (unsigned long)sy0, line);
    }

    /* Per-row structure. "Half the rectangle is wrong" is not diagnosable from a
     * single sample — whether the damage is by row, by column, or a uniform
     * shift determines which packet field is misinterpreted. The source pattern
     * encodes its own (x,y), so printing what LANDED at a known destination
     * pixel reads back as "the hardware fetched source pixel (sx,sy)", which
     * names the actual mapping instead of just flagging a mismatch. */
    for (y = dy0; y < dy0 + rh; y++) {
        ULONG rowbad = 0, v0 = 0, v1 = 0, vlast = 0;

        for (x = dx0; x < dx0 + rw; x++) {
            ULONG off = (y * SDMA_SW_WIDTH + x) * SDMA_SW_BPP;
            ULONG sx  = (x - dx0) + sx0;
            ULONG sy  = (y - dy0) + sy0;

            amdgpu_vram_read(ad, SDMA_BLIT_DST_VRAM_OFF + off, &v, 1);
            if (v != (0x5A000000UL | (sy << 8) | sx))
                rowbad++;
            if (x == dx0)     v0 = v;
            if (x == dx0 + 1) v1 = v;
            if (x == dx0 + rw - 1) vlast = v;
        }
        bug("[amdgpu-sdma] SUBWIN[%s]  dst row y=%lu: bad=%lu/%lu  [0]=%08lx [1]=%08lx "
            "[%lu]=%08lx (want [0]=%08lx)\n", tag, (unsigned long)y,
            (unsigned long)rowbad, (unsigned long)rw,
            (unsigned long)v0, (unsigned long)v1,
            (unsigned long)(rw - 1), (unsigned long)vlast,
            (unsigned long)(0x5A000000UL |
                (((y - dy0) + sy0) << 8) | sx0));
    }
    return FALSE;
}

/*
 * 2D RECTANGLE BLIT VIA PER-SCANLINE COPY_LINEAR.
 *
 * COPY_LINEAR_SUB_WIND is the right packet for a rectangle. It originally
 * duplicated source elements because the SDMA7 CPV header bit was missing.
 * With CPV set, the boot-time oracle verifies offset, no-offset, same-offset
 * and half-width cases exactly before the runtime path is enabled.
 *
 * COPY_LINEAR, however, is verified: 256/256 dwords exact. A rectangle is just
 * one linear copy per scanline, so that is what this does — correct today,
 * at the cost of one packet per row instead of one packet per blit. This is the
 * primitive the BitMap class can actually be built on.
 *
 * Limit worth stating: each row costs 8 ring dwords, so a full-height blit on a
 * real mode needs either a larger ring or chunked submission. The 1024-dword
 * ring here holds ~127 rows. Chunking is straightforward and not yet written.
 */
static BOOL sdma_rect_blit_scanline(struct AmdgpuDevice *ad, ULONG inst,
                                    ULONG sx0, ULONG sy0, ULONG dx0, ULONG dy0,
                                    ULONG rw, ULONG rh, const char *tag)
{
    const UQUAD fence_mc = sdma_fence_addr();
    const ULONG rowbytes = rw * SDMA_SW_BPP;
    ULONG pkt[8 * 24 + 8];
    ULONG n = 0, v, x, y, inside_bad = 0, outside_bad = 0;
    BOOL  retired;

    if (rh > 24)
        return FALSE;                     /* ring budget for this self-test */

    for (y = 0; y < SDMA_SW_HEIGHT; y++) {
        for (x = 0; x < SDMA_SW_WIDTH; x++) {
            ULONG off = (y * SDMA_SW_WIDTH + x) * SDMA_SW_BPP;
            v = 0x5A000000UL | (y << 8) | x;
            amdgpu_vram_write(ad, SDMA_BLIT_SRC_VRAM_OFF + off, &v, 1);
            v = 0xC7000000UL | (y << 8) | x;
            amdgpu_vram_write(ad, SDMA_BLIT_DST_VRAM_OFF + off, &v, 1);
        }
    }

    for (y = 0; y < rh; y++) {
        UQUAD s = SDMA_VRAM_MC_BASE + SDMA_BLIT_SRC_VRAM_OFF
                + (UQUAD)(((sy0 + y) * SDMA_SW_WIDTH + sx0) * SDMA_SW_BPP);
        UQUAD d = SDMA_VRAM_MC_BASE + SDMA_BLIT_DST_VRAM_OFF
                + (UQUAD)(((dy0 + y) * SDMA_SW_WIDTH + dx0) * SDMA_SW_BPP);

        pkt[n++] = (ULONG)SDMA_OP_COPY
                 | ((ULONG)SDMA_SUBOP_COPY_LINEAR << SDMA_COPY_HDR_SUBOP_SHIFT)
                 | (1UL << SDMA_COPY_HDR_CPV_SHIFT);
        pkt[n++] = rowbytes - 1;
        pkt[n++] = 0;
        pkt[n++] = (ULONG)(s & 0xFFFFFFFFUL);
        pkt[n++] = (ULONG)(s >> 32);
        pkt[n++] = (ULONG)(d & 0xFFFFFFFFUL);
        pkt[n++] = (ULONG)(d >> 32);
        pkt[n++] = 0;
    }
    pkt[n++] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[n++] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[n++] = (ULONG)(fence_mc >> 32);
    pkt[n++] = SDMA_FENCE_MAGIC;
    while (n & 7)
        pkt[n++] = (ULONG)SDMA_OP_NOP;

    retired = sdma_submit_and_wait(ad, inst, pkt, n);

    for (y = 0; y < SDMA_SW_HEIGHT; y++) {
        for (x = 0; x < SDMA_SW_WIDTH; x++) {
            ULONG off = (y * SDMA_SW_WIDTH + x) * SDMA_SW_BPP;
            BOOL  in  = (x >= dx0 && x < dx0 + rw && y >= dy0 && y < dy0 + rh);
            ULONG expect = in
                ? (0x5A000000UL | (((y - dy0) + sy0) << 8) | ((x - dx0) + sx0))
                : (0xC7000000UL | (y << 8) | x);

            amdgpu_vram_read(ad, SDMA_BLIT_DST_VRAM_OFF + off, &v, 1);
            if (v != expect) { if (in) inside_bad++; else outside_bad++; }
        }
    }

    bug("[amdgpu-sdma] RECT[%s] %lux%lu (%lu,%lu)->(%lu,%lu) via %lu scanline "
        "COPY_LINEARs (%lu ring dwords): %s  bad inside=%lu outside=%lu\n",
        tag, (unsigned long)rw, (unsigned long)rh,
        (unsigned long)sx0, (unsigned long)sy0, (unsigned long)dx0,
        (unsigned long)dy0, (unsigned long)rh, (unsigned long)n,
        retired ? "retired" : "TIMEOUT",
        (unsigned long)inside_bad, (unsigned long)outside_bad);

    if (retired && !inside_bad && !outside_bad) {
        bug("[amdgpu-sdma] *** RECT[%s] 2D RECTANGLE BLIT VERIFIED — every pixel from "
            "the right source pixel, nothing outside the rectangle touched ***\n", tag);
        return TRUE;
    }
    return FALSE;
}

/*
 * SYSTEM-MEMORY BLIT: VRAM <-> host RAM through the GART.
 *
 * This is the capability an AROS BitMap actually needs. Every blit so far has
 * been VRAM-to-VRAM using raw MC addresses inside the VRAM aperture, which
 * proves the engine works but not that it can reach a bitmap living in system
 * RAM — and on AROS that is where bitmaps live.
 *
 * The GART is already up and proven: amdgpu_gart_enable() runs during PSP init
 * and the PSP loads its entire firmware set through GART VAs with zero faults.
 * So this reuses that arena rather than building new address plumbing: a scratch
 * page near the TOP of the 32 MB arena, well clear of the low region the PSP
 * stages firmware into.
 *
 * The verification is the important part. The destination is read back BY THE
 * CPU, straight from the arena pointer — not through the VRAM window, not
 * through any GPU path. If the pattern is there, the GPU wrote host memory.
 * That is an oracle with no shared failure mode with the rest of this file.
 *
 * Both directions are tested, because they are different hardware paths and a
 * BitMap needs both: VRAM->sysmem is a readback/screenshot, sysmem->VRAM is
 * uploading a bitmap to the card.
 */
#define SDMA_SYS_SCRATCH_OFF  (24UL << 20)   /* into the GART arena, above PSP fw */
#define SDMA_SYS_DWORDS       256            /* 1 KiB each way */

static BOOL sdma_sysmem_blit(struct AmdgpuDevice *ad, ULONG inst)
{
    const UQUAD fence_mc = sdma_fence_addr();
    UQUAD sys_va;
    ULONG *sys;
    ULONG pkt[16], v, i, bad;
    BOOL  ok_down = FALSE, ok_up = FALSE;

    if (!ad->gart_ready || !ad->gart_arena ||
        ad->gart_arena_size < SDMA_SYS_SCRATCH_OFF + (SDMA_SYS_DWORDS * 4)) {
        bug("[amdgpu-sdma] GART not ready (ready=%d arena=%p size=0x%lx) — skipping the "
            "system-memory blit; host bitmaps stay unreachable\n",
            (int)ad->gart_ready, ad->gart_arena,
            (unsigned long)ad->gart_arena_size);
        return FALSE;
    }

    sys    = (ULONG *)(ad->gart_arena + SDMA_SYS_SCRATCH_OFF);
    sys_va = ad->gart_fw_va + (UQUAD)SDMA_SYS_SCRATCH_OFF;

    bug("[amdgpu-sdma] sysmem blit: GART VA 0x%08lx%08lx -> arena+0x%lx (cpu %p)\n",
        (unsigned long)(sys_va >> 32), (unsigned long)(ULONG)sys_va,
        (unsigned long)SDMA_SYS_SCRATCH_OFF, sys);

    /* ---- direction 1: VRAM -> system RAM ---- */
    for (i = 0; i < SDMA_SYS_DWORDS; i++) {
        v = 0x71000000UL | i;
        amdgpu_vram_write(ad, SDMA_BLIT_SRC_VRAM_OFF + (i * 4), &v, 1);
        sys[i] = 0xE5000000UL | i;         /* different fill, so no-op is visible */
    }
    {   /* push the CPU fill out before the GPU reads/overwrites it */
        ULONG len = SDMA_SYS_DWORDS * 4;
        CachePreDMA(sys, &len, DMA_ReadFromRAM);
    }

    i = 0;
    pkt[i++] = (ULONG)SDMA_OP_COPY
             | ((ULONG)SDMA_SUBOP_COPY_LINEAR << SDMA_COPY_HDR_SUBOP_SHIFT)
             | (1UL << SDMA_COPY_HDR_CPV_SHIFT);
    pkt[i++] = (SDMA_SYS_DWORDS * 4) - 1;
    pkt[i++] = 0;
    pkt[i++] = (ULONG)((SDMA_VRAM_MC_BASE + SDMA_BLIT_SRC_VRAM_OFF) & 0xFFFFFFFFUL);
    pkt[i++] = (ULONG)((SDMA_VRAM_MC_BASE + SDMA_BLIT_SRC_VRAM_OFF) >> 32);
    pkt[i++] = (ULONG)(sys_va & 0xFFFFFFFFUL);
    pkt[i++] = (ULONG)(sys_va >> 32);
    pkt[i++] = 0;
    pkt[i++] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[i++] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[i++] = (ULONG)(fence_mc >> 32);
    pkt[i++] = SDMA_FENCE_MAGIC;
    while (i & 7) pkt[i++] = (ULONG)SDMA_OP_NOP;

    if (sdma_submit_and_wait(ad, inst, pkt, i)) {
        ULONG len = SDMA_SYS_DWORDS * 4;
        CachePostDMA(sys, &len, 0);        /* GPU wrote it; make it CPU-visible */
        for (bad = 0, i = 0; i < SDMA_SYS_DWORDS; i++)
            if (sys[i] != (0x71000000UL | i)) bad++;
        ok_down = (bad == 0);
        bug("[amdgpu-sdma] VRAM -> SYSMEM: %lu/%lu dwords bad; sys[0]=%08lx sys[255]=%08lx "
            "(want 71000000 / 710000ff)%s\n", (unsigned long)bad,
            (unsigned long)SDMA_SYS_DWORDS, (unsigned long)sys[0],
            (unsigned long)sys[SDMA_SYS_DWORDS - 1],
            ok_down ? "  *** GPU WROTE HOST MEMORY ***" : "");
    } else
        bug("[amdgpu-sdma] VRAM -> SYSMEM: fence TIMEOUT\n");

    /* ---- direction 2: system RAM -> VRAM ---- */
    for (i = 0; i < SDMA_SYS_DWORDS; i++) {
        sys[i] = 0x3C000000UL | (i * 7);
        v = 0xB9000000UL | i;
        amdgpu_vram_write(ad, SDMA_BLIT_DST_VRAM_OFF + (i * 4), &v, 1);
    }
    {
        ULONG len = SDMA_SYS_DWORDS * 4;
        CachePreDMA(sys, &len, DMA_ReadFromRAM);
    }

    pkt[3] = (ULONG)(sys_va & 0xFFFFFFFFUL);
    pkt[4] = (ULONG)(sys_va >> 32);
    pkt[5] = (ULONG)((SDMA_VRAM_MC_BASE + SDMA_BLIT_DST_VRAM_OFF) & 0xFFFFFFFFUL);
    pkt[6] = (ULONG)((SDMA_VRAM_MC_BASE + SDMA_BLIT_DST_VRAM_OFF) >> 32);

    if (sdma_submit_and_wait(ad, inst, pkt, 16)) {
        for (bad = 0, i = 0; i < SDMA_SYS_DWORDS; i++) {
            amdgpu_vram_read(ad, SDMA_BLIT_DST_VRAM_OFF + (i * 4), &v, 1);
            if (v != (0x3C000000UL | (i * 7))) bad++;
        }
        ok_up = (bad == 0);
        bug("[amdgpu-sdma] SYSMEM -> VRAM: %lu/%lu dwords bad%s\n",
            (unsigned long)bad, (unsigned long)SDMA_SYS_DWORDS,
            ok_up ? "  *** GPU READ HOST MEMORY ***" : "");
    } else
        bug("[amdgpu-sdma] SYSMEM -> VRAM: fence TIMEOUT\n");

    /* A GART translation failure shows up here, not in the data compare. */
    amdgpu_gart_dump_fault(ad, "sdma-sysmem");

    if (ok_down && ok_up) {
        bug("[amdgpu-sdma] *** SYSTEM-MEMORY BLIT VERIFIED BOTH WAYS — host RAM is "
            "reachable by the copy engine through the GART; a BitMap in system memory "
            "can be a blit source AND destination ***\n");
        return TRUE;
    }
    return FALSE;
}

/*
 * The real thing: blit into a buffer AROS allocated, at an address the driver
 * did not choose, mapped on demand.
 *
 * The earlier system-memory test used the PSP firmware arena — already mapped,
 * at a fixed VA, 4 KB-aligned by construction. That proves the GART path works
 * but not that an arbitrary AROS allocation can be a blit target, which is what
 * a BitMap actually is. This allocates a buffer with plain AllocMem, maps it
 * through amdgpu_gart_map(), blits a pattern into it, and reads it back with the
 * CPU.
 *
 * The buffer is deliberately allocated with a NON-page-aligned payload offset,
 * because AllocMem guarantees only 8-byte alignment and a mapper that quietly
 * assumes page alignment would pass a friendly test and corrupt a real bitmap.
 */
#define SDMA_HOSTBUF_DWORDS  512          /* 2 KiB */
#define SDMA_HOSTBUF_SKEW    24           /* force a non-page-aligned payload */

static BOOL sdma_hostbuf_blit(struct AmdgpuDevice *ad, ULONG inst)
{
    const UQUAD fence_mc = sdma_fence_addr();
    const ULONG bytes = SDMA_HOSTBUF_DWORDS * 4;
    UBYTE *raw;
    ULONG *host;
    UQUAD  va;
    ULONG  pkt[16], v, i, bad = 0;
    BOOL   ok = FALSE;

    raw = AllocMem(bytes + SDMA_HOSTBUF_SKEW + 64, MEMF_ANY | MEMF_CLEAR);
    if (!raw) {
        bug("[amdgpu-sdma] hostbuf: AllocMem failed\n");
        return FALSE;
    }
    host = (ULONG *)(raw + SDMA_HOSTBUF_SKEW);

    va = amdgpu_gart_map(ad, host, bytes);
    if (!va) {
        bug("[amdgpu-sdma] hostbuf: amdgpu_gart_map failed — cannot reach an ordinary "
            "AROS allocation\n");
        FreeMem(raw, bytes + SDMA_HOSTBUF_SKEW + 64);
        return FALSE;
    }
    if ((ULONG)(va & (4096 - 1)) != (ULONG)((IPTR)host & (4096 - 1))) {
        bug("[amdgpu-sdma] hostbuf: VA in-page offset 0x%lx != buffer's 0x%lx — the mapper "
            "lost the alignment\n", (unsigned long)(ULONG)(va & 4095),
            (unsigned long)((IPTR)host & 4095));
    }

    for (i = 0; i < SDMA_HOSTBUF_DWORDS; i++) {
        v = 0x2B000000UL | (i * 3);
        amdgpu_vram_write(ad, SDMA_BLIT_SRC_VRAM_OFF + (i * 4), &v, 1);
        host[i] = 0xF0000000UL | i;        /* different fill: a no-op stays visible */
    }
    { ULONG len = bytes; CachePreDMA(host, &len, DMA_ReadFromRAM); }

    i = 0;
    pkt[i++] = (ULONG)SDMA_OP_COPY
             | ((ULONG)SDMA_SUBOP_COPY_LINEAR << SDMA_COPY_HDR_SUBOP_SHIFT)
             | (1UL << SDMA_COPY_HDR_CPV_SHIFT);
    pkt[i++] = bytes - 1;
    pkt[i++] = 0;
    pkt[i++] = (ULONG)((SDMA_VRAM_MC_BASE + SDMA_BLIT_SRC_VRAM_OFF) & 0xFFFFFFFFUL);
    pkt[i++] = (ULONG)((SDMA_VRAM_MC_BASE + SDMA_BLIT_SRC_VRAM_OFF) >> 32);
    pkt[i++] = (ULONG)(va & 0xFFFFFFFFUL);
    pkt[i++] = (ULONG)(va >> 32);
    pkt[i++] = 0;
    pkt[i++] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[i++] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[i++] = (ULONG)(fence_mc >> 32);
    pkt[i++] = SDMA_FENCE_MAGIC;
    while (i & 7) pkt[i++] = (ULONG)SDMA_OP_NOP;

    if (sdma_submit_and_wait(ad, inst, pkt, i)) {
        ULONG len = bytes;
        CachePostDMA(host, &len, 0);
        for (i = 0; i < SDMA_HOSTBUF_DWORDS; i++)
            if (host[i] != (0x2B000000UL | (i * 3))) bad++;
        ok = (bad == 0);
        bug("[amdgpu-sdma] HOSTBUF blit: %lu/%lu dwords bad; host[0]=%08lx host[511]=%08lx "
            "(want 2b000000 / 2b0005fd)%s\n", (unsigned long)bad,
            (unsigned long)SDMA_HOSTBUF_DWORDS, (unsigned long)host[0],
            (unsigned long)host[SDMA_HOSTBUF_DWORDS - 1],
            ok ? "  *** GPU BLITTED INTO AN ORDINARY AllocMem BUFFER ***" : "");
    } else
        bug("[amdgpu-sdma] HOSTBUF blit: fence TIMEOUT\n");

    amdgpu_gart_dump_fault(ad, "sdma-hostbuf");
    amdgpu_gart_unmap(ad, va, bytes);
    FreeMem(raw, bytes + SDMA_HOSTBUF_SKEW + 64);
    return ok;
}

/*
 * Measure the copy engine against the CPU, because "it works" says nothing about
 * whether it is worth using.
 *
 * Every report on this job has carried "NO performance measured" as a limitation,
 * and that gap matters now: the BitMap class inherits CLID_Hidd_ChunkyBM and does
 * its blits on the CPU. Replacing that with SDMA is only an improvement above
 * some size — a GPU blit costs a packet build, an MMIO doorbell-less write
 * pointer poke and a fence round trip, which is pure overhead on a small
 * rectangle. Wiring CopyBox to the GPU without knowing the crossover would be
 * guessing, and would likely make small blits SLOWER.
 *
 * The comparison is deliberately apples to apples: system RAM to system RAM in
 * both cases, since that is what a ChunkyBM blit actually is. Buffers are mapped
 * into the GART once, outside the timed loop, because a real BitMap would be
 * mapped once at creation and reused — charging every iteration for a mapping
 * would misrepresent the steady state.
 *
 * Timed with timer.device / ReadEClock. If the timer cannot be opened this
 * quietly does nothing: a benchmark must never be able to break the boot path.
 */
/* OFF, and its numbers are NOT trustworthy. Two problems, both measured:
 *
 * 1. It corrupts something. Enabling it reliably produces a GP fault in
 *    CloseDevice() during its own teardown (core_IRQHandle(13), boot dies at
 *    ~115 KB instead of ~425 KB). Fixing amdgpu_gart_map()'s CachePreDMA
 *    overreach (337fbc96ac) did NOT clear it, so that was a real bug but not
 *    this one. Remaining suspects: the timer.device open/close, or the 1 MiB
 *    AllocMem/FreeMem pair. Unresolved.
 *
 * 2. It is far too noisy to support the conclusion drawn from it. Back-to-back
 *    runs of the SAME CPU memcpy differ by ~60%: 4593294 vs 7360339 KiB/s at
 *    64 KiB, 4679047 vs 7410561 at 1 MiB. In the first run the GPU won at 1 MiB;
 *    in the second it lost at every size. One run, eight iterations, no
 *    repetition and no clock control is simply not a measurement.
 *
 * What survives is only the ORDER of magnitude at small sizes, where the GPU
 * loses by 19-24x and the gap is far larger than the noise: blit acceleration
 * must be size-gated. The crossover POINT is not established and must not be
 * quoted from this. A real measurement needs repetition, discarded warm-up,
 * variance reported, and fixed clocks. Set to 1 only to re-investigate. */
#define AMDGPU_SDMA_BENCH  0

#define SDMA_BENCH_MAX     (1UL << 20)     /* 1 MiB working set */
#define SDMA_BENCH_ITERS   8

#if AMDGPU_SDMA_BENCH
static BOOL sdma_bench(struct AmdgpuDevice *ad, ULONG inst)
{
    static const ULONG sizes[] = { 4096, 65536, 1048576 };
    const UQUAD fence_mc = sdma_fence_addr();
    struct MsgPort     *port = NULL;
    struct timerequest *treq = NULL;
    struct Device      *TimerBase = NULL;
    struct EClockVal    t0, t1;
    UBYTE *sbuf = NULL, *dbuf = NULL;
    UQUAD  sva = 0, dva = 0;
    ULONG  freq = 0, si, i, len, pkt[16];
    BOOL   ok = FALSE;

    if (!ad->gart_ready)
        return FALSE;

    port = CreateMsgPort();
    if (port)
        treq = (struct timerequest *)CreateIORequest(port, sizeof(struct timerequest));
    if (!treq || OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                            (struct IORequest *)treq, 0)) {
        bug("[amdgpu-sdma] bench: timer.device unavailable — skipping (not fatal)\n");
        goto out;
    }
    TimerBase = treq->tr_node.io_Device;
    freq = ReadEClock(&t0);
    if (!freq) {
        bug("[amdgpu-sdma] bench: EClock frequency 0 — skipping\n");
        goto out;
    }

    sbuf = AllocMem(SDMA_BENCH_MAX, MEMF_ANY | MEMF_CLEAR);
    dbuf = AllocMem(SDMA_BENCH_MAX, MEMF_ANY | MEMF_CLEAR);
    if (!sbuf || !dbuf) { bug("[amdgpu-sdma] bench: AllocMem failed\n"); goto out; }

    sva = amdgpu_gart_map(ad, sbuf, SDMA_BENCH_MAX);
    dva = amdgpu_gart_map(ad, dbuf, SDMA_BENCH_MAX);
    if (!sva || !dva) { bug("[amdgpu-sdma] bench: gart_map failed\n"); goto out; }

    len = SDMA_BENCH_MAX; CachePreDMA(sbuf, &len, DMA_ReadFromRAM);
    len = SDMA_BENCH_MAX; CachePreDMA(dbuf, &len, DMA_ReadFromRAM);

    bug("[amdgpu-sdma] bench: EClock %lu Hz, %lu iterations, sysmem->sysmem both ways\n",
        (unsigned long)freq, (unsigned long)SDMA_BENCH_ITERS);

    for (si = 0; si < 3; si++) {
        ULONG n = sizes[si];
        UQUAD cpu_ticks, gpu_ticks;
        ULONG cpu_kbs = 0, gpu_kbs = 0;

        /* ---- CPU ---- */
        ReadEClock(&t0);
        for (i = 0; i < SDMA_BENCH_ITERS; i++)
            CopyMem(sbuf, dbuf, n);
        ReadEClock(&t1);
        cpu_ticks = ((UQUAD)t1.ev_hi << 32 | t1.ev_lo)
                  - ((UQUAD)t0.ev_hi << 32 | t0.ev_lo);

        /* ---- GPU ---- */
        i = 0;
        pkt[i++] = (ULONG)SDMA_OP_COPY
                 | ((ULONG)SDMA_SUBOP_COPY_LINEAR << SDMA_COPY_HDR_SUBOP_SHIFT)
                 | (1UL << SDMA_COPY_HDR_CPV_SHIFT);
        pkt[i++] = n - 1;
        pkt[i++] = 0;
        pkt[i++] = (ULONG)(sva & 0xFFFFFFFFUL);
        pkt[i++] = (ULONG)(sva >> 32);
        pkt[i++] = (ULONG)(dva & 0xFFFFFFFFUL);
        pkt[i++] = (ULONG)(dva >> 32);
        pkt[i++] = 0;
        pkt[i++] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
        pkt[i++] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
        pkt[i++] = (ULONG)(fence_mc >> 32);
        pkt[i++] = SDMA_FENCE_MAGIC;
        while (i & 7) pkt[i++] = (ULONG)SDMA_OP_NOP;

        ReadEClock(&t0);
        for (len = 0; len < SDMA_BENCH_ITERS; len++)
            if (!sdma_submit_and_wait(ad, inst, pkt, i)) break;
        ReadEClock(&t1);
        gpu_ticks = ((UQUAD)t1.ev_hi << 32 | t1.ev_lo)
                  - ((UQUAD)t0.ev_hi << 32 | t0.ev_lo);

        /* KiB/s = bytes*freq/ticks/1024, computed to avoid 64-bit overflow */
        if (cpu_ticks)
            cpu_kbs = (ULONG)(((UQUAD)n * SDMA_BENCH_ITERS / 1024 * freq) / cpu_ticks);
        if (gpu_ticks)
            gpu_kbs = (ULONG)(((UQUAD)n * SDMA_BENCH_ITERS / 1024 * freq) / gpu_ticks);

        bug("[amdgpu-sdma] bench %7lu B:  CPU %8lu KiB/s   GPU %8lu KiB/s   -> GPU is %s\n",
            (unsigned long)n, (unsigned long)cpu_kbs, (unsigned long)gpu_kbs,
            (gpu_kbs > cpu_kbs) ? "FASTER" : "slower");
        ok = TRUE;
    }

out:
    /* Step markers: a boot after this function currently dies with
     * core_IRQHandle(13) (general protection fault) and the serial log stops
     * here, so the log has to name which teardown call it is rather than leaving
     * it to inference. Cheap, and removed once the fault is understood. */
    bug("[amdgpu-sdma] bench teardown: unmap\n");
    if (sva) amdgpu_gart_unmap(ad, sva, SDMA_BENCH_MAX);
    if (dva) amdgpu_gart_unmap(ad, dva, SDMA_BENCH_MAX);
    bug("[amdgpu-sdma] bench teardown: freemem\n");
    if (sbuf) FreeMem(sbuf, SDMA_BENCH_MAX);
    if (dbuf) FreeMem(dbuf, SDMA_BENCH_MAX);
    bug("[amdgpu-sdma] bench teardown: closedevice\n");
    if (TimerBase) CloseDevice((struct IORequest *)treq);
    bug("[amdgpu-sdma] bench teardown: delete iorequest\n");
    if (treq) DeleteIORequest((struct IORequest *)treq);
    bug("[amdgpu-sdma] bench teardown: delete msgport\n");
    if (port) DeleteMsgPort(port);
    bug("[amdgpu-sdma] bench teardown: done\n");
    return ok;
}
#endif /* AMDGPU_SDMA_BENCH */

/*
 * THE MEASUREMENT THAT MATTERS: system RAM -> VRAM, CPU versus SDMA.
 *
 * The earlier benchmark compared CPU and SDMA doing sysmem->sysmem, and
 * concluded the CPU wins below ~1 MiB so blit acceleration must be size-gated.
 * That was the WRONG OPERATION. A display driver does not copy RAM to RAM; it
 * copies RAM to the SCANOUT, which lives in VRAM behind a PCI BAR.
 *
 * And that BAR is mapped uncached. arch/x86_64-pc/kernel/mmu.c builds every
 * identity 2 MiB page with pwt=0 pcd=0 pat=0 (PAT index 0), AROS never programs
 * IA32_PAT or any MTRR, and the default PAT has NO write-combining entry at all
 * — so with the firmware MTRRs marking the PCI hole UC, the effective type for
 * the framebuffer is UC. Every CPU pixel write is a separate uncached PCIe
 * transaction. That is why the desktop feels slow, and it is also why the
 * previous benchmark flattered the CPU: it never touched VRAM.
 *
 * So: time the same number of bytes landing in VRAM, once via the CPU through
 * the BAR aperture, once via SDMA from a GART-mapped system buffer.
 *
 * Deliberately allocates and maps ONCE and never frees or unmaps. That is a
 * leak in a measurement build, and it is on purpose: the GP fault that killed
 * the previous benchmark appeared in CloseDevice() right after its FreeMem
 * calls, so avoiding the free/unmap path both removes the crash risk and tests
 * whether that path was the corruption.
 */
#define AMDGPU_FBPERF 1
#if AMDGPU_FBPERF

#define FBPERF_BYTES   (256UL << 10)      /* 256 KiB, ~a quarter of a 1024x768 screen */
#define FBPERF_ITERS   4
#define FBPERF_VRAM_OFF ((UQUAD)736 << 20)

static void sdma_fbperf(struct AmdgpuDevice *ad, ULONG inst)
{
    const UQUAD fence_mc = sdma_fence_addr();
    const UQUAD dst_mc   = SDMA_VRAM_MC_BASE + FBPERF_VRAM_OFF;
    struct MsgPort     *port = NULL;
    struct timerequest *treq = NULL;
    struct Device      *TimerBase = NULL;
    struct EClockVal    t0, t1;
    static UBYTE *src;                 /* allocated once, never freed */
    static UQUAD  src_va;
    ULONG freq, i, len, pkt[16];
    UQUAD cpu_ticks, gpu_ticks;
    ULONG cpu_kbs = 0, gpu_kbs = 0;

    if (!ad->gart_ready)
        return;

    port = CreateMsgPort();
    if (port)
        treq = (struct timerequest *)CreateIORequest(port, sizeof(struct timerequest));
    if (!treq || OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                            (struct IORequest *)treq, 0)) {
        bug("[amdgpu-fbperf] timer.device unavailable — skipping\n");
        return;
    }
    TimerBase = treq->tr_node.io_Device;
    freq = ReadEClock(&t0);
    if (!freq) { bug("[amdgpu-fbperf] EClock 0 — skipping\n"); return; }

    if (!src) {
        src = AllocMem(FBPERF_BYTES, MEMF_ANY | MEMF_CLEAR);
        if (!src) { bug("[amdgpu-fbperf] AllocMem failed\n"); return; }
        src_va = amdgpu_gart_map(ad, src, FBPERF_BYTES);
        if (!src_va) { bug("[amdgpu-fbperf] gart_map failed\n"); return; }
        len = FBPERF_BYTES; CachePreDMA(src, &len, DMA_ReadFromRAM);
    }

    /* CPU -> VRAM through the BAR aperture, which is what a software blit does. */
    ReadEClock(&t0);
    for (i = 0; i < FBPERF_ITERS; i++)
        amdgpu_vram_write(ad, FBPERF_VRAM_OFF, (const ULONG *)src, FBPERF_BYTES / 4);
    ReadEClock(&t1);
    cpu_ticks = ((UQUAD)t1.ev_hi << 32 | t1.ev_lo) - ((UQUAD)t0.ev_hi << 32 | t0.ev_lo);

    /* SDMA -> VRAM from the same system buffer. */
    i = 0;
    pkt[i++] = (ULONG)SDMA_OP_COPY
             | ((ULONG)SDMA_SUBOP_COPY_LINEAR << SDMA_COPY_HDR_SUBOP_SHIFT)
             | (1UL << SDMA_COPY_HDR_CPV_SHIFT);
    pkt[i++] = FBPERF_BYTES - 1;
    pkt[i++] = 0;
    pkt[i++] = (ULONG)(src_va & 0xFFFFFFFFUL);
    pkt[i++] = (ULONG)(src_va >> 32);
    pkt[i++] = (ULONG)(dst_mc & 0xFFFFFFFFUL);
    pkt[i++] = (ULONG)(dst_mc >> 32);
    pkt[i++] = 0;
    pkt[i++] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[i++] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[i++] = (ULONG)(fence_mc >> 32);
    pkt[i++] = SDMA_FENCE_MAGIC;
    while (i & 7) pkt[i++] = (ULONG)SDMA_OP_NOP;

    ReadEClock(&t0);
    for (len = 0; len < FBPERF_ITERS; len++)
        if (!sdma_submit_and_wait(ad, inst, pkt, i)) break;
    ReadEClock(&t1);
    gpu_ticks = ((UQUAD)t1.ev_hi << 32 | t1.ev_lo) - ((UQUAD)t0.ev_hi << 32 | t0.ev_lo);

    if (cpu_ticks)
        cpu_kbs = (ULONG)(((UQUAD)FBPERF_BYTES * FBPERF_ITERS / 1024 * freq) / cpu_ticks);
    if (gpu_ticks)
        gpu_kbs = (ULONG)(((UQUAD)FBPERF_BYTES * FBPERF_ITERS / 1024 * freq) / gpu_ticks);

    bug("[amdgpu-fbperf] *** sysmem -> VRAM, %lu KiB x%lu:  CPU-via-BAR %lu KiB/s   "
        "SDMA %lu KiB/s   -> GPU is %s ***\n",
        (unsigned long)(FBPERF_BYTES >> 10), (unsigned long)FBPERF_ITERS,
        (unsigned long)cpu_kbs, (unsigned long)gpu_kbs,
        (gpu_kbs > cpu_kbs) ? "FASTER" : "slower");

    /* Deliberately no FreeMem / no gart_unmap; see the comment above. */
    if (TimerBase) CloseDevice((struct IORequest *)treq);
    DeleteIORequest((struct IORequest *)treq);
    DeleteMsgPort(port);
    bug("[amdgpu-fbperf] teardown survived (no FreeMem, no gart_unmap)\n");
}
#endif /* AMDGPU_FBPERF */

/*
 * THE POINT OF ALL OF THIS: blit a damage rectangle from a system-RAM bitmap
 * into the scanout, with the copy engine instead of the CPU.
 *
 * Measured on this DUT, sysmem -> VRAM at 256 KiB:
 *     CPU through the BAR aperture      5146 KiB/s   (~5 MB/s)
 *     SDMA                           3232318 KiB/s   (~3.1 GB/s)
 * 628x. The CPU number is that bad because the framebuffer BAR is mapped
 * UNCACHED — core_InitMMU() builds every identity PDE with pwt=0/pcd=0/pat=0,
 * AROS never programs IA32_PAT or MTRRs, and the default PAT has no
 * write-combining entry — so every pixel write is its own PCIe transaction. At
 * 5 MB/s a full 1920x1080x32 update takes ~1.6 seconds, which is exactly the
 * slowness terminills reported.
 *
 * One packet per scanline, because COPY_LINEAR_SUB_WIND is broken on this part
 * (see 09b316d4c1: source elements arrive duplicated in pairs across the first
 * half of the rows, cause unexplained). Per-scanline COPY_LINEAR is verified
 * exact and, at a 628x starting advantage, the per-row packet overhead is not
 * what decides this.
 *
 * The source bitmap is GART-mapped ONCE and the VA cached on the bitmap, since
 * mapping per refresh would cost a TLB invalidate on both hubs every frame.
 *
 * Returns FALSE if anything is unavailable so the caller can fall back to the
 * CPU path — this must never be the reason a screen fails to update.
 */
/*
 * amdgpu_sdma_copy_rect_vram -- rectangular VRAM -> VRAM blit.
 *
 * The sibling of amdgpu_sdma_blit_rect() for the case where BOTH surfaces are
 * already MC-addressable, which is now the normal case: displayable bitmaps
 * live in the VRAM pool. No GART mapping, no VA cache, nothing to invalidate --
 * the source is simply another MC address.
 *
 * This remains the format-agnostic fallback. The preferred 32-bit presentation
 * path uses COPY_LINEAR_SUB_WIND after its boot-time content oracle passes.
 *
 * The caller may submit up to AMDGPU_SDMA_COPY_RECT_MAX_ROWS rows. At 120
 * rows the 8-dword scanline packets plus both fences and fetch alignment use
 * 976 of the 1024 ring dwords: two usable 64-byte fetch units remain beyond
 * the fetch unit that the continuous-ring capacity rule always leaves unused.
 */
BOOL amdgpu_sdma_copy_rect_vram(struct AmdgpuDevice *ad,
                                UQUAD src_mc, ULONG src_stride, ULONG src_off,
                                UQUAD dst_mc, ULONG dst_stride, ULONG dst_off,
                                ULONG row_bytes, ULONG rows)
{
    const UQUAD fence_mc = sdma_fence_addr();
    ULONG pkt[8 * AMDGPU_SDMA_COPY_RECT_MAX_ROWS + 8];
    ULONG n = 0, y;

    if (!ad || !src_mc || !dst_mc || !rows || !row_bytes)
        return FALSE;
    if (rows > AMDGPU_SDMA_COPY_RECT_MAX_ROWS)
        return FALSE;                 /* caller chunks */

    for (y = 0; y < rows; y++) {
        UQUAD s = src_mc + src_off + (UQUAD)y * src_stride;
        UQUAD d = dst_mc + dst_off + (UQUAD)y * dst_stride;

        pkt[n++] = (ULONG)SDMA_OP_COPY
                 | ((ULONG)SDMA_SUBOP_COPY_LINEAR << SDMA_COPY_HDR_SUBOP_SHIFT)
                 | (1UL << SDMA_COPY_HDR_CPV_SHIFT);   /* Linux SDMA7 always sets CPV; the
                                                        * passing BLIT/RECT self-tests + image
                                                        * DMA set it, this one didn't -> GHC
                                                        * displayable->displayable CopyBox FAIL
                                                        * (codex TRIG-0543) */
        pkt[n++] = row_bytes - 1;
        pkt[n++] = 0;
        pkt[n++] = (ULONG)(s & 0xFFFFFFFFUL);
        pkt[n++] = (ULONG)(s >> 32);
        pkt[n++] = (ULONG)(d & 0xFFFFFFFFUL);
        pkt[n++] = (ULONG)(d >> 32);
        pkt[n++] = 0;
    }

    pkt[n++] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[n++] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[n++] = (ULONG)(fence_mc >> 32);
    pkt[n++] = SDMA_FENCE_MAGIC;
    while (n & 7)
        pkt[n++] = (ULONG)SDMA_OP_NOP;

    return sdma_submit_and_wait(ad, 0, pkt, n);
}

/* Copy one 32-bit rectangle with the SDMA7 SUB_WIND packet. The addresses are
 * advanced to the rectangle origins, so x/y are zero while the pitches retain
 * the parent-surface layout. This is intentionally separate from the generic
 * scanline helper: callers can fall back after any eligibility or retirement
 * failure without recursively selecting SUB_WIND again. */
BOOL amdgpu_sdma_copy_rect_vram32(struct AmdgpuDevice *ad,
                                  UQUAD src_mc, ULONG src_stride, ULONG src_off,
                                  UQUAD dst_mc, ULONG dst_stride, ULONG dst_off,
                                  ULONG row_bytes, ULONG rows)
{
    const UQUAD fence_mc = sdma_fence_addr();
    UQUAD src_slice, dst_slice;
    ULONG src_pitch, dst_pitch, width;
    ULONG pkt[20];
    static BOOL announced;

    if (!ad || !sdma_subwin32_ready || !src_mc || !dst_mc ||
        !row_bytes || !rows ||
        ((src_mc | dst_mc | src_off | dst_off |
          src_stride | dst_stride | row_bytes) & 3))
        return FALSE;
    if (src_mc > ~(UQUAD)0 - src_off || dst_mc > ~(UQUAD)0 - dst_off)
        return FALSE;

    src_pitch = src_stride >> 2;
    dst_pitch = dst_stride >> 2;
    width = row_bytes >> 2;
    if (!src_pitch || !dst_pitch || !width ||
        src_pitch > 65536 || dst_pitch > 65536 ||
        width > 65536 || rows > 65536 ||
        width > src_pitch || width > dst_pitch)
        return FALSE;

    src_slice = (UQUAD)src_pitch * rows;
    dst_slice = (UQUAD)dst_pitch * rows;
    if (!src_slice || !dst_slice ||
        src_slice > (UQUAD)~(ULONG)0 + 1 ||
        dst_slice > (UQUAD)~(ULONG)0 + 1)
        return FALSE;

    src_mc += src_off;
    dst_mc += dst_off;
    pkt[0]  = (ULONG)SDMA_OP_COPY
            | ((ULONG)SDMA_SUBOP_COPY_LINEAR_SUB_WIND << SDMA_COPY_HDR_SUBOP_SHIFT)
            | (1UL << SDMA_COPY_HDR_CPV_SHIFT)
            | ((ULONG)SDMA_SW_ELEMSIZE << SDMA_SW_HDR_ELEMSIZE_SHIFT);
    pkt[1]  = (ULONG)src_mc;
    pkt[2]  = (ULONG)(src_mc >> 32);
    pkt[3]  = 0;                                  /* source x=0, y=0 */
    pkt[4]  = (src_pitch - 1) << SDMA_SW_PITCH_SHIFT_MESA;
    pkt[5]  = (ULONG)(src_slice - 1);
    pkt[6]  = (ULONG)dst_mc;
    pkt[7]  = (ULONG)(dst_mc >> 32);
    pkt[8]  = 0;                                  /* destination x=0, y=0 */
    pkt[9]  = (dst_pitch - 1) << SDMA_SW_PITCH_SHIFT_MESA;
    pkt[10] = (ULONG)(dst_slice - 1);
    pkt[11] = (width - 1) | ((rows - 1) << 16);
    pkt[12] = 0;                                  /* depth 1 */
    pkt[13] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[14] = (ULONG)fence_mc;
    pkt[15] = (ULONG)(fence_mc >> 32);
    pkt[16] = SDMA_FENCE_MAGIC;
    pkt[17] = pkt[18] = pkt[19] = (ULONG)SDMA_OP_NOP;

    if (!sdma_submit_and_wait(ad, 0, pkt, 20))
        return FALSE;
    if (!announced) {
        announced = TRUE;
        bug("[amdgpu-sdma] runtime SUBWIN32 active: %lux%lu, pitches %lu/%lu, "
            "one 20-dword packet\n", (unsigned long)width,
            (unsigned long)rows, (unsigned long)src_pitch,
            (unsigned long)dst_pitch);
    }
    return TRUE;
}

BOOL amdgpu_sdma_blit_rect(struct AmdgpuDevice *ad, UQUAD *src_va_cache,
                           ULONG *src_size_cache, APTR src_cpu, ULONG src_total,
                           ULONG src_stride, ULONG src_off,
                           UQUAD dst_mc, ULONG dst_stride, ULONG dst_off,
                           ULONG row_bytes, ULONG rows)
{
    const UQUAD fence_mc = sdma_fence_addr();
    ULONG pkt[8 * 64 + 8];
    ULONG n = 0, y;
    UQUAD src_va;

    if (!ad || !ad->gart_ready || !dst_mc || !rows || !row_bytes)
        return FALSE;
    if (rows > 64)
        return FALSE;                 /* caller chunks; ring budget for one pass */

    if (!*src_va_cache) {
        *src_va_cache = amdgpu_gart_map(ad, src_cpu, src_total);
        if (!*src_va_cache)
            return FALSE;
        *src_size_cache = src_total;
        bug("[amdgpu-sdma] scanout blit: bitmap %p (%lu bytes) mapped at VA 0x%08lx%08lx\n",
            src_cpu, (unsigned long)src_total,
            (unsigned long)(*src_va_cache >> 32), (unsigned long)(ULONG)*src_va_cache);
    }
    src_va = *src_va_cache;

    for (y = 0; y < rows; y++) {
        UQUAD s = src_va + src_off + (UQUAD)y * src_stride;
        UQUAD d = dst_mc  + dst_off + (UQUAD)y * dst_stride;

        pkt[n++] = (ULONG)SDMA_OP_COPY
                 | ((ULONG)SDMA_SUBOP_COPY_LINEAR << SDMA_COPY_HDR_SUBOP_SHIFT)
                 | (1UL << SDMA_COPY_HDR_CPV_SHIFT);
        pkt[n++] = row_bytes - 1;
        pkt[n++] = 0;
        pkt[n++] = (ULONG)(s & 0xFFFFFFFFUL);
        pkt[n++] = (ULONG)(s >> 32);
        pkt[n++] = (ULONG)(d & 0xFFFFFFFFUL);
        pkt[n++] = (ULONG)(d >> 32);
        pkt[n++] = 0;
    }
    pkt[n++] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[n++] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[n++] = (ULONG)(fence_mc >> 32);
    pkt[n++] = SDMA_FENCE_MAGIC;
    while (n & 7)
        pkt[n++] = (ULONG)SDMA_OP_NOP;

    return sdma_submit_and_wait(ad, 0, pkt, n);
}

/*
 * Image DMA through a PERSISTENT GART scratch buffer -- the PutImage/GetImage
 * fast path.
 *
 * amdgpu_sdma_blit_rect() above maps the SOURCE and caches its VA on the bitmap,
 * which only works for a stable surface. PutImage/GetImage move a TRANSIENT
 * caller buffer (msg->pixels) that changes every call and cannot own a cached
 * VA -- and amdgpu_gart_map() is a bump allocator (amdgpu_gart.c:417), so a
 * per-call map would leak GART VA forever. Instead we map ONE scratch buffer
 * once (mirroring sdma_fence_init) and stage every transfer through it:
 *
 *   UPLOAD   (PutImage): memcpy caller -> scratch (cached RAM, fast),
 *                        then SDMA COPY_LINEAR scratch_VA -> dst VRAM MC.
 *   DOWNLOAD (GetImage): SDMA COPY_LINEAR src VRAM MC -> scratch_VA,
 *                        then memcpy scratch -> caller.
 *
 * The win is large because the VRAM BAR is CPU-mapped UNCACHED (~5 MB/s,
 * amdgpu_gfx_support.c:613) while a cached-RAM memcpy and the GPU DMA both run
 * at GB/s -- a ~100x speedup for a full-window upload. Byte-for-byte identical
 * to the CPU CopyMemBox32 path: COPY_LINEAR is a raw copy, so callers use this
 * ONLY for the Native (no-conversion) format; the ARGB32 byteswap path stays on
 * the CPU. x86 PCIe DMA is cache-coherent (snooped), so no per-transfer cache
 * flush is needed -- the same assumption amdgpu_sdma_blit_rect relies on for the
 * live scanout refresh.
 */
#define SDMA_SCRATCH_BYTES   (2UL * 1024UL * 1024UL)   /* 64 rows up to ~8K wide @32bpp */

static UBYTE *sdma_scratch_cpu;    /* NULL => not yet allocated / unavailable */
static UQUAD  sdma_scratch_va;     /* GART VA of the scratch (mapped once)     */

/* DMA engine-path evidence (codex TRIG-0537). Cumulative since boot; read via
   amdgpu_sdma_dma_counts(). Incremented under sdma_submit_lock (the transfer is
   already serialized there), so no separate guard is needed. */
static ULONG sdma_dma_up_attempt, sdma_dma_up_ok;
static ULONG sdma_dma_dn_attempt, sdma_dma_dn_ok;

void amdgpu_sdma_dma_counts(ULONG *up_attempt, ULONG *up_ok,
                            ULONG *dn_attempt, ULONG *dn_ok)
{
    if (up_attempt) *up_attempt = sdma_dma_up_attempt;
    if (up_ok)      *up_ok      = sdma_dma_up_ok;
    if (dn_attempt) *dn_attempt = sdma_dma_dn_attempt;
    if (dn_ok)      *dn_ok      = sdma_dma_dn_ok;
}

/* Transform one row of `px` pixels from src ULONGs into dst ULONGs.
 *   RAW         -> straight copy (Native format; no conversion)
 *   BSWAP       -> reverse channel bytes (ARGB32 -> native, PutImage)
 *   BSWAP_ALPHA -> reverse + force opaque low byte (native -> ARGB32, GetImage)
 * These mirror amdgpu_bitmapclass.c exactly so the DMA path is byte-identical to
 * the CPU CopyMemBox32/byteswap path it replaces. */
static void sdma_xform_row(const ULONG *src, ULONG *dst, ULONG px, ULONG xform)
{
    ULONG i;
    switch (xform) {
    case AMDGPU_XFER_BSWAP:
        /* [A,R,G,B] -> [B,G,R,0]. The native 4th byte is PADDING (the driver
         * advertises no framebuffer alpha), so it must be 0, not the source
         * alpha -- codex's native-pixel oracle checks it (TRIG-0539). */
        for (i = 0; i < px; i++) dst[i] = __builtin_bswap32(src[i]) & 0x00FFFFFFUL;
        break;
    case AMDGPU_XFER_BSWAP_ALPHA:
        for (i = 0; i < px; i++) dst[i] = (__builtin_bswap32(src[i]) & 0xFFFFFF00UL) | 0x000000FFUL;
        break;
    default: /* AMDGPU_XFER_RAW */
        CopyMem((APTR)src, dst, (ULONG)px * 4);
        break;
    }
}

static BOOL sdma_scratch_init(struct AmdgpuDevice *ad)
{
    if (sdma_scratch_cpu)       return TRUE;
    if (!ad || !ad->gart_ready) return FALSE;

    sdma_scratch_cpu = AllocMem(SDMA_SCRATCH_BYTES, MEMF_ANY | MEMF_CLEAR);
    if (!sdma_scratch_cpu)      return FALSE;

    sdma_scratch_va = amdgpu_gart_map(ad, sdma_scratch_cpu, SDMA_SCRATCH_BYTES);
    if (!sdma_scratch_va) {
        FreeMem(sdma_scratch_cpu, SDMA_SCRATCH_BYTES);
        sdma_scratch_cpu = NULL;
        return FALSE;
    }
    bug("[amdgpu-sdma] image DMA scratch: cpu %p VA 0x%08lx%08lx (%lu KiB)\n",
        sdma_scratch_cpu,
        (unsigned long)(sdma_scratch_va >> 32), (unsigned long)(ULONG)sdma_scratch_va,
        (unsigned long)(SDMA_SCRATCH_BYTES >> 10));
    return TRUE;
}

/* Emit `rows` per-scanline COPY_LINEAR packets between two SDMA addresses
 * (either may be a VRAM MC address or a GART VA), then a trailing fence, and
 * submit. Same packet shape as copy_rect_vram; rows<=64 (ring budget).
 *
 * CALLER MUST HOLD sdma_submit_lock: the upload/download helpers own the shared
 * scratch across stage+submit+unstage and hold the lock for that whole span
 * (codex TRIG-0536), so this submits through the _locked ring core rather than
 * re-taking the lock. It has no other callers. */
static BOOL sdma_linear_rows(struct AmdgpuDevice *ad,
                             UQUAD src, ULONG src_stride,
                             UQUAD dst, ULONG dst_stride,
                             ULONG row_bytes, ULONG rows)
{
    const UQUAD fence_mc = sdma_fence_addr();
    ULONG pkt[8 * 64 + 8];
    ULONG n = 0, y;

    if (rows > 64) return FALSE;
    for (y = 0; y < rows; y++) {
        UQUAD s = src + (UQUAD)y * src_stride;
        UQUAD d = dst + (UQUAD)y * dst_stride;
        pkt[n++] = (ULONG)SDMA_OP_COPY
                 | ((ULONG)SDMA_SUBOP_COPY_LINEAR << SDMA_COPY_HDR_SUBOP_SHIFT)
                 | (1UL << SDMA_COPY_HDR_CPV_SHIFT);
        pkt[n++] = row_bytes - 1;
        pkt[n++] = 0;
        pkt[n++] = (ULONG)(s & 0xFFFFFFFFUL);
        pkt[n++] = (ULONG)(s >> 32);
        pkt[n++] = (ULONG)(d & 0xFFFFFFFFUL);
        pkt[n++] = (ULONG)(d >> 32);
        pkt[n++] = 0;
    }
    pkt[n++] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[n++] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[n++] = (ULONG)(fence_mc >> 32);
    pkt[n++] = SDMA_FENCE_MAGIC;
    while (n & 7) pkt[n++] = (ULONG)SDMA_OP_NOP;

    return sdma_submit_and_wait_locked(ad, 0, pkt, n);
}

/* Serialize an image transfer over the shared scratch (codex TRIG-0536). The
 * scratch is a single static buffer used across stage+submit+unstage, so the
 * whole transfer -- lazy scratch init, every CopyMem, and every ring submit --
 * must run under one lock, not just the ring submit. We reuse sdma_submit_lock
 * (AROS semaphores nest, and image transfers are the only holder that spans
 * multiple submits) and call the _locked ring core inside, so there is no
 * second lock and no ordering cycle. Skipped only before hw_init arms the lock
 * (single-threaded bring-up), matching sdma_submit_and_wait. */
static void sdma_scratch_lock(void)   { if (sdma_lock_ready) ObtainSemaphore(&sdma_submit_lock); }
static void sdma_scratch_unlock(void) { if (sdma_lock_ready) ReleaseSemaphore(&sdma_submit_lock); }

/* Log the running DMA counts at powers of two so the growth is visible on serial
 * without flooding it (codex TRIG-0537 evidence). */
static void sdma_dma_log(const char *tag, ULONG ok, ULONG attempt)
{
    if ((ok & (ok - 1)) == 0)   /* 1,2,4,8,... */
        bug("[amdgpu-sdma] DMA %s: ok=%lu attempt=%lu (fallback=%lu)\n",
            tag, (unsigned long)ok, (unsigned long)attempt,
            (unsigned long)(attempt - ok));
}

BOOL amdgpu_sdma_upload_rect(struct AmdgpuDevice *ad,
                             const APTR src_cpu, ULONG src_stride,
                             UQUAD dst_mc, ULONG dst_stride,
                             ULONG row_bytes, ULONG rows, ULONG xform)
{
    ULONG scratch_rows, done = 0, px = row_bytes / 4;
    BOOL  ok = TRUE;

    if (!ad || !ad->gart_ready || !dst_mc || !row_bytes || !rows) return FALSE;
    if (!src_cpu)                       return FALSE;
    if (row_bytes > SDMA_SCRATCH_BYTES) return FALSE;   /* one row must fit */
    if (xform != AMDGPU_XFER_RAW && (row_bytes & 3)) return FALSE; /* bswap needs 32bpp */

    sdma_scratch_lock();
    sdma_dma_up_attempt++;
    if (!sdma_scratch_init(ad)) { ok = FALSE; goto out; }

    scratch_rows = (ULONG)(SDMA_SCRATCH_BYTES / row_bytes);
    if (scratch_rows > 64) scratch_rows = 64;           /* ring budget */
    if (!scratch_rows) { ok = FALSE; goto out; }

    while (done < rows) {
        ULONG chunk = rows - done;
        const UBYTE *s = (const UBYTE *)src_cpu + (UQUAD)done * src_stride;
        UBYTE *sc = sdma_scratch_cpu;
        ULONG r;

        if (chunk > scratch_rows) chunk = scratch_rows;
        /* Stage the chunk into the cached scratch (dst stride = row_bytes),
         * applying the pixel transform on the way in. */
        for (r = 0; r < chunk; r++)
            sdma_xform_row((const ULONG *)(s + (UQUAD)r * src_stride),
                           (ULONG *)(sc + (UQUAD)r * row_bytes), px, xform);

        if (!sdma_linear_rows(ad, sdma_scratch_va, row_bytes,
                              dst_mc + (UQUAD)done * dst_stride, dst_stride,
                              row_bytes, chunk)) { ok = FALSE; goto out; }
        done += chunk;
    }
out:
    if (ok) { sdma_dma_up_ok++; sdma_dma_log("upload", sdma_dma_up_ok, sdma_dma_up_attempt); }
    else    bug("[amdgpu-sdma] DMA upload FALLBACK -> CPU (attempt=%lu ok=%lu)\n",   /* every failure, immediately (codex TRIG-0538) */
                (unsigned long)sdma_dma_up_attempt, (unsigned long)sdma_dma_up_ok);
    sdma_scratch_unlock();
    return ok;
}

BOOL amdgpu_sdma_download_rect(struct AmdgpuDevice *ad,
                               UQUAD src_mc, ULONG src_stride,
                               APTR dst_cpu, ULONG dst_stride,
                               ULONG row_bytes, ULONG rows, ULONG xform)
{
    ULONG scratch_rows, done = 0, px = row_bytes / 4;
    BOOL  ok = TRUE;

    if (!ad || !ad->gart_ready || !src_mc || !row_bytes || !rows) return FALSE;
    if (!dst_cpu)                       return FALSE;
    if (row_bytes > SDMA_SCRATCH_BYTES) return FALSE;
    if (xform != AMDGPU_XFER_RAW && (row_bytes & 3)) return FALSE;

    sdma_scratch_lock();
    sdma_dma_dn_attempt++;
    if (!sdma_scratch_init(ad)) { ok = FALSE; goto out; }

    scratch_rows = (ULONG)(SDMA_SCRATCH_BYTES / row_bytes);
    if (scratch_rows > 64) scratch_rows = 64;
    if (!scratch_rows) { ok = FALSE; goto out; }

    while (done < rows) {
        ULONG chunk = rows - done;
        UBYTE *d = (UBYTE *)dst_cpu + (UQUAD)done * dst_stride;
        const UBYTE *sc = sdma_scratch_cpu;
        ULONG r;

        if (chunk > scratch_rows) chunk = scratch_rows;
        if (!sdma_linear_rows(ad, src_mc + (UQUAD)done * src_stride, src_stride,
                              sdma_scratch_va, row_bytes,
                              row_bytes, chunk)) { ok = FALSE; goto out; }
        /* Un-stage from the cached scratch to the caller, applying the transform. */
        for (r = 0; r < chunk; r++)
            sdma_xform_row((const ULONG *)(sc + (UQUAD)r * row_bytes),
                           (ULONG *)(d + (UQUAD)r * dst_stride), px, xform);
        done += chunk;
    }
out:
    if (ok) { sdma_dma_dn_ok++; sdma_dma_log("download", sdma_dma_dn_ok, sdma_dma_dn_attempt); }
    else    bug("[amdgpu-sdma] DMA download FALLBACK -> CPU (attempt=%lu ok=%lu)\n",  /* every failure, immediately (codex TRIG-0538) */
                (unsigned long)sdma_dma_dn_attempt, (unsigned long)sdma_dma_dn_ok);
    sdma_scratch_unlock();
    return ok;
}

/*
 * Constant-fill a rectangle. This is the primitive FillRect and Clear need, and
 * the first drawing op the GPU can do for AROS at all -- until now SDMA could
 * only COPY, so every fill was CPU work.
 *
 * dst_mc is an SDMA address, which means it can be a raw VRAM MC address OR a
 * GART VA from amdgpu_gart_map(). That matters because AROS bitmaps currently
 * live in system memory (the class subclasses ChunkyBM), so a bitmap fill has
 * to go through GART while a framebuffer fill does not. The caller decides;
 * this function does not care.
 *
 * No source buffer means no GART mapping and no cache maintenance here, so a
 * fill is strictly cheaper than the equivalent blit.
 */
/* The SDMA7 CONSTANT_FILL header encoding is generation-specific and the sources
 * disagree: AMD Linux 6.16 v7_0 uses COMPRESS bit16 (0x0001000b); Mesa 26 uses
 * FILLSIZE at bits31:30 (0x8000000b) with a DWORD count; an older note used sw
 * bits17:16 (0x0002000b). The COMPRESS and sw=2 encodings were BOTH proven wrong
 * on this DUT by readback (garbage/no-op fills). Rather than guess a third time,
 * discriminate empirically at init: fill a scratch dword with each candidate and
 * keep the first whose readback is exact. (codex TRIG-0544/0548) */
static ULONG sdma_fill_hdr        = (ULONG)SDMA_OP_CONST_FILL | (1UL << 16); /* default until resolved */
static BOOL  sdma_fill_cnt_dwords = FALSE;                                    /* FALSE = bytes-1, TRUE = dwords-1 */
static BOOL  sdma_fill_resolved   = FALSE;

/* Submit ONE fill of NDW dwords at the blit-dst scratch with the given encoding,
 * then read back. Returns TRUE iff every filled dword equals VAL. */
static BOOL sdma_fill_probe(struct AmdgpuDevice *ad, ULONG hdr, BOOL cnt_dwords, ULONG *got0)
{
    const ULONG OFF      = SDMA_BLIT_DST_VRAM_OFF;
    const UQUAD dst_mc   = SDMA_VRAM_MC_BASE + OFF;
    const UQUAD fence_mc = sdma_fence_addr();
    /* DISTINCT bytes so a BYTE fill (FILLSIZE=0, replicates value[7:0] -> 0x44444444)
     * is rejected while only a true DWORD fill (FILLSIZE=2) reproduces it exactly.
     * The old 0xA5A5A5A5 had identical bytes, so byte- and dword-fill were
     * indistinguishable and several wrong encodings falsely passed (codex TRIG-0552). */
    const ULONG NDW = 8, VAL = 0x11223344UL;
    ULONG pkt[16], n = 0, i, v, allok = 1;

    for (i = 0; i < NDW; i++) { v = 0xDEAD0000UL | i; amdgpu_vram_write(ad, OFF + i * 4, &v, 1); }
    sdma_fence_clear(ad);

    pkt[n++] = hdr;
    pkt[n++] = (ULONG)(dst_mc & 0xFFFFFFFFUL);
    pkt[n++] = (ULONG)(dst_mc >> 32);
    pkt[n++] = VAL;
    pkt[n++] = cnt_dwords ? (NDW - 1) : (NDW * 4 - 1);
    pkt[n++] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[n++] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[n++] = (ULONG)(fence_mc >> 32);
    pkt[n++] = SDMA_FENCE_MAGIC;
    while (n & 7) pkt[n++] = (ULONG)SDMA_OP_NOP;

    (void)sdma_submit_and_wait_locked(ad, 0, pkt, n);

    amdgpu_vram_read(ad, OFF, got0, 1);
    for (i = 0; i < NDW; i++) { amdgpu_vram_read(ad, OFF + i * 4, &v, 1); if (v != VAL) { allok = 0; break; } }
    return allok ? TRUE : FALSE;
}

/* Try every candidate header/count encoding once, log each readback, and latch
 * the first exact match for production use. Runs once at hw_init. */
static void sdma_fill_discriminate(struct AmdgpuDevice *ad)
{
    /* FILLSIZE=2 (bits31:30) selects a DWORD fill; the distinct-byte oracle only
     * accepts that. Try both the plain FILLSIZE and the FILLSIZE|COMPRESS combo
     * (codex TRIG-0552/0553); libdrm uses count=length-1 in BYTES even with
     * FILLSIZE=2, so byte-count is primary. The FILLSIZE=0 encodings (compress,
     * plain, sw2) are kept last only to log that they byte-replicate. */
    static const struct { ULONG hdr; BOOL cnt_dwords; const char *name; } cand[] = {
        { (ULONG)SDMA_OP_CONST_FILL | (2UL << 30),              FALSE, "fillsize2/by"      }, /* 0x8000000b */
        { (ULONG)SDMA_OP_CONST_FILL | (2UL << 30) | (1UL << 16),FALSE, "fillsize2+compress"}, /* 0x8001000b */
        { (ULONG)SDMA_OP_CONST_FILL | (2UL << 30),              TRUE,  "fillsize2/dw"      }, /* 0x8000000b, dw */
        { (ULONG)SDMA_OP_CONST_FILL | (1UL << 16),              FALSE, "compress(byte-fill)"}, /* 0x0001000b */
        { (ULONG)SDMA_OP_CONST_FILL,                            FALSE, "plain(byte-fill)"  }, /* 0x0000000b */
        { (ULONG)SDMA_OP_CONST_FILL | (2UL << 16),              FALSE, "sw2(byte-fill)"    }, /* 0x0002000b */
    };
    ULONG i, got;

    if (sdma_fill_resolved || !ad || !ad->gart_ready) return;

    for (i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        BOOL ok = sdma_fill_probe(ad, cand[i].hdr, cand[i].cnt_dwords, &got);
        bug("[amdgpu-sdma] fill-probe %-16s hdr=%08lx cnt=%s got=%08lx %s\n",
            cand[i].name, (unsigned long)cand[i].hdr, cand[i].cnt_dwords ? "dw" : "by",
            (unsigned long)got, ok ? "*** EXACT ***" : "");
        if (ok && !sdma_fill_resolved) {
            sdma_fill_hdr = cand[i].hdr; sdma_fill_cnt_dwords = cand[i].cnt_dwords;
            sdma_fill_resolved = TRUE;
        }
    }
    if (sdma_fill_resolved)
        bug("[amdgpu-sdma] fill header RESOLVED: hdr=%08lx count=%s\n",
            (unsigned long)sdma_fill_hdr, sdma_fill_cnt_dwords ? "dwords-1" : "bytes-1");
    else
        bug("[amdgpu-sdma] fill-probe: NO candidate exact -- fills will be CPU fallback\n");
}

BOOL amdgpu_sdma_fill_rect(struct AmdgpuDevice *ad, UQUAD dst_mc, ULONG dst_stride,
                           ULONG dst_off, ULONG row_bytes, ULONG rows, ULONG value)
{
    const UQUAD fence_mc = sdma_fence_addr();
    ULONG pkt[5 * 64 + 8];
    ULONG n = 0, y;

    if (!ad || !ad->gart_ready || !dst_mc || !rows || !row_bytes)
        return FALSE;
    if (rows > 64)
        return FALSE;                 /* caller chunks; ring budget for one pass */
    if (!sdma_fill_resolved)
        return FALSE;                 /* no proven CONST_FILL encoding -> CPU fallback, never garbage */
    if (sdma_fill_cnt_dwords && (row_bytes & 3))
        return FALSE;                 /* dword-count encoding needs a 4-byte multiple */

    for (y = 0; y < rows; y++) {
        UQUAD d = dst_mc + dst_off + (UQUAD)y * dst_stride;

        pkt[n++] = sdma_fill_hdr;     /* encoding resolved empirically at init */
        pkt[n++] = (ULONG)(d & 0xFFFFFFFFUL);
        pkt[n++] = (ULONG)(d >> 32);
        pkt[n++] = value;
        pkt[n++] = sdma_fill_cnt_dwords ? (row_bytes / 4 - 1) : (row_bytes - 1);
    }
    pkt[n++] = (ULONG)SDMA_OP_FENCE | ((ULONG)SDMA_FENCE_MTYPE_UC << 16);
    pkt[n++] = (ULONG)(fence_mc & 0xFFFFFFFFUL);
    pkt[n++] = (ULONG)(fence_mc >> 32);
    pkt[n++] = SDMA_FENCE_MAGIC;
    while (n & 7)
        pkt[n++] = (ULONG)SDMA_OP_NOP;

    return sdma_submit_and_wait(ad, 0, pkt, n);
}

BOOL amdgpu_sdma_hw_init(struct AmdgpuDevice *ad)
{
    ULONG reset_ctrl;

    if (!ad || !ad->rmmio || !ad->gc_seg[0] || !ad->gc_seg[1]) {
        bug("[amdgpu-sdma] no GC segments — cannot address SDMA\n");
        return TRUE;    /* not fatal to boot */
    }

    /* Arm the submission lock (audit P0-5) before the first ring submission. */
    if (!sdma_lock_ready) {
        InitSemaphore(&sdma_submit_lock);
        sdma_lock_ready = TRUE;
    }

    if (!sdma_clock_init())
        return TRUE; /* no timed submission possible; software remains usable */

    /* SDMA_RESETB is bit 3 of GFX_IMU_GFX_RESET_CTRL. If the SMU feature-enable
     * did not run, this reads 0x30 and the block is still in reset — say so
     * plainly instead of writing registers into a dead power island and
     * reporting a confusing failure downstream. */
    reset_ctrl = amdgpu_rreg32(ad, ad->gc_seg[1] + 0x40bc);
    if (!(reset_ctrl & (1UL << 3))) {
        bug("[amdgpu-sdma] GFX_IMU_GFX_RESET_CTRL=0x%08lx — SDMA_RESETB (bit 3) still "
            "asserted, engine is in reset; skipping\n", (unsigned long)reset_ctrl);
        return TRUE;
    }

    bug("[amdgpu-sdma] RESET_CTRL=0x%08lx — SDMA out of reset, bringing up ring 0\n",
        (unsigned long)reset_ctrl);

    /* SDMA translates through GFXHUB, and GFXHUB's VMID0 page table is still
     * zero at this point: amdgpu_gart_enable() runs during PSP init when the GFX
     * rail is dark, so its GFXHUB writes drop (MMHUB is what PSP rides). With no
     * GFXHUB translation a system-memory copy retires cleanly and moves nothing.
     * Linux does this in gfx_v12_0_hw_init via gfx_v12_0_gfxhub_enable(); we do
     * it here because SDMA is the first consumer that needs it. */
    amdgpu_gart_enable_gfxhub(ad);

    /* Move the completion fence into system RAM now that the GART can map it —
     * the poll loop becomes a cached load instead of an MMIO read per iteration. */
    sdma_fence_init(ad);

    /* Only attempt the blit once the ring has demonstrably retired a packet —
     * running a copy against a ring that never worked would produce a mismatch
     * report that says nothing about the copy path. */
    if (sdma_ring_bringup(ad, 0)) {
        (void)sdma_blit_test(ad, 0);

        /* Resolve the CONSTANT_FILL header encoding empirically (codex TRIG-0544):
         * two documented encodings failed on this DUT, so probe candidates and
         * latch the one whose readback is exact before FillRect/Clear rely on it. */
        sdma_fill_discriminate(ad);

        /* Pitch shift 16 and element size 2 are settled (09b316d4c1). Adding
         * SDMA7's mandatory CPV bit fixed the formerly duplicated source
         * elements (cf231384bf). Keep all four independent content oracles and
         * enable runtime SUB_WIND only if every offset/width variant passes.
         * Each probe re-fills both surfaces, so they cannot contaminate one
         * another. */
        {
            const ULONG PS = SDMA_SW_PITCH_SHIFT_MESA, ES = SDMA_SW_ELEMSIZE;
            BOOL a, b, c, d;

            a = sdma_subwin_blit(ad, 0, PS, ES,
                8, 4, 32, 2, 16, 8, "A-baseline");
            b = sdma_subwin_blit(ad, 0, PS, ES,
                0, 4, 0, 2, 16, 8, "B-noxoff");
            c = sdma_subwin_blit(ad, 0, PS, ES,
                8, 4, 8, 2, 16, 8, "C-samex");
            d = sdma_subwin_blit(ad, 0, PS, ES,
                0, 4, 0, 2, 8, 8, "D-noxoff-w8");
            sdma_subwin32_ready = a && b && c && d;
            bug("[amdgpu-sdma] runtime SUBWIN32 %s after four exact content oracles\n",
                sdma_subwin32_ready ? "enabled" : "disabled");
        }

        /* The rectangle primitive that actually works today. */
        (void)sdma_rect_blit_scanline(ad, 0,  8, 4, 32, 2, 16, 8, "offset");
        (void)sdma_rect_blit_scanline(ad, 0,  0, 0,  0, 8, 64, 8, "fullwidth");

        /* The capability a host-side BitMap actually needs. */
        (void)sdma_sysmem_blit(ad, 0);

        /* And the same thing for a buffer the driver did not place: an ordinary
         * AllocMem allocation, mapped on demand. That is what a BitMap is. */
        (void)sdma_hostbuf_blit(ad, 0);

#if AMDGPU_SDMA_BENCH
        /* Is any of this actually worth wiring into CopyBox? Measure. */
        (void)sdma_bench(ad, 0);
#endif
#if AMDGPU_FBPERF
        sdma_fbperf(ad, 0);
#endif
        bug("[amdgpu-sdma] sdma hw_init complete\n");
    } else
        bug("[amdgpu-sdma] ring did not retire a packet — skipping the blit test\n");

    return TRUE;    /* diagnostic increment: never fail the boot on SDMA */
}
