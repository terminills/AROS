/*
 * amdgpu_gfx.c — the GFX (graphics/compute) IP block for GFX12/RDNA4 (Navi 44).
 *
 * amdgpu.hidd previously brought up only PSP + GMC + display(DCN); the GFX
 * engine was never powered, so every GC register read 0, MEC never ran, and
 * radv's compute-ring init failed (EnumeratePhysicalDevices -3). This block does
 * the first, foundational step of GFX bring-up: start the IMU (Intelligent
 * Management Unit), the on-die controller that powers up and clock-gates the GFX
 * block before any microcode can run. Until the IMU signals reset-complete, GC
 * register writes do not land — which is exactly the "writes may not have landed"
 * the radv winsys hit.
 *
 * Sequence mirrors Linux imu_v12_0.c + gfx_v12_0_rlc_backdoor_autoload_enable:
 *   1. Stage the RLC_G microcode in VRAM and point GFX_IMU_RLC_BOOTLOADER_ADDR at
 *      it (the IMU bootstraps RLC from there once it is running).
 *   2. Load the IMU microcode into its I-RAM / D-RAM.
 *   3. Setup (open the C2PMSG access windows).
 *   4. Start (clear GFX_IMU_CORE_CTRL soft-reset) and wait for
 *      GFX_IMU_GFX_RESET_CTRL & 0x1f == 0x1f.
 *
 * Register addressing: GFX12 GC registers carry a BASE_IDX. GRBM etc. are
 * BASE_IDX 0 (gc_seg[0]); the IMU / CP_MEC / RLC control registers are BASE_IDX 1
 * (gc_seg[1]). Both segments are captured by the discovery parse. Offsets come
 * from asic_reg/gc/gc_12_0_0_offset.h.
 *
 * This is P0 of the GFX IP block. CP/MEC ring bring-up (the rest of the autoload
 * buffer + TOC + GFXHUB) follows once the IMU is confirmed to power the engine.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/oop.h>
#include <hidd/pci.h>
#include <hidd/amdgpu.h>
#include <hardware/pci.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <string.h>
#include <errno.h>

#include "amdgpu_intern.h"
#include "amdgpu_gfx_mqd.h"
#include "amdgpu_gfx_csb.h"
#include "amdgpu_cs.h"

/* Queue/shader/EOP bases are encoded in 256-byte units. GART preserves the
 * CPU allocation's page offset, so ordinary AllocMem alignment is insufficient:
 * shifting an unaligned VA by eight points the GPU BEFORE the initialized data.
 * Reserve padding and retain the original allocation for correct cleanup.
 * Page alignment also prevents a hardware ring sharing either boundary page
 * with an unrelated allocation. Do not rely on MEMF_HWALIGNED being honored by
 * every allocator backing this driver. */
#define GFX_DMA_ALIGN 4096UL
#define GFX_QUEUE_BYTES 65536UL

static APTR gfx_alloc_aligned(ULONG size, APTR *allocation)
{
    APTR raw;

    if (!size || size > (ULONG)~0UL - (GFX_DMA_ALIGN - 1))
        return NULL;
    raw = AllocMem(size + GFX_DMA_ALIGN - 1, MEMF_ANY | MEMF_CLEAR);
    if (!raw)
        return NULL;
    *allocation = raw;
    return (APTR)(((IPTR)raw + GFX_DMA_ALIGN - 1) & ~(IPTR)(GFX_DMA_ALIGN - 1));
}

static ULONG *gfx_alloc_queue_buffer(struct AmdgpuDevice *ad,
                                     APTR *allocation, UQUAD *va)
{
    ULONG *buf = gfx_alloc_aligned(GFX_QUEUE_BYTES, allocation);

    *va = 0;
    if (!buf)
        return NULL;
    *va = amdgpu_gart_map(ad, buf, GFX_QUEUE_BYTES);
    if (!*va || (*va & (GFX_DMA_ALIGN - 1))) {
        if (*va) {
            bug("[amdgpu-gfx] rejecting unaligned GART queue VA 0x%08lx%08lx\n",
                (unsigned long)(*va >> 32), (unsigned long)(ULONG)*va);
            amdgpu_gart_unmap(ad, *va, GFX_QUEUE_BYTES);
        }
        FreeMem(*allocation, GFX_QUEUE_BYTES + GFX_DMA_ALIGN - 1);
        *allocation = NULL;
        *va = 0;
        return NULL;
    }
    return buf;
}

/* ---- GC BASE_IDX-1 register offsets (gc_12_0_0_offset.h) ---- */
#define regGFX_IMU_C2PMSG_ACCESS_CTRL0    0x4040
#define regGFX_IMU_C2PMSG_ACCESS_CTRL1    0x4041
#define regGFX_IMU_CORE_CTRL              0x40b6
#define regGFX_IMU_GFX_RESET_CTRL         0x40bc
#define regGFX_IMU_D_RAM_ADDR             0x40fc
#define regGFX_IMU_D_RAM_DATA             0x40fd
#define regGFX_IMU_RLC_BOOTLOADER_ADDR_HI 0x5f81
#define regGFX_IMU_RLC_BOOTLOADER_ADDR_LO 0x5f82
#define regGFX_IMU_RLC_BOOTLOADER_SIZE    0x5f83
#define regGFX_IMU_I_RAM_ADDR             0x5f90
#define regGFX_IMU_I_RAM_DATA             0x5f91
#define regGFX_IMU_SCRATCH_10             0x4072
#define regGFX_IMU_RLC_RAM_INDEX          0x40ac
#define regGFX_IMU_RLC_RAM_ADDR_HIGH      0x40ad
#define regGFX_IMU_RLC_RAM_ADDR_LOW       0x40ae
#define regGFX_IMU_RLC_RAM_DATA           0x40af
#define GFX_IMU_RLC_RAM_INDEX_VALID       0x80000000UL

/* Linux gfx_v12_0_wait_for_rlc_autoload_complete() completion condition. */
#define regCP_STAT                         0x0f40
#define regRLC_RLCS_BOOTLOAD_STATUS        0x4e7c
#define RLC_BOOTLOAD_COMPLETE              0x80000000UL

/* MMHUB FB location (GC golden FB values mirror the live display's aperture) */
#define MMHUB_FB_LOCATION_TOP             0x0555

/* Registers whose golden data is computed at runtime (imu_v12_0.c overrides). */
#define regGCMC_VM_FB_LOCATION_BASE_OFF   0x1614
#define regGCMC_VM_FB_LOCATION_TOP_OFF    0x1615
#define regGCMC_VM_AGP_BASE_OFF           0x1618
#define regGCMC_VM_AGP_TOP_OFF            0x1616

/* ---- GC BASE_IDX-0 status register (to prove the block woke) ---- */
#define regGRBM_STATUS                    0x0da4  /* BASE_IDX 0 */

/* ---- CP graphics-ring (CP_RB0) bring-up — gc_12_0_0_offset.h ----
 * Every offset + BASE_IDX verified against gc_12_0_0_offset.h/_sh_mask.h. */
#ifndef AMDGPU_VRAM_MC_BASE
#define AMDGPU_VRAM_MC_BASE 0x8000000000ULL
#endif
/* BASE_IDX 0 (gc_seg[0]) */
#define regCP_RB0_RPTR                  0x0f60
#define regCP_RB_WPTR_DELAY             0x0f61
#define regCP_RB0_BASE                  0x1de0
#define regCP_RB0_CNTL                  0x1de1
#define regCP_RB0_RPTR_ADDR             0x1de3
#define regCP_RB0_RPTR_ADDR_HI          0x1de4
#define regCP_DEVICE_ID                 0x1deb
#define regCP_RB_VMID                   0x1df1
#define regCP_RB0_WPTR                  0x1df4
#define regCP_RB0_WPTR_HI               0x1df5
#define regCP_DEBUG                     0x1e1f
#define regCP_PFP_PRGRM_CNTR_START      0x1e44
#define regCP_ME_PRGRM_CNTR_START       0x1e45
#define regCP_MAX_CONTEXT               0x1e4e
#define regCP_RB0_BASE_HI               0x1e51
#define regCP_PFP_PRGRM_CNTR_START_HI   0x1e59
#define regCP_ME_PRGRM_CNTR_START_HI    0x1e79
#define regCP_RB_WPTR_POLL_ADDR_LO      0x1e8b
#define regCP_RB_WPTR_POLL_ADDR_HI      0x1e8c
#define regCP_RB_ACTIVE                 0x1f40
/* CP_GFX_HQD_* — the GFX12 RS64 graphics dispatch queue (BASE_IDX 0 / gc0). The ME
 * dispatches from an ACTIVE HQD, NOT from CP_RB_ACTIVE. Values per gfx_v12_0_gfx_mqd_init. */
#define regCP_GFX_MQD_BASE_ADDR        0x1e7e
#define regCP_GFX_MQD_BASE_ADDR_HI     0x1e7f
#define regCP_GFX_HQD_ACTIVE           0x1e80
#define regCP_GFX_HQD_VMID             0x1e81
#define regCP_GFX_HQD_QUEUE_PRIORITY   0x1e84
#define regCP_GFX_HQD_QUANTUM          0x1e85
#define regCP_GFX_HQD_BASE             0x1e86
#define regCP_GFX_HQD_BASE_HI          0x1e87
#define regCP_GFX_HQD_RPTR             0x1e88
#define regCP_GFX_HQD_RPTR_ADDR        0x1e89
#define regCP_GFX_HQD_RPTR_ADDR_HI     0x1e8a
#define regCP_GFX_HQD_OFFSET           0x1e8e
#define regCP_GFX_HQD_CNTL             0x1e8f
#define regCP_GFX_HQD_WPTR             0x1e91
#define regCP_GFX_HQD_WPTR_HI          0x1e92
#define regCP_GFX_MQD_CONTROL          0x1e9a
#define CP_GFX_MQD_CONTROL_PRIV_STATE  0x00000100UL   /* PRIV_STATE bit8 */
#define CP_GFX_HQD_QUANTUM_EN          0x00000001UL   /* QUANTUM_EN bit0 */
/* MES engine (me=3): pipe0=scheduler, pipe1=KIQ. All BASE_IDX 1 (gc1). */
#define regCP_MES_PRGRM_CNTR_START     0x2800
#define regCP_MES_CNTL                 0x2807
#define regCP_MES_INSTR_PNTR           0x2813   /* MES live PC (grbm-selected pipe) */
#define regCP_MES_GP3_LO               0x2849   /* MES fw writes its VERSION here at boot (per-pipe) */
#define regCP_MES_MSCRATCH_HI          0x2814   /* MES scratch/event-log buffer addr */
#define regCP_MES_MSCRATCH_LO          0x2815
#define regCP_MES_MSTATUS_LO           0x2816   /* MES machine status (per-pipe) */
#define regCP_MES_MSTATUS_HI           0x2817
#define regCP_MES_RS64_EXCEPTION_STATUS 0x289c  /* RS64 exception cause (per-pipe) */
#define regCP_MES_MDBASE_LO_DIAG       0x5854   /* gc1 — MES data cache PHYSICAL base */
#define regCP_MES_MDBASE_HI_DIAG       0x5855
#define regCP_MES_MDBOUND_LO_DIAG      0x585d
#define regCP_MES_LOCAL_BASE0_LO       0x2883   /* gc1 — MES local memory aperture base */
#define regCP_MES_LOCAL_BASE0_HI       0x2884
#define regCP_MES_LOCAL_MASK0_LO       0x2885
#define regCP_MES_LOCAL_MASK0_HI       0x2886
#define regGCVM_L2_FAULT_ADDR_LO32     0x15d2   /* gc0 — GFXHUB fault address */
#define regGCVM_L2_FAULT_ADDR_HI32     0x15d3
#define regMMVM_L2_FAULT_STATUS_LO32   0x04f0   /* mmhub_seg[0] — MMHUB L2 fault */
#define regMMVM_L2_FAULT_ADDR_LO32     0x04f2
#define regCP_MES_PRGRM_CNTR_START_HI  0x289d
/* Aggregated doorbell control (5 priority levels), gc1 (BASE_IDX 1). Linux
 * mes_v12_0_init_aggregated_doorbell programs these; AROS was missing them and the
 * MES fw spins in SET_HW_RESOURCES on the unconfigured aggregated-doorbell path. */
#define regCP_MES_DOORBELL_CONTROL1    0x283c
#define regCP_MES_DOORBELL_CONTROL2    0x283d
#define regCP_MES_DOORBELL_CONTROL3    0x283e
#define regCP_MES_DOORBELL_CONTROL4    0x283f
#define regCP_MES_DOORBELL_CONTROL5    0x2840
#define CP_MES_DOORBELL_CONTROL__OFFSET_SHIFT 0x2   /* DOORBELL_OFFSET field */
#define CP_MES_DOORBELL_CONTROL__EN           0x40000000UL /* DOORBELL_EN bit30 */
#define MES_AGGREGATED_DB_START        0x800   /* aggregated_doorbells[i] = 0x800 + i*2 (Linux trace) */
/* IH (interrupt handler) ring — OSSSYS BASE_IDX 0 = osssys_seg[0]. The MES is
 * interrupt-driven; SET_HW_RESOURCES needs the IH ring to exist. Port of Linux
 * ih_v7_0 IH_RB_* setup. */
#define regIH_RB_CNTL                  0x0080
#define regIH_RB_RPTR                  0x0081
#define regIH_RB_WPTR                  0x0082
#define regIH_RB_BASE                  0x0083
#define regIH_RB_BASE_HI               0x0084
#define regIH_RB_WPTR_ADDR_HI          0x0085
#define regIH_RB_WPTR_ADDR_LO          0x0086
#define regIH_DOORBELL_RPTR            0x0087
#define IH_RB_CNTL__RB_ENABLE          0x00000001UL /* bit0 */
#define IH_RB_CNTL__RB_SIZE_SHIFT      1
#define IH_RB_CNTL__WPTR_WRITEBACK_EN  0x00000100UL /* bit8 */
#define IH_RB_CNTL__WPTR_OVERFLOW_ENABLE 0x00010000UL /* bit16 */
#define IH_RB_CNTL__ENABLE_INTR        0x00020000UL /* bit17 */
#define IH_RB_CNTL__MC_SNOOP           0x00100000UL /* bit20 */
#define IH_RB_CNTL__MC_SPACE_SHIFT     28
#define IH_RB_CNTL__WPTR_OVERFLOW_CLEAR 0x80000000UL /* bit31 */
#define IH_RING_SIZE                   (256UL * 1024)
#define CP_MES_CNTL__MES_INVALIDATE_ICACHE 0x00000010UL /* bit4 */
#define CP_MES_CNTL__MES_PIPE0_RESET   0x00010000UL
#define CP_MES_CNTL__MES_PIPE1_RESET   0x00020000UL
#define CP_MES_CNTL__MES_PIPE0_ACTIVE  0x04000000UL
#define CP_MES_CNTL__MES_PIPE1_ACTIVE  0x08000000UL
#define CP_MES_CNTL__MES_HALT          0x40000000UL     /* bit0x1e */
#define MESH1_START_LO                 56   /* mes_firmware_header_v1_0.mes_uc_start_addr_lo */
#define MESH1_START_HI                 60
/* mes_firmware_header_v1_0 payload layout (common_firmware_header = 32 bytes). */
#define MESH1_UCODE_SIZE               36   /* mes_ucode_size_bytes (instruction) */
#define MESH1_UCODE_OFF                40   /* mes_ucode_offset_bytes */
#define MESH1_DATA_SIZE                48   /* mes_ucode_data_size_bytes */
#define MESH1_DATA_OFF                 52   /* mes_ucode_data_offset_bytes */
/* MES instruction/data cache base regs (direct-load path). All BASE_IDX 1 (gc1). */
#define regCP_MES_IC_OP_CNTL           0x2820
#define CP_MES_IC_OP_CNTL__INVALIDATE_CACHE 0x00000001UL /* bit0 */
#define CP_MES_IC_OP_CNTL__PRIME_ICACHE     0x00000010UL /* bit4 */
#define CP_MES_IC_OP_CNTL__ICACHE_PRIMED    0x00000020UL /* bit5 (status) */
#define regCP_MES_IC_BASE_LO           0x5850
#define regCP_MES_IC_BASE_HI           0x5851
#define regCP_MES_IC_BASE_CNTL         0x5852
#define regCP_MES_MDBASE_LO            0x5854
#define regCP_MES_MDBASE_HI            0x5855
#define regCP_MES_MIBOUND_LO           0x585b
#define regCP_MES_MDBOUND_LO           0x585d
#define GRBM_GFX_CNTL_MEID_SHIFT       2    /* MEID at bits 2-3; MES = me 3 */
/* CP HQD/MQD registers (compute-style HQD; MES scheduler reuses them). All gc0. */
#define regCP_MQD_BASE_ADDR            0x1fa9
#define regCP_MQD_BASE_ADDR_HI         0x1faa
#define regCP_HQD_ACTIVE              0x1fab
#define regCP_HQD_VMID                0x1fac
#define regCP_HQD_PERSISTENT_STATE    0x1fad
#define regCP_HQD_PQ_BASE            0x1fb1
#define regCP_HQD_PQ_BASE_HI        0x1fb2
#define regCP_HQD_PQ_RPTR_REPORT_ADDR    0x1fb4
#define regCP_HQD_PQ_RPTR_REPORT_ADDR_HI 0x1fb5
#define regCP_HQD_PQ_WPTR_POLL_ADDR     0x1fb6
#define regCP_HQD_PQ_WPTR_POLL_ADDR_HI  0x1fb7
#define regCP_HQD_PQ_DOORBELL_CONTROL   0x1fb8
#define regCP_HQD_PQ_RPTR            0x1fb3
#define regCP_HQD_PQ_CONTROL          0x1fba
#define regCP_HQD_PQ_WPTR_LO          0x1fdf
#define regCP_MQD_CONTROL_HQD          0x1fcb
#define regCP_HQD_DEQUEUE_REQUEST       0x1fc1
#define regCP_HQD_EOP_BASE_ADDR         0x1fce
#define regCP_HQD_EOP_BASE_ADDR_HI      0x1fcf
#define regCP_HQD_EOP_CONTROL           0x1fd0
#define regCP_HQD_PQ_WPTR_HI            0x1fe0
#define regCP_PQ_STATUS                 0x1e58   /* BASE_IDX 0 (gc0) */
#define CP_PQ_STATUS__DOORBELL_ENABLE   0x00000002UL /* bit1 — global CP doorbell processing */
#define regGCVM_L2_PROTECTION_FAULT_STATUS_LO32 0x15d0  /* gc0 — GFXHUB L2 fault */
#define regGCVM_L2_PROTECTION_FAULT_STATUS_HI32 0x15d1
#define regGCVM_L2_PROTECTION_FAULT_ADDR_LO32   0x15d2
#define regGCVM_L2_PROTECTION_FAULT_ADDR_HI32   0x15d3
#define regCP_MEC_DOORBELL_RANGE_LOWER  0x1dfc
#define regCP_MEC_DOORBELL_RANGE_UPPER  0x1dfd
#define regCP_PQ_WPTR_POLL_CNTL         0x1e23   /* BASE_IDX 0 (gc0) */
#define CP_PQ_WPTR_POLL_CNTL__EN        0x80000000UL /* bit31 */
#define regCP_UNMAPPED_DOORBELL         0x0880   /* BASE_IDX 1 (gc1) */
/* Linux writes 0xD31 = ENABLE | QUEUE_LSB=0x3 (bits4,5) | PROC_LSB=0xd. AROS had
 * 0xD01 (QUEUE_LSB=0 — missing). The MES unmapped_doorbell_handling path uses this. */
#define CP_UNMAPPED_DOORBELL_ENABLE_PROCLSB 0x00000D31UL /* ENABLE|QUEUE_LSB=3|PROC_LSB=0xd */
#define regCP_HQD_GFX_CONTROL           0x1e9f   /* BASE_IDX 0 (gc0); Linux MES hw_init writes 0x8000 */
#define regRLC_CP_SCHEDULERS            0x098a   /* BASE_IDX 1 (gc1) — tell RLC which queue is KIQ */
/* MEC (compute microengine) RS64 bring-up — BASE_IDX 1 (gc1). Linux gfx_v12_0
 * config_gfx_rs64 MEC portion + cp_compute_enable. AROS never brought the MEC up
 * ("MEC deferred"), so no compute/KIQ front-end existed. */
#define regCP_MEC_RS64_PRGRM_CNTR_START     0x2900
#define regCP_MEC_RS64_PRGRM_CNTR_START_HI  0x2938
#define regCP_MEC_RS64_CNTL                 0x2904
#define regCP_MEC_RS64_INSTR_PNTR           0x2908
#define CP_MEC_RS64_CNTL__MEC_INVALIDATE_ICACHE 0x00000010UL /* bit4 */
#define CP_MEC_RS64_CNTL__MEC_PIPE0_RESET  0x00010000UL      /* bits16-19 */
#define CP_MEC_RS64_CNTL__MEC_PIPE_RESET_ALL 0x000F0000UL
#define CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE 0x04000000UL      /* bits26-29 */
#define CP_MEC_RS64_CNTL__MEC_PIPE_ACTIVE_ALL 0x3C000000UL
#define CP_MEC_RS64_CNTL__MEC_HALT         0x40000000UL      /* bit30 */
/* BASE_IDX 1 (gc_seg[1]) */
#define regCP_ME_CNTL                   0x0803
#define regGRBM_GFX_CNTL                0x0900
#define regSH_MEM_BASES                 0x09e3   /* BASE_IDX 1 */
#define regSH_MEM_CONFIG               0x09e4   /* BASE_IDX 1 */
/* Linux gfx_v12_0 DEFAULT_SH_MEM_CONFIG: ADDRESS_MODE_64(0<<0) |
 * ALIGNMENT_MODE_UNALIGNED(3<<2) | INITIAL_INST_PREFETCH(3<<14) = 0xC00C.
 * Governs how the ME/shaders address memory; at reset the mode is undefined. */
#define DEFAULT_SH_MEM_CONFIG          0x0000C00CUL
#define GRBM_GFX_CNTL_VMID_SHIFT       4
#define regCP_PFP_INSTR_PNTR           0x0f45   /* BASE_IDX 0 (gc0) — legacy F32 PFP PC */
#define regCP_ME_INSTR_PNTR            0x0f46   /* BASE_IDX 0 (gc0) — legacy F32 ME PC */
#define regCP_GFX_RS64_INSTR_PNTR0     0x2a44   /* BASE_IDX 1 (gc1) — RS64 live PC pipe0 */
#define regCP_GFX_RS64_INSTR_PNTR1     0x2a45   /* BASE_IDX 1 (gc1) — RS64 live PC pipe1 */
/* ME/CP wedge diagnostics (codex audit): why does the ME consume a packet (RPTR++)
 * but not execute it (CP_STAT=ME_BUSY)? */
#define regCP_ME_HEADER_DUMP           0x0f41   /* BASE_IDX 0 (gc0) — last pkt header the ME saw */
#define regCP_CPC_STATUS               0x0e24   /* BASE_IDX 0 (gc0) */
#define regCP_CPF_STATUS               0x0e27   /* BASE_IDX 0 (gc0) */
#define regCP_CPF_BUSY_STAT            0x0e28   /* BASE_IDX 0 (gc0) */
#define regCP_PFP_RS64_EXCEPTION_STATUS 0x2a19  /* BASE_IDX 1 (gc1) */
#define regCP_GFX_RS64_INTERRUPT0      0x2a01   /* BASE_IDX 1 (gc1) */
#define regCP_PFP_IC_BASE_LO           0x5840   /* BASE_IDX 1 (gc1) — PFP instr-cache base */
#define regCP_PFP_IC_BASE_HI           0x5841
#define regCP_ME_IC_BASE_LO            0x5844   /* BASE_IDX 1 (gc1) — ME instr-cache base */
#define regCP_ME_IC_BASE_HI            0x5845
#define regCP_GFX_RS64_DC_BASE0_LO     0x5863   /* BASE_IDX 1 (gc1) — PFP data-cache base */
#define regCP_GFX_RS64_DC_BASE0_HI     0x5865
#define regCP_GFX_RS64_DC_BASE1_LO     0x5864   /* BASE_IDX 1 (gc1) — ME data-cache base */
#define regCP_GFX_RS64_DC_BASE1_HI     0x5866
#define regCP_PFP_IC_BASE_CNTL         0x5842   /* BASE_IDX 1 (gc1) */
#define regCP_ME_IC_BASE_CNTL          0x5846   /* BASE_IDX 1 (gc1) */
#define CP_IC_BASE_CNTL__EXE_DISABLE   0x00800000UL   /* bit23 — MUST be 0 to execute */
#define regCP_ME_IC_OP_CNTL            0x5847   /* BASE_IDX 1 (gc1) */
#define regCP_ME_RS64_EXCEPTION_STATUS 0x2aaa   /* BASE_IDX 1, not PFP exception */
#define regCP_GFX_RS64_INTERRUPT1      0x2aac   /* BASE_IDX 1, ME */
#define CP_ME_IC_OP_CNTL__INVALIDATE_CACHE          0x00000001UL /* bit0 */
#define CP_ME_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE 0x00000002UL /* bit1 */
#define CP_ME_IC_OP_CNTL__PRIME_ICACHE 0x00000010UL
#define CP_ME_IC_OP_CNTL__ICACHE_PRIMED 0x00000020UL
/* gfx_firmware_header_v2_0 payload layout (common_header=32B). */
#define GFXH2_UCODE_SIZE  36
#define GFXH2_UCODE_OFF   40
#define GFXH2_DATA_SIZE   44
#define GFXH2_DATA_OFF    48
#define regCP_GFX_RS64_DC_BASE_CNTL    0x2a08   /* BASE_IDX 1 (gc1) */
#define regCP_GFX_RS64_DC_OP_CNTL      0x2a09   /* BASE_IDX 1 (gc1) */
#define CP_GFX_RS64_DC_OP_CNTL__INVALIDATE_DCACHE          0x00000001UL
#define CP_GFX_RS64_DC_OP_CNTL__INVALIDATE_DCACHE_COMPLETE 0x00000002UL
#define regGRBM_STATUS                 0x0da4   /* BASE_IDX 0 (gc0) */
#define regGRBM_CNTL                   0x0da0   /* BASE_IDX 0 (gc0); READ_TIMEOUT = bits[11:0] */
#define GRBM_CNTL__READ_TIMEOUT_MASK   0x00000FFFUL
#define regGRBM_GFX_INDEX              0x2200   /* BASE_IDX 1 (gc1) */
#define PACKET3_NOP                    0x10
#define regRLC_SRM_CNTL                0x4c80   /* BASE_IDX 1 (gc1) */
#define regRLC_CSIB_ADDR_LO            0x0987   /* BASE_IDX 1, BYTE address */
#define regRLC_CSIB_ADDR_HI            0x0988
#define regRLC_CSIB_LENGTH             0x0989   /* DWORD count */
#define RLC_SRM_CNTL__SRM_ENABLE       0x00000001UL
#define RLC_SRM_CNTL__AUTO_INCR_ADDR   0x00000002UL
#define regRLC_CNTL                    0x4c00   /* BASE_IDX 1 (gc1) */
#define regRLC_STAT                    0x4c04   /* BASE_IDX 1 (gc1) */
#define regRLC_GPM_STAT                0x4e6c   /* BASE_IDX 1 (gc1) */
#define RLC_CNTL__RLC_ENABLE_F32       0x00000001UL
#define GRBM_GFX_INDEX_BROADCAST       0xE0000000UL /* SE|SA|INSTANCE broadcast writes */
#define CP_ME_CNTL__PFP_PIPE0_RESET      (1UL << 18)
#define CP_ME_CNTL__PFP_PIPE1_RESET      (1UL << 19)
#define CP_ME_CNTL__ME_PIPE0_RESET       (1UL << 20)
#define CP_ME_CNTL__ME_PIPE1_RESET       (1UL << 21)
#define CP_ME_CNTL__ME_INVALIDATE_ICACHE (1UL << 8)   /* bit8 — like MES; needed for ME prime to take */
#define CP_ME_CNTL__PFP_HALT             (1UL << 26)
#define CP_ME_CNTL__ME_HALT              (1UL << 28)
#define CP_DEBUG__CPG_UTCL1_ERR_HALT_DIS (1UL << 15)
#define FWH2_START_LO   52   /* gfx_firmware_header_v2_0.ucode_start_addr_lo */
#define FWH2_START_HI   56   /* .ucode_start_addr_hi */
#define PACKET3(op,n)   (((ULONG)3 << 30) | (((ULONG)(op) & 0xff) << 8) | (((ULONG)(n) & 0x3fff) << 16))
#define PACKET3_WRITE_DATA      0x37
#define WRITE_DATA_DST_SEL_MEM  (5UL << 8)
#define WRITE_DATA_WR_CONFIRM   (1UL << 20)
#define CP_RING_VRAM_OFF     ((UQUAD)768 << 20)
#define CP_RPTR_WB_VRAM_OFF  ((UQUAD)769 << 20)
#define CP_FENCE_VRAM_OFF    ((UQUAD)770 << 20)
#define CP_RING_LOG2_DWORDS  10
#define CP_FENCE_MAGIC       0x600DF00DUL
/* Scratch-register liveness test (Linux gfx_v12_0_ring_test_ring): SET_UCONFIG_REG
 * writes a scratch reg the CPU reads back with RREG32 — zero memory, zero VM,
 * so it isolates "CP executes packets" from the GFXHUB-VM memory-write path. */
#define regSCRATCH_REG0              0x2040   /* BASE_IDX 1 (gc_seg[1]) */
#define PACKET3_SET_UCONFIG_REG      0x79
#define PACKET3_SET_UCONFIG_REG_START 0xC000
#define CP_SCRATCH_MAGIC             0xDEADBEEFUL
#ifndef DMA_ReadFromRAM
#define DMA_ReadFromRAM (1L << 0)   /* CachePreDMA: DMA goes RAM -> device (flush CPU writes) */
#endif
#define PPSMC_DisallowGfxOff         0x29   /* smu_v14_0_2_ppsmc.h — keep GFX from power-gating */
#define PACKET3_WAIT_REG_MEM         0x3C   /* diagnostic: stall the ME to prove it dispatches */
#define PACKET3_RELEASE_MEM          0x49   /* the real CPU-visible fence: flushes GL2 -> memory */
/* KIQ MAP_QUEUES — connects the MES scheduler ring to the scheduler fabric. Linux
 * mes_v12_0_kiq_enable_queue does this BEFORE SET_HW_RESOURCES; AROS wrongly removed
 * it (thinking MAP_QUEUES was the wrong path) and MMIO-installed the sched queue only,
 * which the MES fw reads but never completes SET_HW_RESOURCES on -> hangs at 0x64d9.
 * For a MES-type ring (gfx_v12_0_kiq_map_queues): me=2, engine_sel=5. Field shifts
 * from nvd.h. */
#define PACKET3_SET_RESOURCES        0xA0
#define SR_VMID_MASK(x)      (((ULONG)(x)) << 0)
#define SR_QUEUE_TYPE(x)     (((ULONG)(x)) << 29)
#define PACKET3_MAP_QUEUES           0xA2
#define MQ_QUEUE_SEL(x)      (((ULONG)(x)) << 4)
#define MQ_VMID(x)           (((ULONG)(x)) << 8)
#define MQ_QUEUE(x)          (((ULONG)(x)) << 13)
#define MQ_PIPE(x)           (((ULONG)(x)) << 16)
#define MQ_ME(x)             (((ULONG)(x)) << 18)
#define MQ_QUEUE_TYPE(x)     (((ULONG)(x)) << 21)
#define MQ_ALLOC_FORMAT(x)   (((ULONG)(x)) << 24)
#define MQ_ENGINE_SEL(x)     (((ULONG)(x)) << 26)
#define MQ_NUM_QUEUES(x)     (((ULONG)(x)) << 29)
#define MQ_DOORBELL_OFFSET(x) (((ULONG)(x)) << 2)
/* GFX clock/power-gating disable — reads pass but CP write-acks stall if CGCG/PG
 * leave the GRBM/GL2 write domain gated. All BASE_IDX 1 (gc_12_0_0_offset.h). */
#define regRLC_CGCG_CGLS_CTRL        0x4c49
#define regRLC_CGTT_MGCG_OVERRIDE    0x4c48
#define regRLC_PG_CNTL               0x4c43
#define RLC_CGTT_MGCG_OVERRIDE_ALL   0x000000BEUL   /* RLC/GRBM SCLK + MGCG/CGCG/CGLS/3D overrides ON */
#define RLC_PG_CNTL_SMU_HANDSHAKE_DISABLE 0x00800000UL
/* GCVM L2 protection-fault status/addr (BASE_IDX 0) — is the CP write faulting? */
#define regGCVM_L2_PROTECTION_FAULT_STATUS_LO32  0x15d0
#define regGCVM_L2_PROTECTION_FAULT_ADDR_LO32    0x15d2
#define regGCVM_L2_PROTECTION_FAULT_ADDR_HI32    0x15d3

/* VRAM staging offset for the RLC autoload buffer. Above the framebuffer and
 * PSP's 256 MiB stage, well below the discovery table at the top of VRAM. The
 * IMU bootloader addr + the RLC's TOC reads are relative to vram_start, so the
 * buffer's byte offset into VRAM is what we program (matches Linux
 * gpu_addr - gmc.vram_start). */
#define GFX_AUTOLOAD_VRAM_OFF   ((UQUAD)320 << 20)

/* MMHUB FB location (to derive the VRAM MC base, as amdgpu_psp.c does) */
#define MMHUB_FB_LOCATION_BASE  0x0554

/* ---- SOC24 autoload firmware IDs + TOC decode (amdgpu_rlc.h) ---- */
#define SOC24_FW_MAX               43
#define SOC24_ID_RLC_G_UCODE        1
#define SOC24_ID_RLC_TOC            2
#define SOC24_ID_SDMA_TH0          11
#define SOC24_ID_RS64_MES_P0       16
#define SOC24_ID_RS64_MES_P1       17
#define SOC24_ID_RS64_PFP          18
#define SOC24_ID_RS64_ME           19
#define SOC24_ID_RS64_MEC          20
#define SOC24_ID_RS64_MES_P0_STACK 21
#define SOC24_ID_RS64_MES_P1_STACK 22
#define SOC24_ID_RS64_PFP_P0_STACK 23
#define SOC24_ID_RS64_PFP_P1_STACK 24
#define SOC24_ID_RS64_ME_P0_STACK  25
#define SOC24_ID_RS64_ME_P1_STACK  26
#define SOC24_ID_RS64_MEC_P0_STACK 27
#define SOC24_ID_RS64_MEC_P1_STACK 28
#define SOC24_ID_RS64_MEC_P2_STACK 29
#define SOC24_ID_RS64_MEC_P3_STACK 30

#define RLC_TOC_OFFSET_DWUNIT   8
#define RLC_SIZE_MULTIPLE       1024
#define RLC_TOC_FORMAT_API      165UL

/* firmware header field byte offsets (amdgpu_ucode.h) */
#define FWH2_UCODE_SIZE     36   /* gfx_firmware_header_v2_0.ucode_size_bytes */
#define FWH2_UCODE_OFF      40   /* .ucode_offset_bytes */
#define FWH2_DATA_SIZE      44   /* .data_size_bytes */
#define FWH2_DATA_OFF       48   /* .data_offset_bytes */
#define MESH_UCODE_SIZE     36   /* mes_firmware_header_v1_0.mes_ucode_size_bytes */
#define MESH_UCODE_OFF      40   /* .mes_ucode_offset_bytes */
#define MESH_DATA_SIZE      48   /* .mes_ucode_data_size_bytes */
#define MESH_DATA_OFF       52   /* .mes_ucode_data_offset_bytes */

struct toc_slot { ULONG offset; ULONG size; };
static struct toc_slot g_toc[SOC24_FW_MAX];

/* GC segment register access */
static inline ULONG gc0_r(struct AmdgpuDevice *ad, ULONG off)
{ return amdgpu_rreg32(ad, ad->gc_seg[0] + off); }
static inline ULONG gc1_r(struct AmdgpuDevice *ad, ULONG off)
{ return amdgpu_rreg32(ad, ad->gc_seg[1] + off); }
static inline void gc1_w(struct AmdgpuDevice *ad, ULONG off, ULONG val)
{ amdgpu_wreg32(ad, ad->gc_seg[1] + off, val); }
static inline void gc0_w(struct AmdgpuDevice *ad, ULONG off, ULONG val)
{ amdgpu_wreg32(ad, ad->gc_seg[0] + off, val); }

static inline ULONG le32p(const UBYTE *p)
{ return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24); }

BOOL gfx_v12_cp_gfx_ring_bringup(struct AmdgpuDevice *ad);

/* Observe the PSP-owned path after SMU setup and before this file writes any GC
 * state. Linux waits at this exact GFX-hw-init boundary; our earlier PSP-side
 * observation runs before SMU initialization and cannot distinguish ordering
 * from a firmware/autoload failure. */

/* Exported GFX/IMU state snapshot — the post-PSP counterpart to
 * psp_gfx_snapshot(). The per-command PSP trace showed the IMU reaches only
 * SCRATCH_1 = 0xb000b000 while PSP commands are running, yet reads 0xBABEFACE
 * post-boot: so it finishes booting in a window we never instrumented. This
 * makes that window visible. */
void amdgpu_gfx_state_snapshot(struct AmdgpuDevice *ad, const char *tag)
{
    if (!ad || !ad->gc_seg[0] || !ad->gc_seg[1])
        return;
    bug("[amdgpu-gfx]   STATE[%s] RESET_CTRL=%08lx CORE=%08lx SCR10=%08lx SCR1=%08lx "
        "BOOTLOAD=%08lx CP_STAT=%08lx\n", tag,
        (unsigned long)gc1_r(ad, regGFX_IMU_GFX_RESET_CTRL),
        (unsigned long)gc1_r(ad, regGFX_IMU_CORE_CTRL),
        (unsigned long)gc1_r(ad, regGFX_IMU_SCRATCH_10),
        (unsigned long)gc1_r(ad, 0x4069),
        (unsigned long)gc1_r(ad, regRLC_RLCS_BOOTLOAD_STATUS),
        (unsigned long)gc0_r(ad, regCP_STAT));
}

static BOOL gfx_v12_wait_psp_autoload(struct AmdgpuDevice *ad)
{
    ULONG cp_initial, boot_initial, cp_status, boot_status, reset_ctrl, core_ctrl;
    ULONG t;

    cp_initial = gc0_r(ad, regCP_STAT);
    boot_initial = gc1_r(ad, regRLC_RLCS_BOOTLOAD_STATUS);
    cp_status = cp_initial;
    boot_status = boot_initial;

    /* Linux polls this for adev->usec_timeout (1,000,000us = 1s) even though on a
     * healthy card it completes in ~3us — because by the time gfx_v12_0_hw_init()
     * looks, the SMU stage (~45ms) and dm_hw_init (~483ms) have already elapsed
     * since AUTOLOAD_RLC. Our 5000 polls were ~25ms, far short of that budget, so
     * a slow-but-working autoload would read as a timeout. Give it a Linux-sized
     * window before concluding anything. */
    {
        /* Watch the IMU for ANY movement across the whole wait, not just the
         * two registers the success condition uses. The per-command PSP trace
         * proved the IMU is still mid-boot when the PSP stage ends
         * (SCRATCH_1 = 0xb000b000) and reaches 0xBABEFACE later, so the
         * interesting transition happens somewhere in here. Log only on change
         * so a quiet wait stays quiet. */
        ULONG l_rc = 0xFFFFFFFFUL, l_core = 0, l_s10 = 0, l_s1 = 0, changes = 0;

        for (t = 0; t < 200000; t++) {
            ULONG rc, core, s10, s1;

            cp_status = gc0_r(ad, regCP_STAT);
            boot_status = gc1_r(ad, regRLC_RLCS_BOOTLOAD_STATUS);

            rc   = gc1_r(ad, regGFX_IMU_GFX_RESET_CTRL);
            core = gc1_r(ad, regGFX_IMU_CORE_CTRL);
            s10  = gc1_r(ad, regGFX_IMU_SCRATCH_10);
            s1   = gc1_r(ad, 0x4069);
            if (rc != l_rc || core != l_core || s10 != l_s10 || s1 != l_s1) {
                /* Cap the trace: on a re-attached (already-booted) GPU the running
                 * IMU oscillates SCR1 (0xb000b000<->0xBABEFACE) every poll, and CP_STAT
                 * is nonzero so the exit condition below never trips — without this cap
                 * that floods COM1 for all 200000 polls and livelocks the box. */
                if (changes < 16)
                    bug("[amdgpu-gfx]   IMU MOVED @poll %lu: RESET_CTRL=%08lx CORE=%08lx "
                        "SCR10=%08lx SCR1=%08lx (BOOTLOAD=%08lx CP=%08lx)\n",
                        (unsigned long)t, (unsigned long)rc, (unsigned long)core,
                        (unsigned long)s10, (unsigned long)s1,
                        (unsigned long)boot_status, (unsigned long)cp_status);
                l_rc = rc; l_core = core; l_s10 = s10; l_s1 = s1;
                changes++;
            }

            if (cp_status == 0 && (boot_status & RLC_BOOTLOAD_COMPLETE))
                break;
            { volatile ULONG d; for (d = 0; d < 2000; d++) ; }
        }
        bug("[amdgpu-gfx]   IMU watch: %lu distinct state(s) over %lu polls\n",
            (unsigned long)changes, (unsigned long)t);
    }

    reset_ctrl = gc1_r(ad, regGFX_IMU_GFX_RESET_CTRL);
    core_ctrl = gc1_r(ad, regGFX_IMU_CORE_CTRL);
    bug("[amdgpu-gfx] post-SMU PSP autoload: initial CP_STAT=0x%08lx "
        "BOOTLOAD_STATUS=0x%08lx; final CP_STAT=0x%08lx "
        "BOOTLOAD_STATUS=0x%08lx after %lu polls -> %s; "
        "RESET_CTRL=0x%08lx CORE_CTRL=0x%08lx SCRATCH_10=0x%08lx\n",
        (unsigned long)cp_initial, (unsigned long)boot_initial,
        (unsigned long)cp_status, (unsigned long)boot_status,
        (unsigned long)t,
        (cp_status == 0 && (boot_status & RLC_BOOTLOAD_COMPLETE))
            ? "COMPLETE" : "TIMEOUT",
        (unsigned long)reset_ctrl, (unsigned long)core_ctrl,
        (unsigned long)gc1_r(ad, regGFX_IMU_SCRATCH_10));

    return cp_status == 0 && (boot_status & RLC_BOOTLOAD_COMPLETE);
}

/* common_firmware_header (amdgpu_ucode.h): 32 bytes; the fields we use. */
#define FWH_UCODE_VERSION      16
#define FWH_UCODE_SIZE_BYTES   20
#define FWH_UCODE_ARRAY_OFF    24
#define FWH_COMMON_SIZE        32
/* imu_firmware_header_v1_0 extras, immediately after the common header. */
#define IMUH_IRAM_SIZE         (FWH_COMMON_SIZE + 0)   /* 32 */
#define IMUH_IRAM_OFF          (FWH_COMMON_SIZE + 4)   /* 36 */
#define IMUH_DRAM_SIZE         (FWH_COMMON_SIZE + 8)   /* 40 */
#define IMUH_DRAM_OFF          (FWH_COMMON_SIZE + 12)  /* 44 */

/* Parse the RLC TOC (RLC_TABLE_OF_CONTENT_V2 entries) into g_toc[id]. Each entry
 * is 8 bytes: DW0 = offset:25 | id:7; DW1 = ... size_x16:1(bit12) ... size:18(bit14).
 * Returns the number of components the TOC lists. */
static ULONG gfx_v12_parse_toc(const UBYTE *toc, ULONG toc_len)
{
    ULONG p = 0, n = 0;
    memset(g_toc, 0, sizeof(g_toc));
    while (p + 8 <= toc_len) {
        ULONG dw0 = le32p(toc + p), dw1 = le32p(toc + p + 4);
        ULONG id  = (dw0 >> 25) & 0x7f;
        ULONG off = (dw0 & 0x01ffffffUL) * RLC_TOC_OFFSET_DWUNIT * 4;
        ULONG sx16 = (dw1 >> 12) & 1;
        ULONG sz   = (dw1 >> 14) & 0x0003ffffUL;
        if (id == 0 || id >= SOC24_FW_MAX)
            break;
        g_toc[id].offset = off;
        g_toc[id].size   = sx16 ? sz * RLC_SIZE_MULTIPLE * 4 : sz * 4;
        p += 8; n++;
    }
    return n;
}

/* Write one component into the autoload buffer at its TOC offset. */
static void gfx_v12_stage_id(struct AmdgpuDevice *ad, UQUAD base, ULONG id,
                             const UBYTE *data, ULONG size)
{
    if (id >= SOC24_FW_MAX || !g_toc[id].offset || !data || !size)
        return;
    if (size > g_toc[id].size && g_toc[id].size)
        size = g_toc[id].size;             /* never overflow the slot */
    amdgpu_vram_write(ad, base + g_toc[id].offset, (const ULONG *)data,
                      (size + 3) / 4);
}

/* Load a firmware file; caller FreeVec()s *out. */
static BOOL gfx_v12_load_fw(struct AmdgpuDevice *ad, const char *suffix, UBYTE **out, ULONG *out_len)
{
    *out = NULL; *out_len = 0;
    if (!amdgpu_request_ip_firmware(ad, AMDGPU_FW_GC, suffix, out, out_len) || !*out) {
        bug("[amdgpu-gfx] GC firmware %s not found\n", suffix);
        return FALSE;
    }
    return TRUE;
}

/* rlc_firmware_header v2_1/v2_2 field byte offsets (amdgpu_ucode.h):
 * common(32) + v2_0(18*4=72)=104; v2_1 adds 13*4 -> the save_restore fields;
 * v2_2 adds the rlx6 iram/dram fields. */
#define RLCH_GPM_SIZE    132  /* save_restore_list_gpm_size_bytes   -> RLCG_SCRATCH */
#define RLCH_GPM_OFF     136  /* save_restore_list_gpm_offset_bytes */
#define RLCH_SRM_SIZE    148  /* save_restore_list_srm_size_bytes   -> RLC_SRM_ARAM */
#define RLCH_SRM_OFF     152  /* save_restore_list_srm_offset_bytes */
#define RLCH_IRAM_SIZE   156  /* rlc_iram_ucode_size_bytes          -> RLX6_UCODE */
#define RLCH_IRAM_OFF    160  /* rlc_iram_ucode_offset_bytes */
#define RLCH_DRAM_SIZE   164  /* rlc_dram_ucode_size_bytes          -> RLX6_DRAM_BOOT */
#define RLCH_DRAM_OFF    168  /* rlc_dram_ucode_offset_bytes */

/* Stage an RLC sub-component from the RLC firmware (guarded: skip if the header
 * field is zero or out of range). */
static void gfx_v12_stage_rlc_sub(struct AmdgpuDevice *ad, UQUAD base, ULONG id,
                                  const UBYTE *rlc, ULONG rlc_len,
                                  ULONG off_field, ULONG size_field)
{
    ULONG off = le32p(rlc + off_field), sz = le32p(rlc + size_field);
    if (!off || !sz || off + sz > rlc_len)
        return;
    gfx_v12_stage_id(ad, base, id, rlc + off, sz);
}

/* Build the RLC autoload buffer in VRAM and point the IMU bootloader at RLC_G.
 * The TOC lists only RLC-side components (RLC_G, TOC, RLCG_SCRATCH, RLC_SRM_ARAM,
 * RLX6 iram/dram) — all inside gc_12_0_0_rlc.bin's v2.1/v2.2 headers. That is
 * exactly what the RLC needs to finish its bootstrap and release the remaining
 * GFX reset domains (0x30 -> 0x1f). CP/MEC come later (P2), not via this TOC. */
static BOOL gfx_v12_build_autoload(struct AmdgpuDevice *ad)
{
    UBYTE *toc = NULL, *rlc = NULL;
    ULONG toc_len = 0, rlc_len = 0;
    const UBYTE *toc_tab; ULONG toc_tab_off, toc_tab_sz, ncomp;
    UQUAD base = GFX_AUTOLOAD_VRAM_OFF;
    BOOL ok = FALSE;

    if (!gfx_v12_load_fw(ad, "_toc", &toc, &toc_len))
        return FALSE;
    toc_tab_off = le32p(toc + FWH_UCODE_ARRAY_OFF);
    toc_tab_sz  = le32p(toc + FWH_UCODE_SIZE_BYTES);
    if (!toc_tab_off || !toc_tab_sz || toc_tab_off + toc_tab_sz > toc_len) {
        bug("[amdgpu-gfx] TOC header bogus (off=%lu sz=%lu len=%lu)\n",
            (unsigned long)toc_tab_off, (unsigned long)toc_tab_sz, (unsigned long)toc_len);
        goto out;
    }
    toc_tab = toc + toc_tab_off;
    ncomp = gfx_v12_parse_toc(toc_tab, toc_tab_sz);
    bug("[amdgpu-gfx] TOC parsed: %lu components; RLC_G off=0x%lx sz=%lu, SRM off=0x%lx, RLX6 off=0x%lx\n",
        (unsigned long)ncomp,
        (unsigned long)g_toc[SOC24_ID_RLC_G_UCODE].offset, (unsigned long)g_toc[SOC24_ID_RLC_G_UCODE].size,
        (unsigned long)g_toc[4].offset, (unsigned long)g_toc[7].offset);
    if (!g_toc[SOC24_ID_RLC_G_UCODE].size) {   /* RLC_G legitimately sits at offset 0 */
        bug("[amdgpu-gfx] TOC has no RLC_G entry — cannot bootstrap\n");
        goto out;
    }

    if (!gfx_v12_load_fw(ad, "_rlc", &rlc, &rlc_len))
        goto out;

    /* RLC_G (id 1): the main RLC ucode (common header ucode). */
    gfx_v12_stage_id(ad, base, SOC24_ID_RLC_G_UCODE,
                     rlc + le32p(rlc + FWH_UCODE_ARRAY_OFF), le32p(rlc + FWH_UCODE_SIZE_BYTES));
    /* RLC sub-components from the v2.1/v2.2 headers (ids 3,4,7,9). */
    gfx_v12_stage_rlc_sub(ad, base, 3, rlc, rlc_len, RLCH_GPM_OFF,  RLCH_GPM_SIZE);   /* RLCG_SCRATCH */
    gfx_v12_stage_rlc_sub(ad, base, 4, rlc, rlc_len, RLCH_SRM_OFF,  RLCH_SRM_SIZE);   /* RLC_SRM_ARAM */
    gfx_v12_stage_rlc_sub(ad, base, 7, rlc, rlc_len, RLCH_IRAM_OFF, RLCH_IRAM_SIZE);  /* RLX6_UCODE */
    gfx_v12_stage_rlc_sub(ad, base, 9, rlc, rlc_len, RLCH_DRAM_OFF, RLCH_DRAM_SIZE);  /* RLX6_DRAM_BOOT */

    /* RLC_TOC (id 2): the table itself, with Linux's last-2-dword API patch. */
    {
        ULONG tsz = g_toc[SOC24_ID_RLC_TOC].size ? g_toc[SOC24_ID_RLC_TOC].size : toc_tab_sz;
        UBYTE *tcopy = AllocVec(tsz, MEMF_PUBLIC);
        if (tcopy) {
            ULONG cp = tsz < toc_tab_sz ? tsz : toc_tab_sz;
            memset(tcopy, 0, tsz);
            CopyMem((APTR)toc_tab, tcopy, cp);
            if (tsz >= 8)
                *(ULONG *)(tcopy + tsz - 8) = (RLC_TOC_FORMAT_API << 24) | 0x1;
            gfx_v12_stage_id(ad, base, SOC24_ID_RLC_TOC, tcopy, tsz);
            FreeVec(tcopy);
        }
    }

    /* Point the IMU RLC bootloader at RLC_G inside the buffer (VRAM byte offset). */
    {
        UQUAD rlc_g = base + g_toc[SOC24_ID_RLC_G_UCODE].offset;
        gc1_w(ad, regGFX_IMU_RLC_BOOTLOADER_ADDR_HI, (ULONG)(rlc_g >> 32));
        gc1_w(ad, regGFX_IMU_RLC_BOOTLOADER_ADDR_LO, (ULONG)(rlc_g & 0xFFFFFFFFUL));
        gc1_w(ad, regGFX_IMU_RLC_BOOTLOADER_SIZE, g_toc[SOC24_ID_RLC_G_UCODE].size);
        bug("[amdgpu-gfx] RLC autoload staged @VRAM+%luMiB, bootloader -> RLC_G off=0x%lx sz=%lu\n",
            (unsigned long)(base >> 20), (unsigned long)g_toc[SOC24_ID_RLC_G_UCODE].offset,
            (unsigned long)g_toc[SOC24_ID_RLC_G_UCODE].size);
    }
    ok = TRUE;

out:
    if (toc) FreeVec(toc);
    if (rlc) FreeVec(rlc);
    return ok;
}

/* Load the IMU I-RAM / D-RAM microcode (imu_v12_0_load_microcode). */
static BOOL gfx_v12_imu_load_microcode(struct AmdgpuDevice *ad)
{
    UBYTE *imu = NULL;
    ULONG imu_len = 0;
    ULONG array_off, ver, iram_sz, dram_sz, i;
    const UBYTE *iram, *dram;

    if (!amdgpu_request_ip_firmware(ad, AMDGPU_FW_GC, "_imu", &imu, &imu_len) || !imu) {
        bug("[amdgpu-gfx] IMU firmware not found\n");
        return FALSE;
    }

    ver       = le32p(imu + FWH_UCODE_VERSION);
    array_off = le32p(imu + FWH_UCODE_ARRAY_OFF);
    iram_sz   = le32p(imu + IMUH_IRAM_SIZE);
    dram_sz   = le32p(imu + IMUH_DRAM_SIZE);

    if (!array_off || array_off + iram_sz + dram_sz > imu_len) {
        bug("[amdgpu-gfx] IMU header bogus (array_off=%lu iram=%lu dram=%lu len=%lu)\n",
            (unsigned long)array_off, (unsigned long)iram_sz,
            (unsigned long)dram_sz, (unsigned long)imu_len);
        FreeVec(imu);
        return FALSE;
    }

    iram = imu + array_off;
    dram = imu + array_off + iram_sz;

    bug("[amdgpu-gfx] IMU fw v0x%lx iram=%lu B dram=%lu B — loading\n",
        (unsigned long)ver, (unsigned long)iram_sz, (unsigned long)dram_sz);

    /* I-RAM */
    gc1_w(ad, regGFX_IMU_I_RAM_ADDR, 0);
    for (i = 0; i < iram_sz / 4; i++)
        gc1_w(ad, regGFX_IMU_I_RAM_DATA, le32p(iram + i * 4));
    gc1_w(ad, regGFX_IMU_I_RAM_ADDR, ver);

    /* D-RAM */
    gc1_w(ad, regGFX_IMU_D_RAM_ADDR, 0);
    for (i = 0; i < dram_sz / 4; i++)
        gc1_w(ad, regGFX_IMU_D_RAM_DATA, le32p(dram + i * 4));
    gc1_w(ad, regGFX_IMU_D_RAM_ADDR, ver);

    /* Verify the I-RAM load actually landed: rewind + read back the first dwords
     * and compare to the firmware. If they differ, the IMU never got the ucode
     * (and so cannot run) — a very different problem from "RLC won't bootstrap". */
    {
        ULONG rb0, rb1;
        gc1_w(ad, regGFX_IMU_I_RAM_ADDR, 0);
        rb0 = gc1_r(ad, regGFX_IMU_I_RAM_DATA);
        rb1 = gc1_r(ad, regGFX_IMU_I_RAM_DATA);
        bug("[amdgpu-gfx] IMU I-RAM readback [0]=0x%08lx [1]=0x%08lx  expect 0x%08lx 0x%08lx  (%s)\n",
            (unsigned long)rb0, (unsigned long)rb1,
            (unsigned long)le32p(iram), (unsigned long)le32p(iram + 4),
            (rb0 == le32p(iram)) ? "LOAD OK" : "LOAD MISMATCH");
    }

    FreeVec(imu);
    return TRUE;
}

/* imu_v12_0_setup + start + wait_for_reset_status */
static BOOL gfx_v12_imu_start(struct AmdgpuDevice *ad)
{
    ULONG v;
    int i;

    /* setup: open the C2PMSG access windows */
    gc1_w(ad, regGFX_IMU_C2PMSG_ACCESS_CTRL0, 0xffffff);
    gc1_w(ad, regGFX_IMU_C2PMSG_ACCESS_CTRL1, 0xffff);

    /* start: clear the IMU core soft-reset (bit 0) */
    v = gc1_r(ad, regGFX_IMU_CORE_CTRL);
    gc1_w(ad, regGFX_IMU_CORE_CTRL, v & 0xfffffffeUL);
    bug("[amdgpu-gfx] IMU CORE_CTRL 0x%08lx -> 0x%08lx (core reset %s)\n",
        (unsigned long)v, (unsigned long)gc1_r(ad, regGFX_IMU_CORE_CTRL),
        (v & 1) ? "was HELD, now cleared" : "was ALREADY running");

    /* wait: GFX_IMU_GFX_RESET_CTRL low 5 bits all set == GFX out of reset. The
     * IMU/RLC bootstrap takes tens of ms (Linux polls ~100ms with udelay(1)); a
     * bare read loop finishes in ~1ms and gives up mid-bringup, so busy-delay
     * ~1us per poll (amdgpu_gpio_ddc_delay unit) for a real ~100ms window. */
    for (i = 0; i < 100000; i++) {
        v = gc1_r(ad, regGFX_IMU_GFX_RESET_CTRL);
        if ((v & 0x1f) == 0x1f)
            break;
        { volatile ULONG d; for (d = 0; d < 3000; d++) ; }
    }

    if ((v & 0x1f) != 0x1f) {
        ULONG s;
        bug("[amdgpu-gfx] IMU start TIMEOUT after %lu polls — GFX_IMU_GFX_RESET_CTRL=0x%08lx (want low5=0x1f)\n",
            (unsigned long)i, (unsigned long)v);
        /* Diagnose the stall: is the RLC running / bootloading? what does the IMU
         * report in its scratch registers? (all BASE_IDX 1.) */
        bug("[amdgpu-gfx]   RLC_CNTL=0x%08lx GPM_STAT=0x%08lx BOOTLOAD_STATUS=0x%08lx IMU_RLC_MSG=0x%08lx\n",
            (unsigned long)gc1_r(ad, 0x4c00), (unsigned long)gc1_r(ad, 0x4e6c),
            (unsigned long)gc1_r(ad, 0x4e7c), (unsigned long)gc1_r(ad, 0x4edc));
        for (s = 0; s <= 10; s++)
            bug("[amdgpu-gfx]   GFX_IMU_SCRATCH_%lu=0x%08lx\n",
                (unsigned long)s, (unsigned long)gc1_r(ad, 0x4068 + s));
        return FALSE;
    }

    bug("[amdgpu-gfx] IMU started after %lu polls — GFX_IMU_GFX_RESET_CTRL=0x%08lx, GFX block powered\n",
        (unsigned long)i, (unsigned long)v);
    return TRUE;
}

/* IMU RLC-RAM golden values (generated from imu_rlc_ram_golden_12_0_1 in
 * imu_v12_0.c + gc_12_0_0_offset.h). seg = BASE_IDX; special = FB/AGP data is
 * computed at runtime. The IMU applies these to the GFX block during power-up;
 * WITHOUT them GFX_IMU_GFX_RESET_CTRL stalls at 0x30 (HARD_RESETB held). */
struct imu_golden { UBYTE seg; ULONG reg; ULONG data; ULONG mask; UBYTE special; };
static const struct imu_golden gfx12_imu_golden[] = {
    { 1, 0x5b88, 0x000001e4, 0x1c0000, 0 }, { 1, 0x5b85, 0x000001e4, 0x1c0000, 0 },
    { 1, 0x5b84, 0x000001e4, 0x1c0000, 0 }, { 1, 0x5b80, 0x13571357, 0x1c0000, 0 },
    { 1, 0x5b81, 0x64206420, 0x1c0000, 0 }, { 1, 0x5b82, 0x02460246, 0x1c0000, 0 },
    { 1, 0x5b83, 0x75317531, 0x1c0000, 0 }, { 1, 0x2e0c, 0xc0d41183, 0x1c0000, 0 },
    { 0, 0x000e, 0x0507d1c0, 0x1c0000, 0 }, { 0, 0x060e, 0x0507d1c0, 0x1c0000, 0 },
    { 0, 0x0f62, 0x00600100, 0x1c0000, 0 }, { 0, 0x17a3, 0x003f7fff, 0x1c0000, 0 },
    { 0, 0x1823, 0x003f7ebf, 0x1c0000, 0 }, { 0, 0x17a4, 0x02e00000, 0x1c0000, 0 },
    { 0, 0x17a5, 0x0001a078, 0x1c0000, 0 }, { 0, 0x17a6, 0x00000000, 0x1c0000, 0 },
    { 0, 0x1824, 0x00000000, 0x1c0000, 0 }, { 0, 0x1825, 0x00012030, 0x1c0000, 0 },
    { 0, 0x1826, 0x00000000, 0x1c0000, 0 }, { 0, 0x17a7, 0x19041000, 0x1c0000, 0 },
    { 0, 0x17a8, 0x80000000, 0x1c0000, 0 }, { 0, 0x1827, 0x1e080000, 0x1c0000, 0 },
    { 0, 0x1828, 0x80000000, 0x1c0000, 0 }, { 0, 0x17a2, 0x00000880, 0x1c0000, 0 },
    { 0, 0x1822, 0x00008880, 0x1c0000, 0 }, { 0, 0x17a1, 0x00000017, 0x1c0000, 0 },
    { 0, 0x1821, 0x00000077, 0x1c0000, 0 }, { 0, 0x181f, 0x00000001, 0x1c0000, 0 },
    { 0, 0x189f, 0x00000001, 0x1c0000, 0 }, { 0, 0x15cd, 0x00020000, 0x1c0000, 0 },
    { 0, 0x15b0, 0x0000000c, 0x1c0000, 0 }, { 0, 0x15ad, 0x000fffff, 0x1c0000, 0 },
    { 0, 0x17ac, 0x00000091, 0x1c0000, 0 }, { 0, 0x182c, 0x00000091, 0x1c0000, 0 },
    { 1, 0x2200, 0xe0000000, 0x1c0000, 0 }, { 1, 0x1990, 0x00008500, 0x1c0000, 0 },
    { 0, 0x1025, 0x00880007, 0x1c0000, 0 }, { 0, 0x12c5, 0x00000001, 0x1c0000, 0 },
    { 1, 0x2200, 0x00000000, 0x1c0000, 0 }, { 1, 0x1880, 0x01e00000, 0x1c0000, 0 },
    { 1, 0x2200, 0x00000001, 0x1c0000, 0 }, { 1, 0x1880, 0x01e00000, 0x1c0000, 0 },
    { 1, 0x2200, 0x00000100, 0x1c0000, 0 }, { 1, 0x1880, 0x01e00000, 0x1c0000, 0 },
    { 1, 0x2200, 0x00000101, 0x1c0000, 0 }, { 1, 0x1880, 0x01e00000, 0x1c0000, 0 },
    { 1, 0x2200, 0xe0000000, 0x1c0000, 0 }, { 0, 0x13de, 0x08200545, 0x1c0000, 0 },
    { 1, 0x3808, 0x00000000, 0x1c0000, 0 }, { 1, 0x3c02, 0x000fffff, 0x1c0000, 0 },
    { 0, 0x1e0c, 0x00020000, 0x1c0000, 0 }, { 0, 0x1e21, 0x00500010, 0x1c0000, 0 },
    { 0, 0x161b, 0x00000500, 0x1c0000, 0 }, { 0, 0x1619, 0x00000001, 0x1c0000, 0 },
    { 0, 0x161a, 0x00000000, 0x1c0000, 0 }, { 0, 0x15b1, 0x00000000, 0x1c0000, 0 },
    { 0, 0x15b2, 0x0000000f, 0x1c0000, 0 }, { 0, 0x1614, 0x00006000, 0x1c0000, 1 },
    { 0, 0x1615, 0x0000600f, 0x1c0000, 1 }, { 0, 0x1624, 0x00000000, 0x1c0000, 0 },
    { 0, 0x1625, 0x00000000, 0x1c0000, 0 }, { 0, 0x15a4, 0xff800000, 0xe0000000, 0 },
    { 0, 0x15a5, 0x00000001, 0x1c0000, 0 }, { 0, 0x15a6, 0x0000ffff, 0x1c0000, 0 },
    { 0, 0x1618, 0x00000000, 0x1c0000, 1 }, { 0, 0x1617, 0x00000002, 0x1c0000, 0 },
    { 0, 0x1616, 0x00000000, 0x1c0000, 1 }, { 0, 0x15cc, 0x00001ffc, 0x1c0000, 0 },
    { 0, 0x161b, 0x00000551, 0x1c0000, 0 }, { 0, 0x15c4, 0x00080603, 0x1c0000, 0 },
    { 0, 0x15c5, 0x00000003, 0x1c0000, 0 }, { 0, 0x15c6, 0x00100003, 0x1c0000, 0 },
    { 0, 0x15e3, 0x00003fe0, 0x1c0000, 0 }, { 0, 0x1619, 0x0003d000, 0x1c0000, 0 },
    { 0, 0x161a, 0x0003d7ff, 0x1c0000, 0 }, { 0, 0x15a8, 0x00000000, 0x1c0000, 0 },
    { 0, 0x15a9, 0x00000000, 0x1c0000, 0 },
};

/* Program the IMU's internal RLC RAM with the golden GFX config (imu_v12_0
 * program_rlc_ram). Must run BEFORE the IMU is started so it applies them during
 * power-up. FB/AGP entries take the live display's aperture. */
static void gfx_v12_program_rlc_ram(struct AmdgpuDevice *ad)
{
    ULONG mmhub  = ad->ip.mmhub.valid ? ad->ip.mmhub.base : 0;
    ULONG fb_base = mmhub ? (amdgpu_rreg32(ad, mmhub + MMHUB_FB_LOCATION_BASE) & 0x00FFFFFFUL) : 0;
    ULONG fb_top  = mmhub ? (amdgpu_rreg32(ad, mmhub + MMHUB_FB_LOCATION_TOP)  & 0x00FFFFFFUL) : 0;
    ULONG i, n = sizeof(gfx12_imu_golden) / sizeof(gfx12_imu_golden[0]);

    gc1_w(ad, regGFX_IMU_RLC_RAM_INDEX, 0x2);
    for (i = 0; i < n; i++) {
        const struct imu_golden *e = &gfx12_imu_golden[i];
        ULONG reg  = (ad->gc_seg[e->seg] + e->reg) | e->mask;
        ULONG data = e->data;
        if (e->special) {
            if (e->reg == regGCMC_VM_FB_LOCATION_BASE_OFF)      data = fb_base;
            else if (e->reg == regGCMC_VM_FB_LOCATION_TOP_OFF)  data = fb_top;
            else if (e->reg == regGCMC_VM_AGP_BASE_OFF)         data = 0x00ffffffUL;
            else if (e->reg == regGCMC_VM_AGP_TOP_OFF)          data = 0x0;
        }
        gc1_w(ad, regGFX_IMU_RLC_RAM_ADDR_HIGH, 0);
        gc1_w(ad, regGFX_IMU_RLC_RAM_ADDR_LOW, reg);
        gc1_w(ad, regGFX_IMU_RLC_RAM_DATA, data);
    }
    /* latest-entry terminator + mark RAM valid */
    gc1_w(ad, regGFX_IMU_RLC_RAM_ADDR_HIGH, 0);
    gc1_w(ad, regGFX_IMU_RLC_RAM_ADDR_LOW, 0);
    gc1_w(ad, regGFX_IMU_RLC_RAM_DATA, 0);
    gc1_w(ad, regGFX_IMU_RLC_RAM_INDEX,
          gc1_r(ad, regGFX_IMU_RLC_RAM_INDEX) | GFX_IMU_RLC_RAM_INDEX_VALID);
    bug("[amdgpu-gfx] IMU RLC RAM: %lu golden entries programmed (fb_base=0x%lx top=0x%lx)\n",
        (unsigned long)n, (unsigned long)fb_base, (unsigned long)fb_top);
}

BOOL amdgpu_gfx_hw_init(struct AmdgpuDevice *ad)
{
    ULONG grbm_before, grbm_after;

    /* GFX12 only (Navi 44 = GC 12.0.0). Skip quietly on anything else so the IP
     * loop keeps working for other ASICs. */
    if (!ad->ip.gc.valid || ad->ip.gc.major != 12) {
        D(bug("[amdgpu-gfx] GC %u.%u.%u not GFX12 — gfx block skipped\n",
              ad->ip.gc.major, ad->ip.gc.minor, ad->ip.gc.revision));
        return TRUE;
    }

    if (!ad->gc_seg[1]) {
        bug("[amdgpu-gfx] no GC BASE_IDX-1 segment captured — cannot address the IMU\n");
        return FALSE;
    }

    bug("[amdgpu-gfx] GFX12 bring-up: gc_seg[0]=0x%05lx gc_seg[1]=0x%05lx\n",
        (unsigned long)ad->gc_seg[0], (unsigned long)ad->gc_seg[1]);

    /* On the PSP load path this wait is the ENTIRE job. Linux's
     * gfx_v12_0_hw_init() for AMDGPU_FW_LOAD_PSP calls no IMU function at all —
     * load_microcode/setup_imu/start_imu are DIRECT/RLC_BACKDOOR only
     * (gfx_v12_0.c:1338, :3668) — and amdgpu_dpm_set_gfx_power_up_by_imu() is
     * AMD_IS_APU only (imu_v12_0.c:171). On a discrete GFX12 part the PSP powers
     * the GFX rail itself during AUTOLOAD_RLC, which is why Linux's
     * gfx_v12_0_wait_for_rlc_autoload_complete() returns in ~3us. */
    if (gfx_v12_wait_psp_autoload(ad)) {
        bug("[amdgpu-gfx] *** RLC AUTOLOAD COMPLETE — PSP powered the GFX rail; "
            "GRBM_STATUS=0x%08lx ***\n",
            (unsigned long)gc0_r(ad, regGRBM_STATUS));
        gfx_v12_cp_gfx_ring_bringup(ad);   /* P1: first CP graphics-ring submission */
        return TRUE;
    }

    /* Autoload did not complete. Everything below is the DIRECT/BACKDOOR-path
     * IMU sequence, kept ONLY as an explicit fallback experiment: it HALTS the
     * IMU core that the PSP just started (SCRATCH_1=0xbabeface proves the PSP's
     * IMU firmware is live once LOAD_TOC is restored), so on the PSP path it
     * destroys the PSP's own work. Gated off by default — set
     * AMDGPU_GFX_DIRECT_IMU_FALLBACK to re-enable for a bounded experiment. */
#ifndef AMDGPU_GFX_DIRECT_IMU_FALLBACK
#define AMDGPU_GFX_DIRECT_IMU_FALLBACK 0
#endif
    if (!AMDGPU_GFX_DIRECT_IMU_FALLBACK) {
        bug("[amdgpu-gfx] autoload incomplete; DIRECT-path IMU poking stays DISABLED "
            "(Linux never touches the IMU on the PSP path — halting it would undo "
            "the PSP autoload). CP bring-up skipped.\n");
        return TRUE;
    }

    /* DECISIVE TEST: can we WRITE the GFX/IMU block at all in the live-GOP state?
     * Write a marker to a harmless IMU scratch reg (BASE_IDX 1) and read it back.
     * If it doesn't stick, every IMU/golden/I-RAM write has been a no-op and 0x30
     * is just GOP leftover — the block needs a reset/ungate before it's drivable. */
    {
        ULONG rb, imu_state;
        /* SCRATCH_10 is owned by the (PSP-loaded, running) IMU, so test a reg it
         * doesn't continuously write: RLC_RAM_ADDR_HIGH. */
        imu_state = gc1_r(ad, regGFX_IMU_SCRATCH_10);
        gc1_w(ad, regGFX_IMU_RLC_RAM_ADDR_HIGH, 0xDEADBEEFUL);
        rb = gc1_r(ad, regGFX_IMU_RLC_RAM_ADDR_HIGH);
        /* The reg has ~11 valid bits, so a working write reads back masked
         * (0xDEADBEEF & 0x7ff = 0x6ef), NOT the exact value. Match the low bits. */
        bug("[amdgpu-gfx] WRITE TEST: RLC_RAM_ADDR_HIGH <-0xDEADBEEF read=0x%08lx (%s); IMU SCRATCH_10=0x%08lx\n",
            (unsigned long)rb,
            ((rb & 0x7ffUL) == (0xDEADBEEFUL & 0x7ffUL)) ? "WRITES WORK" : "WRITES BLOCKED",
            (unsigned long)imu_state);
    }

    grbm_before = gc0_r(ad, regGRBM_STATUS);
    bug("[amdgpu-gfx] GRBM_STATUS before IMU = 0x%08lx\n", (unsigned long)grbm_before);

    /* GFX bring-up is BEST-EFFORT: a missing blob or a stalled IMU must NOT fail
     * amdgpu_device_init — display/PSP stay up, only 3D/compute is unavailable.
     * So every failure below returns TRUE (device usable) with a loud reason. */
    /* The PSP already loaded + started the IMU (it is running — SCRATCH_10 nonzero
     * — which is what made this block writable), but it started BEFORE our golden
     * config, so it stalled at 0x30. Halt the IMU core, program the golden RLC RAM
     * + the RLC bootloader while halted, then restart so it applies them. Do NOT
     * reload the IMU microcode: the PSP put it in I-RAM, which survives the core
     * soft-reset. */
    {
        ULONG cc = gc1_r(ad, regGFX_IMU_CORE_CTRL);
        gc1_w(ad, regGFX_IMU_CORE_CTRL, cc | 1);   /* assert IMU core soft-reset */
        { volatile ULONG d; for (d = 0; d < 200000; d++) ; }
        bug("[amdgpu-gfx] IMU halted for golden re-init (CORE_CTRL 0x%08lx -> 0x%08lx)\n",
            (unsigned long)cc, (unsigned long)gc1_r(ad, regGFX_IMU_CORE_CTRL));
    }

    gfx_v12_program_rlc_ram(ad);

    if (!gfx_v12_build_autoload(ad)) {
        bug("[amdgpu-gfx] GFX unavailable: RLC autoload buffer build failed (firmware missing?)\n");
        return TRUE;
    }
    /* imu_load_microcode intentionally SKIPPED — the PSP loaded IMU_I/IMU_D. */
    if (!gfx_v12_imu_start(ad)) {
        bug("[amdgpu-gfx] GFX unavailable: IMU did not confirm power-up — CP bring-up skipped\n");
        return TRUE;
    }

    grbm_after = gc0_r(ad, regGRBM_STATUS);
    bug("[amdgpu-gfx] GRBM_STATUS after IMU  = 0x%08lx (was 0x%08lx) — %s\n",
        (unsigned long)grbm_after, (unsigned long)grbm_before,
        grbm_after != grbm_before ? "GFX ALIVE" : "still quiet");

    /* CP graphics-ring bring-up now runs on the autoload-success path above
     * (gfx_v12_cp_gfx_ring_bringup). Remaining for radv/3D compute: CP/MEC
     * compute queues (MQD/HQD/KIQ), MES, doorbell aperture, multi-level GPUVM. */
    return TRUE;
}

/* Read-only ME evidence at initialization boundaries. Do not infer that a
 * requested direct-load address reached hardware from the request alone.
 * Callers restore GRBM_GFX_CNTL themselves; this helper never changes it. */
static void gfx_v12_me_snapshot(struct AmdgpuDevice *ad, const char *stage)
{
    bug("[amdgpu-me] %s bank=%08lx CNTL=%08lx IC=%08lx:%08lx IC_CNTL=%08lx "
        "IC_OP=%08lx DC=%08lx:%08lx DC_CNTL=%08lx DC_OP=%08lx PC=%08lx "
        "ME_EXC=%08lx ME_INT=%08lx\n",
        stage, (unsigned long)gc1_r(ad, regGRBM_GFX_CNTL),
        (unsigned long)gc1_r(ad, regCP_ME_CNTL),
        (unsigned long)gc1_r(ad, regCP_ME_IC_BASE_HI),
        (unsigned long)gc1_r(ad, regCP_ME_IC_BASE_LO),
        (unsigned long)gc1_r(ad, regCP_ME_IC_BASE_CNTL),
        (unsigned long)gc1_r(ad, regCP_ME_IC_OP_CNTL),
        (unsigned long)gc1_r(ad, regCP_GFX_RS64_DC_BASE1_HI),
        (unsigned long)gc1_r(ad, regCP_GFX_RS64_DC_BASE1_LO),
        (unsigned long)gc1_r(ad, regCP_GFX_RS64_DC_BASE_CNTL),
        (unsigned long)gc1_r(ad, regCP_GFX_RS64_DC_OP_CNTL),
        (unsigned long)gc1_r(ad, regCP_GFX_RS64_INSTR_PNTR1),
        (unsigned long)gc1_r(ad, regCP_ME_RS64_EXCEPTION_STATUS),
        (unsigned long)gc1_r(ad, regCP_GFX_RS64_INTERRUPT1));
}

/* Program the RS64 CP program-counter start addresses from the firmware headers
 * and pulse the pipe resets so the (PSP-resident) PFP/ME microcode begins
 * fetching. Linux gfx_v12_0_config_gfx_rs64(); PSP-path only. Navi44 = 1 pipe
 * per ME, so only me0/pipe0. MEC deferred to the compute increment. */
static BOOL gfx_v12_config_gfx_rs64(struct AmdgpuDevice *ad)
{
    UBYTE *pfp = NULL, *me = NULL;
    ULONG pfp_len = 0, me_len = 0, lo, hi, cc;

    gfx_v12_me_snapshot(ad, "before-rs64-config");
    if (!amdgpu_request_ip_firmware(ad, AMDGPU_FW_GC, "_pfp", &pfp, &pfp_len) || !pfp) {
        bug("[amdgpu-gfx] config_gfx_rs64: PFP firmware missing\n"); return FALSE; }
    if (!amdgpu_request_ip_firmware(ad, AMDGPU_FW_GC, "_me", &me, &me_len) || !me) {
        bug("[amdgpu-gfx] config_gfx_rs64: ME firmware missing\n");
        amdgpu_release_firmware(pfp); return FALSE; }

    /* Faithful to Linux gfx_v12_0_config_gfx_rs64 (PSP path): set the PFP start
     * addr for BOTH pipes (Linux loops pipe_id<2, selecting each via
     * GRBM_GFX_CNTL[PIPEID]) -> reset PFP pipes -> set ME start for both pipes ->
     * reset ME pipes. Doing pipe0-only with a combined PFP+ME reset (our old code)
     * left pipe1's core to start on unhalt at a garbage PC and wedge the shared
     * ME/MEQ (CP_STAT=PFP|MEQ|ME busy). Order + both-pipes now match Linux exactly. */
    { ULONG pipe;
      lo = le32p(pfp + FWH2_START_LO); hi = le32p(pfp + FWH2_START_HI);
      for (pipe = 0; pipe < 2; pipe++) {
          gc1_w(ad, regGRBM_GFX_CNTL, pipe);      /* soc24_grbm_select(0,pipe,0,0) */
          gc0_w(ad, regCP_PFP_PRGRM_CNTR_START,    (hi << 30) | (lo >> 2));
          gc0_w(ad, regCP_PFP_PRGRM_CNTR_START_HI, hi >> 2);
      }
      gc1_w(ad, regGRBM_GFX_CNTL, 0);
      bug("[amdgpu-gfx] PFP start lo=0x%08lx hi=0x%08lx (both pipes)\n",
          (unsigned long)lo, (unsigned long)hi);

      /* reset PFP pipes 0+1, then clear */
      cc = gc1_r(ad, regCP_ME_CNTL);
      gc1_w(ad, regCP_ME_CNTL, cc | CP_ME_CNTL__PFP_PIPE0_RESET | CP_ME_CNTL__PFP_PIPE1_RESET);
      gc1_w(ad, regCP_ME_CNTL, cc & ~(CP_ME_CNTL__PFP_PIPE0_RESET | CP_ME_CNTL__PFP_PIPE1_RESET));

      lo = le32p(me + FWH2_START_LO); hi = le32p(me + FWH2_START_HI);
      for (pipe = 0; pipe < 2; pipe++) {
          gc1_w(ad, regGRBM_GFX_CNTL, pipe);
          gc0_w(ad, regCP_ME_PRGRM_CNTR_START,    (hi << 30) | (lo >> 2));
          gc0_w(ad, regCP_ME_PRGRM_CNTR_START_HI, hi >> 2);
      }
      gc1_w(ad, regGRBM_GFX_CNTL, 0);
      bug("[amdgpu-gfx] ME  start lo=0x%08lx hi=0x%08lx (both pipes)\n",
          (unsigned long)lo, (unsigned long)hi);

      /* reset ME pipes 0+1, then clear */
      cc = gc1_r(ad, regCP_ME_CNTL);
      gc1_w(ad, regCP_ME_CNTL, cc | CP_ME_CNTL__ME_PIPE0_RESET | CP_ME_CNTL__ME_PIPE1_RESET);
      gc1_w(ad, regCP_ME_CNTL, cc & ~(CP_ME_CNTL__ME_PIPE0_RESET | CP_ME_CNTL__ME_PIPE1_RESET));

      /* Experimental ME DIRECT-LOAD (gfx_v12_0_cp_gfx_load_me_microcode_rs64).
       * The earlier EXE_DISABLE explanation was a hypothesis, not a demonstrated
       * cause. Immediate requested/readback base comparisons below distinguish
       * rejected writes from later restoration of PSP's firmware bases. Load me.bin's
       * ucode+data into GART, point CP_ME_IC_BASE at it, set CNTL (VMID=0, EXE_DISABLE=0
       * — writing IC_BASE triggers an auto-invalidate), wait INVALIDATE_CACHE_COMPLETE,
       * PRIME, wait ICACHE_PRIMED, then set the ME DC base. ME is halted here. */
      { static ULONG *me_ic; static ULONG *me_dc; UQUAD me_ic_va = 0, me_dc_va = 0;
        ULONG uoff = le32p(me + GFXH2_UCODE_OFF), usz = le32p(me + GFXH2_UCODE_SIZE);
        ULONG doff = le32p(me + GFXH2_DATA_OFF),  dsz = le32p(me + GFXH2_DATA_SIZE);
        ULONG t, ll;
        if (!me_ic && ad->gart_ready && (UQUAD)uoff + usz <= me_len && (UQUAD)doff + dsz <= me_len) {
            me_ic = AllocMem(usz, MEMF_ANY | MEMF_CLEAR);
            me_dc = AllocMem(dsz, MEMF_ANY | MEMF_CLEAR);
            if (me_ic && me_dc) {
                CopyMem(me + uoff, me_ic, usz);
                CopyMem(me + doff, me_dc, dsz);
                ll = usz; CachePreDMA(me_ic, &ll, DMA_ReadFromRAM);
                ll = dsz; CachePreDMA(me_dc, &ll, DMA_ReadFromRAM);
                me_ic_va = amdgpu_gart_map(ad, me_ic, usz);
                me_dc_va = amdgpu_gart_map(ad, me_dc, dsz);
            }
        }
        if (me_ic_va && me_dc_va) {
            ULONG mecc = gc1_r(ad, regCP_ME_CNTL);
            /* Set CP_ME_CNTL.ME_INVALIDATE_ICACHE (bit8) around the prime — the MES
             * prime only took with its CNTL-level INVALIDATE_ICACHE set; the ME needs
             * the same. Keep HALT; clear the invalidate after PRIMED so the later CP
             * unhalt can run the ME. */
            gc1_w(ad, regCP_ME_CNTL, mecc | CP_ME_CNTL__ME_INVALIDATE_ICACHE);
            gc1_w(ad, regCP_ME_IC_BASE_LO, (ULONG)(me_ic_va & 0xFFFFFFFFUL));
            gc1_w(ad, regCP_ME_IC_BASE_HI, (ULONG)(me_ic_va >> 32));
            gc1_w(ad, regCP_ME_IC_BASE_CNTL, 0);   /* VMID=0 CACHE_POLICY=0 EXE_DISABLE=0 -> triggers invalidate */
            bug("[amdgpu-me] IC request=%08lx:%08lx low-mask-loss=%08lx\n",
                (unsigned long)(ULONG)(me_ic_va >> 32), (unsigned long)(ULONG)me_ic_va,
                (unsigned long)((ULONG)me_ic_va & 0xfffUL));
            gfx_v12_me_snapshot(ad, "immediate-after-IC-write");
            for (t = 0; t < 50000; t++)
                if (gc1_r(ad, regCP_ME_IC_OP_CNTL) & CP_ME_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE) break;
            gc1_w(ad, regCP_ME_IC_OP_CNTL,
                  gc1_r(ad, regCP_ME_IC_OP_CNTL) | CP_ME_IC_OP_CNTL__PRIME_ICACHE);
            for (t = 0; t < 50000; t++)
                if (gc1_r(ad, regCP_ME_IC_OP_CNTL) & CP_ME_IC_OP_CNTL__ICACHE_PRIMED) break;
            gc1_w(ad, regCP_ME_CNTL, mecc & ~CP_ME_CNTL__ME_INVALIDATE_ICACHE);  /* clear invalidate, keep HALT */
            gc1_w(ad, regCP_GFX_RS64_DC_BASE1_LO, (ULONG)(me_dc_va & 0xFFFFFFFFUL));
            gc1_w(ad, regCP_GFX_RS64_DC_BASE1_HI, (ULONG)(me_dc_va >> 32));
            gc1_w(ad, regCP_GFX_RS64_DC_BASE_CNTL, 0);
            bug("[amdgpu-me] DC request=%08lx:%08lx low-mask-loss=%08lx\n",
                (unsigned long)(ULONG)(me_dc_va >> 32), (unsigned long)(ULONG)me_dc_va,
                (unsigned long)((ULONG)me_dc_va & 0xffffUL));
            gfx_v12_me_snapshot(ad, "immediate-after-DC-write");
            gc1_w(ad, regCP_GFX_RS64_DC_OP_CNTL,
                  gc1_r(ad, regCP_GFX_RS64_DC_OP_CNTL) | CP_GFX_RS64_DC_OP_CNTL__INVALIDATE_DCACHE);
            for (t = 0; t < 50000; t++)
                if (gc1_r(ad, regCP_GFX_RS64_DC_OP_CNTL) & CP_GFX_RS64_DC_OP_CNTL__INVALIDATE_DCACHE_COMPLETE) break;
        }
        bug("[amdgpu-gfx] ME DIRECT-LOAD: ic_va=0x%08lx%08lx dc_va=0x%08lx%08lx uc=%lu dt=%lu "
            "IC_OP=0x%08lx (PRIMED=%lu) IC_BASE_CNTL=0x%08lx\n",
            (unsigned long)(ULONG)(me_ic_va >> 32), (unsigned long)(ULONG)me_ic_va,
            (unsigned long)(ULONG)(me_dc_va >> 32), (unsigned long)(ULONG)me_dc_va,
            (unsigned long)usz, (unsigned long)dsz,
            (unsigned long)gc1_r(ad, regCP_ME_IC_OP_CNTL),
            (unsigned long)((gc1_r(ad, regCP_ME_IC_OP_CNTL) & CP_ME_IC_OP_CNTL__ICACHE_PRIMED) ? 1 : 0),
            (unsigned long)gc1_r(ad, regCP_ME_IC_BASE_CNTL));
      }
    }

    amdgpu_release_firmware(pfp); amdgpu_release_firmware(me);
    gfx_v12_me_snapshot(ad, "after-rs64-config");
    return TRUE;
}

/* MEC (compute microengine) RS64 bring-up — Linux gfx_v12_0 config_gfx_rs64 MEC
 * portion + cp_compute_enable. AROS had this ENTIRELY missing (config_gfx_rs64
 * comment: "MEC deferred"); no CP_MEC_CNTL / CP_MEC_RS64 programming existed, so the
 * compute front-end that the KIQ path leans on was never started. PSP autoloads
 * gc_12_0_0_mec.bin (FWT_RS64_MEC), so this is config + unhalt only (minimal, per
 * codex's audit): set PRGRM_CNTR_START (mec.bin start addr) on all 4 pipes, pulse
 * pipe reset, then MEC_RS64_CNTL = clear INVALIDATE + clear RESET + set PIPE0-3
 * ACTIVE + clear HALT. Read CP_MEC_RS64_INSTR_PNTR before/after: a moving PC proves
 * the MEC core executes. */
BOOL gfx_v12_mec_bringup(struct AmdgpuDevice *ad)
{
    static ULONG s_lo, s_hi;
    static BOOL  s_fw_ok;      /* cache the fw start addr: this is called in a retry loop */
    UBYTE *mec = NULL;
    ULONG mec_len = 0, lo, hi, pipe, data, pc_pre, pc_post;

    if (!s_fw_ok) {
        if (!amdgpu_request_ip_firmware(ad, AMDGPU_FW_GC, "_mec", &mec, &mec_len) || !mec) {
            bug("[amdgpu-gfx] mec-bringup: MEC firmware missing\n"); return FALSE; }
        s_lo = le32p(mec + FWH2_START_LO); s_hi = le32p(mec + FWH2_START_HI);
        amdgpu_release_firmware(mec);
        s_fw_ok = TRUE;
    }
    lo = s_lo; hi = s_hi;

    gc1_w(ad, regGRBM_GFX_CNTL, (1UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0); /* me=1,pipe0 */
    pc_pre = gc1_r(ad, regCP_MEC_RS64_INSTR_PNTR);

    /* config MEC program start addr on all 4 pipes (soc24_grbm_select(1,pipe,0,0)) */
    for (pipe = 0; pipe < 4; pipe++) {
        gc1_w(ad, regGRBM_GFX_CNTL, (1UL << GRBM_GFX_CNTL_MEID_SHIFT) | pipe);
        gc1_w(ad, regCP_MEC_RS64_PRGRM_CNTR_START,    (hi << 30) | (lo >> 2));
        gc1_w(ad, regCP_MEC_RS64_PRGRM_CNTR_START_HI, hi >> 2);
    }
    gc1_w(ad, regGRBM_GFX_CNTL, 0);

    /* pulse MEC pipe reset (all 4), then clear */
    data = gc1_r(ad, regCP_MEC_RS64_CNTL);
    gc1_w(ad, regCP_MEC_RS64_CNTL, data | CP_MEC_RS64_CNTL__MEC_PIPE_RESET_ALL);
    gc1_w(ad, regCP_MEC_RS64_CNTL, data & ~CP_MEC_RS64_CNTL__MEC_PIPE_RESET_ALL);

    /* cp_compute_enable(true): clear INVALIDATE_ICACHE + RESET, set all ACTIVE, clear HALT */
    data = gc1_r(ad, regCP_MEC_RS64_CNTL);
    data &= ~(CP_MEC_RS64_CNTL__MEC_INVALIDATE_ICACHE
            | CP_MEC_RS64_CNTL__MEC_PIPE_RESET_ALL
            | CP_MEC_RS64_CNTL__MEC_HALT);
    data |= CP_MEC_RS64_CNTL__MEC_PIPE_ACTIVE_ALL;
    gc1_w(ad, regCP_MEC_RS64_CNTL, data);
    { volatile ULONG d; for (d = 0; d < 200000; d++) ; }   /* ~udelay(50) */

    gc1_w(ad, regGRBM_GFX_CNTL, (1UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0); /* pipe0 */
    { volatile ULONG d; for (d = 0; d < 500000; d++) ; }
    pc_post = gc1_r(ad, regCP_MEC_RS64_INSTR_PNTR);
    data = gc1_r(ad, regCP_MEC_RS64_CNTL);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    bug("[amdgpu-gfx] *** MEC BRINGUP: start lo=0x%08lx hi=0x%08lx CP_MEC_RS64_CNTL=0x%08lx "
        "INSTR_PNTR pipe0 %08lx->%08lx -> %s ***\n",
        (unsigned long)lo, (unsigned long)hi, (unsigned long)data,
        (unsigned long)pc_pre, (unsigned long)pc_post,
        (pc_post != pc_pre && pc_post != 0 && pc_post != 0xFFFFFFFFUL)
            ? "MEC ENGINE RUNS (pipe0 PC moving)"
            : "MEC NOT running (pipe0 PC stuck)");
    return TRUE;
}

/* Direct-load one MES pipe's ucode + data into GART and point its instruction/data
 * cache bases at them, then prime the icache (Linux mes_v12_0.c:1405-1440 direct
 * path). PSP autoload brings up pipe0 (scheduler) but NOT pipe1 (KIQ) — measured:
 * pipe1 freezes at reset PC 0x4444 with no IC base, so the KIQ ring is never
 * serviced. Giving pipe1 a valid IC/MD base lets it fetch + execute. Leaves grbm
 * selected at (me=3,pipe) for the caller. Returns firmware uc_start (0 = failed). */
static UQUAD mes_v12_direct_load_pipe(struct AmdgpuDevice *ad, ULONG pipe, CONST_STRPTR fwsuffix, BOOL prime, BOOL load_ic)
{
    UBYTE *fw = NULL;
    ULONG fw_len = 0, uc_off, uc_sz, dt_off, dt_sz, l;
    UQUAD uc_start, ic_va, md_va;
    APTR icbuf, mdbuf;
    char fwname[64];

    if (!amdgpu_fw_name(ad, AMDGPU_FW_GC, (const char *)fwsuffix, fwname, sizeof(fwname)))
        return 0;
    if (!amdgpu_request_firmware(fwname, &fw, &fw_len) || !fw) {
        bug("[amdgpu-mes] direct-load: %s missing\n", fwname); return 0; }
    uc_off = le32p(fw + MESH1_UCODE_OFF); uc_sz = le32p(fw + MESH1_UCODE_SIZE);
    dt_off = le32p(fw + MESH1_DATA_OFF);  dt_sz = le32p(fw + MESH1_DATA_SIZE);
    uc_start = (UQUAD)le32p(fw + MESH1_START_LO) | ((UQUAD)le32p(fw + MESH1_START_HI) << 32);
    if ((UQUAD)uc_off + uc_sz > fw_len || (UQUAD)dt_off + dt_sz > fw_len) {
        bug("[amdgpu-mes] direct-load: %s header out of range\n", fwname);
        amdgpu_release_firmware(fw); return 0; }

    /* PSP-LOADED PATH (matches Linux): the Linux register trace on this exact card
     * proves Linux NEVER writes CP_MES_IC_BASE / IC_OP — the MES icache is loaded
     * entirely by the PSP autoload. Our manual GART-load + IC_BASE + prime is a
     * divergence whose primed icache does NOT survive the enable's reset re-assert,
     * stranding the KIQ at 0x4444. When !load_ic we only read the header for uc_start
     * and let the PSP-loaded ucode stand. */
    if (!load_ic) {
        amdgpu_release_firmware(fw);
        bug("[amdgpu-mes] pipe%lu %s: PSP-loaded (no manual icache), uc_start=0x%08lx%08lx\n",
            (unsigned long)pipe, fwname,
            (unsigned long)(ULONG)(uc_start >> 32), (unsigned long)(ULONG)uc_start);
        return uc_start;
    }

    icbuf = AllocMem(uc_sz, MEMF_ANY | MEMF_CLEAR);
    mdbuf = AllocMem(dt_sz, MEMF_ANY | MEMF_CLEAR);
    if (!icbuf || !mdbuf) {
        bug("[amdgpu-mes] direct-load: %s alloc failed\n", fwname);
        if (icbuf) FreeMem(icbuf, uc_sz);
        if (mdbuf) FreeMem(mdbuf, dt_sz);
        amdgpu_release_firmware(fw); return 0; }
    CopyMem(fw + uc_off, icbuf, uc_sz);
    CopyMem(fw + dt_off, mdbuf, dt_sz);
    amdgpu_release_firmware(fw);
    { l = uc_sz; CachePreDMA(icbuf, &l, DMA_ReadFromRAM); }
    { l = dt_sz; CachePreDMA(mdbuf, &l, DMA_ReadFromRAM); }

    ic_va = amdgpu_gart_map(ad, icbuf, uc_sz);
    md_va = amdgpu_gart_map(ad, mdbuf, dt_sz);
    if (!ic_va || !md_va) {
        bug("[amdgpu-mes] direct-load: %s GART map failed\n", fwname); return 0; }

    gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | pipe);
    gc1_w(ad, regCP_MES_IC_BASE_CNTL, 0);
    gc1_w(ad, regCP_MES_IC_BASE_LO, (ULONG)(ic_va & 0xFFFFFFFFUL));
    gc1_w(ad, regCP_MES_IC_BASE_HI, (ULONG)(ic_va >> 32));
    gc1_w(ad, regCP_MES_MIBOUND_LO, 0x1FFFFFUL);
    gc1_w(ad, regCP_MES_MDBASE_LO, (ULONG)(md_va & 0xFFFFFFFFUL));
    gc1_w(ad, regCP_MES_MDBASE_HI, (ULONG)(md_va >> 32));
    gc1_w(ad, regCP_MES_MDBOUND_LO, 0x7FFFFUL);
    /* invalidate then prime the icache, then WAIT for ICACHE_PRIMED (bit5). The prime
     * only takes with the MES halted (the caller disables it first); without the wait
     * the pipe unhalts before its icache is loaded and freezes at reset PC 0x4444.
     * CRITICAL: Linux mes_v12_0_kiq_hw_init primes ONLY the KIQ pipe (load_microcode
     * SCHED prime=false, KIQ prime=true). Priming BOTH corrupts the shared MES icache
     * state and leaves the KIQ at 0x4444; the sched pipe fetches on-demand from its own
     * IC_BASE and needs no explicit prime. So gate the prime on `prime`. */
    if (prime) {
        gc1_w(ad, regCP_MES_IC_OP_CNTL,
              (gc1_r(ad, regCP_MES_IC_OP_CNTL) & ~CP_MES_IC_OP_CNTL__PRIME_ICACHE)
              | CP_MES_IC_OP_CNTL__INVALIDATE_CACHE);
        gc1_w(ad, regCP_MES_IC_OP_CNTL,
              gc1_r(ad, regCP_MES_IC_OP_CNTL) | CP_MES_IC_OP_CNTL__PRIME_ICACHE);
        { ULONG t; for (t = 0; t < 50000; t++)
              if (gc1_r(ad, regCP_MES_IC_OP_CNTL) & CP_MES_IC_OP_CNTL__ICACHE_PRIMED) break; }
    }
    bug("[amdgpu-mes] direct-load pipe%lu %s: ic_va=0x%08lx%08lx md_va=0x%08lx%08lx "
        "uc=%lu dt=%lu IC_OP=0x%08lx PRIMED=%lu\n", (unsigned long)pipe, fwname,
        (unsigned long)(ULONG)(ic_va >> 32), (unsigned long)(ULONG)ic_va,
        (unsigned long)(ULONG)(md_va >> 32), (unsigned long)(ULONG)md_va,
        (unsigned long)uc_sz, (unsigned long)dt_sz,
        (unsigned long)gc1_r(ad, regCP_MES_IC_OP_CNTL),
        (unsigned long)((gc1_r(ad, regCP_MES_IC_OP_CNTL) & CP_MES_IC_OP_CNTL__ICACHE_PRIMED) ? 1 : 0));
    return uc_start;
}

/* Milestone 1 of the MES bring-up: enable the MES microengine (me=3) and verify it
 * runs. On GFX12 the graphics ME dispatches ONLY from a MES-mapped queue (proven:
 * manual CP_GFX_HQD_ACTIVE=1 is insufficient), so MES is mandatory. PSP already
 * autoloads the MES ucode (uni_mes.bin staged as CP_MES/CP_MES1). Mirror Linux
 * mes_v12_0_enable: per pipe (0=sched,1=kiq) grbm_select(me=3,pipe), pulse
 * MES_PIPEn_RESET, set PRGRM_CNTR_START = uc_start_addr>>2, then write
 * CP_MES_CNTL = MES_PIPEn_ACTIVE (clears reset+halt, starts the engine). Read
 * CP_MES_INSTR_PNTR (pipe0) before/after: if the PC moves, the MES engine executes
 * its firmware — the foundation for KIQ + scheduler + ADD_QUEUE. */
/* MES event-log buffer (diagnostic): CP_MES_MSCRATCH points the MES at a scratch
 * region; the MES ucode writes its event log at (mscratch - LOG_BUFFER_SIZE), i.e.
 * the buffer base. Set per pipe in mes_v12_enable, dumped in scheduler_bringup to
 * SEE what the MES/KIQ ucode actually does (vs inferring from registers). */
#define MES_LOG_BUFFER_SIZE  0x4000UL     /* AMDGPU_MES_LOG_BUFFER_SIZE */
#define MES_MSCRATCH_SIZE    0x40000UL    /* AMDGPU_MES_MSCRATCH_SIZE */
static volatile ULONG *mes_log_buf;       /* CPU ptr to log region (buffer base) */
static UQUAD           mes_log_va;        /* GART VA of buffer base */

BOOL mes_v12_enable(struct AmdgpuDevice *ad)
{
    ULONG pipe, p_pre, p_post, data;
    UQUAD uc[2];

    /* Allocate + GART-map the MES event-log/scratch buffer once. Layout mirrors Linux:
     * [0 .. LOG_BUFFER_SIZE) = event log (what we dump); [LOG_BUFFER_SIZE ..] = scratch
     * that CP_MES_MSCRATCH points at. */
    if (!mes_log_buf) {
        APTR b = AllocMem(MES_LOG_BUFFER_SIZE + MES_MSCRATCH_SIZE, MEMF_ANY | MEMF_CLEAR);
        if (b) {
            UQUAD va = amdgpu_gart_map(ad, b, MES_LOG_BUFFER_SIZE + MES_MSCRATCH_SIZE);
            if (va) { mes_log_buf = (volatile ULONG *)b; mes_log_va = va; }
            else FreeMem(b, MES_LOG_BUFFER_SIZE + MES_MSCRATCH_SIZE);
        }
    }

    /* 0. Tell the RLC which queue is the KIQ (Linux mes_v12_0_kiq_setting — the FIRST
     * step of kiq_hw_init). Without it the CP/RLC scheduler never services the KIQ, so
     * the KIQ runs with wptr set but rptr never advances. me=3,pipe=1,queue=0 for the
     * KIQ: (me<<5)|(pipe<<3)|queue = 0x68, |0x80 (valid). gc1. */
    {
        ULONG sch = gc1_r(ad, regRLC_CP_SCHEDULERS) & 0xFFFFFF00UL;
        sch |= (3UL << 5) | (1UL << 3) | 0UL;   /* me=3, pipe=1, queue=0 */
        gc1_w(ad, regRLC_CP_SCHEDULERS, sch | 0x80UL);
        bug("[amdgpu-mes] RLC_CP_SCHEDULERS <- 0x%08lx (KIQ me3/pipe1) readback=0x%08lx\n",
            (unsigned long)(sch | 0x80UL), (unsigned long)gc1_r(ad, regRLC_CP_SCHEDULERS));
    }

    /* FULL DIRECT-LOAD (Linux mes_v12_0 direct path order), replacing PSP's partial
     * bring-up which wakes pipe0 (scheduler) but never gives pipe1 (KIQ) an IC base.
     * 1. DISABLE the whole MES first (halt both pipes) — the icache prime only takes
     *    with the engine halted; that was why the hybrid (pipe1 reset only) never
     *    primed (IC_OP stayed 0) and pipe1 froze at 0x4444. */
    gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
    data = gc1_r(ad, regCP_MES_CNTL);
    data &= ~(CP_MES_CNTL__MES_PIPE0_ACTIVE | CP_MES_CNTL__MES_PIPE1_ACTIVE);
    data |= CP_MES_CNTL__MES_INVALIDATE_ICACHE
          | CP_MES_CNTL__MES_PIPE0_RESET | CP_MES_CNTL__MES_PIPE1_RESET
          | CP_MES_CNTL__MES_HALT;
    gc1_w(ad, regCP_MES_CNTL, data);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    { volatile ULONG d; for (d = 0; d < 200000; d++) ; }

    /* 2. Direct-load BOTH pipes' ucode/data + IC/MD base + prime (waits ICACHE_PRIMED)
     *    while the MES is halted. pipe0=mes.bin (scheduler), pipe1=mes1.bin (KIQ). */
    /* load_ic=FALSE — do NOT touch the MES icache at all (pure Linux match). PROVEN this
     * session: our PSP autoload's LOAD_IP_FW for MES(type33) AND MES1/KIQ(type81) both
     * return status=0 with TMR addrs 0x83f861d000/0x83f869d000, and the pipe IC_BASE
     * readbacks (pipe0=0xf861d000, pipe1=0x3f869d000) MATCH those TMR addrs — so the PSP
     * loads BOTH MES pipes and our manual direct-load IC_BASE writes were ineffective
     * (PSP-protected) AND its INVALIDATE/prime can only corrupt the PSP-loaded ucode.
     * We only read the header for each pipe's uc_start to program PRGRM_CNTR. */
    uc[0] = mes_v12_direct_load_pipe(ad, 0, (CONST_STRPTR)"_mes",  FALSE, FALSE);
    uc[1] = mes_v12_direct_load_pipe(ad, 1, (CONST_STRPTR)"_mes1", FALSE, FALSE);
    if (!uc[0] || !uc[1]) {
        bug("[amdgpu-mes] direct-load failed (uc0=%d uc1=%d) — MES enable aborted\n",
            uc[0] ? 1 : 0, uc[1] ? 1 : 0);
        return FALSE;
    }

    /* 3. mes_v12_0_enable(true), EXACT per the Linux register trace on this card:
     *    per pipe — select, assert THIS pipe's RESET (preserving other bits), set its
     *    PRGRM_CNTR (uc_start>>2), then CP_MES_CNTL from 0 with cumulative ACTIVE.
     *    The trace showed: sel 0xc; CNTL=0x40010000; PRGRM=0xfc001400/0x3fffffff;
     *    CNTL=0x04000000; sel 0xd; CNTL=0x04020000; PRGRM; CNTL=0x0c000000; sel 0.
     *    This reset-reassert is SAFE now that the icache is PSP-loaded (it was our
     *    manual-primed icache that couldn't survive the reset and stranded the KIQ at
     *    0x4444). MSCRATCH (event log) set per pipe for diagnostics. */
    p_pre = gc1_r(ad, regCP_MES_INSTR_PNTR);
    for (pipe = 0; pipe < 2; pipe++) {
        UQUAD ucode_addr = uc[pipe] >> 2;
        gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | pipe);
        data = gc1_r(ad, regCP_MES_CNTL);
        data |= pipe ? CP_MES_CNTL__MES_PIPE1_RESET : CP_MES_CNTL__MES_PIPE0_RESET;
        gc1_w(ad, regCP_MES_CNTL, data);
        gc1_w(ad, regCP_MES_PRGRM_CNTR_START,    (ULONG)(ucode_addr & 0xFFFFFFFFUL));
        gc1_w(ad, regCP_MES_PRGRM_CNTR_START_HI, (ULONG)(ucode_addr >> 32));
        /* CP_MES_MSCRATCH (regCP_MES_MSCRATCH_LO/HI @ gc[1]+0x2814/0x2815 = dword 0xc814)
         * is the MES core's RISC-V mscratch CSR — the firmware uses it as its
         * scheduler-context BASE pointer. The SET_HW_RESOURCES handler reads it at
         * (1<<48)+0x32050 and, from base, builds+walks a hash table at base+0x3000
         * (memset 0x2b0e0 = 176KB). The 0x64d9 spin = that walk over a garbage table.
         *
         * KEY FINDING (codex TRIG-0520 + RS64 disasm + Linux verify): stock Linux does
         * NOT write MSCRATCH — mes_v12_0_enable gates the write on amdgpu_mes_log_enable
         * which DEFAULTS TO 0, so the firmware SELF-INITIALISES mscratch to its correctly
         * sized internal region during PSP autoload. AROS wrote it UNCONDITIONALLY to
         * mes_log_va+MES_LOG_BUFFER_SIZE — pointing the scheduler base at our event-log
         * GART region, which is NOT the 0x2b0e0-byte control block the fw expects, so the
         * hash walk searches garbage -> deterministic 0x64d9 spin. Match default Linux:
         * do NOT force mscratch; let the fw self-init it. (Re-enable only alongside a
         * properly sized MES_MSCRATCH_SIZE=0x40000 region if event logging is wanted.) */
        data = CP_MES_CNTL__MES_PIPE0_ACTIVE;              /* start from 0: clears RESET+HALT */
        if (pipe) data |= CP_MES_CNTL__MES_PIPE1_ACTIVE;
        gc1_w(ad, regCP_MES_CNTL, data);
        /* Readback the fw-self-initialised mscratch after ACTIVE (per-pipe) — codex's
         * first-class capture point for the scheduler base. */
        {
            ULONG mlo = gc1_r(ad, regCP_MES_MSCRATCH_LO);
            ULONG mhi = gc1_r(ad, regCP_MES_MSCRATCH_HI);
            bug("[amdgpu-mes] MSCRATCH pipe%lu after ACTIVE = 0x%08lx%08lx (fw self-init; "
                "we no longer force it — was mes_log_va+LOG=0x%08lx%08lx)\n",
                (unsigned long)pipe, (unsigned long)mhi, (unsigned long)mlo,
                (unsigned long)((mes_log_va + MES_LOG_BUFFER_SIZE) >> 32),
                (unsigned long)((mes_log_va + MES_LOG_BUFFER_SIZE) & 0xFFFFFFFFUL));
        }
    }
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    { volatile ULONG d; for (d = 0; d < 200000; d++) ; }   /* ~udelay(50) */

    gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);  /* select pipe0 */
    { volatile ULONG d; for (d = 0; d < 500000; d++) ; }
    {
        ULONG cntl, p0_a, p0_b, p1_a, p1_b;
        /* LIVENESS: sample EACH pipe's PC twice with a delay. A MOVING PC proves the
         * core is actively executing. A STATIC non-zero PC is AMBIGUOUS — it can be a
         * healthy idle-halt (MES waiting on the packet-fifo interrupt) OR a genuine
         * stall — so we report the raw before/after and "moving" vs "static", and do
         * NOT over-claim "RUNS" from a single sample (that verdict was misleading me). */
        p0_a = gc1_r(ad, regCP_MES_INSTR_PNTR);
        { volatile ULONG d; for (d = 0; d < 800000; d++) ; }
        p0_b = gc1_r(ad, regCP_MES_INSTR_PNTR);
        p_post = p0_b;
        cntl = gc1_r(ad, regCP_MES_CNTL);
        gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 1);
        p1_a = gc1_r(ad, regCP_MES_INSTR_PNTR);
        { volatile ULONG d; for (d = 0; d < 800000; d++) ; }
        p1_b = gc1_r(ad, regCP_MES_INSTR_PNTR);
        gc1_w(ad, regGRBM_GFX_CNTL, 0);
        (void)p_pre;
        bug("[amdgpu-mes] *** MES ENABLE: uc0=0x%08lx%08lx uc1=0x%08lx%08lx CP_MES_CNTL=0x%08lx ***\n",
            (unsigned long)(ULONG)(uc[0] >> 32), (unsigned long)(ULONG)uc[0],
            (unsigned long)(ULONG)(uc[1] >> 32), (unsigned long)(ULONG)uc[1],
            (unsigned long)cntl);
        bug("[amdgpu-mes]   pipe0(sched) PC %08lx->%08lx = %s | pipe1(kiq) PC %08lx->%08lx = %s\n",
            (unsigned long)p0_a, (unsigned long)p0_b,
            (p0_b != p0_a) ? "MOVING (executing)" : "static (idle-halt or stall — ambiguous)",
            (unsigned long)p1_a, (unsigned long)p1_b,
            (p1_b != p1_a) ? "MOVING (executing)" : "static (idle-halt or stall — ambiguous)");

        /* Per-pipe MES state (both pipes; NOT assuming either is faulted). EXC/MSTATUS
         * expose any RS64 exception; IC_BASE/IC_OP show the PSP-loaded icache base. */
        {
            ULONG p1_exc, p1_ms_lo, p1_ic_lo, p1_ic_hi, p1_icop;
            ULONG p0_exc, p0_ic_lo, p0_icop;
            gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 1);
            p1_exc   = gc1_r(ad, regCP_MES_RS64_EXCEPTION_STATUS);
            p1_ms_lo = gc1_r(ad, regCP_MES_MSTATUS_LO);
            p1_ic_lo = gc1_r(ad, regCP_MES_IC_BASE_LO);
            p1_ic_hi = gc1_r(ad, regCP_MES_IC_BASE_HI);
            p1_icop  = gc1_r(ad, regCP_MES_IC_OP_CNTL);
            gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
            p0_exc   = gc1_r(ad, regCP_MES_RS64_EXCEPTION_STATUS);
            p0_ic_lo = gc1_r(ad, regCP_MES_IC_BASE_LO);
            p0_icop  = gc1_r(ad, regCP_MES_IC_OP_CNTL);
            gc1_w(ad, regGRBM_GFX_CNTL, 0);
            bug("[amdgpu-mes] MES STATE: pipe1 EXC=0x%08lx MSTATUS_LO=0x%08lx "
                "IC_BASE=0x%08lx%08lx IC_OP=0x%08lx || pipe0 EXC=0x%08lx IC_BASE_LO=0x%08lx IC_OP=0x%08lx\n",
                (unsigned long)p1_exc, (unsigned long)p1_ms_lo,
                (unsigned long)p1_ic_hi, (unsigned long)p1_ic_lo, (unsigned long)p1_icop,
                (unsigned long)p0_exc, (unsigned long)p0_ic_lo, (unsigned long)p0_icop);
        }
    }
    return TRUE;
}

/* Build a v12_compute_mqd (the MES scheduler + KIQ both use it). db_off = the
 * doorbell INDEX (already <<1); DOORBELL_OFFSET field = index<<2. */
static void mes_build_mqd(volatile ULONG *m, UQUAD ring_va, UQUAD rptr_va,
                          UQUAD wptr_va, UQUAD eop_va, UQUAD mqd_va, ULONG db_off)
{
    ULONG i;
    for (i = 0; i < 1024; i++) m[i] = 0;
    m[0]   = 0xC0310800UL;                                    /* header */
    m[11]  = 0x00000001UL;                                    /* pipelinestat_enable */
    m[23]  = m[24] = m[26] = m[27] = 0xFFFFFFFFUL;            /* static_thread_mgmt se0-3 */
    m[32]  = 0x00000007UL;                                    /* misc_reserved */
    m[128] = (ULONG)(mqd_va & 0xFFFFFFFCUL);                 /* cp_mqd_base_addr_lo */
    m[129] = (ULONG)(mqd_va >> 32);
    m[130] = 0x00000001UL;                                    /* cp_hqd_active */
    m[131] = 0;                                               /* vmid */
    m[132] = 0x0BE05501UL;                                    /* persistent_state */
    m[135] = 0;                                               /* quantum */
    m[136] = (ULONG)((ring_va >> 8) & 0xFFFFFFFFUL);         /* pq_base_lo */
    m[137] = (ULONG)(ring_va >> 40);                          /* pq_base_hi */
    m[138] = 0;                                               /* pq_rptr */
    m[139] = (ULONG)(rptr_va & 0xFFFFFFFCUL);                /* rptr_report_addr_lo */
    m[140] = (ULONG)((rptr_va >> 32) & 0xFFFFUL);
    m[141] = (ULONG)(wptr_va & 0xFFFFFFF8UL);                /* wptr_poll_addr_lo */
    m[142] = (ULONG)((wptr_va >> 32) & 0xFFFFUL);
    m[143] = 0x40000000UL | (db_off << 2);                   /* doorbell_control: EN | OFFSET */
    m[145] = 0xD8308009UL;                                    /* pq_control */
    m[149] = 0x00300000UL;                                    /* ib_control */
    m[150] = 0;
    m[162] = 0x00000100UL;                                    /* mqd_control */
    m[165] = (ULONG)((eop_va >> 8) & 0xFFFFFFFFUL);          /* eop_base_addr_lo */
    m[166] = (ULONG)(eop_va >> 40);
    m[167] = 0x00000008UL;                                    /* eop_control (EOP_SIZE=8) */
    m[182] = m[183] = 0;                                      /* wptr_lo/hi */
    m[184] = 0x00008000UL;                                    /* DB_UPDATED_MSG_EN */
}

/* MMIO-install an MQD into the live HQD (mes_v12_0_queue_init_register) for a MES
 * pipe (me=3). Used for the KIQ (pipe1, the upstream-tested path). */
static void mes_activate_hqd(struct AmdgpuDevice *ad, ULONG pipe, volatile ULONG *m)
{
    gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | pipe);
    gc0_w(ad, regCP_HQD_VMID, 0);
    gc0_w(ad, regCP_HQD_PQ_DOORBELL_CONTROL, 0);
    gc0_w(ad, regCP_MQD_BASE_ADDR,    m[128]);
    gc0_w(ad, regCP_MQD_BASE_ADDR_HI, m[129]);
    gc0_w(ad, regCP_MQD_CONTROL_HQD, 0);
    gc0_w(ad, regCP_HQD_PQ_BASE,    m[136]);
    gc0_w(ad, regCP_HQD_PQ_BASE_HI, m[137]);
    gc0_w(ad, regCP_HQD_PQ_RPTR_REPORT_ADDR,    m[139]);
    gc0_w(ad, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI, m[140]);
    gc0_w(ad, regCP_HQD_PQ_CONTROL, m[145]);
    gc0_w(ad, regCP_HQD_PQ_WPTR_POLL_ADDR,    m[141]);
    gc0_w(ad, regCP_HQD_PQ_WPTR_POLL_ADDR_HI, m[142]);
    gc0_w(ad, regCP_HQD_PQ_DOORBELL_CONTROL, m[143]);
    gc0_w(ad, regCP_HQD_PERSISTENT_STATE, m[132]);
    gc0_w(ad, regCP_HQD_ACTIVE, 1);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
}

/* ------------------------------------------------------------------------
 * MEC COMPUTE-QUEUE DISPATCH TEST — the doorbell-less / MES-less path.
 *
 * radv needs only a compute ring/queue, NOT its own GFX bring-up (amdgpu.hidd
 * already powers GFX + runs the MEC). Compute queues live on the MEC (me=1),
 * which is RUNNING, and — like our working SDMA ring — can take their write
 * pointer via MMIO (CP_HQD_PQ_WPTR register), needing NO MES scheduler and NO
 * doorbell. This installs a compute HQD on me=1/pipe0, writes a PM4 WRITE_DATA
 * fence into its ring, bumps the wptr via MMIO, and polls the fence. If the
 * fence value lands, the MEC executed a packet from a directly-installed compute
 * queue — i.e. radv has a live dispatch path and the whole MES doorbell saga was
 * a detour. GART VAs throughout (the MEC fetches via GFXHUB VMID0, same as the
 * gfx CP), so ring/rptr/fence all sit in one GART-mapped system-RAM buffer. */
/* ------------------------------------------------------------------------
 * PERSISTENT MEC COMPUTE BUFFER — experimental reusable dispatch primitive.
 *
 * One 64KiB GART buffer holds the ring + rptr/wptr writeback + EOP + MQD + a fence
 * scratch. Ensure allocates the buffer; each serialized submit drains the old
 * HQD, rebuilds the ring at offset zero, and installs it with work present.
 * The two-fence self-test below must pass before claiming reusable submission.
 * A successful first shader dispatch does not prove this separate path. */
static ULONG *g_cq_buf;
static APTR   g_cq_allocation;
static UQUAD  g_cq_va;
static ULONG  g_cq_wptr;     /* ring write pointer, in dwords */
static BOOL   g_cq_ready;
/* Layout EXACTLY matches the proven one-shot (fence=0xC0FFEE): ring 4KiB @0, rptr@4096,
 * wptr@4104, eop@8192, mqd@12288, fence@16384. The refactor's larger layout put the
 * fence at 0x8000 where the MEC's WRITE_DATA didn't land (rptr advanced, fence=0) — the
 * proven offsets write correctly, so keep them. */
#define CQ_RING_OFF      0
#define CQ_RING_DWORDS   1024          /* 4 KiB ring (proven) */
#define CQ_RPTR_OFF      4096
#define CQ_WPTR_OFF      4104
#define CQ_EOP_OFF       8192
#define CQ_MQD_OFF       12288
#define CQ_FENCE_OFF     16384
#define CQ_DOORBELL_IDX  0x03          /* AMDGPU_NAVI10_DOORBELL_MEC_RING0 (dword idx) */

BOOL gfx_v12_compute_queue_ensure(struct AmdgpuDevice *ad)
{
    volatile ULONG *mqd;
    UQUAD ring_va, rptr_va, wptr_va, eop_va, mqd_va;
    ULONG l;

    if (g_cq_ready) return TRUE;
    if (!ad->gart_ready) { bug("[amdgpu-cq] ensure: GART not ready\n"); return FALSE; }
    if (!g_cq_buf) {
        g_cq_buf = gfx_alloc_queue_buffer(ad, &g_cq_allocation, &g_cq_va);
    }
    if (!g_cq_buf || !g_cq_va) { bug("[amdgpu-cq] ensure: no GART buffer\n"); return FALSE; }

    ring_va = g_cq_va + CQ_RING_OFF;
    rptr_va = g_cq_va + CQ_RPTR_OFF;
    wptr_va = g_cq_va + CQ_WPTR_OFF;
    eop_va  = g_cq_va + CQ_EOP_OFF;
    mqd_va  = g_cq_va + CQ_MQD_OFF;   mqd = (volatile ULONG *)((UBYTE *)g_cq_buf + CQ_MQD_OFF);

    /* Map the doorbell BAR if the MES path hasn't yet. The compute queue is DOORBELL-
     * driven so post-connect submits (radv's repeated command buffers) actually re-kick
     * the MEC — an MMIO wptr-register write only takes effect at connect time. The S2A
     * routing (gc_doorbell_init) makes the doorbell live. Compute doorbell index 0x08
     * (dword) is inside the routed GFX range and clear of MES(0x16)/KIQ(0x18). */
    if (!ad->doorbell && ad->doorbell_phys && ad->pciDriver) {
        ULONG dbsz = (ad->doorbell_size && ad->doorbell_size < 0x100000)
                     ? (ULONG)ad->doorbell_size : 0x100000;
        ad->doorbell = (volatile ULONG *)HIDD_PCIDriver_MapPCI(ad->pciDriver,
                          (APTR)ad->doorbell_phys, dbsz);
    }

    /* Build the compute MQD (doorbell DISABLED — we use the proven connect-time path).
     * Do NOT activate here: gfx_v12_compute_submit installs the HQD with the work already
     * present at connect (the mechanism proven to run PM4). */
    mes_build_mqd(mqd, ring_va, rptr_va, wptr_va, eop_va, mqd_va, 0);
    mqd[143] = 0;
    *(volatile ULONG *)((UBYTE *)g_cq_buf + CQ_RPTR_OFF) = 0;
    *(volatile ULONG *)((UBYTE *)g_cq_buf + CQ_WPTR_OFF) = 0;
    { l = 65536; CachePreDMA((APTR)g_cq_buf, &l, DMA_ReadFromRAM); }

    g_cq_wptr  = 0;
    g_cq_ready = TRUE;
    bug("[amdgpu-cq] compute queue READY: ring VA 0x%08lx%08lx (4KiB), connect-time submit\n",
        (unsigned long)(ULONG)(ring_va >> 32), (unsigned long)(ULONG)ring_va);
    return TRUE;
}

/* GART VA of the fence scratch (radv/callers use offsets within the 4KiB region). */
UQUAD gfx_v12_compute_fence_va(void) { return g_cq_va ? g_cq_va + CQ_FENCE_OFF : 0; }
volatile ULONG *gfx_v12_compute_fence_cpu(void)
{ return g_cq_buf ? (volatile ULONG *)((UBYTE *)g_cq_buf + CQ_FENCE_OFF) : 0; }

/* Drain the previous pipe0/queue0 context BEFORE reusing its ring or MQD.
 * Writing new bases while ACTIVE remains set does not perform a queue handoff.
 * Linux gfx_v12_0_kiq_init_register uses DEQUEUE_REQUEST then waits for inactive;
 * unlike its historical timeout fall-through, refuse to overwrite a live HQD.
 * This is a bounded poll count, not a calibrated wall-clock deadline. Callers
 * remain serialized; this does not add multi-client/SMP submission locking. */
#define CQ_DRAIN_POLLS 100000UL
static BOOL cq_quiesce(struct AmdgpuDevice *ad)
{
    ULONG active, t = 0;

    gc1_w(ad, regGRBM_GFX_CNTL, (1UL << GRBM_GFX_CNTL_MEID_SHIFT));
    active = gc0_r(ad, regCP_HQD_ACTIVE);
    if (active & 1) {
        gc0_w(ad, regCP_HQD_DEQUEUE_REQUEST, 1);
        for (t = 0; t < CQ_DRAIN_POLLS; t++) {
            active = gc0_r(ad, regCP_HQD_ACTIVE);
            if (!(active & 1)) break;
        }
        if (active & 1) {
            gc1_w(ad, regGRBM_GFX_CNTL, 0);
            bug("[amdgpu-cq] drain TIMEOUT after %lu polls; live HQD left intact, submit refused\n",
                (unsigned long)t);
            return FALSE;
        }
        bug("[amdgpu-cq] previous HQD drained after %lu polls\n",
            (unsigned long)(t + 1));
    }
    gc0_w(ad, regCP_HQD_DEQUEUE_REQUEST, 0);
    gc0_w(ad, regCP_HQD_PQ_RPTR, 0);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    return TRUE;
}
/* End compute queue drain helper. */

/* Install the compute HQD with work present and ACTIVATE it. The first shader
 * test uses the freshly restarted MEC; subsequent submitters must cq_quiesce
 * before changing the previous queue's memory or calling this helper. */
static void cq_install(struct AmdgpuDevice *ad, volatile ULONG *mqd, ULONG wptr)
{
    /* Preserve the first-load install ordering. A later replacement is a
     * separate drain/inactive transition, not merely another ACTIVE=1 write.
     * The post-active WPTR_LO write supplies the initial work pointer. */
    gc1_w(ad, regGRBM_GFX_CNTL, (1UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
    gc0_w(ad, regCP_HQD_VMID, 0);
    gc0_w(ad, regCP_HQD_PQ_DOORBELL_CONTROL, 0);
    gc0_w(ad, regCP_MQD_BASE_ADDR,    mqd[128]);
    gc0_w(ad, regCP_MQD_BASE_ADDR_HI, mqd[129]);
    gc0_w(ad, regCP_HQD_PQ_BASE,    mqd[136]);
    gc0_w(ad, regCP_HQD_PQ_BASE_HI, mqd[137]);
    gc0_w(ad, regCP_HQD_PQ_RPTR_REPORT_ADDR,    mqd[139]);
    gc0_w(ad, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI, mqd[140]);
    gc0_w(ad, regCP_HQD_PQ_WPTR_POLL_ADDR,    mqd[141]);
    gc0_w(ad, regCP_HQD_PQ_WPTR_POLL_ADDR_HI, mqd[142]);
    gc0_w(ad, regCP_HQD_PQ_CONTROL, mqd[145]);
    gc0_w(ad, regCP_HQD_PERSISTENT_STATE, mqd[132]);
    gc0_w(ad, regCP_HQD_PQ_WPTR_LO,     0);
    gc0_w(ad, regCP_HQD_PQ_WPTR_LO + 1, 0);
    gc0_w(ad, regCP_HQD_ACTIVE, 1);
    gc0_w(ad, regCP_PQ_WPTR_POLL_CNTL,
          gc0_r(ad, regCP_PQ_WPTR_POLL_CNTL) | CP_PQ_WPTR_POLL_CNTL__EN);
    gc0_w(ad, regCP_HQD_PQ_WPTR_LO,     wptr);       /* post-active wptr = the trigger */
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
}

/* Replace the retired PM4 stream after a graceful queue drain, then reinstall
 * with rptr=0 and wptr=ndw. Callers serialize submissions and wait on their
 * fences; this is not an asynchronously appendable or multi-client ring. */
ULONG gfx_v12_compute_submit(struct AmdgpuDevice *ad, const ULONG *pm4, ULONG ndw)
{
    volatile ULONG *mqd, *rbuf;
    UQUAD ring_va, rptr_va, wptr_va, eop_va, mqd_va;
    ULONG i, l;
    if (!ad || !g_cq_ready || !pm4 || !ndw || ndw > CQ_RING_DWORDS) return 0;
    if (!cq_quiesce(ad)) return 0;
    mqd = (volatile ULONG *)((UBYTE *)g_cq_buf + CQ_MQD_OFF);
    rbuf = (volatile ULONG *)((UBYTE *)g_cq_buf + CQ_RING_OFF);
    ring_va = g_cq_va + CQ_RING_OFF; rptr_va = g_cq_va + CQ_RPTR_OFF;
    wptr_va = g_cq_va + CQ_WPTR_OFF; eop_va = g_cq_va + CQ_EOP_OFF;
    mqd_va = g_cq_va + CQ_MQD_OFF;
    /* Each submit re-runs the FULL proven one-shot: rebuild a fresh MQD (pq_rptr=0,
     * pq_wptr=0), write the PM4 at ring[0], install+activate. This is exactly the
     * sequence that produced fence=0xC0FFEE — invoking it per submit is a valid cs_submit
     * (radv command buffers are self-contained). Splitting the MQD-build into ensure()
     * left stale queue state that broke consumption; rebuilding here fixes it. */
    mes_build_mqd(mqd, ring_va, rptr_va, wptr_va, eop_va, mqd_va, 0);
    mqd[143] = 0;                                     /* doorbell disabled (MMIO/connect) */
    *(volatile ULONG *)((UBYTE *)g_cq_buf + CQ_RPTR_OFF) = 0;
    for (i = 0; i < ndw; i++) rbuf[i] = pm4[i];
    g_cq_wptr = ndw;
    *(volatile ULONG *)((UBYTE *)g_cq_buf + CQ_WPTR_OFF) = g_cq_wptr;
    { l = 65536; CachePreDMA((APTR)g_cq_buf, &l, DMA_ReadFromRAM); }
    cq_install(ad, mqd, g_cq_wptr);                   /* proven install, work present */
    return g_cq_wptr;
}

/* Build a WRITE_DATA(fence) PM4 into pkt[], returns dword count (5). */
static ULONG cq_pm4_write_fence(ULONG *pkt, UQUAD dst_va, ULONG val)
{
    pkt[0] = 0xC0033700UL;                          /* PACKET3(WRITE_DATA, count=3) */
    pkt[1] = 0x00100500UL;                          /* WR_CONFIRM | DST_SEL=memory */
    pkt[2] = (ULONG)(dst_va & 0xFFFFFFFCUL);
    pkt[3] = (ULONG)(dst_va >> 32);
    pkt[4] = val;
    return 5;
}

/* ------------------------------------------------------------------------
 * FIRST REAL COMPUTE-SHADER DISPATCH (GFX12/RDNA4) — the milestone past
 * "CP executes PM4". The REUSE self-test proved the compute queue runs
 * WRITE_DATA (a CP-side write). This runs an actual compute SHADER on the
 * CUs: SET_SH_REG the shader state (PGM/RSRC/threads/CU-mask/user-data) then
 * DISPATCH_DIRECT(1,1,1), fenced by RELEASE_MEM with GL2 writeback so the
 * shader's global_store is flushed to memory and the CPU can read it back.
 * The kernel (test/shaders/sentinel_gfx1200.s, llvm-mc gfx1200) writes the
 * sentinel 0x600DC0DE to the VA handed in s[0:1] (= COMPUTE_USER_DATA_0/1).
 * If the CPU reads that value back, a compute shader executed on the GPU —
 * i.e. radv has a live dispatch path. Rides the proven compute-submit path
 * (gfx_v12_compute_submit) so it needs no MES/doorbell. */
#define PACKET3_SET_SH_REG        0x76
#define PACKET3_SET_SH_REG_START  0x00002C00
#define PACKET3_DISPATCH_DIRECT   0x15
/* GFX12 COMPUTE reg offsets, BASE_IDX 0 (gc_seg[0]); gc_12_0_0_offset.h. */
#define regCOMPUTE_START_X_g              0x1ba4
#define regCOMPUTE_NUM_THREAD_X_g         0x1ba7
#define regCOMPUTE_PGM_LO_g               0x1bac
#define regCOMPUTE_PGM_RSRC1_g            0x1bb2
#define regCOMPUTE_RESOURCE_LIMITS_g      0x1bb5   /* +SE0(0x1bb6)+SE1(0x1bb7) */
#define regCOMPUTE_STATIC_THREAD_MGMT_SE2_g 0x1bb9 /* +SE3(0x1bba) */
#define regCOMPUTE_USER_DATA_0_g          0x1be0

#define CQ_SHADER_OFF  0x5000   /* free space in the 64KB CQ buffer (past fence) */
#define CQ_OUT_OFF     0x6000

/* Minimal GFX12 sentinel kernel (9 dwords): v0=s0; v1=s1; v2=0x600DC0DE;
 * global_store_b32 v[0:1],v2,off; s_wait_storecnt 0; s_endpgm. */
static const ULONG sentinel_shader_gfx1200[] = {
    0x7E000200UL, 0x7E020201UL, 0x7E0402FFUL, 0x600DC0DEUL,
    0xEE06807CUL, 0x01000000UL, 0x00000000UL, 0xBFC10000UL, 0xBFB00000UL
};

BOOL gfx_v12_compute_dispatch_test(struct AmdgpuDevice *ad)
{
    static ULONG *dbuf;
    static APTR   dallocation;
    static UQUAD  dva;
    ULONG sh;
    UQUAD ring_va, rptr_va, wptr_va, eop_va, mqd_va, shader_va, out_va, fence_va, fence2_va;
    volatile ULONG *ring, *mqd, *rptr_cpu, *out_cpu, *fence_cpu, *fence2_cpu, *shader_cpu;
    ULONG pm4[64], n = 0, i, t, l, got, fv, fv2, rptr_reg, att, wonatt, gs2a, gs2b;

    /* Self-contained on the PROVEN one-shot install (cq_install == the 17x-
     * working mec-compute-test path), NOT the never-worked reusable submit. Own
     * GART buffer + a single fresh HQD install gets this boot's ~1/7 queue0-scan
     * window; call this FIRST in the boot path so it is that first install. */
    if (!ad->gart_ready) { bug("[amdgpu-disp] GART not ready\n"); return FALSE; }
    if (!dbuf) {
        dbuf = gfx_alloc_queue_buffer(ad, &dallocation, &dva);
    }
    if (!dbuf || !dva) { bug("[amdgpu-disp] no GART buffer\n"); return FALSE; }
    sh         = ad->gc_seg[0];
    ring_va    = dva + 0;             ring       = (volatile ULONG *)((UBYTE *)dbuf + 0);
    rptr_va    = dva + 4096;          rptr_cpu   = (volatile ULONG *)((UBYTE *)dbuf + 4096);
    wptr_va    = dva + 4104;
    eop_va     = dva + 8192;
    mqd_va     = dva + 12288;         mqd        = (volatile ULONG *)((UBYTE *)dbuf + 12288);
    fence_va   = dva + CQ_FENCE_OFF;  fence_cpu  = (volatile ULONG *)((UBYTE *)dbuf + CQ_FENCE_OFF);
    shader_va  = dva + CQ_SHADER_OFF; shader_cpu = (volatile ULONG *)((UBYTE *)dbuf + CQ_SHADER_OFF);
    out_va     = dva + CQ_OUT_OFF;    out_cpu    = (volatile ULONG *)((UBYTE *)dbuf + CQ_OUT_OFF);
    fence2_va  = dva + 28672;         fence2_cpu = (volatile ULONG *)((UBYTE *)dbuf + 28672);

    for (i = 0; i < sizeof(sentinel_shader_gfx1200) / 4; i++)
        shader_cpu[i] = sentinel_shader_gfx1200[i];
    *out_cpu = 0;
    *fence_cpu = 0;
    *fence2_cpu = 0;
    *rptr_cpu = 0;

#define SH_OFF(r) ((sh + (ULONG)(r)) - PACKET3_SET_SH_REG_START)
    /* START_X/Y/Z = 0 */
    pm4[n++] = PACKET3(PACKET3_SET_SH_REG, 3);
    pm4[n++] = SH_OFF(regCOMPUTE_START_X_g);
    pm4[n++] = 0; pm4[n++] = 0; pm4[n++] = 0;
    /* PGM_LO/HI = shader_va >> 8 */
    pm4[n++] = PACKET3(PACKET3_SET_SH_REG, 2);
    pm4[n++] = SH_OFF(regCOMPUTE_PGM_LO_g);
    pm4[n++] = (ULONG)((shader_va >> 8) & 0xFFFFFFFFUL);
    pm4[n++] = (ULONG)((shader_va >> 40) & 0xFFFFFFFFUL);
    /* PGM_RSRC1/RSRC2 (from the amdhsa kernel descriptor) */
    pm4[n++] = PACKET3(PACKET3_SET_SH_REG, 2);
    pm4[n++] = SH_OFF(regCOMPUTE_PGM_RSRC1_g);
    pm4[n++] = 0x600C0000UL;
    pm4[n++] = 0x00000084UL;
    /* NUM_THREAD_X/Y/Z = 1,1,1 */
    pm4[n++] = PACKET3(PACKET3_SET_SH_REG, 3);
    pm4[n++] = SH_OFF(regCOMPUTE_NUM_THREAD_X_g);
    pm4[n++] = 1; pm4[n++] = 1; pm4[n++] = 1;
    /* RESOURCE_LIMITS=0, STATIC_THREAD_MGMT_SE0/SE1 = all CUs */
    pm4[n++] = PACKET3(PACKET3_SET_SH_REG, 3);
    pm4[n++] = SH_OFF(regCOMPUTE_RESOURCE_LIMITS_g);
    pm4[n++] = 0; pm4[n++] = 0xFFFFFFFFUL; pm4[n++] = 0xFFFFFFFFUL;
    /* STATIC_THREAD_MGMT_SE2/SE3 = all CUs */
    pm4[n++] = PACKET3(PACKET3_SET_SH_REG, 2);
    pm4[n++] = SH_OFF(regCOMPUTE_STATIC_THREAD_MGMT_SE2_g);
    pm4[n++] = 0xFFFFFFFFUL; pm4[n++] = 0xFFFFFFFFUL;
    /* USER_DATA_0/1 = output VA (-> s0/s1 in the shader) */
    pm4[n++] = PACKET3(PACKET3_SET_SH_REG, 2);
    pm4[n++] = SH_OFF(regCOMPUTE_USER_DATA_0_g);
    pm4[n++] = (ULONG)(out_va & 0xFFFFFFFFUL);
    pm4[n++] = (ULONG)(out_va >> 32);
    /* DISPATCH_DIRECT 1,1,1; initiator = COMPUTE_SHADER_EN | FORCE_START_AT_000 */
    pm4[n++] = PACKET3(PACKET3_DISPATCH_DIRECT, 3);
    pm4[n++] = 1; pm4[n++] = 1; pm4[n++] = 1;
    pm4[n++] = 0x00000001UL | 0x00000004UL;
    /* DISAMBIGUATION: proven WRITE_DATA fence IMMEDIATELY after DISPATCH_DIRECT,
     * BEFORE the RELEASE_MEM. Both prior boots (full + minimal RELEASE_MEM) stalled
     * at rptr=0x24=36 = the packet right after the dispatch — so the stall is either
     * RELEASE_MEM-specific OR DISPATCH_DIRECT blocking the CP on whatever follows.
     * WRITE_DATA lands 17x on this queue: wrfence landing here (rptr past it) => CP
     * DOES continue past the dispatch => stall is RELEASE_MEM-specific; wrfence 0 +
     * rptr still 36 => the dispatch itself blocks the next packet. */
    pm4[n++] = 0xC0033700UL;                     /* PACKET3(WRITE_DATA, count=3) */
    pm4[n++] = 0x00100500UL;                     /* WR_CONFIRM | DST_SEL=memory | ENGINE=ME */
    pm4[n++] = (ULONG)(fence2_va & 0xFFFFFFFCUL);
    pm4[n++] = (ULONG)(fence2_va >> 32);
    pm4[n++] = 0x600DBEEFUL;
    /* FENCE — was RELEASE_MEM, which HANGS the MEC (diagnosed: rptr froze at the
     * RELEASE_MEM boundary regardless of flags, while a WRITE_DATA right after the
     * dispatch landed). Replace with the compute-native drain + a proven write:
     * EVENT_WRITE(CS_PARTIAL_FLUSH) waits for the dispatch to complete, then a
     * WRITE_DATA writes the real completion fence (eopfence). */
    pm4[n++] = PACKET3(0x46 /*EVENT_WRITE*/, 0);
    pm4[n++] = 0x07UL | (4UL << 8);              /* EVENT_TYPE=CS_PARTIAL_FLUSH(7) | EVENT_INDEX(4) */
    pm4[n++] = 0xC0033700UL;                     /* PACKET3(WRITE_DATA, count=3) */
    pm4[n++] = 0x00100500UL;                     /* WR_CONFIRM | DST_SEL=memory | ENGINE=ME */
    pm4[n++] = (ULONG)(fence_va & 0xFFFFFFFCUL);
    pm4[n++] = (ULONG)(fence_va >> 32);
    pm4[n++] = 0x600DF00DUL;
#undef SH_OFF

    /* Copy the built PM4 into the ring, build the compute MQD, and do the PROVEN
     * one-shot install (cq_install) — NOT the reusable re-submit (0/all-history). */
    for (i = 0; i < n; i++) ring[i] = pm4[i];
    mes_build_mqd(mqd, ring_va, rptr_va, wptr_va, eop_va, mqd_va, 0);
    mqd[143] = 0;                                     /* doorbell disabled (MMIO wptr) */
    *(volatile ULONG *)((UBYTE *)dbuf + 4104) = n;    /* wptr shadow */
    *(volatile ULONG *)((UBYTE *)dbuf + 4108) = 0;
    { l = 65536; CachePreDMA((APTR)dbuf, &l, DMA_ReadFromRAM); }

    /* PIPE-RESET-RETRY: the MEC pipe samples an installed HQD on its OWN timing, so a
     * single cq_install catches that window only ~1/6 of boots (proven: everything
     * observable is identical good-vs-miss -> a fine-grained HW race). Between attempts
     * RESET + re-ACTIVATE the MEC pipes (the exact gfx_v12_mec_bringup enable sequence)
     * to RE-ARM the dequeue, then re-install with the work present — each attempt a
     * fresh window. ~24 attempts/boot should turn "1/6 per boot" into reliable-per-boot
     * WITHOUT solving the underlying HW timing. wonatt = the attempt that executed. */
    /* CODEX PROBE (TRIG-0515): the power/clock-gate + RLC safe-mode/GFXOFF state that
     * everything else's good-vs-miss diff walked past. GRBM_STATUS2 is read twice ~1ms
     * apart to test the "submitted before GFX power/clock settled" hypothesis (does it
     * CHANGE while we wait?). Printed every boot -> miss data immediate, one good boot
     * gives the diff. gc0=gc_seg[0] BASE_IDX0, gc1=gc_seg[1] BASE_IDX1. */
    gs2a = gc0_r(ad, 0x0da2 /*GRBM_STATUS2*/);
    { volatile ULONG d; for (d = 0; d < 4000000; d++) ; }   /* ~1ms settle */
    gs2b = gc0_r(ad, 0x0da2);
    bug("[amdgpu-disp] PWRPROBE: GRBM_STATUS2 %08lx->%08lx(1ms) GRBM_STATUS=%08lx "
        "RLC_SAFE_MODE=%08lx RLC_STAT=%08lx RLC_GPM_STAT=%08lx CGCG_CGLS=%08lx "
        "CP_STAT=%08lx IMU_RESET_CTRL=%08lx\n",
        (unsigned long)gs2a, (unsigned long)gs2b,
        (unsigned long)gc0_r(ad, regGRBM_STATUS),
        (unsigned long)gc1_r(ad, 0x0980 /*RLC_SAFE_MODE*/),
        (unsigned long)gc1_r(ad, regRLC_STAT),
        (unsigned long)gc1_r(ad, regRLC_GPM_STAT),
        (unsigned long)gc1_r(ad, 0x4c49 /*RLC_CGCG_CGLS_CTRL*/),
        (unsigned long)gc0_r(ad, 0x0f40 /*CP_STAT*/),
        (unsigned long)gc1_r(ad, regGFX_IMU_GFX_RESET_CTRL));

    wonatt = 0;
    for (att = 1; att <= 24; att++) {
        /* FULL-GFX-RE-BRINGUP-UNTIL-EXECUTE: the MEC execute state is per-BOOT (a
         * plain pipe-reset-retry did NOT recover a miss boot). gfx_v12_mec_bringup
         * does the FULLER re-init — it REWRITES the MEC RS64 program-counter start
         * on all 4 pipes (restarting the firmware from its entry) then resets +
         * re-activates the pipes. Re-running it may re-roll the per-boot MEC state.
         * fw start addr is cached, so repeated calls are cheap. */
        gfx_v12_mec_bringup(ad);

        *out_cpu = 0; *fence_cpu = 0; *fence2_cpu = 0;
        *(volatile ULONG *)((UBYTE *)dbuf + 4096) = 0;         /* rptr writeback */
        { l = 65536; CachePreDMA((APTR)dbuf, &l, DMA_ReadFromRAM); }
        cq_install(ad, mqd, n);                                /* re-install, work present */

        for (t = 0; t < 400000; t++) {
            l = 4; CachePostDMA((APTR)out_cpu, &l, 0);
            if (*out_cpu == 0x600DC0DEUL) break;
        }
        if (*out_cpu == 0x600DC0DEUL) {
            /* Shader ran (out landed). The eopfence WRITE_DATA runs AFTER the
             * CS_PARTIAL_FLUSH drains the wave — i.e. LATER than the store — so
             * do NOT read it yet: keep polling until it lands (or times out).
             * Reading fv right after `out` sampled it too early (that was the
             * eopfence=0 despite rptr=48 bug). */
            for (t = 0; t < 2000000; t++) {
                l = 4; CachePostDMA((APTR)fence_cpu, &l, 0);
                if (*fence_cpu == 0x600DF00DUL) break;
            }
            wonatt = att; break;
        }
    }

    { l = 4; CachePostDMA((APTR)fence_cpu,  &l, 0); } fv  = *fence_cpu;
    { l = 4; CachePostDMA((APTR)fence2_cpu, &l, 0); } fv2 = *fence2_cpu;
    got = *out_cpu;
    { l = 4; CachePostDMA((APTR)rptr_cpu, &l, 0); }
    gc1_w(ad, regGRBM_GFX_CNTL, (1UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
    rptr_reg = gc0_r(ad, regCP_HQD_PQ_RPTR);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    bug("[amdgpu-disp] *** COMPUTE SHADER DISPATCH: out=0x%08lx (want 600DC0DE) "
        "eopfence=0x%08lx wrfence=0x%08lx rptr_reg=0x%08lx won@att=%lu/24 ndw=%lu -> %s ***\n",
        (unsigned long)got, (unsigned long)fv, (unsigned long)fv2,
        (unsigned long)rptr_reg, (unsigned long)wonatt, (unsigned long)n,
        (got == 0x600DC0DEUL)
            ? "COMPUTE SHADER RAN ON THE CUs via FULL-MEC-REBRINGUP — radv path LIVE!"
            : (rptr_reg >= n || fv2 == 0x600DBEEFUL)
                ? "CP consumed full ring but shader did NOT write (dispatch/PGM issue)"
            : (rptr_reg != 0)
                ? "CP partially consumed the ring"
                : "all 24 full-MEC-rebringup attempts missed (per-boot execute state)");
    return got == 0x600DC0DEUL;
}

/* ------------------------------------------------------------------------
 * radv cs_submit BRIDGE — run an IB (radv's PM4 command buffer at ib_va,
 * ib_ndw dwords) on the compute queue via the PROVEN one-shot install.
 *
 * This is the amdgpu side of ac_drm_cs_submit_raw2: radv builds a compute IB
 * in a BO (GART-mapped) and hands us its VA + size; we run it by placing a
 * single PM4 INDIRECT_BUFFER packet in our HQD ring pointing at that IB,
 * followed by a WRITE_DATA completion fence the caller polls for the submit's
 * seq_no. Uses cq_install (the 17x-proven install that runs shaders) — NOT the
 * never-worked reusable submit.
 *
 * GATES (both tracked elsewhere, submit path itself is correct now):
 *  1. radv's IB ends in its own RELEASE_MEM EOP fence -> the MEC RELEASE_MEM
 *     stall must be fixed (fence diagnostic in flight) or the IB never returns.
 *  2. ib_va must be GART-mapped so the MEC can fetch it -> needs the
 *     amdgpu_bo_alloc / amdgpu_bo_va_map LVOs (audit gaps) for real radv BOs.
 * Standalone-testable by handing it a hand-built dispatch IB in GART memory. */
#define PACKET3_INDIRECT_BUFFER   0x3F

static ULONG *g_ib_buf;
static APTR   g_ib_allocation;
static UQUAD  g_ib_va;
#define IB_RING_OFF   0
#define IB_RPTR_OFF   4096
#define IB_WPTR_OFF   4104
#define IB_EOP_OFF    8192
#define IB_MQD_OFF    12288
#define IB_FENCE_OFF  16384
#define IB_DONE_MAGIC 0x600D1B1BUL    /* "IB done" */

ULONG gfx_v12_compute_submit_ib(struct AmdgpuDevice *ad, UQUAD ib_va, ULONG ib_ndw)
{
    UQUAD ring_va, rptr_va, wptr_va, eop_va, mqd_va, fence_va;
    volatile ULONG *ring, *mqd;
    ULONG n = 0, l;

    if (!ad || !ad->gart_ready || !ib_va || !ib_ndw) return 0;
    if (ib_ndw > 0x000FFFFFUL) return 0;                 /* IB_SIZE is 20 bits */
    if (!g_ib_buf) {
        g_ib_buf = gfx_alloc_queue_buffer(ad, &g_ib_allocation, &g_ib_va);
    }
    if (!g_ib_buf || !g_ib_va) return 0;
    if (!cq_quiesce(ad)) return 0;

    ring_va  = g_ib_va + IB_RING_OFF;  ring = (volatile ULONG *)((UBYTE *)g_ib_buf + IB_RING_OFF);
    rptr_va  = g_ib_va + IB_RPTR_OFF;
    wptr_va  = g_ib_va + IB_WPTR_OFF;
    eop_va   = g_ib_va + IB_EOP_OFF;
    mqd_va   = g_ib_va + IB_MQD_OFF;   mqd  = (volatile ULONG *)((UBYTE *)g_ib_buf + IB_MQD_OFF);
    fence_va = g_ib_va + IB_FENCE_OFF;
    *(volatile ULONG *)((UBYTE *)g_ib_buf + IB_FENCE_OFF) = 0;   /* completion fence */
    *(volatile ULONG *)((UBYTE *)g_ib_buf + IB_RPTR_OFF)  = 0;

    /* INDIRECT_BUFFER -> radv's IB (ib_va, ib_ndw dwords), VMID0 */
    ring[n++] = PACKET3(PACKET3_INDIRECT_BUFFER, 2);
    ring[n++] = (ULONG)(ib_va & 0xFFFFFFFCUL);
    ring[n++] = (ULONG)(ib_va >> 32);
    ring[n++] = (ib_ndw & 0x000FFFFFUL);                 /* IB_SIZE (dwords), VMID0 */
    /* WRITE_DATA completion fence after the IB returns (the submit's seq_no) */
    ring[n++] = 0xC0033700UL;                            /* PACKET3(WRITE_DATA, count=3) */
    ring[n++] = 0x00100500UL;                            /* WR_CONFIRM | DST_SEL=memory | ENGINE=ME */
    ring[n++] = (ULONG)(fence_va & 0xFFFFFFFCUL);
    ring[n++] = (ULONG)(fence_va >> 32);
    ring[n++] = IB_DONE_MAGIC;

    mes_build_mqd(mqd, ring_va, rptr_va, wptr_va, eop_va, mqd_va, 0);
    mqd[143] = 0;                                        /* doorbell disabled (MMIO wptr) */
    *(volatile ULONG *)((UBYTE *)g_ib_buf + IB_WPTR_OFF)     = n;
    *(volatile ULONG *)((UBYTE *)g_ib_buf + IB_WPTR_OFF + 4) = 0;
    { l = 65536; CachePreDMA((APTR)g_ib_buf, &l, DMA_ReadFromRAM); }
    cq_install(ad, mqd, n);
    return n;                                            /* submitted; poll gfx_v12_compute_ib_fence_cpu */
}

/* CPU pointer to the IB-submit completion fence (== IB_DONE_MAGIC when done). */
volatile ULONG *gfx_v12_compute_ib_fence_cpu(void)
{ return g_ib_buf ? (volatile ULONG *)((UBYTE *)g_ib_buf + IB_FENCE_OFF) : NULL; }

/* Inline-PM4 one-shot submit — the working sibling of gfx_v12_compute_submit for
 * the existing radv winsys, which hands us cs->buf (raw PM4) + cs->cdw inline
 * (not an IB VA). Copies the PM4 into a FRESH ring, builds a fresh compute MQD,
 * and does the PROVEN one-shot cq_install (the 17x-working path). This replaces
 * gfx_v12_compute_submit (reusable g_cq path, 0/all-history) as amdgpu_compute_
 * submit's backing. Still gated on the ~1/7 queue0-scan window (the MES-connection
 * reliability work) and, for radv PM4 that ends in RELEASE_MEM, on the MEC
 * RELEASE_MEM stall fix. Returns wptr (dwords) or 0. */
static ULONG *g_os_buf;
static APTR   g_os_allocation;
static UQUAD  g_os_va;
ULONG gfx_v12_compute_submit_oneshot(struct AmdgpuDevice *ad, const ULONG *pm4, ULONG ndw)
{
    UQUAD ring_va, rptr_va, wptr_va, eop_va, mqd_va;
    volatile ULONG *ring, *mqd;
    ULONG i, l;

    if (!ad || !ad->gart_ready || !pm4 || !ndw) return 0;
    if (ndw > 1024) return 0;                            /* 4KB ring */
    if (!g_os_buf) {
        g_os_buf = gfx_alloc_queue_buffer(ad, &g_os_allocation, &g_os_va);
    }
    if (!g_os_buf || !g_os_va) return 0;
    if (!cq_quiesce(ad)) return 0;

    ring_va = g_os_va + IB_RING_OFF;  ring = (volatile ULONG *)((UBYTE *)g_os_buf + IB_RING_OFF);
    rptr_va = g_os_va + IB_RPTR_OFF;
    wptr_va = g_os_va + IB_WPTR_OFF;
    eop_va  = g_os_va + IB_EOP_OFF;
    mqd_va  = g_os_va + IB_MQD_OFF;   mqd  = (volatile ULONG *)((UBYTE *)g_os_buf + IB_MQD_OFF);

    for (i = 0; i < ndw; i++) ring[i] = pm4[i];
    *(volatile ULONG *)((UBYTE *)g_os_buf + IB_RPTR_OFF) = 0;
    mes_build_mqd(mqd, ring_va, rptr_va, wptr_va, eop_va, mqd_va, 0);
    mqd[143] = 0;                                        /* doorbell disabled (MMIO wptr) */
    *(volatile ULONG *)((UBYTE *)g_os_buf + IB_WPTR_OFF)     = ndw;
    *(volatile ULONG *)((UBYTE *)g_os_buf + IB_WPTR_OFF + 4) = 0;
    { l = 65536; CachePreDMA((APTR)g_os_buf, &l, DMA_ReadFromRAM); }
    cq_install(ad, mqd, ndw);
    return ndw;
}


/* ------------------------------------------------------------------------
 * MEC COMPUTE-QUEUE DISPATCH TEST — the doorbell-less / MES-less path.
 *
 * radv needs only a compute ring/queue, NOT its own GFX bring-up (amdgpu.hidd
 * already powers GFX + runs the MEC). Compute queues live on the MEC (me=1),
 * which is RUNNING, and — like our working SDMA ring — can take their write
 * pointer via MMIO (CP_HQD_PQ_WPTR register), needing NO MES scheduler and NO
 * doorbell. This installs a compute HQD on me=1/pipe0, writes a PM4 WRITE_DATA
 * fence into its ring, bumps the wptr via MMIO, and polls the fence. If the
 * fence value lands, the MEC executed a packet from a directly-installed compute
 * queue — i.e. radv has a live dispatch path and the whole MES doorbell saga was
 * a detour. GART VAs throughout (the MEC fetches via GFXHUB VMID0, same as the
 * gfx CP), so ring/rptr/fence all sit in one GART-mapped system-RAM buffer. */
BOOL gfx_v12_mec_compute_test(struct AmdgpuDevice *ad)
{
    static ULONG *cbuf;
    static APTR   callocation;
    static UQUAD  cva;
    UQUAD ring_va, rptr_va, wptr_va, eop_va, mqd_va, fence_va;
    volatile ULONG *ring, *mqd, *fence_cpu, *rptr_cpu;
    ULONG l, t, rptr_reg;

    if (!ad->gart_ready) { bug("[amdgpu-mec] compute-test: GART not ready\n"); return FALSE; }
    if (!cbuf) {
        cbuf = gfx_alloc_queue_buffer(ad, &callocation, &cva);
    }
    if (!cbuf || !cva) { bug("[amdgpu-mec] compute-test: no GART buffer\n"); return FALSE; }
    if (!cq_quiesce(ad)) return FALSE;

    ring_va  = cva + 0;      ring      = (volatile ULONG *)((UBYTE *)cbuf + 0);
    rptr_va  = cva + 4096;   rptr_cpu  = (volatile ULONG *)((UBYTE *)cbuf + 4096);
    wptr_va  = cva + 4104;
    eop_va   = cva + 8192;
    mqd_va   = cva + 12288;  mqd       = (volatile ULONG *)((UBYTE *)cbuf + 12288);
    fence_va = cva + 16384;  fence_cpu = (volatile ULONG *)((UBYTE *)cbuf + 16384);

    /* v12 compute MQD; doorbell DISABLED (we drive wptr by MMIO, SDMA-style). */
    mes_build_mqd(mqd, ring_va, rptr_va, wptr_va, eop_va, mqd_va, 0);
    mqd[143] = 0;                                   /* CP_HQD_PQ_DOORBELL_CONTROL: EN=0 */
    *fence_cpu = 0;
    *rptr_cpu  = 0;
    /* PM4 WRITE_DATA (5 dwords): write 0x00C0FFEE to fence_va. type3, count=3, op=0x37. */
    ring[0] = 0xC0033700UL;                         /* PACKET3(WRITE_DATA, count=3) */
    ring[1] = 0x00100500UL;                         /* WR_CONFIRM | DST_SEL=memory | ENGINE=ME */
    ring[2] = (ULONG)(fence_va & 0xFFFFFFFCUL);
    ring[3] = (ULONG)(fence_va >> 32);
    ring[4] = 0x00C0FFEEUL;
    *(volatile ULONG *)((UBYTE *)cbuf + 4104) = 5;  /* wptr shadow (poll path) = 5 dwords */
    *(volatile ULONG *)((UBYTE *)cbuf + 4108) = 0;
    { l = 65536; CachePreDMA((APTR)cbuf, &l, DMA_ReadFromRAM); }

    /* Install the HQD on me=1/pipe0/queue0 (compute) + activate. */
    gc1_w(ad, regGRBM_GFX_CNTL, (1UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
    gc0_w(ad, regCP_HQD_VMID, 0);
    gc0_w(ad, regCP_HQD_PQ_DOORBELL_CONTROL, 0);
    gc0_w(ad, regCP_MQD_BASE_ADDR,    mqd[128]);
    gc0_w(ad, regCP_MQD_BASE_ADDR_HI, mqd[129]);
    gc0_w(ad, regCP_HQD_PQ_BASE,    mqd[136]);
    gc0_w(ad, regCP_HQD_PQ_BASE_HI, mqd[137]);
    gc0_w(ad, regCP_HQD_PQ_RPTR_REPORT_ADDR,    mqd[139]);
    gc0_w(ad, regCP_HQD_PQ_RPTR_REPORT_ADDR_HI, mqd[140]);
    gc0_w(ad, regCP_HQD_PQ_WPTR_POLL_ADDR,    mqd[141]);
    gc0_w(ad, regCP_HQD_PQ_WPTR_POLL_ADDR_HI, mqd[142]);
    gc0_w(ad, regCP_HQD_PQ_CONTROL, mqd[145]);
    gc0_w(ad, regCP_HQD_PERSISTENT_STATE, mqd[132]);
    gc0_w(ad, regCP_HQD_PQ_WPTR_LO,     0);
    gc0_w(ad, regCP_HQD_PQ_WPTR_LO + 1, 0);
    gc0_w(ad, regCP_HQD_ACTIVE, 1);
    /* Enable CP wptr polling so the MEC picks the wptr up from wptr_poll_addr (the
     * doorbell-less delivery), AND bump the HQD wptr register directly by MMIO. */
    gc0_w(ad, regCP_PQ_WPTR_POLL_CNTL,
          gc0_r(ad, regCP_PQ_WPTR_POLL_CNTL) | CP_PQ_WPTR_POLL_CNTL__EN);
    gc0_w(ad, regCP_HQD_PQ_WPTR_LO, 5);             /* wptr = 5 dwords (MMIO, no doorbell) */
    gc1_w(ad, regGRBM_GFX_CNTL, 0);

    for (t = 0; t < 3000000; t++) {
        l = 4; CachePostDMA((APTR)fence_cpu, &l, 0);
        if (*fence_cpu == 0x00C0FFEEUL) break;
    }
    { l = 4; CachePostDMA((APTR)rptr_cpu, &l, 0); }
    gc1_w(ad, regGRBM_GFX_CNTL, (1UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
    rptr_reg = gc0_r(ad, regCP_HQD_PQ_RPTR);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    bug("[amdgpu-mec] *** COMPUTE DISPATCH TEST: fence=0x%08lx (want 0x00C0FFEE) "
        "rptr_mem=0x%08lx HQD_RPTR_reg=0x%08lx after %lu polls -> %s ***\n",
        (unsigned long)*fence_cpu, (unsigned long)*rptr_cpu, (unsigned long)rptr_reg,
        (unsigned long)t,
        (*fence_cpu == 0x00C0FFEEUL)
            ? "MEC EXECUTED PM4 — compute queue LIVE via MMIO wptr (NO MES/doorbell)! radv can ride this."
        : (*rptr_cpu != 0 || rptr_reg != 0)
            ? "MEC consumed the ring but fence not written (addressing/WRITE_DATA issue)"
            : "MEC did not consume the ring (wptr not seen — try doorbell or poll-enable)");

    /* BASELINE FORK DIAGNOSTIC (additive, reads only — does NOT touch the proven
     * install). The compute dispatch is boot-variable 1/7: a raw MMIO-installed HQD is
     * only serviced when the MEC pipe happens to scan queue0. This reads the GC (GFXHUB)
     * L2 fault + the HQD state AFTER the attempt to decide the baseline path:
     *   fault=1 & addr==ring_va  -> the MEC WOKE but FAULTED fetching the ring via the
     *                               GFXHUB (ring-fetch / TLB / RLC-mediated-access bug)
     *   no fault & HQD_ACTIVE & RPTR=0 -> the pipe never scanned queue0 -> needs the
     *                               MES SET_RESOURCES(compute_hqd_mask) / KIQ MAP_QUEUES
     *                               connection step. */
    {
        ULONG fst  = gc0_r(ad, regGCVM_L2_PROTECTION_FAULT_STATUS_LO32);
        ULONG falo = gc0_r(ad, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32);
        ULONG fahi = gc0_r(ad, regGCVM_L2_PROTECTION_FAULT_ADDR_HI32);
        UQUAD fva  = (((UQUAD)(fahi & 0xFUL) << 32) | falo) << 12;
        ULONG hact;
        gc1_w(ad, regGRBM_GFX_CNTL, (1UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
        hact = gc0_r(ad, regCP_HQD_ACTIVE);
        gc1_w(ad, regGRBM_GFX_CNTL, 0);
        bug("[amdgpu-mec] *** BASELINE FORK: GC_L2_FAULT=0x%08lx (detected=%lu walker=%lu) "
            "fault_va=0x%llx ring_va=0x%llx HQD_ACTIVE=0x%08lx HQD_RPTR=0x%08lx -> %s ***\n",
            (unsigned long)fst, (unsigned long)(fst & 1), (unsigned long)((fst >> 1) & 7),
            (unsigned long long)fva, (unsigned long long)ring_va,
            (unsigned long)hact, (unsigned long)rptr_reg,
            (fst & 1) ? "CP FAULTED fetching ring -> GFXHUB/TLB fix"
            : (hact & 1) ? "no fault, HQD active, not consumed -> needs MES/KIQ CONNECT"
            : "HQD inactive -> queue self-deactivated");
    }
    return (*fence_cpu == 0x00C0FFEEUL);
}
/* MES bring-up Milestones 2-5 (KIQ path): stand up the KIQ + MES SCHEDULER queues,
 * register the scheduler ring via a KIQ MAP_QUEUES (the upstream-tested bootstrap;
 * the KIQ-less MMIO shortcut left the scheduler unserviced, rptr=0), then submit
 * SET_HW_RESOURCES to the now-registered scheduler ring. A scheduler ACK means MES
 * is live and ADD_QUEUE can map the gfx ring so the ME dispatches. */
/* ------------------------------------------------------------------------
 * PCIe bridge-forwarding diagnostic.
 *
 * The Linux register trace PROVED the GPU doorbell registers are identical to
 * AROS (RCC_DOORBELL_APER_EN=1; Linux never programs the doorbell base into any
 * GPU register). Yet AROS's doorbell BAR2 (0x74_0000_0000, 64-bit, prefetchable)
 * master-aborts to 0xffffffff while VRAM (0x70..0x73) decodes. A >4GB BAR can
 * only be forwarded through a bridge's PREFETCHABLE window via the upper-32
 * base/limit registers. If AROS's PCI resource assignment sized a parent
 * bridge's prefetch window to span VRAM but stop short of the doorbell at 0x74,
 * that EXACTLY explains "VRAM decodes, doorbell just past it master-aborts."
 * This walks every bridge whose [sec,sub] bus range contains the GPU and prints
 * its MEM + PREFETCH windows, flagging whether each of the GPU's BARs is inside.
 * ---------------------------------------------------------------------- */
struct AmdgpuBridgeScan {
    IPTR                 gpu_bus;
    struct AmdgpuDevice *ad;
};

static AROS_UFH3(void, amdgpu_bridge_cb,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(OOP_Object *, dev, A2),
    AROS_UFHA(APTR, message, A1))
{
    AROS_USERFUNC_INIT

    struct AmdgpuBridgeScan *bs = hook->h_Data;
    IPTR cls = 0, sub = 0, bus = 0, d = 0, fn = 0;
    UBYTE secbus, subbus;
    UWORD cmd, brctl, pfb, pfl, mb, ml;
    ULONG pfbu, pflu;
    UQUAD pf_base, pf_lim, mm_base, mm_lim, db, vram;

    OOP_GetAttr(dev, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_Class, &cls);
    OOP_GetAttr(dev, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_SubClass, &sub);
    if (cls != 0x06 || sub != 0x04)             /* PCI-to-PCI bridge only */
        goto out;

    secbus = HIDD_PCIDevice_ReadConfigByte(dev, PCIBR_SECBUS);
    subbus = HIDD_PCIDevice_ReadConfigByte(dev, PCIBR_SUBBUS);
    if (bs->gpu_bus < secbus || bs->gpu_bus > subbus)   /* not an ancestor */
        goto out;

    OOP_GetAttr(dev, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_Bus, &bus);
    OOP_GetAttr(dev, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_Dev, &d);
    OOP_GetAttr(dev, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_Sub, &fn);

    cmd   = HIDD_PCIDevice_ReadConfigWord(dev, PCIBR_COMMAND);
    brctl = HIDD_PCIDevice_ReadConfigWord(dev, PCIBR_CONTROL);
    mb    = HIDD_PCIDevice_ReadConfigWord(dev, PCIBR_MEMBASE);
    ml    = HIDD_PCIDevice_ReadConfigWord(dev, PCIBR_MEMLIMIT);
    pfb   = HIDD_PCIDevice_ReadConfigWord(dev, PCIBR_PREFETCHBASE);
    pfl   = HIDD_PCIDevice_ReadConfigWord(dev, PCIBR_PREFETCHLIMIT);
    pfbu  = HIDD_PCIDevice_ReadConfigLong(dev, PCIBR_PREBASEUPPER);
    pflu  = HIDD_PCIDevice_ReadConfigLong(dev, PCIBR_PRELIMITUPPER);

    /* 32-bit non-prefetch MEM window (bits 31:20 in the word). */
    mm_base = ((UQUAD)(mb & 0xfff0)) << 16;
    mm_lim  = (((UQUAD)(ml & 0xfff0)) << 16) | 0xfffffULL;
    /* 64-bit prefetch window: low bits from the word (31:20), high from upper regs. */
    pf_base = ((UQUAD)pfbu << 32) | (((UQUAD)(pfb & 0xfff0)) << 16);
    pf_lim  = ((UQUAD)pflu << 32) | (((UQUAD)(pfl & 0xfff0)) << 16) | 0xfffffULL;

    db   = (UQUAD)bs->ad->doorbell_phys;
    vram = (UQUAD)bs->ad->vram_bar_phys;

    bug("[amdgpu-brdg] bridge %02lx:%02lx.%lx sec=%02lx sub=%02lx cmd=0x%04lx brctl=0x%04lx\n",
        (unsigned long)bus, (unsigned long)d, (unsigned long)fn,
        (unsigned long)secbus, (unsigned long)subbus,
        (unsigned long)cmd, (unsigned long)brctl);
    bug("[amdgpu-brdg]   MEM32  win 0x%08lx%08lx..0x%08lx%08lx\n",
        (unsigned long)(ULONG)(mm_base >> 32), (unsigned long)(ULONG)mm_base,
        (unsigned long)(ULONG)(mm_lim >> 32),  (unsigned long)(ULONG)mm_lim);
    bug("[amdgpu-brdg]   PREF64 win 0x%08lx%08lx..0x%08lx%08lx (pfb=0x%04lx pfl=0x%04lx up=0x%08lx/0x%08lx)\n",
        (unsigned long)(ULONG)(pf_base >> 32), (unsigned long)(ULONG)pf_base,
        (unsigned long)(ULONG)(pf_lim >> 32),  (unsigned long)(ULONG)pf_lim,
        (unsigned long)pfb, (unsigned long)pfl, (unsigned long)pfbu, (unsigned long)pflu);
    bug("[amdgpu-brdg]   doorbell 0x%08lx%08lx -> %s\n",
        (unsigned long)(ULONG)(db >> 32), (unsigned long)(ULONG)db,
        (db >= pf_base && db <= pf_lim) ? "in PREF (forwarded)" :
        (db >= mm_base && db <= mm_lim) ? "in MEM32 (forwarded)" :
        "*** OUTSIDE ALL WINDOWS = master-abort ***");
    bug("[amdgpu-brdg]   vram     0x%08lx%08lx -> %s\n",
        (unsigned long)(ULONG)(vram >> 32), (unsigned long)(ULONG)vram,
        (vram >= pf_base && vram <= pf_lim) ? "in PREF (forwarded)" :
        (vram >= mm_base && vram <= mm_lim) ? "in MEM32 (forwarded)" :
        "*** OUTSIDE ALL WINDOWS ***");

out:
    AROS_USERFUNC_EXIT
}

static void amdgpu_dump_pci_bridges(struct AmdgpuDevice *ad)
{
    OOP_Object *pci;
    struct Hook hook;
    struct AmdgpuBridgeScan bs;
    IPTR gpu_bus = 0;
    struct TagItem reqs[] = { { TAG_DONE, 0 } };

    OOP_GetAttr(ad->pciDevice, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_Bus, &gpu_bus);
    bs.gpu_bus = gpu_bus;
    bs.ad      = ad;

    pci = OOP_NewObject(NULL, CLID_Hidd_PCI, NULL);
    if (!pci) { bug("[amdgpu-brdg] no PCI subsystem for bridge scan\n"); return; }

    hook.h_Entry = (HOOKFUNC)amdgpu_bridge_cb;
    hook.h_Data  = &bs;

    struct pHidd_PCI_EnumDevices enumMsg = {
        .mID = OOP_GetMethodID(IID_Hidd_PCI, moHidd_PCI_EnumDevices),
        .callback = &hook,
        .requirements = reqs,
    };
    bug("[amdgpu-brdg] GPU on bus 0x%02lx; scanning ancestor bridges for BAR forwarding...\n",
        (unsigned long)gpu_bus);
    OOP_DoMethod(pci, (OOP_Msg)&enumMsg);
    OOP_DisposeObject(pci);

    /* GROUND TRUTH: read the GPU's OWN config BARs raw from hardware and compare
     * to what AROS believes (doorbell_phys / vram_bar_phys from the OOP attrs). If
     * the hardware BAR2/BAR3 (0x18/0x1c) does NOT equal 0x7400000000, AROS's PCI
     * enumeration mis-assigned the 64-bit doorbell BAR -> the device master-aborts
     * BAR2 while VRAM (BAR0) works, exactly the symptom. Also dump COMMAND. */
    {
        ULONG cmd  = HIDD_PCIDevice_ReadConfigWord(ad->pciDevice, PCICS_COMMAND);
        ULONG b0   = HIDD_PCIDevice_ReadConfigLong(ad->pciDevice, 0x10);
        ULONG b1   = HIDD_PCIDevice_ReadConfigLong(ad->pciDevice, 0x14);
        ULONG b2   = HIDD_PCIDevice_ReadConfigLong(ad->pciDevice, 0x18);
        ULONG b3   = HIDD_PCIDevice_ReadConfigLong(ad->pciDevice, 0x1c);
        ULONG b4   = HIDD_PCIDevice_ReadConfigLong(ad->pciDevice, 0x20);
        ULONG b5   = HIDD_PCIDevice_ReadConfigLong(ad->pciDevice, 0x24);
        UQUAD hw_bar0 = ((UQUAD)b1 << 32) | (b0 & 0xFFFFFFF0UL);   /* VRAM (64-bit) */
        UQUAD hw_bar2 = ((UQUAD)b3 << 32) | (b2 & 0xFFFFFFF0UL);   /* doorbell (64-bit) */
        bug("[amdgpu-brdg] GPU cfg: CMD=0x%04lx BAR0=0x%08lx BAR1=0x%08lx BAR2=0x%08lx BAR3=0x%08lx BAR4=0x%08lx BAR5=0x%08lx\n",
            (unsigned long)cmd, (unsigned long)b0, (unsigned long)b1,
            (unsigned long)b2, (unsigned long)b3, (unsigned long)b4, (unsigned long)b5);
        bug("[amdgpu-brdg] HW BAR0(vram)=0x%08lx%08lx (AROS believes 0x%08lx%08lx) %s\n",
            (unsigned long)(ULONG)(hw_bar0 >> 32), (unsigned long)(ULONG)hw_bar0,
            (unsigned long)(ULONG)((UQUAD)ad->vram_bar_phys >> 32), (unsigned long)(ULONG)ad->vram_bar_phys,
            (hw_bar0 == (UQUAD)ad->vram_bar_phys) ? "MATCH" : "*** MISMATCH ***");
        bug("[amdgpu-brdg] HW BAR2(doorbell)=0x%08lx%08lx (AROS believes 0x%08lx%08lx) %s\n",
            (unsigned long)(ULONG)(hw_bar2 >> 32), (unsigned long)(ULONG)hw_bar2,
            (unsigned long)(ULONG)((UQUAD)ad->doorbell_phys >> 32), (unsigned long)(ULONG)ad->doorbell_phys,
            (hw_bar2 == (UQUAD)ad->doorbell_phys) ? "MATCH" : "*** MISMATCH = device BAR2 not where AROS thinks ***");
    }
}

/* ------------------------------------------------------------------------
 * IH (interrupt handler) IP block bring-up — Linux ih_v7_0_irq_init core.
 *
 * AROS never brought the IH up (gfx/ih/sdma were NULL). The MES is INTERRUPT-
 * DRIVEN (packet-fifo); SET_HW_RESOURCES configures the MES scheduler's interrupt
 * infrastructure, which needs a live IH ring — without it the MES consumes the
 * packet then cleanly spins forever (PC 0x64d9, EXC=0), invariant to every packet
 * field. Minimal port: one GART-mapped ring, IH_RB_* programmed, enabled. GART VA
 * + MC_SPACE=4 (GPUVM) reuses the proven CP-ring addressing rather than needing a
 * physically-contiguous bus buffer. Runs once, before the MES SET_HW_RESOURCES. */
static BOOL g_ih_up;
static ULONG *g_ih_ring;
static APTR   g_ih_allocation;
static UQUAD  g_ih_va;

static BOOL amdgpu_ih_hw_init(struct AmdgpuDevice *ad)
{
    ULONG l, rb_bufsz, cntl, n;
    UQUAD wptr_va;
    ULONG obase;

    if (g_ih_up) return TRUE;
    if (!ad->osssys_seg[0]) { bug("[amdgpu-ih] no osssys seg — skip\n"); return FALSE; }
    if (!ad->gart_ready)    { bug("[amdgpu-ih] GART not ready — skip\n"); return FALSE; }
    obase = ad->osssys_seg[0];

    if (!g_ih_ring) {
        g_ih_ring = gfx_alloc_aligned(IH_RING_SIZE + 4096, &g_ih_allocation);
        if (!g_ih_ring) { bug("[amdgpu-ih] alloc failed\n"); return FALSE; }
    }
    /* Match Linux: use_bus_addr — the IH consumes a PHYSICAL/bus address (MC_SPACE=2),
     * NOT a GART VA. CachePreDMA returns the physical address + the contiguous length. */
    *(volatile ULONG *)((UBYTE *)g_ih_ring + IH_RING_SIZE) = 0;
    l = IH_RING_SIZE + 4096;
    g_ih_va = (UQUAD)(IPTR)CachePreDMA((APTR)g_ih_ring, &l, DMA_ReadFromRAM);
    if (!g_ih_va || (g_ih_va & 255) || l < IH_RING_SIZE + 4096) {
        bug("[amdgpu-ih] refusing unaligned/noncontiguous IH bus buffer (%lu of %lu bytes)\n",
            (unsigned long)l, (unsigned long)(IH_RING_SIZE + 4096));
        return FALSE;  /* Never enable DMA into a rounded-down or partial buffer. */
    }
    wptr_va = g_ih_va + IH_RING_SIZE;                 /* wptr writeback just past the ring */

    /* disable first */
    amdgpu_wreg32(ad, obase + regIH_RB_CNTL, 0);
    amdgpu_wreg32(ad, obase + regIH_RB_WPTR, 0);
    amdgpu_wreg32(ad, obase + regIH_RB_RPTR, 0);
    /* base = va >> 8 */
    amdgpu_wreg32(ad, obase + regIH_RB_BASE,    (ULONG)((g_ih_va >> 8) & 0xFFFFFFFFUL));
    amdgpu_wreg32(ad, obase + regIH_RB_BASE_HI, (ULONG)((g_ih_va >> 40) & 0xffUL));
    /* rb_bufsz = order_base_2(ring_size/4) */
    rb_bufsz = 0; n = IH_RING_SIZE / 4; while (n > 1) { n >>= 1; rb_bufsz++; }
    cntl = (2UL << IH_RB_CNTL__MC_SPACE_SHIFT)        /* MC_SPACE=2 bus/physical (Linux use_bus_addr) */
         | IH_RB_CNTL__WPTR_OVERFLOW_CLEAR
         | IH_RB_CNTL__WPTR_OVERFLOW_ENABLE
         | (rb_bufsz << IH_RB_CNTL__RB_SIZE_SHIFT)
         | IH_RB_CNTL__WPTR_WRITEBACK_EN
         | IH_RB_CNTL__MC_SNOOP;
    amdgpu_wreg32(ad, obase + regIH_RB_CNTL, cntl);
    amdgpu_wreg32(ad, obase + regIH_RB_WPTR_ADDR_LO, (ULONG)(wptr_va & 0xFFFFFFFFUL));
    amdgpu_wreg32(ad, obase + regIH_RB_WPTR_ADDR_HI, (ULONG)((wptr_va >> 32) & 0xFFFFUL));
    amdgpu_wreg32(ad, obase + regIH_RB_WPTR, 0);
    amdgpu_wreg32(ad, obase + regIH_RB_RPTR, 0);
    amdgpu_wreg32(ad, obase + regIH_DOORBELL_RPTR, 0);   /* no doorbell */
    /* enable ring + interrupts */
    cntl |= IH_RB_CNTL__RB_ENABLE | IH_RB_CNTL__ENABLE_INTR;
    amdgpu_wreg32(ad, obase + regIH_RB_CNTL, cntl);

    bug("[amdgpu-ih] *** IH RING UP: va=0x%08lx%08lx size=%luKB rb_bufsz=%lu CNTL=0x%08lx readback=0x%08lx ***\n",
        (unsigned long)(ULONG)(g_ih_va >> 32), (unsigned long)(ULONG)g_ih_va,
        (unsigned long)(IH_RING_SIZE / 1024), (unsigned long)rb_bufsz, (unsigned long)cntl,
        (unsigned long)amdgpu_rreg32(ad, obase + regIH_RB_CNTL));
    g_ih_up = TRUE;
    return TRUE;
}

/* Private one-shot continuation of the diagnostic scheduler ring. This is
 * initialization-time state, not a public or SMP-safe submission API. */
static struct {
    struct AmdgpuDevice *ad;
    ULONG *buf;
    UQUAD va;
    BOOL ready;
} gfx_mes_map_state;

BOOL mes_v12_scheduler_bringup(struct AmdgpuDevice *ad)
{
    static ULONG *buf;                 /* 64 KiB: sched + kiq rings/mqds + scratch */
    static APTR   allocation;
    static UQUAD  va;
    UQUAD s_ring, s_rptr, s_wptr, s_eop, s_mqd;
    UQUAD k_ring, k_wptr, k_eop, k_mqd;
    UQUAD sch_ctx, qfence, status;
    UQUAD add_q_mqd_va = 0, add_q_wptr_va = 0, add_q_fence_va;
    volatile ULONG *add_q_fence;
    volatile ULONG *smqd, *kmqd, *sring, *kring, *status_wb, *s_rptr_wb;
    ULONG i, ctrl, got, l, kact, sact, add_ack = 0;
    ULONG pkt[64];

    gfx_mes_map_state.ready = FALSE;
    if (!ad->gart_ready) { bug("[amdgpu-mes] sched: GART not ready\n"); return FALSE; }

    /* Bring the IH (interrupt handler) ring up FIRST — the MES is interrupt-driven
     * and SET_HW_RESOURCES needs it live (Linux runs ih_hw_init before the MES). */
    amdgpu_ih_hw_init(ad);

    if (!buf) {
        buf = gfx_alloc_queue_buffer(ad, &allocation, &va);
    }
    if (!buf || !va) { bug("[amdgpu-mes] sched: no GART buffer\n"); return FALSE; }

    if (!ad->doorbell && ad->doorbell_phys && ad->pciDriver) {
        ULONG dbsz = (ad->doorbell_size && ad->doorbell_size < 0x100000)
                     ? (ULONG)ad->doorbell_size : 0x100000;
        ad->doorbell = (volatile ULONG *)HIDD_PCIDriver_MapPCI(ad->pciDriver,
                          (APTR)ad->doorbell_phys, dbsz);
    }
    if (!ad->doorbell) { bug("[amdgpu-mes] sched: doorbell BAR not mapped\n"); return FALSE; }

    /* DISCRIMINATOR: map the VRAM BAR via the SAME runtime MapPCI path and read [0]
     * from BOTH. VRAM decodes (scanout works) — if VRAM[0] reads real data here but
     * doorbell[0]=0xffffffff, the runtime high-MMIO mapping is FINE and BAR2 genuinely
     * doesn't decode device-side. If BOTH read 0xffffffff, runtime MapPCI can't map
     * high MMIO at all (a kernel/MMU limit) and the doorbell was never reachable. */
    if (ad->vram_bar_phys) {
        volatile ULONG *vr = (volatile ULONG *)HIDD_PCIDriver_MapPCI(ad->pciDriver,
                                 (APTR)ad->vram_bar_phys, 0x1000);
        bug("[amdgpu-mes] BAR-MAP DISCRIMINATOR: VRAM_phys=0x%08lx%08lx VRAM[0]=0x%08lx | "
            "doorbell_phys=0x%08lx%08lx doorbell[0]=0x%08lx\n",
            (unsigned long)(ULONG)((UQUAD)ad->vram_bar_phys >> 32), (unsigned long)(ULONG)ad->vram_bar_phys,
            (unsigned long)(vr ? vr[0] : 0xDEADUL),
            (unsigned long)(ULONG)((UQUAD)ad->doorbell_phys >> 32), (unsigned long)(ULONG)ad->doorbell_phys,
            (unsigned long)ad->doorbell[0]);
        /* DECISIVE: does an ABOVE-4GB MapPCI mapping accept WRITES that reach the device?
         * Our register writes go via BAR5 (rmmio, BELOW 4GB) and work; VRAM reads (>4GB)
         * work; but a >4GB MapPCI WRITE reaching the device is UNTESTED — and the doorbell
         * BAR2 is >4GB. VRAM is real R/W memory, so write a sentinel to a scratch offset
         * (0x800, away from scanout) and read it back. ==sentinel => >4GB MMIO writes DO
         * reach the device (so the doorbell wall is device-side routing, not our write
         * path). !=sentinel => AROS >4GB MMIO writes don't post — THAT is the doorbell
         * wall (a mapping/MMU issue, not amdgpu). */
        if (vr) {
            volatile ULONG *wt = vr + (0x800 / 4);
            ULONG pre = *wt;
            *wt = 0xA5C30F1EUL;
            { volatile ULONG f = vr[0]; (void)f; }   /* flush/post */
            bug("[amdgpu-mes] >4GB-WRITE PROBE: VRAM+0x800 pre=0x%08lx wrote=0xA5C30F1E readback=0x%08lx -> %s\n",
                (unsigned long)pre, (unsigned long)*wt,
                (*wt == 0xA5C30F1EUL) ? "ABOVE-4GB MapPCI WRITES REACH DEVICE (doorbell wall is device routing)"
                                      : "ABOVE-4GB MapPCI WRITE DID NOT LAND (our >4GB write path is the wall)");
        }
    }

    /* BRIDGE-FORWARDING CHECK: with GPU doorbell registers proven identical to
     * Linux, the remaining suspect for BAR2 master-abort is a parent PCIe bridge
     * whose prefetch window doesn't span the doorbell base (0x74). Dump the chain.
     * (Also dumps the GPU's raw config BARs vs what AROS believes.) */
    amdgpu_dump_pci_bridges(ad);

    /* DIRECT APERTURE R/W PROBE (independent of any engine). Write distinct values
     * to two doorbell slots and read the SAME slots back. On RDNA the doorbell
     * aperture is backed by on-die RAM that IS CPU-readable when it decodes, so a
     * write+readback of the matching value proves BAR2 writes physically land. All
     * 0xffffffff => the aperture master-aborts (BAR2 not decoding CPU-side at all).
     * A mix (some slots hold, some don't) points at slot/routing, not the BAR. */
    {
        volatile ULONG *db = ad->doorbell;
        ULONG r16a, r18a, r16b, r18b;
        r16a = db[0x16]; r18a = db[0x18];              /* pre-write reads */
        db[0x16] = 0x0000ABCDUL;
        db[0x18] = 0x00001234UL;
        { volatile ULONG f = db[0]; (void)f; }         /* flush WC */
        r16b = db[0x16]; r18b = db[0x18];              /* post-write reads */
        bug("[amdgpu-mes] APERTURE R/W PROBE: slot0x16 pre=0x%08lx post=0x%08lx (wrote 0xABCD) | "
            "slot0x18 pre=0x%08lx post=0x%08lx (wrote 0x1234) -> %s\n",
            (unsigned long)r16a, (unsigned long)r16b,
            (unsigned long)r18a, (unsigned long)r18b,
            (r16b == 0x0000ABCDUL || r18b == 0x00001234UL)
                ? "WRITES LAND + READABLE (BAR2 decodes CPU-side)"
                : (r16b == 0xffffffffUL && r18b == 0xffffffffUL)
                ? "ALL 0xffffffff (BAR2 master-aborts CPU-side)"
                : "write-only aperture (reads 0xffffffff by design; write reach unproven here)");
    }

    /* *** THE CONFIRMED BLOCKER *** Enable the doorbell aperture. BAR2 reads back
     * 0xffffffff = the device is NOT decoding doorbell writes until the nbio
     * RCC_DOORBELL_APER_EN bit is set (nbio_v7_11_enable_doorbell_aperture). Without
     * it every doorbell goes into the void and the MES pipes wait forever.
     * RCC_DOORBELL_APER_EN = 0x00c0, NBIO BASE_IDX 2 -> nbio_seg[2]. Full diagnostic:
     * log all nbio segments, write bit0, read back, and re-check BAR2 decoding. */
    {
        ULONG apoff = ad->nbio_seg[2] + 0x00c0;
        ULONG dae0  = amdgpu_rreg32(ad, apoff);
        ULONG bar2_before = ad->doorbell[0];
        amdgpu_wreg32(ad, apoff, dae0 | 0x00000001UL);   /* BIF_DOORBELL_APER_EN */
        bug("[amdgpu-mes] nbio_seg[0..5]=%05lx %05lx %05lx %05lx %05lx %05lx\n",
            (unsigned long)ad->nbio_seg[0], (unsigned long)ad->nbio_seg[1],
            (unsigned long)ad->nbio_seg[2], (unsigned long)ad->nbio_seg[3],
            (unsigned long)ad->nbio_seg[4], (unsigned long)ad->nbio_seg[5]);
        bug("[amdgpu-mes] DOORBELL_APER_EN @0x%05lx: 0x%08lx -> 0x%08lx | BAR2[0] before=0x%08lx after=0x%08lx\n",
            (unsigned long)apoff, (unsigned long)dae0,
            (unsigned long)amdgpu_rreg32(ad, apoff),
            (unsigned long)bar2_before, (unsigned long)ad->doorbell[0]);
    }

    /* Layout in the 64 KiB buffer. */
    s_ring = va + 0;      sring     = (volatile ULONG *)((UBYTE *)buf + 0);
    s_rptr = va + 4096;   s_rptr_wb = (volatile ULONG *)((UBYTE *)buf + 4096);
    s_wptr = va + 4104;
    s_eop  = va + 8192;
    s_mqd  = va + 12288;  smqd      = (volatile ULONG *)((UBYTE *)buf + 12288);
    k_ring = va + 16384;  kring     = (volatile ULONG *)((UBYTE *)buf + 16384);
    k_wptr = va + 20488;
    k_eop  = va + 24576;
    k_mqd  = va + 28672;  kmqd      = (volatile ULONG *)((UBYTE *)buf + 28672);
    sch_ctx = va + 32768;
    qfence  = va + 32776;
    status  = va + 32784; status_wb = (volatile ULONG *)((UBYTE *)buf + 32784);

    /* Build both MQDs (scheduler doorbell 0x16, KIQ doorbell 0x18). */
    mes_build_mqd(smqd, s_ring, s_rptr, s_wptr, s_eop, s_mqd, 0x16);
    mes_build_mqd(kmqd, k_ring, va + 20480, k_wptr, k_eop, k_mqd, 0x18);

    /* A COMPUTE queue to hand the live scheduler via MES ADD_QUEUE (map_legacy_kq) —
     * frame 1 of the sched submit. Laid out in the free tail of the 64 KiB buffer.
     * doorbell 0x20 (mes-managed compute), pipe 0 / queue 0. */
    {
        UQUAD c_mqd  = va + 40960;
        UQUAD c_ring = va + 45056;
        UQUAD c_rptr = va + 49160;
        UQUAD c_wptr = va + 49152;
        UQUAD c_eop  = va + 53248;
        volatile ULONG *cmqd = (volatile ULONG *)((UBYTE *)buf + 40960);
        add_q_mqd_va   = c_mqd;
        add_q_wptr_va  = c_wptr;
        mes_build_mqd(cmqd, c_ring, c_rptr, c_wptr, c_eop, c_mqd, 0x20);
    }
    { l = 65536; CachePreDMA((APTR)buf, &l, DMA_ReadFromRAM); }

    /* Enable MES unmapped-doorbell handling (Linux mes_v12_0_hw_init does this
     * BEFORE the scheduler queue init) — the MES must have doorbell monitoring on
     * before it services ring doorbells. */
    gc1_w(ad, regCP_UNMAPPED_DOORBELL, CP_UNMAPPED_DOORBELL_ENABLE_PROCLSB);
    gc0_w(ad, regCP_MEC_DOORBELL_RANGE_LOWER, 0);
    gc0_w(ad, regCP_MEC_DOORBELL_RANGE_UPPER, 0x450);

    /* Enable CP write-pointer POLLING (CP_PQ_WPTR_POLL_CNTL.EN). The doorbell BAR
     * won't decode on this bare-metal path (BAR2 reads 0xffffffff), so the KIQ never
     * sees a rung doorbell. With WPTR_POLL on, the CP instead polls each HQD's
     * cp_hqd_pq_wptr_poll_addr (set from the MQD, = the same k_wptr slot we write
     * below) — a doorbell-less delivery, exactly how our SDMA ring already works.
     * The KIQ engine is now live (boots to its wait-loop), so it can consume this. */
    gc0_w(ad, regCP_PQ_WPTR_POLL_CNTL,
          gc0_r(ad, regCP_PQ_WPTR_POLL_CNTL) | CP_PQ_WPTR_POLL_CNTL__EN);
    bug("[amdgpu-mes] CP_PQ_WPTR_POLL_CNTL=0x%08lx (EN set) — doorbell-less KIQ wptr delivery\n",
        (unsigned long)gc0_r(ad, regCP_PQ_WPTR_POLL_CNTL));

    /* Activate the KIQ HQD (me3/pipe1) + the scheduler HQD (me3/pipe0) via MMIO. The MMIO
     * activation is what makes the sched queue live on the MES engine so db0x16 WAKES the
     * MES (removing it stops the wake entirely). The KIQ SET_RESOURCES + MAP_QUEUES below
     * then registers the sched queue with the scheduler fabric. */
    mes_activate_hqd(ad, 1, kmqd);
    gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 1);
    kact = gc0_r(ad, regCP_HQD_ACTIVE);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    mes_activate_hqd(ad, 0, smqd);
    gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
    sact = gc0_r(ad, regCP_HQD_ACTIVE);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    bug("[amdgpu-mes] HQDs: KIQ_ACTIVE=0x%08lx sched_ACTIVE=0x%08lx doorbell=%p\n",
        (unsigned long)kact, (unsigned long)sact, (APTR)ad->doorbell);

    /* KIQ MAP_QUEUES of the MES SCHEDULER ring (Linux mes_v12_0_kiq_enable_queue) —
     * connect the sched queue to the scheduler fabric via the KIQ BEFORE SET_HW_RESOURCES.
     * AROS wrongly dropped this ("MAP_QUEUES was wrong") and MMIO-installed the sched queue
     * only; the MES fw reads its ring but never completes SET_HW_RESOURCES (hangs 0x64d9)
     * because its queue was never registered by the KIQ. MES-type ring: me=2, engine_sel=5. */
    {
        volatile ULONG *k_rptr_wb = (volatile ULONG *)((UBYTE *)buf + 20480);
        volatile ULONG *kfence    = (volatile ULONG *)((UBYTE *)buf + 32800);
        UQUAD kfence_va = va + 32800;
        ULONG kmq_rptr = 0, t2, scr_off, scr_seed, scr_after;
        /* *** READ-vs-EXECUTE DISAMBIGUATION *** The old probe judged KIQ execution
         * ONLY by a WRITE_DATA memory fence. But the MES/KIQ firmware may not implement
         * generic WRITE_DATA (it handles queue-mgmt opcodes), so fence=0 could mean
         * "opcode ignored" not "engine dead". Add TWO more independent execution signals
         * on the SAME ring, put FIRST so they run before anything else:
         *   [0..2] SET_UCONFIG_REG(SCRATCH_REG0=magic) — a REGISTER write, zero memory/VM,
         *          the exact op Linux gfx ring_test uses. If SCRATCH_REG0 changes, the KIQ
         *          EXECUTES PM4 (register path) even if WRITE_DATA(memory) does not.
         *   [3..7] WRITE_DATA(kfence) — the memory-write signal (as before).
         * Then SET_RESOURCES + MAP_QUEUES(sched). Reading SCRATCH_REG0 + kfence + whether
         * the sched HQD got mapped triple-checks "does the KIQ execute ANY packet". */
        scr_off = (ad->gc_seg[1] + regSCRATCH_REG0) - 0x0000C000UL;   /* uconfig offset (=0x40) */
        scr_seed = 0xCAFED00DUL;
        gc1_w(ad, regGRBM_GFX_CNTL, 0);
        gc1_w(ad, regSCRATCH_REG0, scr_seed);                 /* seed so a change is unambiguous */
        kring[0] = 0xC0017900UL;                              /* PACKET3(SET_UCONFIG_REG, count=1) */
        kring[1] = scr_off;                                   /* SCRATCH_REG0 uconfig offset */
        kring[2] = 0x5A5A0001UL;                              /* KIQ scratch magic */
        kring[3] = 0xC0033700UL;                              /* PACKET3(WRITE_DATA, count=3) */
        kring[4] = 0x00100500UL;                              /* WR_CONFIRM | DST_SEL=memory */
        kring[5] = (ULONG)(kfence_va & 0xFFFFFFFCUL);
        kring[6] = (ULONG)(kfence_va >> 32);
        kring[7] = 0x0C0FFEE1UL;
        /* [8..15]: SET_RESOURCES (KIQ resource context). */
        kring[8]  = PACKET3(PACKET3_SET_RESOURCES, 6);
        kring[9]  = SR_VMID_MASK(0) | SR_QUEUE_TYPE(0);
        kring[10] = 0xFFFFFFFFUL; kring[11] = 0xFFFFFFFFUL;   /* queue_mask */
        kring[12] = 0; kring[13] = 0; kring[14] = 0; kring[15] = 0;
        /* [16..22]: MAP_QUEUES(sched) — MES-type ring me=2, engine_sel=5. */
        kring[16] = PACKET3(PACKET3_MAP_QUEUES, 5);
        kring[17] = MQ_QUEUE_SEL(0) | MQ_VMID(0) | MQ_QUEUE(0) | MQ_PIPE(0)
                  | MQ_ME(2) | MQ_QUEUE_TYPE(0) | MQ_ALLOC_FORMAT(0)
                  | MQ_ENGINE_SEL(5) | MQ_NUM_QUEUES(1);
        kring[18] = MQ_DOORBELL_OFFSET(0x16);
        kring[19] = (ULONG)(s_mqd  & 0xFFFFFFFFUL);
        kring[20] = (ULONG)(s_mqd  >> 32);
        kring[21] = (ULONG)(s_wptr & 0xFFFFFFFFUL);
        kring[22] = (ULONG)(s_wptr >> 32);
        *kfence = 0;
        *k_rptr_wb = 0;
        *(volatile ULONG *)((UBYTE *)buf + 20488) = 23;  /* KIQ wptr shadow = 23 dwords */
        *(volatile ULONG *)((UBYTE *)buf + 20492) = 0;
        { l = 65536; CachePreDMA((APTR)buf, &l, DMA_ReadFromRAM); }
        gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 1);  /* me3/pipe1 = KIQ */
        gc0_w(ad, regCP_HQD_PQ_WPTR_LO, 23);
        gc1_w(ad, regGRBM_GFX_CNTL, 0);
        if (ad->doorbell)
            *(volatile UQUAD *)((UBYTE *)ad->doorbell + 0x18 * 4) = 23;      /* KIQ doorbell */
        for (t2 = 0; t2 < 3000000; t2++) {
            l = 4; CachePostDMA((APTR)kfence, &l, 0);
            if (*kfence == 0x0C0FFEE1UL) break;
            l = 4; CachePostDMA((APTR)k_rptr_wb, &l, 0);
            if (*k_rptr_wb >= 23) break;
        }
        { l = 4; CachePostDMA((APTR)kfence, &l, 0); }
        gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 1);
        kmq_rptr = gc0_r(ad, regCP_HQD_PQ_RPTR);
        gc1_w(ad, regGRBM_GFX_CNTL, 0);
        scr_after = gc1_r(ad, regSCRATCH_REG0);               /* did the KIQ's reg-write land? */
        bug("[amdgpu-mes] *** KIQ EXECUTE-DISAMBIG: SCRATCH_REG0 0x%08lx->0x%08lx (want 0x5A5A0001) | "
            "WRITE_DATA fence=0x%08lx (want 0x0C0FFEE1) | kiq_HQD_RPTR=0x%08lx (want>=23) after %lu polls\n",
            (unsigned long)scr_seed, (unsigned long)scr_after,
            (unsigned long)*kfence, (unsigned long)kmq_rptr, (unsigned long)t2);
        bug("[amdgpu-mes]     VERDICT: %s\n",
            (scr_after == 0x5A5A0001UL) ? "KIQ EXECUTES PM4 register-writes -> engine RUNS; WRITE_DATA/memory is the gap, NOT execution"
            : (*kfence == 0x0C0FFEE1UL) ? "KIQ EXECUTES memory-writes -> engine RUNS"
            : (kmq_rptr >= 23) ? "KIQ READS ring but executes NEITHER reg NOR memory write -> RS64 not running its execute loop, OR mes1.bin does not consume PM4 (expects MES_API frames)"
            : "KIQ did not even advance rptr");
    }

    /* SCHEDULER BOOTSTRAP, matching Linux mes_v12_0_hw_init exactly (non-uni MES):
     * submit SET_HW_RESOURCES as a 64-dword MES_API frame + a 64-dword
     * QUERY_SCHEDULER_STATUS frame (=128 dwords) to the SCHEDULER ring (doorbell 0x16),
     * ring the doorbell with wptr=128 (DWORDS), and poll the completion fence. The post-
     * init doorbell trace on Linux PROVED: (a) MES submissions are 64-dword MES_API
     * frames — NOT PM4 (our old KIQ PACKET3 MAP_QUEUES was wrong); (b) each submit is
     * packet+QUERY_STATUS=128 dwords, doorbell wptr in dwords (128,256,...); (c) non-uni
     * map/set-resources go to the SCHED pipe (0x16), no KIQ. REMOVED: the PM4 KIQ block
     * and the pre-submission doorbell reach-tests — those rang 0x16/0x18 with junk wptrs
     * on an unfilled ring, desyncing wptr/rptr before the real submission. */
    /* Program the aggregated doorbell control registers (Linux mes_v12_0_init_aggregated_
     * doorbell) BEFORE submitting SET_HW_RESOURCES. AROS was missing these — the MES fw
     * spins in SET_HW_RESOURCES on the unconfigured aggregated-doorbell path (proven: it
     * consumes the packet then cleanly halts at PC 0x64d9, EXC=0). Values EN | (idx<<2),
     * idx = 0x800 + i*2 (matches the Linux register trace and the packet's field above). */
    {
        static const ULONG dbctl[5] = {
            regCP_MES_DOORBELL_CONTROL1, regCP_MES_DOORBELL_CONTROL2,
            regCP_MES_DOORBELL_CONTROL3, regCP_MES_DOORBELL_CONTROL4,
            regCP_MES_DOORBELL_CONTROL5 };
        ULONG di;
        for (di = 0; di < 5; di++)
            gc1_w(ad, dbctl[di], CP_MES_DOORBELL_CONTROL__EN |
                  (((ULONG)(MES_AGGREGATED_DB_START + di * 2)) << CP_MES_DOORBELL_CONTROL__OFFSET_SHIFT));
        /* CP_HQD_GFX_CONTROL = 0x8000 (Linux MES hw_init writes it here, broadcast GRBM). */
        gc0_w(ad, regCP_HQD_GFX_CONTROL, 0x00008000UL);
        bug("[amdgpu-mes] aggregated doorbell control1..5 + HQD_GFX_CONTROL programmed\n");
    }
    for (i = 0; i < 128; i++) sring[i] = 0;
    /* ZERO the packet FIRST — Linux mes_v12_0_set_hw_resources does
     * memset(&mes_set_hw_res_pkt, 0, sizeof(...)) before filling specific fields.
     * AROS never did, so pkt[3], the 3-dword gaps between the base arrays
     * (pkt[31..33]/[39..41]/[47..49]), and the trailing fields (pkt[55..63]) carried
     * UNINITIALIZED STACK GARBAGE — the DUT dump showed stack pointers (0x0ee5xxxx) and
     * a bogus oversubscription_timer there. The MES reads the whole 64-dword frame, so
     * garbage in any field it interprets (a count/addr/flag) can hang SET_HW_RESOURCES. */
    for (i = 0; i < 64; i++) pkt[i] = 0;
    /* frame 0 @ dword 0: SET_HW_RESOURCES (type=1 SCHEDULER, opcode=0, dwsize=64) */
    pkt[0]  = 0x00040001UL;
    pkt[1]  = 0x0000FF00UL; pkt[2] = 0x0000FF00UL;
    /* HQD masks — MUST match the real GFX12/Navi44 topology or the MES iterates
     * nonexistent pipes/queues during SET_HW_RESOURCES and cleanly spins (0x64d9).
     * Navi44 (gc_12_0_0): compute num_pipe_per_mec=2, num_queue_per_pipe=4 (4-bit mask,
     * pipes 0,1 only); gfx num_pipe_per_me=1, num_queue_per_pipe=8 (pipe 0 only); sdma 2.
     * Was 0xFF across all 8 compute pipes = 8-bit masks (queues 4-7 don't exist) on pipes
     * 2-7 (don't exist). */
    pkt[5]  = 0x0000000FUL; pkt[6] = 0x0000000FUL;   /* compute_hqd_mask[0..1] = 4 queues */
    for (i = 7; i <= 12; i++) pkt[i] = 0;            /* compute pipes 2..7 do NOT exist */
    pkt[13] = 0x000000FEUL; pkt[14] = 0;             /* gfx_hqd_mask: pipe 0 only (8 queues) */
    pkt[15] = pkt[16] = 0x000000FCUL;                /* sdma_hqd_mask[0..1] */
    /* aggregated_doorbells[5] (dwords 17..21) = 0x800 + i*2 (Linux). Was 0 = invalid;
     * the MES fw uses these for priority scheduling and must match CP_MES_DOORBELL_CONTROL. */
    for (i = 0; i < 5; i++) pkt[17 + i] = (ULONG)(MES_AGGREGATED_DB_START + i * 2);
    pkt[22] = (ULONG)(sch_ctx & 0xFFFFFFFFUL); pkt[23] = (ULONG)(sch_ctx >> 32);
    pkt[24] = (ULONG)(qfence & 0xFFFFFFFFUL);  pkt[25] = (ULONG)(qfence >> 32);
    for (i = 0; i < 5; i++) pkt[26 + i] = ad->gc_seg[i];
    for (i = 0; i < 5; i++) pkt[34 + i] = ad->mmhub_seg[i];
    for (i = 0; i < 5; i++) pkt[42 + i] = ad->osssys_seg[i];
    /* Verify the SEGMENT BASES the MES uses to compute register addresses during
     * SET_HW_RESOURCES. Linux (from the reg trace): gc[0]=0x1260, gc[1]=0xa000. If ours
     * differ (esp. gc[2..4]/mmhub/osssys = 0 or wrong), the MES accesses unmapped MMIO and
     * cleanly spins — the exact 0x64d9 symptom. */
    bug("[amdgpu-mes] SEG BASES sent: gc=%05lx %05lx %05lx %05lx %05lx | mmhub=%05lx %05lx %05lx %05lx %05lx | "
        "oss=%05lx %05lx %05lx %05lx %05lx\n",
        (unsigned long)ad->gc_seg[0], (unsigned long)ad->gc_seg[1], (unsigned long)ad->gc_seg[2],
        (unsigned long)ad->gc_seg[3], (unsigned long)ad->gc_seg[4],
        (unsigned long)ad->mmhub_seg[0], (unsigned long)ad->mmhub_seg[1], (unsigned long)ad->mmhub_seg[2],
        (unsigned long)ad->mmhub_seg[3], (unsigned long)ad->mmhub_seg[4],
        (unsigned long)ad->osssys_seg[0], (unsigned long)ad->osssys_seg[1], (unsigned long)ad->osssys_seg[2],
        (unsigned long)ad->osssys_seg[3], (unsigned long)ad->osssys_seg[4]);
    pkt[50] = (ULONG)(status & 0xFFFFFFFFUL); pkt[51] = (ULONG)(status >> 32);
    pkt[52] = 1; pkt[53] = 0;
    /* Flags = Linux-EXACT 0x00080447: disable_reset(0) | use_different_vmid_compute(1) |
     * disable_mes_log(2) | enable_level_process_quantum_check(6) | enable_reg_active_poll(10)
     * | unmapped_doorbell_handling(19). We PREVIOUSLY cleared bit10 (reg_active_poll) — but
     * that verdict predates the doorbell working: the MES never actually PROCESSED the packet
     * then. Now it wakes and stalls in the resource-iteration (RS64 disasm: hung load @0x64d8
     * with an out-of-region index). Restore bit10 to match Linux exactly — clearing it may
     * take a code path that reads an uninitialised per-queue struct. */
    pkt[54] = 0x00080447UL;
    /* oversubscription_timer (dw55): Linux = (mes_rev < 0x8b) ? 0 : 50, where
     * mes_rev = sched_version & AMDGPU_MES_VERSION_MASK and the MASK IS 0x00000fff
     * (amdgpu_mes.h). Our MES sched version (CP_MES_GP3_LO) = 0x01019050, so the CORRECT
     * mes_rev = 0x01019050 & 0xfff = 0x050 = 80 < 0x8b -> Linux sends 0. We previously
     * masked with 0x0FFFFFFF (WRONG), got 0x1019050 >= 0x8b, and hardcoded 50 — a real
     * SET_HW_RESOURCES field divergence. A nonzero oversubscription_timer arms an MES
     * internal timer during resource setup and is a prime suspect for the clean 0x64d9
     * spin (codex TRIG-0517 DELTA-OVERSUB). Send 0 to match Linux for this GFX12 fw. */
    pkt[55] = 0UL;
    for (i = 0; i < 64; i++) sring[i] = pkt[i];
    /* PROBE: full packet hexdump (dwords 0..55, the meaningful SET_HW_RESOURCES body)
     * so we can byte-diff the exact frame AROS submits against the Linux mes_v12_0_
     * set_hw_resources builder. pkt[55] = oversubscription_timer (Linux: mes_rev<0x8b?0:50)
     * — currently 0; the version read below tells us whether Linux would send 50. */
    {
        ULONG pd;
        for (pd = 0; pd < 56; pd += 8)
            bug("[amdgpu-mes] PKT[%02lu..%02lu] = %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
                (unsigned long)pd, (unsigned long)pd + 7,
                (unsigned long)pkt[pd+0], (unsigned long)pkt[pd+1],
                (unsigned long)pkt[pd+2], (unsigned long)pkt[pd+3],
                (unsigned long)pkt[pd+4], (unsigned long)pkt[pd+5],
                (unsigned long)pkt[pd+6], (unsigned long)pkt[pd+7]);
    }
    /* frame 1 (QUERY_SCHEDULER_STATUS) DROPPED: with the MES now LIVE (SET_HW_RESOURCES
     * ACKs, status=1), the header-only QUERY frame made the MES jump to PC 0x4f000 (beyond
     * the 0x1b390 code region) and trap (EXC=0x30c8a) — a malformed/unsupported query frame.
     * Submit ONLY frame 0 (SET_HW_RESOURCES) = 64 dwords, keeping the scheduler healthy for
     * the follow-on ADD_QUEUE. (A proper MESAPI__QUERY_MES_STATUS can be built later.) */
    /* frame 1 @ dword 64: QUERY_SCHEDULER_STATUS (opcode=11). The MES only PROCESSES a frame
     * when wptr is advanced PAST it, so 2 frames (wptr=128) are required — a 1-frame form
     * NEVER ACKs (0/9 boots). This QUERY frame is what advanced the wptr on the boot that
     * ACKed frame 0 (status=1). It DOES trap post-ACK (jump to 0x4f000) — separable; the
     * frame-0 ACK is the point. A duplicate SET_HW_RESOURCES here regressed (double sch_ctx
     * init, 0/5). NEXT: replace this frame 1 with the ADD_QUEUE frame. */
    /* frame 1 = MES ADD_QUEUE (map_legacy_kq) for the compute queue built above — the
     * step that hands a queue to the LIVE scheduler so radv's cs_submit can ride it.
     * MESAPI__ADD_QUEUE dword offsets (u64-aligned): header@0, doorbell_offset@18,
     * mqd_addr@20-21, wptr_addr@22-23, queue_type@28, flags(map_legacy_kq=bit13)@37,
     * api_status(fence_addr@38-39,value@40-41), pipe_id@50, queue_id@51. */
    add_q_fence_va = va + 57344;
    add_q_fence    = (volatile ULONG *)((UBYTE *)buf + 57344);
    *add_q_fence   = 0;
    for (i = 64; i < 128; i++) sring[i] = 0;
    sring[64 +  0] = 0x00040021UL;                          /* type=1, opcode=2 ADD_QUEUE, dwsize=64 */
    sring[64 + 18] = 0x20;                                  /* doorbell_offset */
    sring[64 + 20] = (ULONG)(add_q_mqd_va & 0xFFFFFFFFUL);  /* mqd_addr */
    sring[64 + 21] = (ULONG)(add_q_mqd_va >> 32);
    sring[64 + 22] = (ULONG)(add_q_wptr_va & 0xFFFFFFFFUL); /* wptr_addr */
    sring[64 + 23] = (ULONG)(add_q_wptr_va >> 32);
    sring[64 + 28] = 1;                                     /* queue_type = COMPUTE */
    sring[64 + 37] = 0x00002000UL;                          /* flags: map_legacy_kq (bit13) */
    sring[64 + 38] = (ULONG)(add_q_fence_va & 0xFFFFFFFFUL);/* api_status.completion_fence_addr */
    sring[64 + 39] = (ULONG)(add_q_fence_va >> 32);
    sring[64 + 40] = 1; sring[64 + 41] = 0;                 /* api_status.completion_fence_value = 1 */
    sring[64 + 50] = 0;                                     /* pipe_id */
    sring[64 + 51] = 0;                                     /* queue_id */
    *status_wb = 0;
    *s_rptr_wb = 0;
    *(volatile ULONG *)((UBYTE *)buf + 4104) = 128;       /* sched wptr = 128 dwords (2 frames; MES needs wptr PAST frame 0 to process it) */
    *(volatile ULONG *)((UBYTE *)buf + 4108) = 0;
    { l = 65536; CachePreDMA((APTR)buf, &l, DMA_ReadFromRAM); }
    {
        ULONG pc_before, pc_after, sched_ver;
        gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
        pc_before = gc1_r(ad, regCP_MES_INSTR_PNTR);
        /* CAPTURE the scheduler-base scratch the MES will read during SET_HW_RESOURCES
         * (the (1<<48)+0x32050 read = regCP_MES_MSCRATCH). This is the base the 0x64d9
         * hash-walk uses (base+0x3000). Nonzero fw-self-init value = healthy; if it were
         * our old mes_log VA the walk would search a too-small region. (pipe0 selected.) */
        bug("[amdgpu-mes] MSCRATCH at SET_HW submit (pipe0) = 0x%08lx%08lx (scheduler base; "
            "walk table @ base+0x3000)\n",
            (unsigned long)gc1_r(ad, regCP_MES_MSCRATCH_HI),
            (unsigned long)gc1_r(ad, regCP_MES_MSCRATCH_LO));
        /* PROBE: the MES scheduler pipe writes its firmware version into CP_MES_GP3_LO
         * at boot (Linux mes_v12_0_queue_init reads it here, pipe-selected). mes_rev =
         * ver & AMDGPU_MES_VERSION_MASK (0x00000fff, amdgpu_mes.h); Linux gates
         * oversubscription_timer (mes_rev<0x8b?0:50) and a SCHED-path step SET_HW_RSRC_1
         * (>=0x4b) on it. Tells us if our pkt[55] diverges. */
        sched_ver = gc1_r(ad, regCP_MES_GP3_LO);
        gc1_w(ad, regGRBM_GFX_CNTL, 0);
        bug("[amdgpu-mes] *** MES SCHED VERSION (CP_MES_GP3_LO, pipe0) = 0x%08lx (mes_rev=0x%lx) "
            "-> Linux oversubscription_timer would be %d; our pkt[55]=%lu ***\n",
            (unsigned long)sched_ver, (unsigned long)(sched_ver & 0x00000FFFUL),
            ((sched_ver & 0x00000FFFUL) < 0x8b) ? 0 : 50, (unsigned long)pkt[55]);
        /* Deliver the sched wptr via a DIRECT MMIO HQD_PQ_WPTR_LO write (me3/pipe0
         * grbm-selected) — the SAME proven path the KIQ kick uses (CP_HQD_PQ_WPTR_LO=20
         * above), NOT just the doorbell. On this bare-metal path the BAR2 doorbell
         * master-aborts (reads 0xffffffff), so db0x16 alone never delivered the wptr and
         * the sched HQD_RPTR stayed 0 while the KIQ (which got the MMIO write) advanced.
         * This is the doorbell-less delivery our SDMA ring already relies on. */
        gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0); /* me3/pipe0 = sched */
        gc0_w(ad, regCP_HQD_PQ_WPTR_LO, 128);
        gc1_w(ad, regGRBM_GFX_CNTL, 0);
        *(volatile UQUAD *)((UBYTE *)ad->doorbell + 0x16 * 4) = 128; /* WDOORBELL64(sched, 128) — 2 frames */
        { volatile ULONG dbrb = ad->doorbell[0x16]; (void)dbrb; }    /* read BAR2 back = flush WC + post */
        got = 0;
        for (i = 0; i < 3000000; i++) {
            l = 8; CachePostDMA((APTR)status_wb, &l, 0);
            got = *status_wb;
            if (got != 0) break;
        }
        /* If frame 0 (SET_HW_RESOURCES) ACKed, the MES then processes frame 1 (ADD_QUEUE).
         * Poll its completion fence (value 1) — a live scheduler MAPPING a compute queue. */
        {
            ULONG aq = 0, t;
            if (got == 1) {
                for (t = 0; t < 2000000; t++) {
                    l = 4; CachePostDMA((APTR)add_q_fence, &l, 0);
                    aq = *add_q_fence;
                    if (aq != 0) break;
                }
            }
            bug("[amdgpu-mes] *** ADD_QUEUE (compute, map_legacy_kq, db0x20 pipe0/q0 mqd=0x%08lx%08lx): "
                "fence=0x%08lx -> %s ***\n",
                (unsigned long)(ULONG)(add_q_mqd_va >> 32), (unsigned long)(ULONG)add_q_mqd_va,
                (unsigned long)aq,
                (got != 1) ? "skipped (SET_HW did not ACK this boot)"
                : (aq == 1) ? "*** COMPUTE QUEUE MAPPED — MES ADD_QUEUE ACKED! radv can ride this ***"
                : "SET_HW ACKed but ADD_QUEUE did not complete (frame/queue field issue)");
            add_ack = aq;
        }
        l = 4; CachePostDMA((APTR)s_rptr_wb, &l, 0);
        gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
        pc_after = gc1_r(ad, regCP_MES_INSTR_PNTR);
        gc1_w(ad, regGRBM_GFX_CNTL, 0);
        /* DECISIVE WAKE TEST: did ringing db0x16 move the sched pipe's PC? PC change =>
         * the doorbell REACHED the MES (it woke & ran, even if it rejected the packet).
         * PC unchanged (still idle-halt PC) => the doorbell did NOT wake the MES at all
         * (device-side doorbell->packet-fifo routing is the wall, not the packet/format). */
        bug("[amdgpu-mes] *** SCHED SET_HW_RESOURCES (direct MES_API, db0x16 wptr=128): "
            "status=0x%08lx (want 1) sched_rptr=0x%08lx pipe0_PC %08lx->%08lx (%s) after %lu polls -> %s ***\n",
            (unsigned long)got, (unsigned long)*s_rptr_wb,
            (unsigned long)pc_before, (unsigned long)pc_after,
            (pc_after != pc_before) ? "PC MOVED: doorbell WOKE the MES" : "PC unchanged: doorbell did NOT wake the MES",
            (unsigned long)i,
            (got == 1) ? "MES SCHEDULER ACKED — MES IS LIVE!"
            : (*s_rptr_wb != 0) ? "scheduler CONSUMED packet but no ACK (fw rejected a field)"
            : "no response (MES did not consume the sched ring)");

        /* RICHER MES STATE (additive, reads only) — distinguish "MES saw an EMPTY queue
         * (wptr never delivered)" from "MES processed but its fence-write FAULTED". Read the
         * sched HQD pointers (me=3/pipe0), the GC L2 fault (the MES writes status VA via the
         * GFXHUB), and re-sample the MES PC to see if it is looping or halted. */
        {
            ULONG hqw, hqr, hqa, fst, pc2, pc3, exc, msl, msh;
            gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
            hqw = gc0_r(ad, regCP_HQD_PQ_WPTR_LO);
            hqr = gc0_r(ad, regCP_HQD_PQ_RPTR);
            hqa = gc0_r(ad, regCP_HQD_ACTIVE);
            /* POLL-vs-STALL: sample the MES PC 32x rapidly. A busy POLL LOOP shows a
             * RANGE of PCs (min!=max, the loop body); a truly FROZEN instruction shows
             * ONE fixed PC. This decides whether 0x64d9 is a queue-readiness poll or a
             * hung memory op. (pipe0 already grbm-selected.) */
            {
                ULONG s, pmin = 0xFFFFFFFFUL, pmax = 0, distinct = 0, prev = 0xFFFFFFFFUL;
                for (s = 0; s < 32; s++) {
                    ULONG p = gc1_r(ad, regCP_MES_INSTR_PNTR);
                    if (p < pmin) pmin = p;
                    if (p > pmax) pmax = p;
                    if (p != prev) { distinct++; prev = p; }
                }
                bug("[amdgpu-mes] *** MES PC SAMPLE(32): min=0x%08lx max=0x%08lx span=0x%lx distinct-transitions=%lu -> %s ***\n",
                    (unsigned long)pmin, (unsigned long)pmax, (unsigned long)(pmax - pmin),
                    (unsigned long)distinct,
                    (pmax != pmin) ? "POLL LOOP (PC ranges -> waiting/spinning on a condition)"
                                   : "FROZEN at 1 PC (true stall on the instruction)");
            }
            pc2 = gc1_r(ad, regCP_MES_INSTR_PNTR);
            { volatile ULONG d; for (d = 0; d < 200000; d++) ; }
            pc3 = gc1_r(ad, regCP_MES_INSTR_PNTR);
            exc = gc1_r(ad, regCP_MES_RS64_EXCEPTION_STATUS);   /* pipe0 trap cause */
            msl = gc1_r(ad, regCP_MES_MSTATUS_LO);
            msh = gc1_r(ad, regCP_MES_MSTATUS_HI);
            gc1_w(ad, regGRBM_GFX_CNTL, 0);
            fst = gc0_r(ad, regGCVM_L2_PROTECTION_FAULT_STATUS_LO32);
            bug("[amdgpu-mes] *** MES POST-SUBMIT STATE: sched HQD_WPTR=0x%08lx HQD_RPTR=0x%08lx "
                "HQD_ACTIVE=0x%08lx | GC_L2_FAULT=0x%08lx (det=%lu) | PC %08lx->%08lx (%s) | "
                "pipe0 EXC=0x%08lx MSTATUS=0x%08lx%08lx -> %s ***\n",
                (unsigned long)hqw, (unsigned long)hqr, (unsigned long)hqa,
                (unsigned long)fst, (unsigned long)(fst & 1),
                (unsigned long)pc2, (unsigned long)pc3,
                (pc2 != pc3) ? "RUNNING" : "halted@1PC",
                (unsigned long)exc, (unsigned long)msh, (unsigned long)msl,
                (exc != 0) ? "MES TRAPPED (bad field/access -> EXC set)"
                           : "MES cleanly spinning (waiting on an op, no trap)");
        }

        /* STALL-ADDRESS DIAGNOSTIC (2026-08-26): the RS64 disasm shows the MES spins at
         * 0x64d8 on `ld a5,-0x328(a5)` — a hung memory read. GRBM read-timeout did NOT
         * help, so it is NOT a GRBM register read. Capture: (1) CP_MES_LOCAL_BASE0/MASK0
         * = the MES's local memory aperture (where its VA 0xf01xxxxx maps physically —
         * if the stall addr is outside this window the read hangs); (2) BOTH hubs' L2
         * fault ADDR (a faulting read leaves the addr here even if we sampled STATUS=0
         * earlier — read STATUS+ADDR together now). This tells us the exact hung address
         * and whether the MES reads via GFXHUB, MMHUB, or its own local aperture. */
        {
            ULONG lb_lo, lb_hi, lm_lo, lm_hi, gfst, gfa_lo, gfa_hi, mfst, mfa_lo;
            gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
            lb_lo = gc1_r(ad, regCP_MES_LOCAL_BASE0_LO);
            lb_hi = gc1_r(ad, regCP_MES_LOCAL_BASE0_HI);
            lm_lo = gc1_r(ad, regCP_MES_LOCAL_MASK0_LO);
            lm_hi = gc1_r(ad, regCP_MES_LOCAL_MASK0_HI);
            gc1_w(ad, regGRBM_GFX_CNTL, 0);
            gfst   = gc0_r(ad, regGCVM_L2_PROTECTION_FAULT_STATUS_LO32);
            gfa_lo = gc0_r(ad, regGCVM_L2_FAULT_ADDR_LO32);
            gfa_hi = gc0_r(ad, regGCVM_L2_FAULT_ADDR_HI32);
            mfst   = amdgpu_rreg32(ad, ad->mmhub_seg[0] + regMMVM_L2_FAULT_STATUS_LO32);
            mfa_lo = amdgpu_rreg32(ad, ad->mmhub_seg[0] + regMMVM_L2_FAULT_ADDR_LO32);
            bug("[amdgpu-mes] *** STALL DIAG: MES_LOCAL_BASE0=0x%08lx%08lx MASK0=0x%08lx%08lx | "
                "GFXHUB fault st=0x%08lx addr=0x%08lx%08lx | MMHUB fault st=0x%08lx addr=0x%08lx (page<<12) ***\n",
                (unsigned long)lb_hi, (unsigned long)lb_lo, (unsigned long)lm_hi, (unsigned long)lm_lo,
                (unsigned long)gfst, (unsigned long)gfa_hi, (unsigned long)gfa_lo,
                (unsigned long)mfst, (unsigned long)mfa_lo);
        }

        /* PEEK THE MES BASE POINTER (2026-08-26): the static trace hit a contradiction —
         * everything says the 0x64d8 load should land in-region, yet it hangs. Resolve it
         * by reading base=[VA 0xf013b9f0] LIVE. MES data VA 0xf0100000 is backed at the
         * physical MDBASE (CP_MES_MDBASE, the data-cache load address, pipe0-selected). So
         * [0xf013b9f0] lives at MDBASE + (0xf013b9f0 - 0xf0100000) = MDBASE + 0x3b9f0. If
         * MDBASE is a VRAM MC addr (>=0x80_00000000), peek it through the VRAM aperture. */
        {
            ULONG md_lo, md_hi, mdb, mi;
            UQUAD mdbase, off = 0x3b9f0ULL;
            gc1_w(ad, regGRBM_GFX_CNTL, (3UL << GRBM_GFX_CNTL_MEID_SHIFT) | 0);
            md_lo = gc1_r(ad, regCP_MES_MDBASE_LO_DIAG);
            md_hi = gc1_r(ad, regCP_MES_MDBASE_HI_DIAG);
            mdb   = gc1_r(ad, regCP_MES_MDBOUND_LO_DIAG);
            mi    = gc1_r(ad, regCP_MES_MIBOUND_LO);
            gc1_w(ad, regGRBM_GFX_CNTL, 0);
            mdbase = ((UQUAD)md_hi << 32) | md_lo;
            bug("[amdgpu-mes] *** MES DATA-CACHE: MDBASE=0x%08lx%08lx MDBOUND=0x%08lx MIBOUND=0x%08lx ***\n",
                (unsigned long)md_hi, (unsigned long)md_lo, (unsigned long)mdb, (unsigned long)mi);
            if (mdbase >= 0x8000000000ULL && mdbase < 0x8400000000ULL) {
                ULONG bv_lo = 0, bv_hi = 0;
                UQUAD voff = (mdbase - 0x8000000000ULL) + off;
                amdgpu_vram_read(ad, voff, &bv_lo, 1);
                amdgpu_vram_read(ad, voff + 4, &bv_hi, 1);
                bug("[amdgpu-mes] *** BASE PEEK: [VA 0xf013b9f0] = 0x%08lx%08lx (VRAM off 0x%08lx%08lx) "
                    "-> stall load base; data region = [0xf0100000,0xf0180000) ***\n",
                    (unsigned long)bv_hi, (unsigned long)bv_lo,
                    (unsigned long)(ULONG)(voff >> 32), (unsigned long)(ULONG)voff);
            } else if (mdbase >= 0x1000000ULL && mdbase < 0x2000000000ULL) {
                /* MES data is in system RAM (GTT). AROS identity-maps physical <=128GB, so
                 * read base=[MDBASE + 0x3b9f0] as a direct pointer. Also dump 8 dwords around
                 * it so we can see the base + neighbouring struct pointers. */
                volatile ULONG *p = (volatile ULONG *)(IPTR)(mdbase + off);
                { ULONG ln = 64; CachePostDMA((APTR)(IPTR)(mdbase + off - 16), &ln, 0); }
                bug("[amdgpu-mes] *** BASE PEEK (GTT direct): [VA 0xf013b9f0] @phys 0x%08lx%08lx = "
                    "0x%08lx%08lx | ctx: %08lx %08lx [%08lx %08lx] %08lx %08lx ***\n",
                    (unsigned long)(ULONG)((mdbase+off)>>32), (unsigned long)(ULONG)(mdbase+off),
                    (unsigned long)p[1], (unsigned long)p[0],
                    (unsigned long)p[-2], (unsigned long)p[-1],
                    (unsigned long)p[0], (unsigned long)p[1],
                    (unsigned long)p[2], (unsigned long)p[3]);
            } else {
                bug("[amdgpu-mes] *** BASE PEEK: MDBASE=0x%08lx%08lx out of readable range ***\n",
                    (unsigned long)md_hi, (unsigned long)md_lo);
            }
        }

        /* MES EVENT-LOG DUMP: if the MES/KIQ ucode wrote anything to its log buffer,
         * it is ALIVE and doing work — the content shows what. All-zero = the ucode
         * never logged (didn't run past init, or MSCRATCH not honored). Scan the log
         * region (buffer base) for the first non-zero dwords. */
        if (mes_log_buf) {
            ULONG n, first = 0xFFFFFFFFUL, nonzero = 0, words = (ULONG)(MES_LOG_BUFFER_SIZE / 4);
            { ULONG ll = (ULONG)(MES_LOG_BUFFER_SIZE + MES_MSCRATCH_SIZE);
              CachePostDMA((APTR)mes_log_buf, &ll, 0); }
            for (n = 0; n < words; n++)
                if (mes_log_buf[n]) { if (first == 0xFFFFFFFFUL) first = n; nonzero++; }
            bug("[amdgpu-mes] MES EVENT-LOG: %lu non-zero dwords (first@%ld) -> %s\n",
                (unsigned long)nonzero, (long)(first == 0xFFFFFFFFUL ? -1 : (long)first),
                nonzero ? "MES ucode WROTE to its log (alive+working)"
                        : "log empty (EXPECTED pre-scheduling; MES only logs scheduling events — NOT a liveness signal)");
            if (nonzero) {
                ULONG base = (first > 2) ? first - 2 : 0;
                bug("[amdgpu-mes]   log[%lu..%lu] = %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
                    (unsigned long)base, (unsigned long)base + 7,
                    (unsigned long)mes_log_buf[base+0], (unsigned long)mes_log_buf[base+1],
                    (unsigned long)mes_log_buf[base+2], (unsigned long)mes_log_buf[base+3],
                    (unsigned long)mes_log_buf[base+4], (unsigned long)mes_log_buf[base+5],
                    (unsigned long)mes_log_buf[base+6], (unsigned long)mes_log_buf[base+7]);
            }
        }
        if (got == 1 && add_ack == 1) {
            gfx_mes_map_state.ad = ad;
            gfx_mes_map_state.buf = buf;
            gfx_mes_map_state.va = va;
            gfx_mes_map_state.ready = TRUE;
        }
        return gfx_mes_map_state.ready;
    }
}

/* Append one graphics ADD_QUEUE and its completion barrier to the scheduler
 * ring that already processed SET_HW + compute ADD_QUEUE. Never overwrite an
 * unconsumed frame or retry an ambiguous mapping. Failure retains the buffers
 * and requires a fresh device initialization, not forced HQD deactivation. */
/* Begin testable graphics MES map helper. */
static BOOL gfx_v12_mes_map_graphics(struct AmdgpuDevice *ad,
                                    UQUAD mqd_va, UQUAD wptr_va)
{
    ULONG *buf = gfx_mes_map_state.buf;
    UQUAD va = gfx_mes_map_state.va;
    volatile ULONG *frames;
    volatile UQUAD *ack, *query, *sched_wptr;
    UQUAD ack_value = 0, query_value = 0;
    ULONG rptr, wptr, active, i, l;

    if (!gfx_mes_map_state.ready || gfx_mes_map_state.ad != ad)
        return FALSE;
    gfx_mes_map_state.ready = FALSE; /* one attempt, including failure */
    gc1_w(ad, regGRBM_GFX_CNTL, 3UL << GRBM_GFX_CNTL_MEID_SHIFT);
    active = gc0_r(ad, regCP_HQD_ACTIVE);
    rptr = gc0_r(ad, regCP_HQD_PQ_RPTR);
    wptr = gc0_r(ad, regCP_HQD_PQ_WPTR_LO);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    if (active != 1 || rptr != 128 || wptr != 128) {
        bug("[amdgpu-me] GFX map refused: scheduler ACTIVE=%08lx RPTR=%08lx WPTR=%08lx (need 1/128/128)\n",
            (unsigned long)active, (unsigned long)rptr, (unsigned long)wptr);
        return FALSE;
    }
    frames = (volatile ULONG *)buf + 128;
    ack = (volatile UQUAD *)((UBYTE *)buf + 57352);
    query = (volatile UQUAD *)((UBYTE *)buf + 57360);
    sched_wptr = (volatile UQUAD *)((UBYTE *)buf + 4104);
    if (!gfx12_build_map_frames(frames, mqd_va, wptr_va, va + 57352, va + 57360))
        return FALSE;
    *ack = 0;
    *query = 0;
    l = 16; CachePreDMA((APTR)ack, &l, DMA_ReadFromRAM);
    l = GFX12_MAP_DWORDS * 4; CachePreDMA((APTR)frames, &l, DMA_ReadFromRAM);
    *sched_wptr = 256;
    l = 8; CachePreDMA((APTR)sched_wptr, &l, DMA_ReadFromRAM);
    gc1_w(ad, regGRBM_GFX_CNTL, 3UL << GRBM_GFX_CNTL_MEID_SHIFT);
    gc0_w(ad, regCP_HQD_PQ_WPTR_LO, 256);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    *(volatile UQUAD *)((UBYTE *)ad->doorbell + 0x16 * 4) = 256;
    { volatile ULONG posted = ad->doorbell[0x16]; (void)posted; }
    for (i = 0; i < 3000000; i++) {
        l = 16; CachePostDMA((APTR)ack, &l, 0);
        ack_value = *ack;
        query_value = *query;
        if ((ack_value == 1 && query_value == 1) || (ack_value >> 32)) break;
    }
    gc1_w(ad, regGRBM_GFX_CNTL, 3UL << GRBM_GFX_CNTL_MEID_SHIFT);
    /* The completion store can precede the final RPTR update. Bound that
     * retirement wait too; do not turn a single early sample into a failure. */
    for (l = 0; l < 10000; l++) {
        rptr = gc0_r(ad, regCP_HQD_PQ_RPTR);
        if (rptr == 256 || ack_value != 1 || query_value != 1) break;
    }
    wptr = gc0_r(ad, regCP_HQD_PQ_WPTR_LO);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    bug("[amdgpu-me] GFX ADD_QUEUE+QUERY: ack=%08lx:%08lx query=%08lx:%08lx sched RPTR=%lu WPTR=%lu polls=%lu -> %s\n",
        (unsigned long)(ack_value >> 32), (unsigned long)(ULONG)ack_value,
        (unsigned long)(query_value >> 32), (unsigned long)(ULONG)query_value,
        (unsigned long)rptr, (unsigned long)wptr, (unsigned long)i,
        (ack_value == 1 && query_value == 1 && rptr == 256) ? "mapping acknowledged" : "mapping unconfirmed; no graphics submit");
    return ack_value == 1 && query_value == 1 && rptr == 256 && wptr == 256;
}
/* End testable graphics MES map helper. */

_Static_assert(GFX12_CSB_OFFSET >= 16384 + GFX12_MQD_DWORDS * 4 &&
               GFX12_CSB_OFFSET + GFX12_CSB_PAGE_BYTES <= GFX_QUEUE_BYTES,
               "retained CSIB page must not overlap graphics MQD or leave allocation");
_Static_assert(GFX12_CSB_DWORDS * 4 <= GFX12_CSB_PAGE_BYTES,
               "clear-state data must fit retained page");

/* Begin testable graphics CSIB helper.
 * Linux's PSP resume still initializes CSIB before enabling RLC SRM. Native
 * boot7 had a null CSIB and SRM disabled. This bounded diagnostic fills an
 * otherwise unused page of the retained graphics allocation before CP config.
 * Never replace an existing CSIB or a running SRM. Failed readback leaves the
 * allocation retained and stops bring-up; no blind enable or reset fallback.
 */
static BOOL gfx_v12_init_csb(struct AmdgpuDevice *ad, ULONG *csb, UQUAD va)
{
    ULONG lo, hi, size, srm, dwords, length, actual;
    if (!csb || ((IPTR)csb & (GFX12_CSB_PAGE_BYTES - 1)) || !va ||
        (va & (GFX12_CSB_PAGE_BYTES - 1)) || (va >> 48)) return FALSE;
    lo = gc1_r(ad, regRLC_CSIB_ADDR_LO);
    hi = gc1_r(ad, regRLC_CSIB_ADDR_HI);
    size = gc1_r(ad, regRLC_CSIB_LENGTH);
    srm = gc1_r(ad, regRLC_SRM_CNTL);
    bug("[amdgpu-csib] before: addr=%08lx:%08lx dwords=%lu SRM=%08lx\n",
        (unsigned long)hi, (unsigned long)lo, (unsigned long)size, (unsigned long)srm);
    if (lo || hi || size || (srm & RLC_SRM_CNTL__SRM_ENABLE)) {
        bug("[amdgpu-csib] refused: existing CSIB or enabled SRM; nothing replaced\n");
        return FALSE;
    }
    dwords = gfx12_build_csb(csb, GFX12_CSB_PAGE_BYTES / 4);
    if (dwords != GFX12_CSB_DWORDS) return FALSE;
    length = dwords * 4;
    CachePreDMA(csb, &length, DMA_ReadFromRAM);
    if (length != dwords * 4) return FALSE;
    gc1_w(ad, regRLC_CSIB_ADDR_HI, (ULONG)(va >> 32));
    gc1_w(ad, regRLC_CSIB_ADDR_LO, (ULONG)va);
    gc1_w(ad, regRLC_CSIB_LENGTH, dwords);
    hi = gc1_r(ad, regRLC_CSIB_ADDR_HI);
    lo = gc1_r(ad, regRLC_CSIB_ADDR_LO);
    size = gc1_r(ad, regRLC_CSIB_LENGTH);
    bug("[amdgpu-csib] requested=%08lx:%08lx/%lu actual=%08lx:%08lx/%lu clusters=%lu\n",
        (unsigned long)(va >> 32), (unsigned long)(ULONG)va, (unsigned long)dwords,
        (unsigned long)hi, (unsigned long)lo, (unsigned long)size, (unsigned long)csb[0]);
    if (hi != (ULONG)(va >> 32) || lo != (ULONG)va || size != dwords) {
        bug("[amdgpu-csib] readback mismatch; SRM not enabled, buffer retained\n");
        return FALSE;
    }
    srm |= RLC_SRM_CNTL__AUTO_INCR_ADDR | RLC_SRM_CNTL__SRM_ENABLE;
    gc1_w(ad, regRLC_SRM_CNTL, srm);
    actual = gc1_r(ad, regRLC_SRM_CNTL);
    bug("[amdgpu-csib] SRM requested=%08lx actual=%08lx; retained GART page, not an ME execution proof\n",
        (unsigned long)srm, (unsigned long)actual);
    return actual == srm;
}
/* End testable graphics CSIB helper. */

/* Begin testable graphics reuse helper.
 * A bounded follow-up to the first ME/PFP write, NOT a public submission API.
 * Append 32 unique pairs without reset/remap or ring wrap. Both side effects,
 * neighboring guards, pointer retirement and idle must agree before reusing
 * the targets. An old fence/RPTR or a timed-out packet can never be replayed.
 */
#define GFX_REPEAT_COUNT 32UL
#define GFX_REPEAT_POLLS 100000UL
#define GFX_REPEAT_GUARD_LO 0x6ac0ffeeUL
#define GFX_REPEAT_GUARD_HI 0xc001cafeUL
_Static_assert(10 + GFX_REPEAT_COUNT * 10 < 1024, "bounded repeat stays inside 4KiB ring");

static BOOL gfx_v12_gfx_repeat_test(struct AmdgpuDevice *ad, ULONG *buf, UQUAD va)
{
    volatile ULONG *ring = buf, *rptr_wb, *me, *pfp;
    volatile UQUAD *wptr_wb;
    ULONG seq, wptr = 10, maximum_polls = 0;
    if (!buf || ((IPTR)buf & 4095) || !gfx12_address_valid(va, 4096) ||
        !gfx12_address_valid(va + 65536 - 4096, 4096)) return FALSE;
    rptr_wb = (volatile ULONG *)((UBYTE *)buf + 4096);
    wptr_wb = (volatile UQUAD *)((UBYTE *)buf + 4104);
    me = (volatile ULONG *)((UBYTE *)buf + 8192);
    pfp = (volatile ULONG *)((UBYTE *)buf + 12288);
    if (gc1_r(ad, regGRBM_GFX_CNTL) != 0 ||
        gc0_r(ad, regCP_GFX_HQD_ACTIVE) != 1 ||
        gc0_r(ad, regCP_GFX_HQD_BASE) != (ULONG)(va >> 8) ||
        gc0_r(ad, regCP_GFX_HQD_BASE_HI) != (ULONG)(va >> 40) ||
        gc0_r(ad, regCP_RB0_RPTR) != 10 || gc0_r(ad, regCP_STAT) != 0) {
        bug("[amdgpu-gfx-reuse] refused: initial bank/queue/retirement/idle mismatch\n");
        return FALSE;
    }
    {
        ULONG length = 4;
        CachePostDMA((APTR)rptr_wb, &length, 0);
        if (*rptr_wb != 10 || *wptr_wb != 10) return FALSE;
    }
    for (seq = 1; seq <= GFX_REPEAT_COUNT; seq++) {
        ULONG want_me = 0x4d450000UL | seq, want_pfp = 0x50460000UL | seq;
        ULONG poll, length, got_me = 0, got_pfp = 0, rptr = 0, wb = 0, state = 0;
        BOOL guards = FALSE, complete = FALSE;
        volatile ULONG *packet = ring + wptr;
        me[-1] = pfp[-1] = GFX_REPEAT_GUARD_LO;
        me[1] = pfp[1] = GFX_REPEAT_GUARD_HI;
        *me = ~want_me; *pfp = ~want_pfp;
        length = 12; CachePreDMA((APTR)(me - 1), &length, DMA_ReadFromRAM);
        if (length != 12) return FALSE;
        length = 12; CachePreDMA((APTR)(pfp - 1), &length, DMA_ReadFromRAM);
        if (length != 12) return FALSE;
        packet[0] = PACKET3(PACKET3_WRITE_DATA, 3);
        packet[1] = WRITE_DATA_DST_SEL_MEM;
        packet[2] = (ULONG)(va + 8192); packet[3] = (ULONG)((va + 8192) >> 32);
        packet[4] = want_me;
        packet[5] = PACKET3(PACKET3_WRITE_DATA, 3);
        packet[6] = WRITE_DATA_DST_SEL_MEM | (1UL << 30);
        packet[7] = (ULONG)(va + 12288); packet[8] = (ULONG)((va + 12288) >> 32);
        packet[9] = want_pfp;
        length = 40; CachePreDMA((APTR)packet, &length, DMA_ReadFromRAM);
        if (length != 40) return FALSE;
        wptr += 10;
        *wptr_wb = wptr;
        length = 8; CachePreDMA((APTR)wptr_wb, &length, DMA_ReadFromRAM);
        if (length != 8) return FALSE;
        gc0_w(ad, regCP_RB0_WPTR, wptr);
        gc0_w(ad, regCP_RB0_WPTR_HI, 0);
        for (poll = 0; poll < GFX_REPEAT_POLLS; poll++) {
            length = 12; CachePostDMA((APTR)(me - 1), &length, 0);
            length = 12; CachePostDMA((APTR)(pfp - 1), &length, 0);
            length = 4; CachePostDMA((APTR)rptr_wb, &length, 0);
            got_me = *me; got_pfp = *pfp; wb = *rptr_wb;
            guards = me[-1] == GFX_REPEAT_GUARD_LO && me[1] == GFX_REPEAT_GUARD_HI &&
                     pfp[-1] == GFX_REPEAT_GUARD_LO && pfp[1] == GFX_REPEAT_GUARD_HI;
            rptr = gc0_r(ad, regCP_RB0_RPTR);
            state = gc0_r(ad, regCP_STAT);
            if (!guards) break;
            if (got_me == want_me && got_pfp == want_pfp &&
                rptr == wptr && wb == wptr && state == 0) { complete = TRUE; break; }
        }
        if (poll > maximum_polls) maximum_polls = poll;
        if (!complete || seq == 1 || seq == GFX_REPEAT_COUNT)
            bug("[amdgpu-gfx-reuse] seq=%lu/%lu ME=%08lx/%08lx PFP=%08lx/%08lx RPTR=%lu WB=%lu WPTR=%lu CP_STAT=%08lx guards=%lu polls=%lu -> %s\n",
                (unsigned long)seq, (unsigned long)GFX_REPEAT_COUNT,
                (unsigned long)got_me, (unsigned long)want_me,
                (unsigned long)got_pfp, (unsigned long)want_pfp,
                (unsigned long)rptr, (unsigned long)wb, (unsigned long)wptr,
                (unsigned long)state, (unsigned long)guards, (unsigned long)poll,
                complete ? "PASS" : "FAIL; no retry or next submit");
        if (!complete) return FALSE;
    }
    bug("[amdgpu-gfx-reuse] PASS %lu/%lu unique ME+PFP pairs, final WPTR=%lu max_polls=%lu; no reset/remap/wrap, bounded single-producer only\n",
        (unsigned long)GFX_REPEAT_COUNT, (unsigned long)GFX_REPEAT_COUNT,
        (unsigned long)wptr, (unsigned long)maximum_polls);
    return TRUE;
}
/* End testable graphics reuse helper. */

/* Managed IB publication on the SAME graphics queue just accepted above.
 * Reuse only retired storage in the original page; no reinstall/reset.
 * The CS service serializes entry and owns every referenced BO/VA lifetime.
 * MQD lives at +16384, CSIB at +32768; this private fence uses +24576.
 * Linux gfx_v12_0_ring_emit_ib_gfx/fence and nvd.h/soc24_enum.h define these
 * packets. In particular graphics IB has no compute-only VALID bit.
 */
static struct AmdgpuDevice *managed_ad;
static ULONG *managed_buf;
static UQUAD managed_va, managed_token;
static ULONG managed_wptr, managed_pending_polls;
static BOOL managed_publish_reported, managed_retire_reported;
#define MANAGED_RING_DWORDS 1024UL
#define MANAGED_PACKET_DWORDS 12UL
#define MANAGED_FENCE_OFFSET 24576UL
#define MANAGED_FENCE_GUARD 0x6d674658c001cafeULL
_Static_assert(330ULL + AMDGPU_CS_BOOT_SUBMITS * 12ULL <= 0xffffffffULL,
               "managed producer counter does not roll over");

BOOL gfx_v12_managed_ready(struct AmdgpuDevice *ad)
{ return ad && ad == managed_ad && managed_buf && managed_va; }

static BOOL managed_identity(struct AmdgpuDevice *ad)
{
    return gc1_r(ad, regGRBM_GFX_CNTL) == 0 &&
        gc0_r(ad, regCP_GFX_HQD_ACTIVE) == 1 &&
        gc0_r(ad, regCP_GFX_HQD_BASE) == (ULONG)(managed_va >> 8) &&
        gc0_r(ad, regCP_GFX_HQD_BASE_HI) == (ULONG)(managed_va >> 40) &&
        gfx12_diag_hqd_cntl_valid(GFX12_DIAG_HQD_CNTL, gc0_r(ad, regCP_GFX_HQD_CNTL)) &&
        gc0_r(ad, regCP_GFX_HQD_VMID) == 0;
}

/* Cold54 first wrap: MMIO RPTR=2, WPTR=1026, CP_STAT=0. The MMIO
 * read pointer is a ring position, not the monotonic producer counter.
 * Compare writeback positions modulo the same storage size too. This alone
 * NEVER retires a submission: the exact private64-bit EOP token and guards
 * remain mandatory below, disambiguating every reused position/epoch. */
static BOOL managed_ring_drained(struct AmdgpuDevice *ad, ULONG rp, ULONG wb, ULONG state)
{
    ULONG slot = managed_wptr & (MANAGED_RING_DWORDS - 1);
    return rp == slot && (wb & (MANAGED_RING_DWORDS - 1)) == slot && !state &&
        gc0_r(ad, regCP_RB0_WPTR) == managed_wptr && gc0_r(ad, regCP_RB0_WPTR_HI) == 0;
}

LONG gfx_v12_managed_submit(struct AmdgpuDevice *ad, UQUAD ib, ULONG dwords, UQUAD token)
{
    volatile ULONG *rptr;
    volatile UQUAD *fence, *wptr;
    UQUAD fence_va;
    ULONG packet[MANAGED_PACKET_DWORDS], length, first, count, i;
    if (!gfx_v12_managed_ready(ad)) return -ENODEV;
    if (!token || token == managed_token || !dwords || dwords > AMDGPU_CS_EXTENDED_MAX_IB_DWORDS ||
        !gfx12_address_valid(ib, 4) || !gfx12_address_valid(ib + dwords * 4 - 4, 4))
        return -EINVAL;
    if (managed_wptr > 0xffffffffUL - MANAGED_PACKET_DWORDS) return -ENOSPC;
    if (!managed_identity(ad)) return -EIO;
    rptr = (volatile ULONG *)((UBYTE *)managed_buf + 4096);
    length = 4; CachePostDMA((APTR)rptr, &length, 0);
    if (length != 4 || !managed_ring_drained(ad, gc0_r(ad, regCP_RB0_RPTR),
                                           *rptr, gc0_r(ad, regCP_STAT))) return -EBUSY;
    fence = (volatile UQUAD *)((UBYTE *)managed_buf + MANAGED_FENCE_OFFSET);
    fence_va = managed_va + MANAGED_FENCE_OFFSET;
    fence[-1] = fence[1] = MANAGED_FENCE_GUARD;
    *fence = ~token;
    length = 24; CachePreDMA((APTR)(fence - 1), &length, DMA_ReadFromRAM);
    if (length != 24) return -EIO;
    packet[0] = PACKET3(0x3f, 2); /* INDIRECT_BUFFER, graphics VMID0 */
    packet[1] = (ULONG)ib; packet[2] = (ULONG)(ib >> 32); packet[3] = dwords;
    packet[4] = PACKET3(0x49, 6); /* RELEASE_MEM */
    packet[5] = (1UL << 22) | (1UL << 21) | (3UL << 25) | (5UL << 8) | 0x14;
    packet[6] = 2UL << 29;       /* 64-bit sequence, no interrupt */
    packet[7] = (ULONG)fence_va; packet[8] = (ULONG)(fence_va >> 32);
    packet[9] = (ULONG)token; packet[10] = (ULONG)(token >> 32); packet[11] = 0;
    /* GFX12 uses a monotonic64-bit WPTR, but the MMIO RPTR wraps at the ring
     * boundary (native cold54). Storage is masked as in Linux's
     * amdgpu_ring_write_multiple; a PM4 packet may cross the page end.
     * This tier stops before32-bit producer rollover. The ring-position
     * check and unique-token base retirement above precede ANY
     * reused-ring write. Split DMA synchronization stays inside the ring
     * page: +4096 contains the RPTR writeback, not packet storage. */
    first = managed_wptr & (MANAGED_RING_DWORDS - 1);
    count = MANAGED_RING_DWORDS - first;
    if (count > MANAGED_PACKET_DWORDS) count = MANAGED_PACKET_DWORDS;
    for (i = 0; i < count; i++) ((volatile ULONG *)managed_buf)[first + i] = packet[i];
    for (i = count; i < MANAGED_PACKET_DWORDS; i++)
        ((volatile ULONG *)managed_buf)[i - count] = packet[i];
    length = count * 4;
    CachePreDMA((APTR)(managed_buf + first), &length, DMA_ReadFromRAM);
    if (length != count * 4) return -EIO;
    if (count < MANAGED_PACKET_DWORDS) {
        length = (MANAGED_PACKET_DWORDS - count) * 4;
        CachePreDMA(managed_buf, &length, DMA_ReadFromRAM);
        if (length != (MANAGED_PACKET_DWORDS - count) * 4) return -EIO;
    }
    wptr = (volatile UQUAD *)((UBYTE *)managed_buf + 4104);
    *wptr = managed_wptr + 12;
    length = 8; CachePreDMA((APTR)wptr, &length, DMA_ReadFromRAM);
    if (length != 8) { *wptr = managed_wptr; return -EIO; }
    __sync_synchronize();
    managed_wptr += 12;
    managed_token = token;
    managed_pending_polls = 0;
    gc0_w(ad, regCP_RB0_WPTR, managed_wptr);
    gc0_w(ad, regCP_RB0_WPTR_HI, 0);
    /* No negative return after publication. Even a subsequent fault pins refs. */
    /* One success discriminator is sufficient. Storage wraps are an expected
     * consequence of the masked ring and were still producing one UART line
     * every few frames; faults and bounded pending diagnostics remain live. */
    if (!managed_publish_reported) {
        managed_publish_reported = TRUE;
        bug("[amdgpu-cs-ring] publish-v3 token=%08lx:%08lx "
            "ib=%08lx:%08lx/%lu wptr=%lu slot=%lu split=%lu epoch=%lu\n",
            (unsigned long)(token >> 32), (unsigned long)token,
            (unsigned long)(ib >> 32), (unsigned long)ib,
            (unsigned long)dwords, (unsigned long)managed_wptr,
            (unsigned long)first,
            (unsigned long)(MANAGED_PACKET_DWORDS - count),
            (unsigned long)(managed_wptr / MANAGED_RING_DWORDS));
    }
    return 0;
}

LONG gfx_v12_managed_retired(struct AmdgpuDevice *ad, UQUAD token)
{
    volatile UQUAD *fence;
    volatile ULONG *rptr;
    ULONG length, rp, state;
    if (!gfx_v12_managed_ready(ad)) return -ENODEV;
    if (!token || token != managed_token || !managed_identity(ad)) return -EIO;
    fence = (volatile UQUAD *)((UBYTE *)managed_buf + MANAGED_FENCE_OFFSET);
    rptr = (volatile ULONG *)((UBYTE *)managed_buf + 4096);
    length = 24; CachePostDMA((APTR)(fence - 1), &length, 0);
    if (length != 24 || fence[-1] != MANAGED_FENCE_GUARD || fence[1] != MANAGED_FENCE_GUARD)
        return -EIO;
    length = 4; CachePostDMA((APTR)rptr, &length, 0);
    if (length != 4) return -EIO;
    rp = gc0_r(ad, regCP_RB0_RPTR); state = gc0_r(ad, regCP_STAT);
    if (*fence != token || !managed_ring_drained(ad, rp, *rptr, state)) {
        /* One bounded diagnostic per slow submission, not sysdebug=all. */
        if (managed_pending_polls < 64 && ++managed_pending_polls == 64)
            bug("[amdgpu-cs-ring] pending64 token=%08lx:%08lx EOP=%08lx:%08lx RPTR=%lu WB=%lu WPTR=%lu slot=%lu CP_STAT=%08lx guards=PASS\n",
                (unsigned long)(token >> 32), (unsigned long)token,
                (unsigned long)(*fence >> 32), (unsigned long)*fence,
                (unsigned long)rp, (unsigned long)*rptr, (unsigned long)managed_wptr,
                (unsigned long)(managed_wptr & (MANAGED_RING_DWORDS - 1)), (unsigned long)state);
        return 0;
    }
    if (!managed_retire_reported) {
        managed_retire_reported = TRUE;
        bug("[amdgpu-cs-ring] retire token=%08lx:%08lx EOP=%08lx:%08lx "
            "RPTR=%lu WB=%lu WPTR=%lu idle=%08lx guards=PASS epoch=%lu\n",
            (unsigned long)(token >> 32), (unsigned long)token,
            (unsigned long)(*fence >> 32), (unsigned long)*fence,
            (unsigned long)rp, (unsigned long)*rptr,
            (unsigned long)managed_wptr, (unsigned long)state,
            (unsigned long)(managed_wptr / MANAGED_RING_DWORDS));
    }
    return 1;
}

/* First graphics-CP submission on GFX12: MES-mapped kernel graphics MQD,
 * MMIO WPTR delivery, and separate ME/PFP WRITE_DATA targets in GART RAM.
 * Require observable memory effects; a queue ACK or RPTR is insufficient.
 * Best-effort: failure returns FALSE without failing display device init. */
BOOL gfx_v12_cp_gfx_ring_bringup(struct AmdgpuDevice *ad)
{
    /* The CP reaches memory ONLY through GFXHUB VMID0, which has just its GART
     * page-table base programmed (no FB aperture) — so it can address GART VAs
     * (0x0..0x1FFFFFFF) but NOT raw VRAM MC. (SDMA's raw-MC ring works only because
     * MMHUB has the FB aperture.) A raw-VRAM ring makes the CP fetch GARBAGE: RPTR
     * advances but nothing executes — proven, SCRATCH_REG0 stayed at its seed. So
     * put the ring, the rptr-writeback and the fence all in ONE GART-mapped
     * system-RAM buffer. Names kept as _mc for downstream continuity; they are VAs. */
    static ULONG *cp_buf;            /* aligned 64 KiB: ring, writebacks, MQD */
    static APTR cp_allocation;
    static UQUAD  cp_va;
    UQUAD ring_mc, rptr_mc, fence_addr;
    volatile ULONG *ring_cpu, *fence_cpu;
    ULONG pkt[8], rb_cntl, rptr0, rptr1, fence_val = 0, base_rd, cc, i;
    BOOL retired = FALSE;

    amdgpu_gart_enable_gfxhub(ad);   /* CP fetches ring via GFXHUB (idempotent) */
    gc0_w(ad, regCP_DEBUG, gc0_r(ad, regCP_DEBUG) | CP_DEBUG__CPG_UTCL1_ERR_HALT_DIS);

    /* NOTE: MES/KIQ bring-up MOVED to AFTER config_gfx_rs64 + gfx_v12_mec_bringup +
     * GFXOFF/CGCG-disable below. The KIQ needs the MEC (compute front-end) already
     * running and GFXOFF off before it can service its ring — previously the KIQ ran
     * here (first), while the MEC was still halted, so MAP_QUEUES never completed. */

    /* Disable GFXOFF. On the PSP/PMFW-brought-up path the GFX block can come up with
     * GFXOFF armed, which power-gates it when idle: the CP front-end still advances
     * RPTR but every ME register/memory write silently drops -- exactly our symptom
     * (scratch + fence both dead). Linux's bookkeeping starts GFXOFF disallowed and
     * so never sends this; on our path we must say it explicitly. */
    {
        ULONG gv = 0, gst = amdgpu_smu_send_msg(ad, PPSMC_DisallowGfxOff, 0, &gv);
        bug("[amdgpu-gfx] SMU DisallowGfxOff(0x29) -> resp=0x%08lx (keep GFX powered)\n",
            (unsigned long)gst);
    }

    /* Disable GFX clock/power gating so the CP's WRITES COMMIT. On the PSP path the
     * block comes up with CGCG/CGLS gating the GRBM/GL2 write domain: reads pass but
     * WRITE_DATA/WR_CONFIRM never ACKs (CP_STAT stuck busy 0x80038000, fence never
     * lands). Mirror Linux's DIRECT-path RLC_CGCG_CGLS_CTRL=0, force the MGCG/CGCG
     * overrides ON, and disable PG + the SMU handshake (GFXOFF). RLC is already
     * running (PSP started it), so plain writes to these RLC regs take. */
    gc1_w(ad, regRLC_CGCG_CGLS_CTRL, 0);
    gc1_w(ad, regRLC_CGTT_MGCG_OVERRIDE,
          gc1_r(ad, regRLC_CGTT_MGCG_OVERRIDE) | RLC_CGTT_MGCG_OVERRIDE_ALL);
    gc1_w(ad, regRLC_PG_CNTL, RLC_PG_CNTL_SMU_HANDSHAKE_DISABLE);
    bug("[amdgpu-gfx] GFX CG/PG disabled: CGCG_CTRL=0x%08lx MGCG_OVR=0x%08lx PG_CNTL=0x%08lx\n",
        (unsigned long)gc1_r(ad, regRLC_CGCG_CGLS_CTRL),
        (unsigned long)gc1_r(ad, regRLC_CGTT_MGCG_OVERRIDE),
        (unsigned long)gc1_r(ad, regRLC_PG_CNTL));

    /* Verify + ensure the RLC F32 microengine is RUNNING. Decoding the wedge
     * (CP_STAT=0x80038000) shows ME_BUSY with RCIU_BUSY/DMA_BUSY/UTCL2IU_BUSY all
     * CLEAR — the ME is not mid-flight on a register write, a memory write, or a VM
     * translation. It never reaches the write opcode: it spins in its own firmware in
     * a wait-loop BEFORE executing, most likely on the RLC<->CP handshake it does
     * when the first command asserts GUI_ACTIVE. If the RLC F32 is not servicing, that
     * handshake never completes. PSP autoload should start the RLC, but our CG/PG pokes
     * may have knocked it out; mirror Linux gfx_v12_0_rlc_start: RLC_CNTL.RLC_ENABLE_F32=1.
     * (SRM enable removed: it needs a valid CSIB first and did not change the wedge.) */
    {
        ULONG rc = gc1_r(ad, regRLC_CNTL);
        bug("[amdgpu-gfx] RLC pre-CP: RLC_CNTL=0x%08lx (F32_EN=%lu) RLC_STAT=0x%08lx GPM_STAT=0x%08lx\n",
            (unsigned long)rc, (unsigned long)(rc & RLC_CNTL__RLC_ENABLE_F32),
            (unsigned long)gc1_r(ad, regRLC_STAT), (unsigned long)gc1_r(ad, regRLC_GPM_STAT));
        if (!(rc & RLC_CNTL__RLC_ENABLE_F32)) {
            gc1_w(ad, regRLC_CNTL, rc | RLC_CNTL__RLC_ENABLE_F32);
            { volatile ULONG d; for (d = 0; d < 200000; d++) ; }   /* ~udelay(50) */
            bug("[amdgpu-gfx] RLC F32 was OFF -> enabled: RLC_CNTL=0x%08lx\n",
                (unsigned long)gc1_r(ad, regRLC_CNTL));
        }
    }

    /* GRBM_CNTL.READ_TIMEOUT = 0xff — the FIRST thing Linux gfx_v12_0_constants_init
     * does, which we skipped. Without it a GC register read to a block that does not
     * respond HANGS FOREVER; with it the read TIMES OUT and returns. The RS64 MES
     * firmware stalls mid-SET_HW_RESOURCES on exactly such a read (disasm: a deterministic
     * `ld` at 0xf00064d8 to a per-queue GC register bank, no VM fault) — a read timeout
     * lets the MES's register read complete instead of wedging its RISC-V core. */
    {
        ULONG gc = gc0_r(ad, regGRBM_CNTL);
        gc = (gc & ~GRBM_CNTL__READ_TIMEOUT_MASK) | 0xFFUL;
        gc0_w(ad, regGRBM_CNTL, gc);
        bug("[amdgpu-gfx] GRBM_CNTL.READ_TIMEOUT=0xff set (constants_init) -> readback=0x%08lx\n",
            (unsigned long)gc0_r(ad, regGRBM_CNTL));
    }

    /* Program SH_MEM_CONFIG for the graphics VMIDs — Linux gfx_v12_0_constants_init,
     * which our driver otherwise skips ENTIRELY. This defines how the ME/shaders
     * ADDRESS memory (ADDRESS_MODE_64 = flat GPUVM, UNALIGNED alignment, prefetch=3).
     * At reset the addressing mode is undefined, so the ME's data reads/writes route
     * into nowhere and never ACK — while the CP ring FETCHER (its own translation
     * path) still works. That is exactly our symptom: fetch advances RPTR, but any
     * ME memory op (even a pre-satisfied WAIT_REG_MEM read) wedges CP_STAT=0x80038000.
     * For VMID0 (our ring's VMID) SH_MEM_BASES stays at reset(0). Select each VMID
     * via GRBM_GFX_CNTL[VMID], then write its SH_MEM_CONFIG. */
    {
        ULONG v;
        for (v = 0; v < 16; v++) {
            gc1_w(ad, regGRBM_GFX_CNTL, (v << GRBM_GFX_CNTL_VMID_SHIFT));
            gc1_w(ad, regSH_MEM_CONFIG, DEFAULT_SH_MEM_CONFIG);
        }
        gc1_w(ad, regGRBM_GFX_CNTL, 0);            /* restore VMID0 selection */
        bug("[amdgpu-gfx] SH_MEM_CONFIG=0x%08lx programmed for VMID0..15 (constants_init gap); "
            "readback VMID0=0x%08lx\n",
            (unsigned long)DEFAULT_SH_MEM_CONFIG,
            (unsigned long)gc1_r(ad, regSH_MEM_CONFIG));
    }

    if (ad->gart_ready && !cp_buf) {
        cp_buf = gfx_alloc_queue_buffer(ad, &cp_allocation, &cp_va);
    }
    if (!cp_buf || !cp_va) {
        bug("[amdgpu-gfx] CP bring-up skipped — no GART ring buffer (a raw-VRAM ring "
            "is unreachable by GFXHUB and fetches garbage)\n");
        return FALSE;
    }
    {
        /* CP_RB0_BASE is addr>>8, so the ring MUST be 256-byte aligned; AllocMem is
         * only 8-byte aligned, so round the ring up within the 16 KiB buffer. */
        ULONG a = (ULONG)((256 - (cp_va & 255)) & 255);
        ring_mc  = cp_va + a;         /* CP_RB0_BASE — 256-aligned GART VA */
        ring_cpu = (volatile ULONG *)((UBYTE *)cp_buf + a);
    }
    rptr_mc    = cp_va + 4096;     /* rptr writeback (own page) */
    fence_addr = cp_va + 8192;     /* WRITE_DATA fence (own page) */
    fence_cpu  = (volatile ULONG *)((UBYTE *)cp_buf + 8192);
    bug("[amdgpu-gfx] CP ring/rptr/fence in system RAM: cpu %p base VA 0x%08lx%08lx\n",
        cp_buf, (unsigned long)(cp_va >> 32), (unsigned long)(ULONG)cp_va);

    if (!gfx_v12_init_csb(ad, (ULONG *)((UBYTE *)cp_buf + GFX12_CSB_OFFSET),
                         cp_va + GFX12_CSB_OFFSET)) {
        bug("[amdgpu-gfx] CP bring-up stopped: CSIB/SRM setup unconfirmed\n");
        return FALSE;
    }

    if (!gfx_v12_config_gfx_rs64(ad)) {
        bug("[amdgpu-gfx] CP bring-up skipped — RS64 config failed\n"); return FALSE; }

    /* *** DOORBELL ROUTING — THE MISSING PIECE *** Program the S2A doorbell entry that
     * ROUTES a rung GFX/compute/MES doorbell to the on-die engine monitor. This is
     * Linux nbif_v6_3_1_gc_doorbell_init(), called from gfx_v12_0_hw_init — we never did
     * it. RCC_DOORBELL_APER_EN (set in nbio) only lets the CPU WRITE the doorbell BAR;
     * the S2A_DOORBELL_ENTRY_0 routing is what delivers that write to the engine. Without
     * it the doorbell lands in BAR2 but wakes NOTHING (measured: ringing db0x16 left the
     * MES PC unchanged, MEC compute queue never consumed). Confirmed in the Linux reg
     * trace: nbio_seg[2]+0x1cb (=abs 0xeeb) <- 0x30000007 (regGDC_S2A0_S2A_DOORBELL_
     * ENTRY_0_CTRL, BASE_IDX 2, the fixed GFX-range routing value). */
    if (ad->nbio_seg[2]) {
        /* Linux nbif_v6_3_1_gc_doorbell_init programs BOTH entries: ENTRY_0 (0x30000007)
         * and ENTRY_3 (0x3000000d). ENTRY_0 alone left the compute doorbell (index 0x08)
         * unrouted — the CP never saw the ring. Set both, exactly as Linux. */
        amdgpu_wreg32(ad, ad->nbio_seg[2] + 0x01cbUL, 0x30000007UL);
        amdgpu_wreg32(ad, ad->nbio_seg[2] + 0x01ceUL, 0x3000000dUL);
        bug("[amdgpu-gfx] *** GC DOORBELL ROUTING: S2A_ENTRY_0 @0x%05lx=0x%08lx "
            "S2A_ENTRY_3 @0x%05lx=0x%08lx (both routed, matching Linux) ***\n",
            (unsigned long)(ad->nbio_seg[2] + 0x01cbUL),
            (unsigned long)amdgpu_rreg32(ad, ad->nbio_seg[2] + 0x01cbUL),
            (unsigned long)(ad->nbio_seg[2] + 0x01ceUL),
            (unsigned long)amdgpu_rreg32(ad, ad->nbio_seg[2] + 0x01ceUL));
    }

    /* Bring up the MEC (compute microengine) — previously "deferred" (unimplemented).
     * The KIQ/compute front-end needs it running; codex's audit found it missing. */
    gfx_v12_mec_bringup(ad);

    /* MEC COMPUTE-QUEUE DISPATCH TEST — the doorbell-less / MES-less path radv needs.
     * Runs BEFORE the MES bring-up: if a directly-installed compute queue on the running
     * MEC executes a PM4 fence via MMIO wptr (like SDMA), radv has a live dispatch path
     * and does NOT need the MES scheduler at all. This is the decisive test. */
    /* FIRST compute install of the boot = the real shader DISPATCH_DIRECT, so it
     * claims this boot's ~1/7 queue0-scan window on the proven cq_install path
     * (the WRITE_DATA mec-test below is the baseline/second attempt). */
    gfx_v12_compute_dispatch_test(ad);
    gfx_v12_mec_compute_test(ad);

    /* REUSABLE-SUBMIT SELF-TEST — the radv cs_submit primitive. Prove TWO back-to-back
     * dispatches on the persistent queue (gfx_v12_compute_submit re-runs the proven install
     * each time). Additive: runs AFTER the known-good one-shot, does not touch it. */
    if (gfx_v12_compute_queue_ensure(ad)) {
        volatile ULONG *fc = gfx_v12_compute_fence_cpu();
        UQUAD fva = gfx_v12_compute_fence_va();
        ULONG pkt[8], nd, t, l, r1 = 0, r2 = 0;
        *fc = 0;
        nd = cq_pm4_write_fence(pkt, fva, 0xA1A1A1A1UL);
        gfx_v12_compute_submit(ad, pkt, nd);
        for (t = 0; t < 3000000; t++) { l = 4; CachePostDMA((APTR)fc, &l, 0); if (*fc == 0xA1A1A1A1UL) break; }
        r1 = *fc;
        *fc = 0;
        nd = cq_pm4_write_fence(pkt, fva, 0xB2B2B2B2UL);
        gfx_v12_compute_submit(ad, pkt, nd);
        for (t = 0; t < 3000000; t++) { l = 4; CachePostDMA((APTR)fc, &l, 0); if (*fc == 0xB2B2B2B2UL) break; }
        r2 = *fc;
        bug("[amdgpu-cq] *** REUSE SELF-TEST: submit1=0x%08lx (want A1A1A1A1) submit2=0x%08lx "
            "(want B2B2B2B2) -> %s ***\n", (unsigned long)r1, (unsigned long)r2,
            (r1 == 0xA1A1A1A1UL && r2 == 0xB2B2B2B2UL) ? "REUSABLE SUBMIT WORKS (radv can ride)"
            : (r1 == 0xA1A1A1A1UL) ? "1st OK, 2nd FAILED (reuse broken)" : "1st FAILED");
    }

    gfx_v12_me_snapshot(ad, "after-compute-before-MES");

    /* Linux's async graphics path enables the CP before MES queue mapping.
     * The ring remains empty; do not reset either engine after the handoff. */
    gfx_v12_me_snapshot(ad, "before-unhalt-for-MES");
    cc = gc1_r(ad, regCP_ME_CNTL);
    gc1_w(ad, regCP_ME_CNTL, cc & ~(CP_ME_CNTL__PFP_HALT | CP_ME_CNTL__ME_HALT));
    for (i = 0; i < 100000; i++) if (gc0_r(ad, regCP_STAT) == 0) break;
    bug("[amdgpu-me] CP enabled before MES: CNTL=%08lx CP_STAT=%08lx polls=%lu\n",
        (unsigned long)gc1_r(ad, regCP_ME_CNTL), (unsigned long)gc0_r(ad, regCP_STAT), (unsigned long)i);
    gfx_v12_me_snapshot(ad, "after-unhalt-before-MES");

    /* Keep compute's already proven scheduler setup, then append an actual
     * graphics legacy-queue mapping. An ACK is a prerequisite, not a fence pass. */
    {
        BOOL mes_ok = mes_v12_enable(ad);
        if (mes_ok) mes_ok = mes_v12_scheduler_bringup(ad);
        if (!mes_ok) {
            bug("[amdgpu-me] graphics skipped: scheduler setup did not acknowledge both frames\n");
            return FALSE;
        }
    }
    gfx_v12_me_snapshot(ad, "after-MES-before-graphics");

    for (i = 0; i < 16; i++) ring_cpu[i] = 0;            /* clear ring head */
    *(volatile UQUAD *)((UBYTE *)cp_buf + 4096) = 0;   /* rptr writeback slot */
    *(volatile UQUAD *)((UBYTE *)cp_buf + 4104) = 0;   /* distinct wptr slot */
    *fence_cpu = 0;

    /* Restore GRBM_GFX_INDEX to broadcast (SE|SA|INSTANCE) — POST/GOP may have left
     * it selecting one SE/instance, which can stall the ME's GRBM-routed writes. */
    gc1_w(ad, regGRBM_GFX_INDEX, GRBM_GFX_INDEX_BROADCAST);
    gc1_w(ad, regGRBM_GFX_CNTL, 0);
    /* No forced ACTIVE=0/1 or manual register descriptor load. MES consumes the
     * aligned MQD and owns installation. Refuse a pre-existing active queue. */
    {
        UQUAD mqd_va = cp_va + 16384;
        volatile ULONG *mqd = (volatile ULONG *)((UBYTE *)cp_buf + 16384);
        ULONG active = gc0_r(ad, regCP_GFX_HQD_ACTIVE), l;
        if (active != 0) {
            bug("[amdgpu-me] GFX map refused: pre-existing HQD_ACTIVE=%08lx\n", (unsigned long)active);
            return FALSE;
        }
        if (!gfx12_build_gfx_mqd(mqd, mqd_va, ring_mc, rptr_mc, cp_va + 4104)) {
            bug("[amdgpu-me] GFX map refused: invalid MQD/ring/writeback address encoding\n");
            return FALSE;
        }
        l = GFX_QUEUE_BYTES; CachePreDMA((APTR)cp_buf, &l, DMA_ReadFromRAM);
        gfx_v12_me_snapshot(ad, "before-graphics-MES-map");
        if (!gfx_v12_mes_map_graphics(ad, mqd_va, cp_va + 4104)) return FALSE;
        active = gc0_r(ad, regCP_GFX_HQD_ACTIVE);
        base_rd = gc0_r(ad, regCP_GFX_HQD_BASE);
        rb_cntl = gc0_r(ad, regCP_GFX_HQD_CNTL);
        bug("[amdgpu-me] mapped GFX HQD: ACTIVE=%08lx BASE=%08lx:%08lx CNTL=%08lx MQD=%08lx:%08lx legacy_BASE=%08lx\n",
            (unsigned long)active, (unsigned long)gc0_r(ad, regCP_GFX_HQD_BASE_HI), (unsigned long)base_rd,
            (unsigned long)rb_cntl, (unsigned long)gc0_r(ad, regCP_GFX_MQD_BASE_ADDR_HI),
            (unsigned long)gc0_r(ad, regCP_GFX_MQD_BASE_ADDR), (unsigned long)gc0_r(ad, regCP_RB0_BASE));
        if (active != 1 || base_rd != (ULONG)(ring_mc >> 8) ||
            gc0_r(ad, regCP_GFX_HQD_BASE_HI) != (ULONG)(ring_mc >> 40) ||
            gc0_r(ad, regCP_GFX_MQD_BASE_ADDR) != (ULONG)mqd_va ||
            gc0_r(ad, regCP_GFX_MQD_BASE_ADDR_HI) != (ULONG)(mqd_va >> 32) ||
            gc0_r(ad, regCP_GFX_HQD_VMID) != 0 ||
            !gfx12_diag_hqd_cntl_valid(mqd[145], rb_cntl)) {
            bug("[amdgpu-me] GFX map readback differs from descriptor; no graphics submit\n");
            return FALSE;
        }
        bug("[amdgpu-me] diagnostic identity accepted: VMID=0 CNTL requested=%08lx actual=%08lx delta=%08lx; control not rewritten\n",
            (unsigned long)mqd[145], (unsigned long)rb_cntl,
            (unsigned long)(mqd[145] ^ rb_cntl));
    }
    gc0_w(ad, regCP_MAX_CONTEXT, 7);
    gc0_w(ad, regCP_DEVICE_ID, 1);
    gfx_v12_me_snapshot(ad, "after-graphics-MES-map-before-submit");

    /* Diagnostic engine-select comparison: the same memory write is routed
     * once to ME and once to PFP. This is not Linux's scratch-register test. */
    {
        ULONG scr_val, st, st_nop, pfp0, me0, pfp1, me1;
        gc1_w(ad, regSCRATCH_REG0, 0xCAFEDEADUL);   /* seed a non-magic value */
        /* Snapshot PCs and start registers without treating an unchanged PC as
         * proof of a wedge; a service loop can return to the same instruction.
         * Start readback should match (hi<<30)|(lo>>2) from configuration. */
        bug("[amdgpu-gfx]     PRGRM_CNTR readback: PFP_START=%08lx:%08lx ME_START=%08lx:%08lx\n",
            (unsigned long)gc0_r(ad, regCP_PFP_PRGRM_CNTR_START_HI),
            (unsigned long)gc0_r(ad, regCP_PFP_PRGRM_CNTR_START),
            (unsigned long)gc0_r(ad, regCP_ME_PRGRM_CNTR_START_HI),
            (unsigned long)gc0_r(ad, regCP_ME_PRGRM_CNTR_START));
        /* The one asymmetric per-core resource: PFP uses IC/DC_BASE0, ME uses
         * IC/DC_BASE1. PFP executes; ME does not. If the ME's IC or DC base is
         * zero/stale while the PFP's is valid, the ME boots but reads a bad data
         * segment and spins in init, never draining the MEQ. Compare them. */
        bug("[amdgpu-gfx]     PFP IC=%08lx:%08lx DC0=%08lx:%08lx | ME IC=%08lx:%08lx DC1=%08lx:%08lx\n",
            (unsigned long)gc1_r(ad, regCP_PFP_IC_BASE_HI), (unsigned long)gc1_r(ad, regCP_PFP_IC_BASE_LO),
            (unsigned long)gc1_r(ad, regCP_GFX_RS64_DC_BASE0_HI), (unsigned long)gc1_r(ad, regCP_GFX_RS64_DC_BASE0_LO),
            (unsigned long)gc1_r(ad, regCP_ME_IC_BASE_HI), (unsigned long)gc1_r(ad, regCP_ME_IC_BASE_LO),
            (unsigned long)gc1_r(ad, regCP_GFX_RS64_DC_BASE1_HI), (unsigned long)gc1_r(ad, regCP_GFX_RS64_DC_BASE1_LO));
        pfp0 = gc1_r(ad, regCP_GFX_RS64_INSTR_PNTR0);
        me0  = gc1_r(ad, regCP_GFX_RS64_INSTR_PNTR1);
        /* WRITE_DATA ENGINE_SEL is bit30: ME=0, PFP=1. Distinct CPU-observable
         * targets separate execution effects from the front-end's ring fetch. */
        /* Keep the already configured/unhalted ME intact for the packet test.
         * The former late ME-only reset/prime was an experiment, not part of
         * Linux's normal PSP queue-start sequence. Native boot4 still failed
         * after it, with ME_EXC=c122. Omitting that extra reset is a bounded
         * discriminator, not a claim that reset caused the exception. Initial
         * firmware-start resets above are unchanged, as are both packets. */
        gfx_v12_me_snapshot(ad, "before-engine-packets-no-late-reset");
        *fence_cpu = 0;                                    /* slot A: ME target */
        *(volatile ULONG *)((UBYTE *)cp_buf + 12288) = 0;  /* slot B: PFP target */
        { ULONG l0 = 4; CachePreDMA((APTR)fence_cpu, &l0, DMA_ReadFromRAM); }
        { ULONG l0 = 4; CachePreDMA((APTR)((UBYTE *)cp_buf + 12288), &l0, DMA_ReadFromRAM); }
        ring_cpu[0] = PACKET3(PACKET3_WRITE_DATA, 3);      /* ME write -> slot A */
        ring_cpu[1] = WRITE_DATA_DST_SEL_MEM;              /* engine=ME(0), no WR_CONFIRM */
        ring_cpu[2] = (ULONG)(fence_addr & 0xFFFFFFFCUL);
        ring_cpu[3] = (ULONG)(fence_addr >> 32);
        ring_cpu[4] = CP_FENCE_MAGIC;
        ring_cpu[5] = PACKET3(PACKET3_WRITE_DATA, 3);      /* PFP write -> slot B */
        ring_cpu[6] = WRITE_DATA_DST_SEL_MEM | (1UL << 30);/* engine=PFP(1) */
        ring_cpu[7] = (ULONG)((cp_va + 12288) & 0xFFFFFFFCUL);
        ring_cpu[8] = (ULONG)((cp_va + 12288) >> 32);
        ring_cpu[9] = CP_FENCE_MAGIC;
        { ULONG l = 10 * 4; CachePreDMA((APTR)ring_cpu, &l, DMA_ReadFromRAM); }
        *(volatile UQUAD *)((UBYTE *)cp_buf + 4104) = 10;
        { ULONG l = 8; CachePreDMA((APTR)((UBYTE *)cp_buf + 4104), &l, DMA_ReadFromRAM); }
        gc0_w(ad, regCP_RB0_WPTR, 10);
        gc0_w(ad, regCP_RB0_WPTR_HI, 0);
        { volatile ULONG d; for (d = 0; d < 4000000; d++) ; }
        st = gc0_r(ad, regCP_STAT);
        { ULONG lf = 4; CachePostDMA((APTR)fence_cpu, &lf, 0); }
        { ULONG lf = 4; CachePostDMA((APTR)((UBYTE *)cp_buf + 12288), &lf, 0); }
        scr_val = *fence_cpu;                              /* ME fence (slot A) */
        st_nop  = *(volatile ULONG *)((UBYTE *)cp_buf + 12288);  /* PFP fence (slot B) */
        pfp1 = gc1_r(ad, regCP_GFX_RS64_INSTR_PNTR0);
        me1  = gc1_r(ad, regCP_GFX_RS64_INSTR_PNTR1);
        {
            ULONG rptr_reg = gc0_r(ad, regCP_RB0_RPTR);
            ULONG lr = 4; ULONG rptr_wb;
            CachePostDMA((APTR)((UBYTE *)cp_buf + 4096), &lr, 0);
            rptr_wb = *(volatile ULONG *)((UBYTE *)cp_buf + 4096);
            bug("[amdgpu-gfx] *** ENGINE TEST: ME_fence=0x%08lx PFP_fence=0x%08lx (want 0x%08lx) "
                "RPTR=0x%08lx CP_STAT=0x%08lx rptr_wb=0x%08lx -> %s ***\n",
                (unsigned long)scr_val, (unsigned long)st_nop, (unsigned long)CP_FENCE_MAGIC,
                (unsigned long)rptr_reg, (unsigned long)st, (unsigned long)rptr_wb,
                (scr_val == CP_FENCE_MAGIC && st_nop == CP_FENCE_MAGIC) ? "BOTH engines execute!"
                : (st_nop == CP_FENCE_MAGIC) ? "PFP write observed; ME write absent"
                : (scr_val == CP_FENCE_MAGIC) ? "ME executes, PFP does NOT"
                : "NEITHER engine executes command effects");
        }
        bug("[amdgpu-gfx]     RS64 INSTR_PNTR: pipe0 %08lx->%08lx pipe1 %08lx->%08lx GRBM_STATUS=%08lx "
            "(PC samples alone do not prove execution or a stall)\n",
            (unsigned long)pfp0, (unsigned long)pfp1, (unsigned long)me0, (unsigned long)me1,
            (unsigned long)gc0_r(ad, regGRBM_STATUS));
        /* State samples supplement command effects. Native CSIB success still
         * has EXCEPTION_STATUS=c122, so nonzero alone is not a fatal-fault oracle. */
        bug("[amdgpu-gfx]     ME-WEDGE: ME_HEADER_DUMP=0x%08lx CPF_STATUS=0x%08lx CPC_STATUS=0x%08lx "
            "CPF_BUSY=0x%08lx PFP_RS64_EXC=0x%08lx GFX_RS64_INT0=0x%08lx\n",
            (unsigned long)gc0_r(ad, regCP_ME_HEADER_DUMP),
            (unsigned long)gc0_r(ad, regCP_CPF_STATUS),
            (unsigned long)gc0_r(ad, regCP_CPC_STATUS),
            (unsigned long)gc0_r(ad, regCP_CPF_BUSY_STAT),
            (unsigned long)gc1_r(ad, regCP_PFP_RS64_EXCEPTION_STATUS),
            (unsigned long)gc1_r(ad, regCP_GFX_RS64_INTERRUPT0));
        gfx_v12_me_snapshot(ad, "after-graphics-engine-test");
        if (scr_val != CP_FENCE_MAGIC || st_nop != CP_FENCE_MAGIC) return FALSE;
        if (!gfx_v12_gfx_repeat_test(ad, cp_buf, cp_va)) return FALSE;
        managed_ad = ad; managed_buf = cp_buf; managed_va = cp_va;
        managed_wptr = 10 + GFX_REPEAT_COUNT * 10;
        bug("[amdgpu-cs-ring] managed-gfx-v1 ready after retained 32-pair gate; wptr=%lu\n",
            (unsigned long)managed_wptr);
        return TRUE;
    }

    /* FENCE-READ TEST: the ME is proven to execute (WAIT_REG_MEM stalled the CP). So
     * does its memory WRITE actually land? Submit ONLY a WRITE_DATA fence to a GART
     * slot that is CachePreDMA-prepared for a device write (so a real write is
     * coherently visible to the CPU), then read it back. Landing => the CP is
     * FUNCTIONAL and the earlier "fence=0" was a stale read. */
    {
        UQUAD f_va = cp_va + 12288;
        volatile ULONG *f_cpu = (volatile ULONG *)((UBYTE *)cp_buf + 12288);
        ULONG got, st;
        *f_cpu = 0;
        { ULONG l = 4; CachePreDMA((APTR)f_cpu, &l, 0); }         /* device will WRITE here */
        /* RELEASE_MEM with GL2 writeback (Linux gfx_v12_0_ring_emit_fence). WRITE_DATA
         * left the value in GL2 and hung on WR_CONFIRM; RELEASE_MEM flushes GL2->mem
         * (CACHE_FLUSH_AND_INV_TS event) then writes the fence, so the CPU sees it. */
        pkt[0] = PACKET3(PACKET3_RELEASE_MEM, 6);
        pkt[1] = 0x14 | (5UL << 8) | (1UL << 21) | (1UL << 22) | (3UL << 25);
             /* EVENT_TYPE=CACHE_FLUSH_AND_INV_TS(0x14) | EVENT_INDEX(5) | GCR_GL2_WB | GCR_SEQ | CACHE_POLICY(3) */
        pkt[2] = (1UL << 29);                     /* DATA_SEL=1 (write low 32 bits), INT_SEL=0 */
        pkt[3] = (ULONG)(f_va & 0xFFFFFFFCUL);
        pkt[4] = (ULONG)(f_va >> 32);
        pkt[5] = CP_FENCE_MAGIC;                  /* data lo */
        pkt[6] = 0;                               /* data hi */
        pkt[7] = 0;                               /* int ctxid */
        for (i = 0; i < 8; i++) ring_cpu[i] = pkt[i];
        { ULONG l = 8 * 4; CachePreDMA((APTR)ring_cpu, &l, DMA_ReadFromRAM); }  /* device READS ring */
        gc0_w(ad, regCP_RB0_WPTR, 8);
        gc0_w(ad, regCP_RB0_WPTR_HI, 0);
        { volatile ULONG d; for (d = 0; d < 2000000; d++) ; }
        got = *f_cpu;
        st  = gc0_r(ad, regCP_STAT);
        bug("[amdgpu-gfx] *** FENCE-READ TEST: fence=0x%08lx (want 0x600DF00D) RPTR=0x%08lx "
            "CP_STAT=0x%08lx -> %s ***\n", (unsigned long)got,
            (unsigned long)gc0_r(ad, regCP_RB0_RPTR), (unsigned long)st,
            got == CP_FENCE_MAGIC ? "MEMORY WRITE LANDS -- CP FUNCTIONAL!"
                                  : "write did NOT land (ME writes still dropping)");
        {
            ULONG fst = gc0_r(ad, regGCVM_L2_PROTECTION_FAULT_STATUS_LO32);
            bug("[amdgpu-gfx] L2 FAULT_STATUS=0x%08lx (walker=%lx perm=%lx map=%lx RW=%lx vmid=%lx) "
                "fault_addr=%08lx:%08lx (want fence VA %08lx:%08lx)\n", (unsigned long)fst,
                (unsigned long)((fst >> 1) & 0x7), (unsigned long)((fst >> 4) & 0xf),
                (unsigned long)((fst >> 8) & 0x1), (unsigned long)((fst >> 18) & 0x1),
                (unsigned long)((fst >> 20) & 0xf),
                (unsigned long)gc0_r(ad, regGCVM_L2_PROTECTION_FAULT_ADDR_HI32),
                (unsigned long)gc0_r(ad, regGCVM_L2_PROTECTION_FAULT_ADDR_LO32),
                (unsigned long)(f_va >> 32), (unsigned long)(ULONG)f_va);
        }
        return (got == CP_FENCE_MAGIC);
    }

    rptr0 = gc0_r(ad, regCP_RB0_RPTR);

    /* Two packets in one ring:
     *  1) SET_UCONFIG_REG scratch write — proves the CP EXECUTES packets, no VM.
     *  2) WRITE_DATA memory fence — exercises the GFXHUB VM write path.
     * The packet offset for SET_UCONFIG_REG is the reg's RREG32 dword offset minus
     * 0xC000 (CP uconfig base); base-independent, computed from gc_seg[1]. */
    gc1_w(ad, regSCRATCH_REG0, 0xCAFEDEADUL);   /* seed a non-magic value */
    {
        ULONG scr_off = (ad->gc_seg[1] + regSCRATCH_REG0) - PACKET3_SET_UCONFIG_REG_START;
        pkt[0] = PACKET3(PACKET3_SET_UCONFIG_REG, 1);
        pkt[1] = scr_off;
        pkt[2] = CP_SCRATCH_MAGIC;
        pkt[3] = PACKET3(PACKET3_WRITE_DATA, 3);
        pkt[4] = WRITE_DATA_DST_SEL_MEM | WRITE_DATA_WR_CONFIRM;
        pkt[5] = (ULONG)(fence_addr & 0xFFFFFFFCUL);
        pkt[6] = (ULONG)(fence_addr >> 32);
        pkt[7] = CP_FENCE_MAGIC;
    }
    for (i = 0; i < 8; i++) ring_cpu[i] = pkt[i];   /* packets to the system-RAM ring */
    /* gart_map flushed the buffer at map time (when it was zeros); these CPU writes
     * are newer, so flush them to RAM or the CP fetches the stale zeros — which is
     * exactly "RPTR advances, SCRATCH never changes." */
    { ULONG l = 8 * 4; CachePreDMA((APTR)ring_cpu, &l, DMA_ReadFromRAM); }
    gc0_w(ad, regCP_RB0_WPTR, 8);      /* CP wptr in DWORDS (not byte<<2 like SDMA) */
    gc0_w(ad, regCP_RB0_WPTR_HI, 0);

    bug("[amdgpu-gfx] CP submitted SET_UCONFIG_REG(scratch)+WRITE_DATA (wptr=8), polling...\n");
    {
        ULONG scr_val = 0;
        for (i = 0; i < 200000; i++) {
            scr_val = gc1_r(ad, regSCRATCH_REG0);
            fence_val = *fence_cpu;
            if (scr_val == CP_SCRATCH_MAGIC && fence_val == CP_FENCE_MAGIC) { retired = TRUE; break; }
            if (scr_val == CP_SCRATCH_MAGIC) break;   /* CP alive; fence path may lag/fail */
        }
        bug("[amdgpu-gfx] CP scratch: SCRATCH_REG0=0x%08lx (want 0x%08lx) — %s\n",
            (unsigned long)scr_val, (unsigned long)CP_SCRATCH_MAGIC,
            scr_val == CP_SCRATCH_MAGIC ? "*** CP EXECUTES PACKETS ***"
                                        : "unchanged (CP not executing)");
    }
    rptr1 = gc0_r(ad, regCP_RB0_RPTR);
    bug("[amdgpu-gfx] CP RPTR %08lx -> %08lx fence=%08lx after %lu polls\n",
        (unsigned long)rptr0, (unsigned long)rptr1, (unsigned long)fence_val, (unsigned long)i);

    if (retired) {
        bug("[amdgpu-gfx] *** FIRST CP SUBMISSION RETIRED — the graphics command "
            "processor executed a PM4 packet we wrote. CP is WORKING. ***\n");
        return TRUE;
    }
    if (rptr1 != rptr0)
        bug("[amdgpu-gfx] CP FETCHED the ring (RPTR moved) but fence never landed — WRITE_DATA address problem\n");
    else
        bug("[amdgpu-gfx] CP RPTR never moved — PFP/ME not fetching (HALT/ucode/RB_ACTIVE)\n");
    return FALSE;
}


/* ---- GFX clock gating (late) ----------------------------------------------
 * Port of Linux gfx_v12_0_update_gfx_clock_gating(adev, true) for GC 12.0.0
 * (soc24.c cg_flags: GFX CGCG, CGLS, MGCG, 3D_CGCG, 3D_CGLS, REPEATER_FGCG,
 * FGCG, PERF_CLK). Linux enables this from late_init, after every IP block is
 * up; amdgpu_gfx_hw_init disables gating on purpose because the CP's writes did
 * not commit with gating on during bring-up, so this runs only once the IP
 * ladder is complete.
 *
 * Why it matters: without it the CP fetcher's write-pointer poll keeps
 * GRBM_STATUS.GUI_ACTIVE set and the PMFW reports GFX 100% busy on an idle
 * desktop (CP_CPF_BUSY_STAT.HQD_WPTR_POLL_BUSY). CGCG gates the CP clock when
 * the RLC sees it idle.
 *
 * Opt-in with AMDGPU=cg on the kernel command line until it is proven on
 * hardware: it also enables the CP GUI-idle interrupts the RLC's gating FSM
 * uses, which this driver's IH ring does not consume. */
#define CG_regRLC_SAFE_MODE              0x0980   /* gc1 */
#define CG_RLC_SAFE_MODE_CMD             0x00000001UL
#define CG_RLC_SAFE_MODE_MESSAGE_SHIFT   1
#define CG_regRLC_CGCG_CGLS_CTRL_3D      0x4cc5   /* gc1 */
#define CG_regCP_RB_WPTR_POLL_CNTL       0x0f62   /* gc0 */
#define CG_regCP_INT_CNTL                0x1de9   /* gc0 */
#define CG_regCP_INT_CNTL_RING0          0x1e0a   /* gc0 */
#define CG_regSDMA0_RLC_CGCG_CTRL        0x0055   /* gc0 */
#define CG_OVR_RLC_REPEATER_FGCG         0x00000001UL
#define CG_OVR_RLC_CGTT_SCLK             0x00000002UL
#define CG_OVR_GFXIP_MGCG                0x00000004UL
#define CG_OVR_GFXIP_CGCG                0x00000008UL
#define CG_OVR_GFXIP_CGLS                0x00000010UL
#define CG_OVR_GRBM_CGTT_SCLK            0x00000020UL
#define CG_OVR_GFXIP_GFX3D_CG            0x00000080UL
#define CG_OVR_GFXIP_FGCG                0x00000100UL
#define CG_OVR_GFXIP_REPEATER_FGCG       0x00000200UL
#define CG_OVR_PERFMON_CLOCK_STATE       0x00000400UL
#define CG_CTRL_CGCG_EN                  0x00000001UL
#define CG_CTRL_CGLS_EN                  0x00000002UL
#define CG_CTRL_CGLS_DELAY_MASK          0x000000FCUL
#define CG_CTRL_CGLS_DELAY_SHIFT         2
#define CG_CTRL_IDLE_THRESHOLD_MASK      0x07FFFF00UL
#define CG_CTRL_IDLE_THRESHOLD_SHIFT     8
#define CG_INT_CMP_BUSY                  0x00040000UL
#define CG_INT_CNTX_BUSY                 0x00080000UL
#define CG_INT_CNTX_EMPTY                0x00100000UL
#define CG_INT_GFX_IDLE                  0x00200000UL
#define CG_SDMA_CGCG_INT_ENABLE          0x00000002UL

static BOOL cg_enter_safe_mode(struct AmdgpuDevice *ad)
{
    ULONG i;

    gc1_w(ad, CG_regRLC_SAFE_MODE,
          CG_RLC_SAFE_MODE_CMD | (1UL << CG_RLC_SAFE_MODE_MESSAGE_SHIFT));
    for (i = 0; i < 100000; i++) {          /* Linux: usec_timeout */
        if (!(gc1_r(ad, CG_regRLC_SAFE_MODE) & CG_RLC_SAFE_MODE_CMD))
            return TRUE;
    }
    return FALSE;
}

static void cg_exit_safe_mode(struct AmdgpuDevice *ad)
{
    gc1_w(ad, CG_regRLC_SAFE_MODE, CG_RLC_SAFE_MODE_CMD);
}

BOOL amdgpu_gfx_set_clock_gating(struct AmdgpuDevice *ad, BOOL enable)
{
    ULONG ovr0, ovr, ctl, ctl3d, v;
    const ULONG gui_idle = CG_INT_CNTX_BUSY | CG_INT_CNTX_EMPTY | CG_INT_CMP_BUSY |
                           CG_INT_GFX_IDLE;

    if (!ad || !ad->rmmio || !ad->gc_seg[0] || !ad->gc_seg[1])
        return FALSE;
    if (!cg_enter_safe_mode(ad)) {
        bug("[amdgpu-cg] RLC did not enter safe mode; clock gating unchanged\n");
        return FALSE;
    }
    ovr0 = gc1_r(ad, regRLC_CGTT_MGCG_OVERRIDE);

    if (enable) {
        /* coarse grain: unset the CGCG/CGLS/3D overrides, enable the FSMs */
        gc1_w(ad, regRLC_CGTT_MGCG_OVERRIDE,
              ovr0 & ~(CG_OVR_GFXIP_CGCG | CG_OVR_GFXIP_CGLS | CG_OVR_GFXIP_GFX3D_CG));

        ctl = gc1_r(ad, regRLC_CGCG_CGLS_CTRL);
        ctl &= ~(CG_CTRL_IDLE_THRESHOLD_MASK | CG_CTRL_CGLS_DELAY_MASK);
        ctl |= (0x36UL << CG_CTRL_IDLE_THRESHOLD_SHIFT) | CG_CTRL_CGCG_EN |
               (0xFUL << CG_CTRL_CGLS_DELAY_SHIFT) | CG_CTRL_CGLS_EN;
        gc1_w(ad, regRLC_CGCG_CGLS_CTRL, ctl);

        ctl3d = gc1_r(ad, CG_regRLC_CGCG_CGLS_CTRL_3D);
        ctl3d &= ~(CG_CTRL_IDLE_THRESHOLD_MASK | CG_CTRL_CGLS_DELAY_MASK);
        ctl3d |= (0x36UL << CG_CTRL_IDLE_THRESHOLD_SHIFT) | CG_CTRL_CGCG_EN |
                 (0xFUL << CG_CTRL_CGLS_DELAY_SHIFT) | CG_CTRL_CGLS_EN;
        gc1_w(ad, CG_regRLC_CGCG_CGLS_CTRL_3D, ctl3d);

        /* IDLE_POLL_COUNT 0x90, POLL_FREQUENCY 0x100; the two fields span the register */
        gc0_w(ad, CG_regCP_RB_WPTR_POLL_CNTL, (0x0090UL << 16) | 0x0100UL);

        gc0_w(ad, CG_regCP_INT_CNTL, gc0_r(ad, CG_regCP_INT_CNTL) | gui_idle);
        /* Linux also sets SDMA1's CGCG interrupt when a second SDMA exists; this
         * driver discovers only SDMA0, so only SDMA0 is touched. */
        v = gc0_r(ad, CG_regSDMA0_RLC_CGCG_CTRL);
        gc0_w(ad, CG_regSDMA0_RLC_CGCG_CTRL, v | CG_SDMA_CGCG_INT_ENABLE);

        /* medium grain, repeater/SRAM fine grain, perfmon clock: unset overrides */
        ovr = gc1_r(ad, regRLC_CGTT_MGCG_OVERRIDE);
        ovr &= ~(CG_OVR_GRBM_CGTT_SCLK | CG_OVR_RLC_CGTT_SCLK | CG_OVR_GFXIP_MGCG |
                 CG_OVR_GFXIP_REPEATER_FGCG | CG_OVR_RLC_REPEATER_FGCG |
                 CG_OVR_GFXIP_FGCG | CG_OVR_PERFMON_CLOCK_STATE);
        gc1_w(ad, regRLC_CGTT_MGCG_OVERRIDE, ovr);

        /* GUI idle interrupt on ME0 pipe0 (gfx_v12_0_enable_gui_idle_interrupt) */
        gc0_w(ad, CG_regCP_INT_CNTL_RING0, gc0_r(ad, CG_regCP_INT_CNTL_RING0) | gui_idle);
    } else {
        /* Linux's disable path: stop the coarse-grain FSMs, set the medium and
         * fine grain and perfmon overrides again, mask the GUI idle interrupt. */
        gc1_w(ad, regRLC_CGCG_CGLS_CTRL,
              gc1_r(ad, regRLC_CGCG_CGLS_CTRL) & ~(CG_CTRL_CGCG_EN | CG_CTRL_CGLS_EN));
        gc1_w(ad, CG_regRLC_CGCG_CGLS_CTRL_3D,
              gc1_r(ad, CG_regRLC_CGCG_CGLS_CTRL_3D) & ~(CG_CTRL_CGCG_EN | CG_CTRL_CGLS_EN));
        ovr = gc1_r(ad, regRLC_CGTT_MGCG_OVERRIDE);
        ovr |= CG_OVR_RLC_CGTT_SCLK | CG_OVR_GRBM_CGTT_SCLK | CG_OVR_GFXIP_MGCG |
               CG_OVR_GFXIP_REPEATER_FGCG | CG_OVR_RLC_REPEATER_FGCG |
               CG_OVR_GFXIP_FGCG | CG_OVR_PERFMON_CLOCK_STATE;
        gc1_w(ad, regRLC_CGTT_MGCG_OVERRIDE, ovr);
        gc0_w(ad, CG_regCP_INT_CNTL_RING0, gc0_r(ad, CG_regCP_INT_CNTL_RING0) & ~gui_idle);
    }

    cg_exit_safe_mode(ad);

    bug("[amdgpu-cg] GFX clock gating %s: MGCG_OVERRIDE %08lx -> %08lx, CGCG_CGLS_CTRL %08lx, "
        "3D %08lx, WPTR_POLL_CNTL %08lx, CP_INT_CNTL_RING0 %08lx\n",
        enable ? "ON" : "OFF",
        (unsigned long)ovr0, (unsigned long)gc1_r(ad, regRLC_CGTT_MGCG_OVERRIDE),
        (unsigned long)gc1_r(ad, regRLC_CGCG_CGLS_CTRL),
        (unsigned long)gc1_r(ad, CG_regRLC_CGCG_CGLS_CTRL_3D),
        (unsigned long)gc0_r(ad, CG_regCP_RB_WPTR_POLL_CNTL),
        (unsigned long)gc0_r(ad, CG_regCP_INT_CNTL_RING0));
    return TRUE;
}

LONG amdgpu_set_gfx_clock_gating(struct AmdgpuDevice *ad, ULONG enable)
{
    if (!ad || !ad->rmmio)
        return -ENODEV;
    return amdgpu_gfx_set_clock_gating(ad, enable ? TRUE : FALSE) ? 0 : -EIO;
}
