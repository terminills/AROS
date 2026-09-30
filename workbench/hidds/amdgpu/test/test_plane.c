/* Exercise the actual transaction with an emulated double-buffered pipe. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../amdgpu_plane.c"

#ifdef VERIFY_LINUX_DCN41
#include "asic_reg/dcn/dcn_4_1_0_offset.h"
#include "asic_reg/dcn/dcn_4_1_0_sh_mask.h"
_Static_assert(P_LOCK == regOTG0_OTG_MASTER_UPDATE_LOCK, "OTG lock offset");
_Static_assert(P_FLIP == regHUBPREQ0_DCSURF_FLIP_CONTROL, "flip offset");
_Static_assert(P_ADDR == regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, "address offset");
_Static_assert(P_ADDR_HI == regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, "address high");
_Static_assert(P_INUSE == regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, "in-use offset");
_Static_assert(P_INUSE_HI == regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, "in-use high");
_Static_assert(P_PENDING == HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK, "pending mask");
_Static_assert(P_FLIP_FIELDS == (HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_UPDATE_LOCK_MASK |
    HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK |
    HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_MODE_FOR_STEREOSYNC_MASK |
    HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_IN_STEREOSYNC_MASK), "flip fields");
#define CHECK(index, name, expected_mask) do { \
    assert(plane_fields[index].reg == reg##name); \
    assert(reg##name##_BASE_IDX == 2); \
    assert(plane_fields[index].mask == (uint32_t)(expected_mask)); \
} while (0)
static void reference(void)
{
    CHECK(0, HUBP0_DCSURF_SURFACE_CONFIG, HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK);
    CHECK(1, HUBP0_DCSURF_TILING_CONFIG, HUBP0_DCSURF_TILING_CONFIG__SW_MODE_MASK);
    CHECK(2, HUBPREQ0_VMID_SETTINGS_0, HUBPREQ0_VMID_SETTINGS_0__VMID_MASK);
    CHECK(3, HUBPREQ0_DCSURF_SURFACE_CONTROL,
        HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_TMZ_MASK |
        HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN_MASK);
    CHECK(4, HUBP0_DCSURF_PRI_VIEWPORT_START,
        HUBP0_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_X_START_MASK |
        HUBP0_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_Y_START_MASK);
    CHECK(5, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION,
        HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH_MASK |
        HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT_MASK);
    CHECK(6, HUBPREQ0_DCSURF_SURFACE_PITCH, HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK);
    CHECK(7, DSCL0_DSCL_AUTOCAL, DSCL0_DSCL_AUTOCAL__AUTOCAL_MODE_MASK |
        DSCL0_DSCL_AUTOCAL__AUTOCAL_NUM_PIPE_MASK | DSCL0_DSCL_AUTOCAL__AUTOCAL_PIPE_ID_MASK);
    CHECK(8, DSCL0_DSCL_CONTROL, DSCL0_DSCL_CONTROL__SCL_BOUNDARY_MODE_MASK);
    CHECK(9, DSCL0_RECOUT_START, DSCL0_RECOUT_START__RECOUT_START_X_MASK |
        DSCL0_RECOUT_START__RECOUT_START_Y_MASK);
    CHECK(10, DSCL0_RECOUT_SIZE, DSCL0_RECOUT_SIZE__RECOUT_WIDTH_MASK |
        DSCL0_RECOUT_SIZE__RECOUT_HEIGHT_MASK);
    CHECK(11, DSCL0_MPC_SIZE, DSCL0_MPC_SIZE__MPC_WIDTH_MASK | DSCL0_MPC_SIZE__MPC_HEIGHT_MASK);
    CHECK(12, DSCL0_SCL_MODE, DSCL0_SCL_MODE__DSCL_MODE_MASK);
    CHECK(13, DSCL0_LB_DATA_FORMAT, DSCL0_LB_DATA_FORMAT__INTERLEAVE_EN_MASK |
        DSCL0_LB_DATA_FORMAT__ALPHA_EN_MASK);
    CHECK(14, DSCL0_LB_MEMORY_CTRL, DSCL0_LB_MEMORY_CTRL__MEMORY_CONFIG_MASK |
        DSCL0_LB_MEMORY_CTRL__LB_MAX_PARTITIONS_MASK);
}
#else
static void reference(void) { }
#endif

struct fixture {
    uint32_t regs[0x1c00], saved[0x1c00];
    unsigned reads, writes, latch_wait, high_write;
    unsigned die_read, corrupt_reg, corrupt_count;
    uint64_t tick;
    int dead, stuck_clock, no_latch, no_lock, tear;
};
static struct fixture f;
static uint32_t fake_read(void *ctx, uint32_t reg)
{
    struct fixture *s = ctx;
    assert(reg < 0x1c00);
    ++s->reads;
    if (s->die_read && s->reads >= s->die_read) s->dead = 1;
    if (s->dead) return UINT32_MAX;
    if (!(s->regs[P_LOCK] & 1) && !(s->regs[P_FLIP] & 1) &&
        (s->regs[P_FLIP] & P_PENDING) && !s->no_latch && ++s->latch_wait > 8) {
        s->regs[P_INUSE] = s->regs[P_ADDR];
        s->regs[P_INUSE_HI] = s->regs[P_ADDR_HI];
        s->regs[P_FLIP] &= ~P_PENDING;
    }
    if (s->tear && s->writes && reg == P_INUSE_HI) return s->tear++ & 1 ? 0x80 : 0x81;
    return s->regs[reg];
}
static void fake_write(void *ctx, uint32_t reg, uint32_t value)
{
    struct fixture *s = ctx;
    unsigned i;
    assert(!s->dead && reg < 0x1c00);
    ++s->writes;
    if (reg == P_LOCK) {
        s->regs[reg] = (value & 1) ? (s->no_lock ? 1 : 0x101) : 0;
        if (s->regs[reg] & 0x100) s->regs[P_FLIP] |= 0x200;
        else s->regs[P_FLIP] &= ~0x200u;
    } else if (reg == P_FLIP) {
        assert(s->regs[P_LOCK] == 0x101);
        s->regs[reg] = value;
    } else {
        assert(s->regs[P_LOCK] == 0x101 && (s->regs[P_FLIP] & 1));
        if (reg == P_ADDR_HI) s->high_write = s->writes;
        if (reg == P_ADDR) {
            assert(s->high_write && s->writes == s->high_write + 1);
            s->regs[P_FLIP] |= P_PENDING;
            s->latch_wait = 0;
        }
        s->regs[reg] = value;
    }
    if (s->corrupt_count && reg == s->corrupt_reg) {
        for (i = 0; i < P_FIELDS; ++i) if (plane_fields[i].reg == reg) {
            s->regs[reg] ^= plane_fields[i].mask & (0u - plane_fields[i].mask);
            --s->corrupt_count;
        }
    }
}
static uint64_t fake_ticks(void *ctx)
{
    struct fixture *s = ctx;
    if (!s->stuck_clock) ++s->tick;
    return s->tick;
}
static struct AmdgpuPlane p;
static struct AmdgpuPlaneIO io = { &f, fake_read, fake_write, fake_ticks, 30 };
static void reset(void)
{
    memset(&f, 0, sizeof(f));
    p = (struct AmdgpuPlane){ UINT64_C(0x8002000000), 1920,1080,1920,4,1920,1080,
                             0,0,1080 };
    f.regs[P_CONTROL] = 0x10301;
    f.regs[P_HBLANK] = (192u << 16) | 2112u;
    f.regs[P_VBLANK] = (41u << 16) | 1121u;
    f.regs[P_FLIP] = 0x04100000;
    f.regs[P_ADDR_HI] = f.regs[P_INUSE_HI] = 0x80;
    f.regs[0x05e5] = 8;
    f.regs[0x05eb] = (768u << 16) | 1024u;
    f.regs[0x0607] = 1023;
    f.regs[0x0d19] = f.regs[0x0d0a] = f.regs[0x0d08] = 1;
    f.regs[0x0d1f] = f.regs[0x0d20] = (1080u << 16) | 1920u;
    memcpy(f.saved, f.regs, sizeof(f.saved));
}
static void preserved(void)
{
    assert(amdgpu_plane_update(&io, &p) == AMDGPU_PLANE_PRESERVED);
    assert(!f.writes);
}
int main(void)
{
    unsigned i, normal_reads;
    reference();
    reset(); assert(amdgpu_plane_update(&io, &p) == AMDGPU_PLANE_ACTIVE);
    normal_reads = f.reads;
    assert(f.regs[P_INUSE] == (uint32_t)p.mc && f.regs[P_INUSE_HI] == p.mc >> 32);
    assert(f.regs[0x0607] == 1919 && f.regs[0x0d19] == 0 && f.regs[0x0d08] == 0);
    assert(f.regs[0x0d22] == 0x3f00 && f.regs[P_LOCK] == 0);
    assert(!(f.regs[P_FLIP] & (P_PENDING | 0x201)));
    reset(); p.width=1366; p.height=768; p.pitch_pixels=1376; p.bytes_per_pixel=2;
    assert(amdgpu_plane_update(&io, &p) == AMDGPU_PLANE_ACTIVE);
    assert(f.regs[0x05e5] == 3 && f.regs[0x0607] == 1375);
    assert(f.regs[0x0d1e] == (156u << 16 | 277u));
    reset(); p.mc++; preserved();
    reset(); p.mc=UINT64_C(0x1000000000000); preserved();
    reset(); p.width=0; preserved();
    reset(); p.width=1921; preserved();
    reset(); p.pitch_pixels=1919; preserved();
    reset(); p.pitch_pixels=65537; preserved();
    reset(); p.bytes_per_pixel=3; preserved();
    reset(); f.regs[P_CONTROL]=0; preserved();
    reset(); f.regs[P_LOCK]=1; preserved();
    reset(); f.regs[P_FLIP]|=1; preserved();
    reset(); f.regs[P_FLIP]|=P_PENDING; preserved();
    reset(); f.regs[P_INUSE]=0x100000; preserved();
    reset(); f.regs[P_INUSE_HI]|=0x10000000; preserved();
    reset(); f.no_lock=1;
    assert(amdgpu_plane_update(&io,&p)==AMDGPU_PLANE_PRESERVED);
    assert(f.writes==2 && f.regs[P_LOCK]==0);
    for (i=0;i<P_FIELDS;i++) {
        reset(); f.corrupt_reg=plane_fields[i].reg; f.corrupt_count=1;
        assert(amdgpu_plane_update(&io,&p)==AMDGPU_PLANE_PRESERVED);
        assert(!memcmp(f.regs,f.saved,sizeof(f.regs)));
        reset(); f.corrupt_reg=plane_fields[i].reg; f.corrupt_count=2;
        assert(amdgpu_plane_update(&io,&p)==AMDGPU_PLANE_UNCERTAIN);
    }
    reset(); f.no_latch=1;
    assert(amdgpu_plane_update(&io,&p)==AMDGPU_PLANE_UNCERTAIN);
    assert(f.regs[P_ADDR] != f.regs[P_INUSE]);
    reset(); f.tear=1;
    assert(amdgpu_plane_update(&io,&p)==AMDGPU_PLANE_UNCERTAIN);
    reset(); f.no_latch=f.stuck_clock=1;
    assert(amdgpu_plane_update(&io,&p)==AMDGPU_PLANE_UNCERTAIN);
    for (i=1;i<=normal_reads;i++) {
        reset(); f.die_read=i;
        assert(amdgpu_plane_update(&io,&p)==AMDGPU_PLANE_UNCERTAIN);
        assert(f.dead); /* fake_write asserts no writes after an all-ones read. */
    }
    puts("PASS: actual DCN plane sequencing, masks,1080p/RGB565, preflight, all-stage rollback, latch/torn/dead-MMIO failures");
    return 0;
}
