/* Fixed read-only plane/scaler snapshot for JOB-0084 Navi44, not discovery.
 * DMU segment2=0x34c0 was captured from this DUT's IP discovery.
 * Offsets/base indices: Linux f377d0025eb0 dcn_4_1_0_offset.h.
 * Direct configuration and read-only flip observation; no interrupt ACK or indexes.
 * X(name, segment index, relative dword offset).
 */
#ifndef AMDGPU_TEST_DCN_STATE_REGS_H
#define AMDGPU_TEST_DCN_STATE_REGS_H
#define DCN_STATE_REGS(X) \
    X(DCHUBBUB_ARB_MALL_CNTL,                    2, 0x0525) \
    X(HUBP0_DCSURF_SURFACE_CONFIG,                2, 0x05e5) \
    X(HUBP0_DCSURF_TILING_CONFIG,                 2, 0x05e7) \
    X(HUBP0_DCSURF_PRI_VIEWPORT_START,            2, 0x05e9) \
    X(HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION,        2, 0x05eb) \
    X(HUBP0_DCHUBP_REQ_SIZE_CONFIG,               2, 0x05f2) \
    X(HUBP0_DCHUBP_REQ_SIZE_CONFIG_C,             2, 0x05f3) \
    X(HUBP0_DCHUBP_CNTL,                          2, 0x05f4) \
    X(HUBP0_DCHUBP_MALL_CONFIG,                   2, 0x05f7) \
    X(HUBP0_HUBP_MALL_STATUS,                     2, 0x0603) \
    X(HUBPREQ0_DCSURF_SURFACE_PITCH,              2, 0x0607) \
    X(HUBPREQ0_VMID_SETTINGS_0,                   2, 0x0609) \
    X(HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS,    2, 0x060a) \
    X(HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,2, 0x060b) \
    X(HUBPREQ0_DCSURF_SURFACE_CONTROL,            2, 0x0612) \
    X(HUBPREQ0_DCSURF_FLIP_CONTROL,              2, 0x0613) \
    X(HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE,     2, 0x061c) \
    X(HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH,2, 0x061d) \
    X(HUBPREQ0_DCN_EXPANSION_MODE,                2, 0x0620) \
    X(HUBPREQ0_DCN_TTU_QOS_WM,                   2, 0x0621) \
    X(HUBPREQ0_DCN_GLOBAL_TTU_CNTL,              2, 0x0622) \
    X(HUBPREQ0_DCN_SURF0_TTU_CNTL0,              2, 0x0623) \
    X(HUBPREQ0_BLANK_OFFSET_0,                   2, 0x063b) \
    X(HUBPREQ0_BLANK_OFFSET_1,                   2, 0x063c) \
    X(HUBPREQ0_DST_DIMENSIONS,                   2, 0x063d) \
    X(HUBPREQ0_DST_AFTER_SCALER,                 2, 0x063e) \
    X(HUBPREQ0_PREFETCH_SETTINGS,                2, 0x063f) \
    X(HUBPREQ0_VBLANK_PARAMETERS_0,              2, 0x0641) \
    X(HUBPREQ0_VBLANK_PARAMETERS_1,              2, 0x0642) \
    X(HUBPREQ0_FLIP_PARAMETERS_0,                2, 0x0646) \
    X(HUBPREQ0_FLIP_PARAMETERS_1,                2, 0x0647) \
    X(HUBPREQ0_PER_LINE_DELIVERY,                2, 0x0652) \
    X(HUBPREQ0_REF_FREQ_TO_PIX_FREQ,             2, 0x0654) \
    X(HUBPRET0_HUBPRET_CONTROL,                  2, 0x066d) \
    X(CNVC_CFG0_FORMAT_CONTROL,                  2, 0x0cd0) \
    X(DSCL0_SCL_MODE,                            2, 0x0d08) \
    X(DSCL0_DSCL_CONTROL,                        2, 0x0d0a) \
    X(DSCL0_DSCL_AUTOCAL,                        2, 0x0d19) \
    X(DSCL0_RECOUT_START,                        2, 0x0d1e) \
    X(DSCL0_RECOUT_SIZE,                         2, 0x0d1f) \
    X(DSCL0_MPC_SIZE,                            2, 0x0d20) \
    X(DSCL0_LB_DATA_FORMAT,                      2, 0x0d21) \
    X(DSCL0_LB_MEMORY_CTRL,                      2, 0x0d22) \
    X(OTG0_OTG_H_TOTAL,                          2, 0x1b2a) \
    X(OTG0_OTG_V_TOTAL,                          2, 0x1b2f) \
    X(OTG0_OTG_CONTROL,                          2, 0x1b43) \
    X(OTG0_OTG_MASTER_UPDATE_LOCK,               2, 0x1b89)

#define DCN_STATE_DMU2 0x34c0u
#define DCN_STATE_BAR_BYTES (512u * 1024u)
#define DCN_STATE_FLIP_PENDING 0x00000100u
#endif
