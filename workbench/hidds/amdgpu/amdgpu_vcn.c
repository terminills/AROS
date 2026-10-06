/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    VCN 5.0 video core bring-up: dynamic power gating start through the PSP
    indirect SRAM load, and the unified ring, after Linux vcn_v5_0_0.c.
*/

#include <aros/debug.h>

#include <proto/exec.h>
#include <exec/memory.h>
#include <hidd/amdgpu.h>
#include <errno.h>
#include <string.h>

#include "amdgpu_intern.h"
#include "asic_reg/vcn_5_0_0_excerpt.h"

#define PPSMC_MSG_PowerUpVcn        0x2A

#define VCN_VRAM_OFF                ((UQUAD)800 << 20)
#define VCN_FW_SHARED_OFF           0x00000
#define VCN_FW_SHARED_SIZE          0x01000
#define VCN_RING_OFF                0x01000
#define VCN_RING_SIZE               0x04000
#define VCN_STACK_OFF               0x10000
#define VCN_STACK_SIZE              (128 * 1024)
#define VCN_CONTEXT_OFF             (VCN_STACK_OFF + VCN_STACK_SIZE)
#define VCN_CONTEXT_SIZE            (512 * 1024)
#define VCN_SCRATCH_PAGES           3
#define VCN_SCRATCH_IB              0x0000
#define VCN_SCRATCH_MSG             0x1000
#define VCN_SCRATCH_FENCE           0x2000

#define VCN_IB_FLAG_DECODE_BUFFER   0x00000001
#define VCN_CMD_FLAG_MSG_BUFFER     0x00000001
#define VCN_DECODE_BUFFER_DW        33

#define VCN_FW_SHARED_FLAG_0_UNIFIED_QUEUE  (1 << 2)
#define VCN_FW_QUEUE_RING_RESET     1
#define VCN_FW_QUEUE_DPG_HOLD_OFF   2

#define VCN_ENC_CMD_NO_OP           0x00000000
#define VCN_ENC_CMD_END             0x00000001
#define VCN_ENC_CMD_IB              0x00000002
#define VCN_ENC_CMD_FENCE           0x00000003
#define VCN_ENC_CMD_TRAP            0x00000004
#define VCN_RING_ALIGN_DW           64

#define AMDGPU_VRAM_MC_BASE         0x8000000000ULL

static ULONG vcn_reg(struct AmdgpuDevice *ad, ULONG base_idx, ULONG reg)
{
    return ad->vcn_seg[base_idx] + reg;
}

#define RREG(r)       amdgpu_rreg32(ad, vcn_reg(ad, r##_BASE_IDX, r))
#define WREG(r, v)    amdgpu_wreg32(ad, vcn_reg(ad, r##_BASE_IDX, r), (v))
#define WREG_P(r, v, keep) \
    do { \
        ULONG _o = vcn_reg(ad, r##_BASE_IDX, r); \
        amdgpu_wreg32(ad, _o, (amdgpu_rreg32(ad, _o) & (keep)) | ((v) & ~(keep))); \
    } while (0)

static void vcn_spin(ULONG loops)
{
    volatile ULONG i = 0;
    while (i < loops)
        i++;
}

static void vcn_fw_shared_byte(struct AmdgpuDevice *ad, ULONG off, UBYTE v, BOOL set)
{
    UQUAD pos = VCN_VRAM_OFF + VCN_FW_SHARED_OFF + (off & ~3UL);
    ULONG dw;
    ULONG shift = (off & 3) * 8;

    amdgpu_vram_read(ad, pos, &dw, 1);
    if (set)
        dw |= (ULONG)v << shift;
    else
        dw &= ~((ULONG)v << shift);
    amdgpu_vram_write(ad, pos, &dw, 1);
}

#define VCN_SRAM_OFF                0x08000
#define VCN_SRAM_MAX_DW             128
#define PSP_FW_TYPE_VCN0_RAM        49

#define VCN_DOORBELL_INDEX          ((0x188 << 1) + 2)
#define NBIF_S2A_DOORBELL_ENTRY_4   0x01cf

#define VCN_VID_SOC_ADDRESS         0x1FC00
#define VCN_AON_SOC_ADDRESS         0x1F800
#define VCN_VID_IP_ADDRESS          0x0
#define VCN_AON_IP_ADDRESS          0x30000

struct VcnDpg
{
    ULONG   sram[VCN_SRAM_MAX_DW];
    ULONG   count;
    BOOL    indirect;
};

static ULONG vcn_dpg_offset(struct AmdgpuDevice *ad, ULONG base_idx, ULONG reg)
{
    ULONG addr = ((ad->vcn_seg[base_idx] + reg) << 2) & 0xFFFFF;

    if (addr >= VCN_VID_SOC_ADDRESS && addr < VCN_VID_SOC_ADDRESS + 0x2600)
        addr = addr - VCN_VID_SOC_ADDRESS + VCN_VID_IP_ADDRESS;
    else if (addr >= VCN_AON_SOC_ADDRESS && addr < VCN_AON_SOC_ADDRESS + 0x600)
        addr = addr - VCN_AON_SOC_ADDRESS + VCN_AON_IP_ADDRESS;
    return addr >> 2;
}

static void vcn_dpg_write(struct AmdgpuDevice *ad, struct VcnDpg *dpg, ULONG off, ULONG val)
{
    if (dpg->indirect) {
        if (dpg->count + 2 <= VCN_SRAM_MAX_DW) {
            dpg->sram[dpg->count++] = off;
            dpg->sram[dpg->count++] = val;
        }
        return;
    }
    WREG(regUVD_DPG_LMA_DATA, val);
    WREG(regUVD_DPG_LMA_CTL, (1 << UVD_DPG_LMA_CTL__READ_WRITE__SHIFT) |
                             (off << UVD_DPG_LMA_CTL__READ_WRITE_ADDR__SHIFT));
}


#define DPG_W(r, v)  vcn_dpg_write(ad, dpg, vcn_dpg_offset(ad, r##_BASE_IDX, r), (v))

static void vcn_mc_resume_dpg(struct AmdgpuDevice *ad, struct VcnDpg *dpg)
{
    UQUAD mc = AMDGPU_VRAM_MC_BASE + VCN_VRAM_OFF;
    UQUAD stack = mc + VCN_STACK_OFF, context = mc + VCN_CONTEXT_OFF;
    ULONG fw_size = (ad->vcn_fw_size + 8 + 4095) & ~4095UL;

    if (dpg->indirect) {
        DPG_W(regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW, 0);
        DPG_W(regUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH, 0);
        DPG_W(regUVD_VCPU_CACHE_OFFSET0, 0);
        DPG_W(regUVD_VCPU_CACHE_SIZE0, 0);
        DPG_W(regUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW, 0);
        DPG_W(regUVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH, 0);
    } else {
        DPG_W(regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW, (ULONG)ad->vcn_fw_tmr_mc);
        DPG_W(regUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH, (ULONG)(ad->vcn_fw_tmr_mc >> 32));
        DPG_W(regUVD_VCPU_CACHE_OFFSET0, 0);
        DPG_W(regUVD_VCPU_CACHE_SIZE0, fw_size);
        DPG_W(regUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW, (ULONG)stack);
        DPG_W(regUVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH, (ULONG)(stack >> 32));
    }
    DPG_W(regUVD_VCPU_CACHE_OFFSET1, 0);
    DPG_W(regUVD_VCPU_CACHE_SIZE1, VCN_STACK_SIZE);

    DPG_W(regUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW, (ULONG)context);
    DPG_W(regUVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH, (ULONG)(context >> 32));
    DPG_W(regUVD_VCPU_CACHE_OFFSET2, 0);
    DPG_W(regUVD_VCPU_CACHE_SIZE2, VCN_CONTEXT_SIZE);

    DPG_W(regUVD_LMI_VCPU_NC0_64BIT_BAR_LOW, (ULONG)(mc + VCN_FW_SHARED_OFF));
    DPG_W(regUVD_LMI_VCPU_NC0_64BIT_BAR_HIGH, (ULONG)((mc + VCN_FW_SHARED_OFF) >> 32));
    DPG_W(regUVD_VCPU_NONCACHE_OFFSET0, 0);
    DPG_W(regUVD_VCPU_NONCACHE_SIZE0, VCN_FW_SHARED_SIZE);

    if (ad->gc_seg[0])
        DPG_W(regUVD_GFX10_ADDR_CONFIG, amdgpu_rreg32(ad, ad->gc_seg[0] + 0x13de));
}

static BOOL vcn_start_dpg(struct AmdgpuDevice *ad, BOOL indirect)
{
    UQUAD ring_mc = AMDGPU_VRAM_MC_BASE + VCN_VRAM_OFF + VCN_RING_OFF;
    struct VcnDpg dpgdata, *dpg = &dpgdata;
    ULONG tmp;

    dpg->count = 0;
    dpg->indirect = indirect;

    WREG_P(regUVD_POWER_STATUS, 1, ~UVD_POWER_STATUS__UVD_POWER_STATUS_MASK);
    WREG(regUVD_POWER_STATUS, RREG(regUVD_POWER_STATUS) |
         UVD_POWER_STATUS__UVD_PG_MODE_MASK | UVD_POWER_STATUS__UVD_PG_EN_MASK);

    DPG_W(regUVD_VCPU_CNTL, (0xFF << UVD_VCPU_CNTL__PRB_TIMEOUT_VAL__SHIFT) |
          UVD_VCPU_CNTL__CLK_EN_MASK | UVD_VCPU_CNTL__BLK_RST_MASK);
    DPG_W(regUVD_MASTINT_EN, 0);
    DPG_W(regUVD_LMI_CTRL, UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK |
          UVD_LMI_CTRL__REQ_MODE_MASK | UVD_LMI_CTRL__CRC_RESET_MASK |
          UVD_LMI_CTRL__MASK_MC_URGENT_MASK | UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK |
          UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK |
          (8 << UVD_LMI_CTRL__WRITE_CLEAN_TIMER__SHIFT) | 0x00100000);

    vcn_mc_resume_dpg(ad, dpg);

    DPG_W(regUVD_VCPU_CNTL, (0xFF << UVD_VCPU_CNTL__PRB_TIMEOUT_VAL__SHIFT) |
          UVD_VCPU_CNTL__CLK_EN_MASK);
    DPG_W(regUVD_LMI_CTRL2, 0x1f << UVD_LMI_CTRL2__RE_OFLD_MIF_WR_REQ_NUM__SHIFT);
    DPG_W(regUVD_MASTINT_EN, UVD_MASTINT_EN__VCPU_EN_MASK);

    if (indirect) {
        ULONG status = 0xffffffff;
        BOOL done;

        amdgpu_vram_write(ad, VCN_VRAM_OFF + VCN_SRAM_OFF, dpg->sram, dpg->count);
        done = amdgpu_psp_load_ip_fw(ad, AMDGPU_VRAM_MC_BASE + VCN_VRAM_OFF + VCN_SRAM_OFF,
                                     dpg->count * 4, PSP_FW_TYPE_VCN0_RAM, "VCN0_RAM",
                                     &status);
        if (!done || status != 0) {
            bug("[amdgpu-vcn] PSP VCN0_RAM load of %lu register writes failed "
                "(complete=%d status 0x%08lx)\n", (unsigned long)dpg->count / 2,
                (int)done, (unsigned long)status);
            return FALSE;
        }
    }


    WREG(regUVD_RB_BASE_LO, (ULONG)ring_mc);
    WREG(regUVD_RB_BASE_HI, (ULONG)(ring_mc >> 32));
    WREG(regUVD_RB_SIZE, VCN_RING_SIZE / 4);

    tmp = RREG(regVCN_RB_ENABLE);
    WREG(regVCN_RB_ENABLE, tmp & ~VCN_RB_ENABLE__RB1_EN_MASK);
    vcn_fw_shared_byte(ad, 17, VCN_FW_QUEUE_RING_RESET, TRUE);
    WREG(regUVD_RB_RPTR, 0);
    WREG(regUVD_RB_WPTR, 0);
    WREG(regUVD_RB_WPTR, RREG(regUVD_RB_RPTR));
    ad->vcn_wptr = RREG(regUVD_RB_WPTR);

    WREG(regVCN_RB_ENABLE, RREG(regVCN_RB_ENABLE) | VCN_RB_ENABLE__RB1_EN_MASK);
    vcn_fw_shared_byte(ad, 17, VCN_FW_QUEUE_RING_RESET | VCN_FW_QUEUE_DPG_HOLD_OFF, FALSE);
    ad->vcn_doorbell = amdgpu_boot_option("vcndb") && ad->nbio_seg[2] && amdgpu_doorbell_map(ad);
    if (ad->vcn_doorbell) {
        amdgpu_wreg32(ad, ad->nbio_seg[2] + NBIF_S2A_DOORBELL_ENTRY_4,
                      1 | (0x4 << 1) | ((VCN_DOORBELL_INDEX - 2) << 7) | (8 << 17) | (0x4UL << 28));
        WREG(regVCN_RB1_DB_CTRL, (VCN_DOORBELL_INDEX << VCN_RB1_DB_CTRL__OFFSET__SHIFT) |
                                 VCN_RB1_DB_CTRL__EN_MASK);
        bug("[amdgpu-vcn] unified ring on doorbell 0x%x\n", VCN_DOORBELL_INDEX);
    } else
        WREG(regVCN_RB1_DB_CTRL, 0);

    (void)RREG(regUVD_STATUS);
    return TRUE;
}

static BOOL vcn_ring_test(struct AmdgpuDevice *ad)
{
    ULONG ring[VCN_RING_ALIGN_DW];
    ULONG rptr, n, t, ring_dw = VCN_RING_SIZE / 4;

    for (n = 0; n < VCN_RING_ALIGN_DW; n++)
        ring[n] = VCN_ENC_CMD_NO_OP;
    ring[0] = VCN_ENC_CMD_END;

    rptr = RREG(regUVD_RB_RPTR);
    if (ad->vcn_wptr + VCN_RING_ALIGN_DW > ring_dw)
        return FALSE;
    amdgpu_vram_write(ad, VCN_VRAM_OFF + VCN_RING_OFF + (UQUAD)ad->vcn_wptr * 4,
                      ring, VCN_RING_ALIGN_DW);
    (void)amdgpu_rreg32(ad, vcn_reg(ad, regUVD_STATUS_BASE_IDX, regUVD_STATUS));
    ad->vcn_wptr += VCN_RING_ALIGN_DW;
    if (ad->vcn_doorbell)
        ad->doorbell[VCN_DOORBELL_INDEX] = ad->vcn_wptr;
    else
        WREG(regUVD_RB_WPTR, ad->vcn_wptr);

    for (t = 0; t < 100000; t++) {
        if (RREG(regUVD_RB_RPTR) != rptr) {
            bug("[amdgpu-vcn] ring test passed: RPTR 0x%lx -> 0x%lx after %lu polls\n",
                (unsigned long)rptr, (unsigned long)RREG(regUVD_RB_RPTR),
                (unsigned long)t);
            return TRUE;
        }
        vcn_spin(3000);
    }
    bug("[amdgpu-vcn] ring test TIMEOUT: RPTR 0x%lx WPTR 0x%lx UVD_STATUS=0x%08lx\n",
        (unsigned long)RREG(regUVD_RB_RPTR), (unsigned long)RREG(regUVD_RB_WPTR),
        (unsigned long)RREG(regUVD_STATUS));
    return FALSE;
}

static struct SignalSemaphore vcn_ring_lock;
static APTR vcn_scratch_mem;
static BOOL vcn_busy;
static UBYTE *vcn_scratch;
static UQUAD vcn_scratch_va;

static void vcn_ring_emit(struct AmdgpuDevice *ad, const ULONG *pkt, ULONG dw)
{
    ULONG ring_dw = VCN_RING_SIZE / 4, padded = (dw + VCN_RING_ALIGN_DW - 1) & ~(VCN_RING_ALIGN_DW - 1);
    ULONG buf[VCN_RING_ALIGN_DW * 2], pos = ad->vcn_wptr % ring_dw, i;

    for (i = 0; i < padded; i++)
        buf[i] = i < dw ? pkt[i] : VCN_ENC_CMD_NO_OP;
    if (pos + padded > ring_dw) {
        ULONG nop[VCN_RING_ALIGN_DW];

        for (i = 0; i < VCN_RING_ALIGN_DW; i++)
            nop[i] = VCN_ENC_CMD_NO_OP;
        while (pos < ring_dw) {
            amdgpu_vram_write(ad, VCN_VRAM_OFF + VCN_RING_OFF + (UQUAD)pos * 4, nop,
                              VCN_RING_ALIGN_DW);
            pos += VCN_RING_ALIGN_DW;
            ad->vcn_wptr += VCN_RING_ALIGN_DW;
        }
        pos = 0;
    }
    amdgpu_vram_write(ad, VCN_VRAM_OFF + VCN_RING_OFF + (UQUAD)pos * 4, buf, padded);
    ad->vcn_wptr += padded;
    if (ad->vcn_doorbell)
        ad->doorbell[VCN_DOORBELL_INDEX] = ad->vcn_wptr % ring_dw;
    else
        WREG(regUVD_RB_WPTR, ad->vcn_wptr % ring_dw);
}

static ULONG vcn_ring_submit_ib(struct AmdgpuDevice *ad, UQUAD ib_va, ULONG ib_dwords)
{
    UQUAD fence = vcn_scratch_va + VCN_SCRATCH_FENCE;
    ULONG pkt[10], seq;

    ObtainSemaphore(&vcn_ring_lock);
    seq = ++ad->vcn_fence_seq;
    pkt[0] = VCN_ENC_CMD_IB;
    pkt[1] = 0;
    pkt[2] = (ULONG)ib_va;
    pkt[3] = (ULONG)(ib_va >> 32);
    pkt[4] = ib_dwords;
    pkt[5] = VCN_ENC_CMD_FENCE;
    pkt[6] = (ULONG)fence;
    pkt[7] = (ULONG)(fence >> 32);
    pkt[8] = seq;
    pkt[9] = VCN_ENC_CMD_TRAP;
    vcn_ring_emit(ad, pkt, 10);
    if (!vcn_busy)
        amdgpu_busy_begin(ad, AMDGPU_ENGINE_VCN);
    vcn_busy = TRUE;
    ReleaseSemaphore(&vcn_ring_lock);
    return seq;
}

static ULONG vcn_fence_value(struct AmdgpuDevice *ad)
{
    volatile ULONG *f = (volatile ULONG *)(vcn_scratch + VCN_SCRATCH_FENCE);

    (void)ad;
    CacheClearE((APTR)f, 4, CACRF_InvalidateD);
    return *f;
}

static BOOL vcn_wait_fence(struct AmdgpuDevice *ad, ULONG seq)
{
    ULONG t;

    for (t = 0; t < 200000; t++) {
        if ((LONG)(vcn_fence_value(ad) - seq) >= 0)
            return TRUE;
        vcn_spin(3000);
    }
    return FALSE;
}

static ULONG vcn_send_dec_msg(struct AmdgpuDevice *ad, const ULONG *msg, ULONG msg_dw)
{
    ULONG *ib = (ULONG *)(vcn_scratch + VCN_SCRATCH_IB);
    ULONG *m = (ULONG *)(vcn_scratch + VCN_SCRATCH_MSG);
    UQUAD msg_va = vcn_scratch_va + VCN_SCRATCH_MSG;
    ULONG n = 0, pack = VCN_DECODE_BUFFER_DW + 4 + 2, csum = 0, i;

    memset(m, 0, 4096);
    memcpy(m, msg, msg_dw * 4);
    memset(ib, 0, 72 * 4);
    ib[n++] = 0x00000010;
    ib[n++] = 0x30000002;
    n++;
    ib[n++] = pack;
    ib[n++] = 0x00000010;
    ib[n++] = 0x30000001;
    ib[n++] = 0x3;
    ib[n++] = pack * 4;
    ib[n++] = VCN_DECODE_BUFFER_DW * 4 + 8;
    ib[n++] = VCN_IB_FLAG_DECODE_BUFFER;
    ib[n++] = VCN_CMD_FLAG_MSG_BUFFER;
    ib[n++] = (ULONG)(msg_va >> 32);
    ib[n++] = (ULONG)msg_va;
    for (i = 0; i < pack; i++)
        csum += ib[4 + i];
    ib[2] = csum;
    CacheClearE(vcn_scratch, VCN_SCRATCH_PAGES * 4096, CACRF_ClearD);
    return vcn_ring_submit_ib(ad, vcn_scratch_va + VCN_SCRATCH_IB, n + VCN_DECODE_BUFFER_DW - 3);
}

static BOOL vcn_ib_test(struct AmdgpuDevice *ad)
{
    static const ULONG create[14] = {
        0x28, 0x38, 0x1, 0x0, 0x1, 0x0, 0x1, 0x28, 0x10, 0x0, 0x7, 0x0, 0x780, 0x440
    };
    static const ULONG destroy[6] = { 0x28, 0x18, 0x0, 0x2, 0x1, 0x0 };
    ULONG seq;

    seq = vcn_send_dec_msg(ad, create, 14);
    if (!vcn_wait_fence(ad, seq)) {
        bug("[amdgpu-vcn] IB test: decoder create did not signal fence %lu (fence %lu, RPTR 0x%lx WPTR 0x%lx UVD_STATUS 0x%08lx)\n",
            (unsigned long)seq, (unsigned long)vcn_fence_value(ad),
            (unsigned long)RREG(regUVD_RB_RPTR), (unsigned long)RREG(regUVD_RB_WPTR),
            (unsigned long)RREG(regUVD_STATUS));
        amdgpu_gart_dump_fault(ad, "VCN IB test");
        return FALSE;
    }
    seq = vcn_send_dec_msg(ad, destroy, 6);
    if (!vcn_wait_fence(ad, seq)) {
        bug("[amdgpu-vcn] IB test: decoder destroy did not signal fence %lu (fence %lu)\n",
            (unsigned long)seq, (unsigned long)vcn_fence_value(ad));
        return FALSE;
    }
    bug("[amdgpu-vcn] IB test passed: decoder session created and destroyed (fence %lu)\n",
        (unsigned long)vcn_fence_value(ad));
    return TRUE;
}

BOOL amdgpu_vcn_hw_init(struct AmdgpuDevice *ad)
{
    ULONG zero[256], result = 0, st, i;

    if (!ad->vcn_fw_tmr_mc || !ad->vcn_fw_size) {
        bug("[amdgpu-vcn] %s: PSP did not load VCN firmware - skipped\n", ad->name);
        return TRUE;
    }

    st = amdgpu_smu_send_msg(ad, PPSMC_MSG_PowerUpVcn, 0, &result);
    if (st != 1)
        bug("[amdgpu-vcn] SMU PowerUpVcn answered 0x%02lx\n", (unsigned long)st);

    for (i = 0; i < 256; i++)
        zero[i] = 0;
    for (i = 0; i < (VCN_FW_SHARED_SIZE + VCN_RING_SIZE) / sizeof(zero); i++)
        amdgpu_vram_write(ad, VCN_VRAM_OFF + VCN_FW_SHARED_OFF + (UQUAD)i * sizeof(zero),
                          zero, 256);

    zero[0] = VCN_FW_SHARED_FLAG_0_UNIFIED_QUEUE;
    amdgpu_vram_write(ad, VCN_VRAM_OFF + VCN_FW_SHARED_OFF, zero, 1);
    vcn_fw_shared_byte(ad, 16, 1, TRUE);

    bug("[amdgpu-vcn] %s: VCN %u.%u.%u, firmware at TMR 0x%08lx%08lx (%lu bytes)\n",
        ad->name, ad->ip.vcn.major, ad->ip.vcn.minor, ad->ip.vcn.revision,
        (unsigned long)(ad->vcn_fw_tmr_mc >> 32), (unsigned long)(ad->vcn_fw_tmr_mc & 0xffffffff),
        (unsigned long)ad->vcn_fw_size);

    if (!vcn_start_dpg(ad, TRUE) && !vcn_start_dpg(ad, FALSE))
        return TRUE;
    InitSemaphore(&vcn_ring_lock);
    vcn_scratch_mem = AllocVec((VCN_SCRATCH_PAGES + 1) * 4096, MEMF_PUBLIC | MEMF_CLEAR);
    if (!vcn_scratch_mem)
        return TRUE;
    vcn_scratch = (UBYTE *)(((IPTR)vcn_scratch_mem + 4095) & ~(IPTR)4095);
    vcn_scratch_va = amdgpu_gart_map(ad, vcn_scratch, VCN_SCRATCH_PAGES * 4096);
    if (!vcn_scratch_va) {
        bug("[amdgpu-vcn] no GART mapping for the fence page - VCN left idle\n");
        return TRUE;
    }
    ad->vcn_fence_seq = 0;
    ad->vcn_ready = vcn_ring_test(ad);
    if (ad->vcn_ready)
        vcn_ib_test(ad);
    return TRUE;
}

LONG amdgpu_vcn_submit_ib(struct AmdgpuDevice *ad, UQUAD ib_va, ULONG ib_ndw, ULONG *fence)
{
    if (!fence)
        return -EINVAL;
    *fence = 0;
    if (!ad || !ad->vcn_ready)
        return -ENODEV;
    if (!ib_va || (ib_va & 3) || !ib_ndw || ib_ndw > 0x100000)
        return -EINVAL;
    *fence = vcn_ring_submit_ib(ad, ib_va, ib_ndw);
    return 0;
}

LONG amdgpu_vcn_query_fence(struct AmdgpuDevice *ad, ULONG fence, ULONG *complete)
{
    if (!complete)
        return -EINVAL;
    *complete = 0;
    if (!ad || !ad->vcn_ready)
        return -ENODEV;
    *complete = (LONG)(vcn_fence_value(ad) - fence) >= 0;
    amdgpu_vcn_busy_poll(ad);
    return 0;
}

void amdgpu_vcn_busy_poll(struct AmdgpuDevice *ad)
{
    if (!vcn_busy || !ad->vcn_ready)
        return;
    ObtainSemaphore(&vcn_ring_lock);
    if (vcn_busy && (LONG)(vcn_fence_value(ad) - ad->vcn_fence_seq) >= 0) {
        vcn_busy = FALSE;
        amdgpu_busy_end(ad, AMDGPU_ENGINE_VCN);
    }
    ReleaseSemaphore(&vcn_ring_lock);
}
