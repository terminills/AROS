/* Host regression harness for the production allocation helpers extracted by
 * test_gfx_alignment.sh. No GPU access; the GART mapper is an explicit stub. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef uintptr_t IPTR;
typedef void *APTR;
struct AmdgpuDevice { int unused; };
#define MEMF_ANY 0
#define MEMF_CLEAR 1

static unsigned char arena[300000] __attribute__((aligned(4096)));
static ULONG offset, requested, freed, unmapped;
static int allocation_fails, mapping_fails, mapping_unaligned;
static APTR allocated;

static APTR AllocMem(ULONG size, ULONG flags)
{
    assert(flags == MEMF_CLEAR);
    requested = size;
    allocated = arena + offset;
    return allocation_fails ? NULL : allocated;
}
static void FreeMem(APTR ptr, ULONG size)
{
    assert(ptr == allocated && size == requested);
    freed++;
}
static UQUAD amdgpu_gart_map(struct AmdgpuDevice *ad, APTR ptr, ULONG size)
{
    (void)ad;
    assert(size == 65536);
    return mapping_fails ? 0 : 0x2000000ULL + ((IPTR)ptr & 4095) +
                                      (mapping_unaligned ? 16 : 0);
}
static void amdgpu_gart_unmap(struct AmdgpuDevice *ad, UQUAD va, ULONG size)
{
    (void)ad;
    assert(va == 0x2000010 && size == 65536);
    unmapped++;
}
static void bug(const char *fmt, ...) { (void)fmt; }

#include "helpers.inc"

int main(void)
{
    struct AmdgpuDevice ad = { 0 };
    APTR raw, ptr;
    UQUAD va;
    unsigned i;

    for (i = 0; i < 4096; i++) {
        offset = i;
        ptr = gfx_alloc_queue_buffer(&ad, &raw, &va);
        assert(ptr && raw == allocated && va == 0x2000000);
        assert(((IPTR)ptr & 4095) == 0);
        assert((unsigned char *)ptr >= (unsigned char *)raw);
        assert((unsigned char *)ptr + 65536 <= (unsigned char *)raw + requested);
        /* GPU's 256-byte address encoding preserves ring/EOP/shader starts. */
        assert(((va >> 8) << 8) == va);
        assert((((va + 0x5000) >> 8) << 8) == va + 0x5000);
        FreeMem(raw, requested);
    }
    freed = unmapped = 0;
    allocation_fails = 1;
    assert(!gfx_alloc_queue_buffer(&ad, &raw, &va) && !va && !freed);
    allocation_fails = 0;
    mapping_fails = 1;
    assert(!gfx_alloc_queue_buffer(&ad, &raw, &va) && !raw && !va && freed == 1);
    mapping_fails = 0;
    mapping_unaligned = 1;
    assert(!gfx_alloc_queue_buffer(&ad, &raw, &va) && !raw && !va);
    assert(freed == 2 && unmapped == 1);
    assert(!gfx_alloc_aligned(0, &raw));
    assert(!gfx_alloc_aligned(UINT32_MAX, &raw));
    puts("PASS: 4096 allocation offsets, address encoding, bounds, and failure cleanup");
    return 0;
}
