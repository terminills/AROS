/* Actual production lock/unlock helpers; bounded fake MMIO, no GPU access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t ULONG;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
struct AmdgpuDevice { void *rmmio; ULONG dmu_seg[3]; };
static ULONG control_dw, lock_dw, initial, lock_value;
static unsigned reads, writes, polls, acquire_after, vanish_after;
static BOOL absent, requested;
static void bug(const char *format, ...) { (void)format; }
static ULONG amdgpu_rreg32(struct AmdgpuDevice *ad, ULONG dw)
{
    assert(ad && (dw == control_dw || dw == lock_dw));
    ++reads;
    if (dw == control_dw)
        return absent ? UINT32_MAX : 1;
    if (!requested)
        return initial;
    ++polls;
    assert(polls <= 5000);
    if (vanish_after && polls >= vanish_after)
        return UINT32_MAX;
    return acquire_after && polls >= acquire_after ? 0x101 : lock_value;
}
static void amdgpu_wreg32(struct AmdgpuDevice *ad, ULONG dw, ULONG value)
{
    assert(ad && dw == lock_dw && (value == 0 || value == 1));
    assert(!(vanish_after && polls >= vanish_after));
    ++writes;
    assert(writes <= 2);
    lock_value = value;
    if (value)
        requested = TRUE;
}
#include "modeset_lock.inc"
static void reset(unsigned pipe)
{
    /* Independent DCN4.1 offsets; pipe stride0x80. */
    control_dw = 0x34c0 + 0x1b43 + pipe * 0x80;
    lock_dw = 0x34c0 + 0x1b89 + pipe * 0x80;
    initial = lock_value = 0;
    reads = writes = polls = acquire_after = vanish_after = 0;
    absent = requested = FALSE;
}
int main(void)
{
    struct AmdgpuDevice ad = { &ad, {0, 0, 0x34c0} };
    unsigned p, i;
    for (p = 0; p < 4; ++p) {
        for (i = 1; i <= 5000; i += 4999) {
            reset(p); acquire_after = i;
            assert(amdgpu_modeset_lock(&ad, p));
            assert(writes == 1 && polls == i && lock_value == 1);
            amdgpu_modeset_unlock(&ad, p);
            assert(writes == 2 && lock_value == 0);
        }
        reset(p);
        assert(!amdgpu_modeset_lock(&ad, p));
        assert(polls == 5000 && writes == 2 && lock_value == 0);
        for (i = 1; i <= 3; ++i) {
            reset(p); initial = i == 1 ? 1 : i == 2 ? 0x100 : 0x101;
            assert(!amdgpu_modeset_lock(&ad, p));
            assert(writes == 0 && polls == 0);
        }
        reset(p); initial = UINT32_MAX;
        assert(!amdgpu_modeset_lock(&ad, p));
        assert(writes == 0 && polls == 0);
        reset(p); absent = TRUE;
        assert(!amdgpu_modeset_lock(&ad, p));
        assert(reads == 1 && writes == 0);
        reset(p); vanish_after = 7;
        assert(!amdgpu_modeset_lock(&ad, p));
        assert(writes == 1 && polls == 7); /* no write to vanished hardware */
    }
    reset(0);
    assert(!amdgpu_modeset_lock(NULL, 0));
    assert(!amdgpu_modeset_lock(&ad, 4));
    ad.rmmio = NULL; assert(!amdgpu_modeset_lock(&ad, 0)); ad.rmmio = &ad;
    ad.dmu_seg[2] = 0; assert(!amdgpu_modeset_lock(&ad, 0));
    assert(reads == 0 && writes == 0);
    puts("PASS: actual modeset lock bounds, cancellation, ownership refusal and no writes after device loss (all four pipes)");
    return 0;
}
