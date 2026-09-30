/* Fixed read-only snapshot for the JOB-0084 Navi44 DUT, not a discovery API.
 * GC segment bases 0x1260/0xa000 were read from IP discovery on this DUT.
 * Register offsets/base indices: Linux f377d0025eb0, gc_12_0_0_offset.h.
 * X(name, base index, relative dword offset). No indexed/window registers.
 * GRBM/CP busy registers (2026-09-24): which block keeps GFX activity at
 * 100% on an idle desktop.
 */
#ifndef AMDGPU_TEST_GFX_STATE_REGS_H
#define AMDGPU_TEST_GFX_STATE_REGS_H
#define GFX_STATE_REGS(X) \
    X(GRBM_STATUS,                0, 0x0da4) \
    X(GRBM_STATUS2,               0, 0x0da2) \
    X(GRBM_STATUS3,               0, 0x0da7) \
    X(GRBM_STATUS_SE0,            0, 0x0da5) \
    X(CP_BUSY_STAT,               0, 0x0f3f) \
    X(CP_CPC_STATUS,              0, 0x0e24) \
    X(CP_CPC_BUSY_STAT,           0, 0x0e25) \
    X(CP_CPC_BUSY_STAT2,          0, 0x0e2a) \
    X(CP_CPF_STATUS,              0, 0x0e27) \
    X(CP_CPF_BUSY_STAT,           0, 0x0e28) \
    X(RLC_STAT,                   1, 0x4c04) \
    X(CP_MES_CNTL,                1, 0x2807) \
    X(CP_STAT,                    0, 0x0f40) \
    X(CP_ME_HEADER_DUMP,          0, 0x0f41) \
    X(CP_RB0_RPTR,                0, 0x0f60) \
    X(CP_RB0_CNTL,                0, 0x1de1) \
    X(CP_RB0_WPTR,                0, 0x1df4) \
    X(CP_RB0_WPTR_HI,             0, 0x1df5) \
    X(CP_GFX_HQD_ACTIVE,          0, 0x1e80) \
    X(CP_RB_WPTR_POLL_ADDR_LO,    0, 0x1e8b) \
    X(CP_RB_WPTR_POLL_ADDR_HI,    0, 0x1e8c) \
    X(CP_RB_DOORBELL_CONTROL,     0, 0x1e8d) \
    X(CP_GFX_HQD_QUE_MGR_CONTROL, 0, 0x1e95) \
    X(CP_RB_DOORBELL_RANGE_LOWER, 0, 0x1dfa) \
    X(CP_RB_DOORBELL_RANGE_UPPER, 0, 0x1dfb) \
    X(CP_RB_WPTR_POLL_CNTL,       0, 0x0f62) \
    X(CP_GFX_HQD_RPTR,            0, 0x1e88) \
    X(CP_GFX_HQD_CNTL,            0, 0x1e8f) \
    X(CP_GFX_HQD_WPTR,            0, 0x1e91) \
    X(CP_GFX_HQD_WPTR_HI,         0, 0x1e92) \
    X(GRBM_GFX_CNTL,              1, 0x0900) \
    X(CP_ME_CNTL,                 1, 0x0803) \
    X(RLC_CSIB_ADDR_LO,           1, 0x0987) \
    X(RLC_CSIB_ADDR_HI,           1, 0x0988) \
    X(RLC_CSIB_LENGTH,            1, 0x0989) \
    X(CP_GFX_RS64_INSTR_PNTR0,    1, 0x2a44) \
    X(CP_GFX_RS64_INSTR_PNTR1,    1, 0x2a45) \
    X(CP_ME_RS64_EXCEPTION_STATUS,1, 0x2aaa) \
    X(CP_GFX_RS64_INTERRUPT1,     1, 0x2aac) \
    X(RLC_CNTL,                   1, 0x4c00) \
    X(RLC_SRM_CNTL,               1, 0x4c80)

#define GFX_STATE_GC0 0x1260u
#define GFX_STATE_GC1 0xa000u
#define GFX_STATE_BAR_BYTES (512u * 1024u)
#endif
