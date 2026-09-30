/* Descriptor/frame encoding only; no claim of GPU execution. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#include "../amdgpu_gfx_mqd.h"

#ifdef HAVE_LINUX_MQD_LAYOUT
#include <v12_structs.h>
#include <mes_v12_api_def.h>
#include <asic_reg/gc/gc_12_0_0_sh_mask.h>
_Static_assert(GFX12_HQD_TMZ_MATCH == CP_GFX_HQD_CNTL__TMZ_MATCH_MASK, "TMZ_MATCH bit");
_Static_assert(sizeof(struct v12_gfx_mqd) == GFX12_MQD_DWORDS * 4, "MQD size");
#define CHECK_MQD(field, dw) _Static_assert(offsetof(struct v12_gfx_mqd, field) == (dw) * 4, #field)
CHECK_MQD(cp_mqd_base_addr, 128);
CHECK_MQD(cp_mqd_base_addr_hi, 129);
CHECK_MQD(cp_gfx_hqd_active, 130);
CHECK_MQD(cp_gfx_hqd_quantum, 135);
CHECK_MQD(cp_gfx_hqd_base, 136);
CHECK_MQD(cp_gfx_hqd_base_hi, 137);
CHECK_MQD(cp_gfx_hqd_rptr_addr, 139);
CHECK_MQD(cp_rb_wptr_poll_addr_lo, 141);
CHECK_MQD(cp_gfx_hqd_cntl, 145);
CHECK_MQD(cp_gfx_mqd_control, 162);
_Static_assert(sizeof(union MESAPI__ADD_QUEUE) == 256, "ADD_QUEUE size");
_Static_assert(offsetof(union MESAPI__ADD_QUEUE, mqd_addr) == 20 * 4, "MQD addr");
_Static_assert(offsetof(union MESAPI__ADD_QUEUE, api_status) == 38 * 4, "map ACK");
_Static_assert(offsetof(union MESAPI__ADD_QUEUE, pipe_id) == 50 * 4, "pipe");
_Static_assert(offsetof(union MESAPI__QUERY_MES_STATUS, api_status) == 2 * 4, "query ACK");
_Static_assert(MES_SCH_API_QUERY_SCHEDULER_STATUS == 11, "query opcode");
_Static_assert(MES_QUEUE_TYPE_GFX == 0, "gfx queue type");
#endif

int main(void)
{
    ULONG mqd[GFX12_MQD_DWORDS + 2], expect[GFX12_MQD_DWORDS];
    ULONG pkt[GFX12_MAP_DWORDS + 2], exp_pkt[GFX12_MAP_DWORDS];
    UQUAD base = 0x0000ab1234560000ULL;
    unsigned i;

    /* Only the exact diagnostic control and the one observed post-map bit
     * addition are accepted. Reject every other single-bit change, including
     * TMZ_STATE, privilege, size, byte order and cache policy. */
    assert(gfx12_diag_hqd_cntl_valid(0x00f00709, 0x00f00709));
    assert(gfx12_diag_hqd_cntl_valid(0x00f00709, 0x00f00789));
    for (i = 0; i < 32; i++) {
        ULONG bit = (ULONG)1 << i;
        assert(!gfx12_diag_hqd_cntl_valid(0x00f00709 ^ bit, 0x00f00789));
        if (i == 7) continue;
        assert(!gfx12_diag_hqd_cntl_valid(0x00f00709, 0x00f00709 ^ bit));
        assert(!gfx12_diag_hqd_cntl_valid(0x00f00709, 0x00f00789 ^ bit));
    }
    assert(!gfx12_diag_hqd_cntl_valid(0x00f00709, 0));
    assert(!gfx12_diag_hqd_cntl_valid(0x00f00709, 0xffffffff));

    memset(mqd, 0xa5, sizeof(mqd));
    memset(expect, 0, sizeof(expect));
    assert(gfx12_build_gfx_mqd(mqd + 1, base + 0x4000, base,
                             base + 0x1000, base + 0x1008));
    expect[128] = 0x34564000;
    expect[129] = 0xab12;
    expect[130] = 1;
    expect[135] = 0xa01;
    expect[136] = 0x12345600;
    expect[137] = 0xab;
    expect[139] = 0x34561000;
    expect[140] = 0xab12;
    expect[141] = 0x34561008;
    expect[142] = 0xab12;
    expect[145] = 0xf00709;
    expect[162] = 0x100;
    assert(memcmp(mqd + 1, expect, sizeof(expect)) == 0);
    assert(mqd[0] == 0xa5a5a5a5 && mqd[GFX12_MQD_DWORDS + 1] == 0xa5a5a5a5);

    memset(pkt, 0xa5, sizeof(pkt));
    memset(exp_pkt, 0, sizeof(exp_pkt));
    assert(gfx12_build_map_frames(pkt + 1, base + 0x4000, base + 0x1008,
                                 base + 0x5000, base + 0x5008));
    exp_pkt[0] = 0x40021;
    exp_pkt[18] = 0x116;
    exp_pkt[20] = 0x34564000;
    exp_pkt[21] = 0xab12;
    exp_pkt[22] = 0x34561008;
    exp_pkt[23] = 0xab12;
    exp_pkt[37] = 0x2000;
    exp_pkt[38] = 0x34565000;
    exp_pkt[39] = 0xab12;
    exp_pkt[40] = 1;
    exp_pkt[64] = 0x400b1;
    exp_pkt[66] = 0x34565008;
    exp_pkt[67] = 0xab12;
    exp_pkt[68] = 1;
    assert(memcmp(pkt + 1, exp_pkt, sizeof(exp_pkt)) == 0);
    assert(pkt[0] == 0xa5a5a5a5 && pkt[GFX12_MAP_DWORDS + 1] == 0xa5a5a5a5);

    /* Reject lossy encodings without touching an existing descriptor/frame. */
    for (i = 1; i < 256; i++) {
        assert(!gfx12_build_gfx_mqd(mqd + 1, base + i, base, base + 0x1000, base + 0x1008));
        assert(!gfx12_build_gfx_mqd(mqd + 1, base, base + i, base + 0x1000, base + 0x1008));
        assert(!gfx12_build_map_frames(pkt + 1, base + i, base + 0x1008, base + 0x5000, base + 0x5008));
    }
    for (i = 1; i < 8; i++) {
        assert(!gfx12_build_gfx_mqd(mqd + 1, base, base, base + i, base + 0x1008));
        assert(!gfx12_build_gfx_mqd(mqd + 1, base, base, base + 0x1000, base + i));
        assert(!gfx12_build_map_frames(pkt + 1, base, base + i, base + 0x5000, base + 0x5008));
        assert(!gfx12_build_map_frames(pkt + 1, base, base, base + i, base + 0x5008));
        assert(!gfx12_build_map_frames(pkt + 1, base, base, base + 0x5000, base + i));
    }
    assert(!gfx12_build_gfx_mqd(mqd + 1, 1ULL << 48, base, base + 0x1000, base + 0x1008));
    assert(!gfx12_build_gfx_mqd(mqd + 1, base, 1ULL << 48, base + 0x1000, base + 0x1008));
    assert(!gfx12_build_gfx_mqd(mqd + 1, base, base, 1ULL << 48, base + 0x1008));
    assert(!gfx12_build_gfx_mqd(mqd + 1, base, base, base + 0x1000, 1ULL << 48));
    assert(!gfx12_build_gfx_mqd(mqd + 1, base, base, base, base));
    assert(!gfx12_build_gfx_mqd(NULL, base, base, base + 0x1000, base + 0x1008));
    assert(!gfx12_build_map_frames(pkt + 1, base, base, base, base));
    assert(!gfx12_build_map_frames(pkt + 1, 0, base, base + 0x5000, base + 0x5008));
    assert(!gfx12_build_map_frames(pkt + 1, base, base, 1ULL << 48, base + 0x5008));
    assert(!gfx12_build_map_frames(NULL, base, base, base + 0x5000, base + 0x5008));
    assert(memcmp(mqd + 1, expect, sizeof(expect)) == 0);
    assert(memcmp(pkt + 1, exp_pkt, sizeof(exp_pkt)) == 0);
    puts("PASS: GFX12 MQD/MES encoding and bounded diagnostic control readback validation");
    return 0;
}
