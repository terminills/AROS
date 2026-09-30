/* Exercise the production one-shot MES handoff using an MMIO/cache model. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef unsigned char UBYTE;
typedef void *APTR;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define DMA_ReadFromRAM 1
#define GRBM_GFX_CNTL_MEID_SHIFT 2
#include "../amdgpu_gfx_mqd.h"
enum { regGRBM_GFX_CNTL, regCP_HQD_ACTIVE, regCP_HQD_PQ_RPTR,
       regCP_HQD_PQ_WPTR_LO };
struct AmdgpuDevice { volatile ULONG *doorbell; };
static struct {
    struct AmdgpuDevice *ad;
    ULONG *buf;
    UQUAD va;
    BOOL ready;
} gfx_mes_map_state;
static ULONG bank, active, rptr, wptr, kicks, flushes, posts, reads, retire_at;
static UQUAD result_ack, result_query;
static _Alignas(8) ULONG buffer[16384], doorbells[64];

static void bug(const char *format, ...) { (void)format; }
static void gc1_w(struct AmdgpuDevice *ad, ULONG reg, ULONG value)
{
    (void)ad;
    assert(reg == regGRBM_GFX_CNTL && (value == 0 || value == 12));
    bank = value;
}
static ULONG gc0_r(struct AmdgpuDevice *ad, ULONG reg)
{
    (void)ad;
    assert(bank == 12);
    if (reg == regCP_HQD_ACTIVE) return active;
    if (reg == regCP_HQD_PQ_WPTR_LO) return wptr;
    assert(reg == regCP_HQD_PQ_RPTR);
    if (kicks && ++reads == retire_at) rptr = 256;
    return rptr;
}
static void gc0_w(struct AmdgpuDevice *ad, ULONG reg, ULONG value)
{
    (void)ad;
    assert(bank == 12 && reg == regCP_HQD_PQ_WPTR_LO && value == 256);
    assert(flushes == 3); /* ACKs, frames, then WPTR visible before kick. */
    assert(buffer[128] == 0x40021 && buffer[192] == 0x400b1);
    assert(*(UQUAD *)((UBYTE *)buffer + 4104) == 256);
    kicks++;
    wptr = value;
}
static void CachePreDMA(APTR p, ULONG *len, ULONG flags)
{
    assert(bank == 0 && flags == DMA_ReadFromRAM && !kicks);
    if (flushes == 0) assert(p == (UBYTE *)buffer + 57352 && *len == 16);
    if (flushes == 1) assert(p == buffer + 128 && *len == 512);
    if (flushes == 2) assert(p == (UBYTE *)buffer + 4104 && *len == 8);
    flushes++;
}
static void CachePostDMA(APTR p, ULONG *len, ULONG flags)
{
    assert(bank == 0 && flags == 0 && kicks == 1);
    assert(p == (UBYTE *)buffer + 57352 && *len == 16);
    posts++;
    *(volatile UQUAD *)p = result_ack;
    *(volatile UQUAD *)((UBYTE *)p + 8) = result_query;
}
#include "gfx_map.inc"

static void setup(struct AmdgpuDevice *ad)
{
    memset(buffer, 0xa5, sizeof(buffer));
    memset(doorbells, 0, sizeof(doorbells));
    ad->doorbell = doorbells;
    gfx_mes_map_state.ad = ad;
    gfx_mes_map_state.buf = buffer;
    gfx_mes_map_state.va = 0x2000000;
    gfx_mes_map_state.ready = TRUE;
    bank = kicks = flushes = posts = reads = 0;
    active = 1; rptr = wptr = 128; retire_at = 1;
    result_ack = result_query = 1;
}

int main(void)
{
    struct AmdgpuDevice ad, other;
    unsigned which;
    setup(&ad);
    assert(!gfx_v12_mes_map_graphics(&other, 0x2104000, 0x2101008));
    assert(gfx_mes_map_state.ready && !kicks && !flushes);
    assert(gfx_v12_mes_map_graphics(&ad, 0x2104000, 0x2101008));
    assert(kicks == 1 && posts == 1 && bank == 0 && !gfx_mes_map_state.ready);
    assert(!gfx_v12_mes_map_graphics(&ad, 0x2104000, 0x2101008));
    assert(kicks == 1); /* No replay into an already-used ring. */

    setup(&ad); retire_at = 10000;
    assert(gfx_v12_mes_map_graphics(&ad, 0x2104000, 0x2101008));
    assert(reads == 10000);
    setup(&ad); retire_at = 10001;
    assert(!gfx_v12_mes_map_graphics(&ad, 0x2104000, 0x2101008));
    assert(reads == 10000 && bank == 0 && !gfx_mes_map_state.ready);
    for (which = 0; which < 3; which++) {
        setup(&ad);
        if (which == 0) active = 0;
        if (which == 1) rptr = 64;
        if (which == 2) wptr = 192;
        assert(!gfx_v12_mes_map_graphics(&ad, 0x2104000, 0x2101008));
        assert(!kicks && !flushes && bank == 0 && !gfx_mes_map_state.ready);
        assert(buffer[128] == 0xa5a5a5a5); /* Preserve unconsumed frames. */
    }
    setup(&ad);
    assert(!gfx_v12_mes_map_graphics(&ad, 0x2104001, 0x2101008));
    assert(!kicks && !flushes && bank == 0);
    setup(&ad); result_ack = 0x81000201ULL << 32; result_query = 0;
    assert(!gfx_v12_mes_map_graphics(&ad, 0x2104000, 0x2101008));
    assert(posts == 1 && kicks == 1 && !gfx_mes_map_state.ready && bank == 0);
    setup(&ad); result_query = 0;
    assert(!gfx_v12_mes_map_graphics(&ad, 0x2104000, 0x2101008));
    assert(posts == 3000000 && !gfx_mes_map_state.ready && bank == 0);
    puts("PASS: MES handoff ACK/query/retirement, ordering, bounded failures, no ambiguous replay");
    return 0;
}
