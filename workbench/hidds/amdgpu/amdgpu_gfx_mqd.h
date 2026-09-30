/* GFX12 legacy kernel-queue descriptor and MES frame encoders.
 * Reference: Linux gfx_v12_0_gfx_mqd_init(), v12_structs.h and
 * mes_v12_api_def.h, checkout f377d0025eb0. No MMIO or allocation here.
 * Callers provide ULONG/UQUAD/BOOL and at least the stated output sizes. */
#ifndef AMDGPU_GFX_MQD_H
#define AMDGPU_GFX_MQD_H

#define GFX12_MQD_DWORDS 512UL
#define GFX12_MAP_DWORDS 128UL /* ADD_QUEUE followed by QUERY_SCHEDULER_STATUS */
#define GFX12_KERNEL_DOORBELL 0x116UL /* Linux GFX_RING0 0x8b in dword units */
#define GFX12_DIAG_HQD_CNTL 0x00f00709UL
#define GFX12_HQD_TMZ_MATCH 0x00000080UL

/* Bounded VMID0/non-TMZ bring-up diagnostic, NOT a secure-queue policy.
 * Native boot6 mapped this exact MQD but read CNTL=00f00789. Accept only
 * that observed bit7 addition or an exact match; TMZ_STATE and every other
 * bit remain exact. Linux also sets TMZ_MATCH for TMZ queue descriptors,
 * so this does not assert that the bit is universally read-only. The caller
 * must verify queue identity/VMID and must not rewrite the hardware control
 * to make it match. A memory-effect test, not this check, decides execution. */
static inline BOOL gfx12_diag_hqd_cntl_valid(ULONG requested, ULONG actual)
{
    return requested == GFX12_DIAG_HQD_CNTL &&
        (actual == requested || actual == (requested | GFX12_HQD_TMZ_MATCH));
}

static inline BOOL gfx12_address_valid(UQUAD addr, ULONG alignment)
{
    return addr && !(addr >> 48) && !(addr & (alignment - 1));
}

static inline BOOL gfx12_build_gfx_mqd(volatile ULONG *mqd, UQUAD mqd_va,
    UQUAD ring_va, UQUAD rptr_va, UQUAD wptr_va)
{
    ULONG i;

    if (!mqd || !gfx12_address_valid(mqd_va, 256) ||
        !gfx12_address_valid(ring_va, 256) ||
        !gfx12_address_valid(rptr_va, 8) ||
        !gfx12_address_valid(wptr_va, 8) || rptr_va == wptr_va)
        return FALSE;
    for (i = 0; i < GFX12_MQD_DWORDS; i++) mqd[i] = 0;
    mqd[128] = (ULONG)mqd_va;
    mqd[129] = (ULONG)(mqd_va >> 32);
    mqd[130] = 1;                  /* ACTIVE; VMID0, low priority */
    mqd[135] = 0x00000a01UL;      /* QUANTUM default, not just enable bit */
    mqd[136] = (ULONG)(ring_va >> 8);
    mqd[137] = (ULONG)(ring_va >> 40);
    mqd[139] = (ULONG)rptr_va;
    mqd[140] = (ULONG)(rptr_va >> 32);
    mqd[141] = (ULONG)wptr_va;
    mqd[142] = (ULONG)(wptr_va >> 32);
    /* Doorbell disabled: this diagnostic delivers WPTR via CP_RB0 MMIO.
     * MES still needs a unique doorbell index as its legacy queue identity. */
    mqd[145] = GFX12_DIAG_HQD_CNTL; /* Linux defaults + 4 KiB ring, 1 KiB blocks */
    mqd[162] = 0x00000100UL;      /* MQD VMID0, privileged kernel queue */
    return TRUE;
}

static inline BOOL gfx12_build_map_frames(volatile ULONG *frames,
    UQUAD mqd_va, UQUAD wptr_va, UQUAD ack_va, UQUAD query_va)
{
    ULONG i;

    if (!frames || !gfx12_address_valid(mqd_va, 256) ||
        !gfx12_address_valid(wptr_va, 8) ||
        !gfx12_address_valid(ack_va, 8) ||
        !gfx12_address_valid(query_va, 8) || ack_va == query_va)
        return FALSE;
    for (i = 0; i < GFX12_MAP_DWORDS; i++) frames[i] = 0;
    frames[0] = 0x00040021UL;    /* scheduler ADD_QUEUE, 64 dwords */
    frames[18] = GFX12_KERNEL_DOORBELL;
    frames[20] = (ULONG)mqd_va;
    frames[21] = (ULONG)(mqd_va >> 32);
    frames[22] = (ULONG)wptr_va;
    frames[23] = (ULONG)(wptr_va >> 32);
    frames[28] = 0;              /* GFX, not COMPUTE */
    frames[37] = 1UL << 13;     /* map_legacy_kq */
    frames[38] = (ULONG)ack_va;
    frames[39] = (ULONG)(ack_va >> 32);
    frames[40] = 1;
    /* pipe0 / queue0; the rest of the legacy-map fields remain zero. */
    frames[64] = 0x000400b1UL;  /* QUERY_SCHEDULER_STATUS, 64 dwords */
    frames[66] = (ULONG)query_va;
    frames[67] = (ULONG)(query_va >> 32);
    frames[68] = 1;
    return TRUE;
}

#endif
