/* Actual pool structure/functions, bounded fake aperture, no hardware access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef uintptr_t IPTR;
typedef void *APTR;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define D(x) do {} while (0)
struct SignalSemaphore { unsigned held; };
static unsigned locks, unlocks;
static void ObtainSemaphore(struct SignalSemaphore *s)
{ assert(!s->held); s->held = 1; ++locks; }
static void ReleaseSemaphore(struct SignalSemaphore *s)
{ assert(s->held); s->held = 0; ++unlocks; }
static void bug(const char *fmt, ...) { (void)fmt; }
#include "pool_structure.inc"
#include "pool_functions.inc"
static _Alignas(256) unsigned char aperture[4096];
static struct HWData hw;
static void reset(void)
{
    memset(&hw, 0, sizeof(hw));
    hw.pool_lock_ready = TRUE;
    hw.vram_pool_cpu = aperture;
    hw.vram_pool_mc = UINT64_C(0x8001000000);
    hw.vram_pool_size = sizeof(aperture);
    locks = unlocks = 0;
}
static UQUAD allocate(ULONG bytes)
{
    APTR cpu = NULL; UQUAD mc = 0;
    assert(amdgpu_vram_alloc(&hw, bytes, 256, &cpu, &mc));
    assert(cpu == aperture + (mc - hw.vram_pool_mc));
    assert(!hw.pool_lock.held && locks == unlocks);
    return mc;
}
static void retire(UQUAD mc, ULONG size)
{
    ObtainSemaphore(&hw.pool_lock);
    amdgpu_vram_retire_scanout_locked(&hw, mc, size);
    ReleaseSemaphore(&hw.pool_lock);
}
int main(void)
{
    UQUAD a, b, c, d, out;
    APTR cpu;
    reset(); a = allocate(1024); b = allocate(1024);
    retire(a, 1024);
    amdgpu_vram_free(&hw, a, 1024);
    assert(hw.scanout_release_mc == a && hw.scanout_release_size == 1024);
    assert(!hw.vram_free_count && hw.vram_pool_used == 2048);
    c = allocate(1024); assert(c != a && c != b);
    retire(a, 1024); assert(hw.scanout_release_size == 1024);
    retire(b, 1024); assert(!hw.scanout_release_size);
    d = allocate(1024); assert(d == a);
    amdgpu_vram_free(&hw, c, 1024); assert(hw.vram_pool_used == 2048);
    amdgpu_vram_free(&hw, d, 1024); assert(hw.vram_free_count == 1);
    amdgpu_vram_free(&hw, b, 1024); assert(hw.scanout_release_size == 1024);
    retire(0, 0); assert(!hw.vram_pool_used && !hw.vram_free_count);
    assert(!hw.pool_lock.held && locks == unlocks);

    /* Faulted hardware retains both possible scanouts, including a candidate
       whose OOP object has already been disposed by the compositor. */
    reset(); a = allocate(1024); b = allocate(1024); retire(a, 1024);
    hw.plane_faulted = TRUE;
    amdgpu_vram_free(&hw, a, 1024); amdgpu_vram_free(&hw, b, 1024);
    cpu = (APTR)(IPTR)1; out = 7;
    assert(!amdgpu_vram_alloc(&hw, 256, 256, &cpu, &out));
    assert(cpu == (APTR)(IPTR)1 && out == 7);
    assert(hw.vram_pool_used == 2048 && !hw.vram_free_count);

    reset(); a = allocate(1024);
    amdgpu_vram_free(&hw, a + (UINT64_C(1) << 32), 1024);
    amdgpu_vram_free(&hw, a - 1, 1024);
    amdgpu_vram_free(&hw, a, 4097);
    assert(hw.vram_pool_used == 1024 && !hw.vram_free_count);
    assert(!amdgpu_vram_alloc(&hw, 256, 768, NULL, NULL));
    assert(!amdgpu_vram_alloc(&hw, 0, 256, NULL, NULL));
    assert(!amdgpu_vram_alloc(&hw, UINT32_MAX, 256, NULL, NULL));
    hw.vram_pool_used = UINT32_MAX - 5;
    assert(!amdgpu_vram_alloc(&hw, 256, 256, NULL, NULL));
    assert(!hw.pool_lock.held && locks == unlocks);

    reset(); a = allocate(1024); retire(a, 1024);
    amdgpu_vram_free(&hw, a, 1024);
    amdgpu_vram_free(&hw, a, 1024); /* repeated identical deferred free */
    assert(!hw.plane_faulted);
    amdgpu_vram_free(&hw, a + 256, 256); /* inconsistent overlap */
    assert(hw.plane_faulted && !hw.vram_free_count);
    assert(!hw.pool_lock.held && locks == unlocks);
    puts("PASS: actual VRAM pool serialized allocation, deferred active free, verified retirement, uncertain quarantine and wide-address bounds");
    return 0;
}
