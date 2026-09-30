/* Execute the actual viewport helper against bounded fake MMIO. No GPU access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t ULONG;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define AMDGPU_HUBP_INST_MAX 4
struct AmdgpuDevice { void *rmmio; ULONG dmu_seg[3]; };
static ULONG regs[0x8000], addresses[3], values[3];
static unsigned writes, reads, fail_read;
static void bug(const char *format, ...) { (void)format; }
static void amdgpu_wreg32(struct AmdgpuDevice *ad, ULONG dw, ULONG value)
{
    assert(ad && dw < 0x8000 && writes < 3 && reads == 0);
    addresses[writes] = dw; values[writes++] = value; regs[dw] = value;
}
static ULONG amdgpu_rreg32(struct AmdgpuDevice *ad, ULONG dw)
{
    assert(ad && dw < 0x8000 && writes == 3 && reads < 3);
    assert(dw == addresses[reads]);
    return ++reads == fail_read ? regs[dw] ^ 1 : regs[dw];
}
#include "viewport.inc"
static void reset(void)
{
    memset(regs, 0xa5, sizeof(regs));
    writes = reads = fail_read = 0;
}
static void valid(struct AmdgpuDevice *ad, ULONG pipe,
                  ULONG w, ULONG h, ULONG pitch)
{
    /* Independent exact DCN4.1 offsets, same values verified against Linux
     * dcn_4_1_0_offset.h by test_dcn_state_regs for pipe0. */
    const ULONG starts[] = { 0x05e9, 0x06c5, 0x07a1, 0x087d };
    const ULONG dims[] = { 0x05eb, 0x06c7, 0x07a3, 0x087f };
    const ULONG pitches[] = { 0x0607, 0x06e3, 0x07bf, 0x089b };
    reset();
    assert(amdgpu_modeset_set_viewport(ad, pipe, w, h, pitch));
    assert(writes == 3 && reads == 3);
    assert(addresses[0] == 0x34c0 + starts[pipe] && values[0] == 0);
    assert(addresses[1] == 0x34c0 + dims[pipe] && values[1] == ((h << 16) | w));
    assert(addresses[2] == 0x34c0 + pitches[pipe] && values[2] == pitch - 1);
    assert(regs[addresses[0] - 1] == 0xa5a5a5a5);
    assert(regs[addresses[2] + 1] == 0xa5a5a5a5);
}
static void invalid(struct AmdgpuDevice *ad, ULONG pipe,
                    ULONG w, ULONG h, ULONG pitch)
{
    reset();
    assert(!amdgpu_modeset_set_viewport(ad, pipe, w, h, pitch));
    assert(writes == 0 && reads == 0);
}
int main(void)
{
    struct AmdgpuDevice ad = { regs, {0, 0, 0x34c0} };
    unsigned i;
    for (i = 0; i < 4; i++) {
        valid(&ad, i, 1024, 768, 1024); /* native firmware oracle: pitch 0x3ff */
        valid(&ad, i, 1366, 768, 1376); /* aligned storage != visible width */
        valid(&ad, i, 1920, 1080, 1920);
        valid(&ad, i, 1, 1, 1);        /* minimum encodes zero */
        valid(&ad, i, 0x3fff, 0x3fff, 0x10000); /* max pitch encodes ffff */
    }
    invalid(NULL, 0, 1024, 768, 1024);
    invalid(&ad, 4, 1024, 768, 1024);
    ad.rmmio = NULL; invalid(&ad, 0, 1024, 768, 1024); ad.rmmio = regs;
    ad.dmu_seg[2] = 0; invalid(&ad, 0, 1024, 768, 1024); ad.dmu_seg[2] = 0x34c0;
    invalid(&ad, 0, 0, 768, 1024);
    invalid(&ad, 0, 1024, 0, 1024);
    invalid(&ad, 0, 1024, 768, 0);
    invalid(&ad, 0, 0x4000, 768, 0x4000);
    invalid(&ad, 0, 1024, 0x4000, 1024);
    invalid(&ad, 0, 1024, 768, 0x10001);
    invalid(&ad, 0, 1024, 768, 1023);
    invalid(&ad, 0, UINT32_MAX, UINT32_MAX, UINT32_MAX);
    for (i = 1; i <= 3; i++) {
        reset(); fail_read = i;
        assert(!amdgpu_modeset_set_viewport(&ad, 0, 1024, 768, 1024));
        assert(writes == 3 && reads == 3); /* refusal, not rollback */
    }
    puts("PASS: actual viewport helper pitch-1, all pipes, aligned stride, field bounds, no invalid writes, readback refusals");
    return 0;
}
