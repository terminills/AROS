/* Compile the actual driver helper against an MMIO/cache fixture. This proves
 * bounded control flow and completion discrimination, not hardware execution. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef uintptr_t IPTR;
typedef uint8_t UBYTE;
typedef void *APTR;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define DMA_ReadFromRAM 2
#define PACKET3(op,n) (((ULONG)3 << 30) | (((ULONG)(op) & 0xff) << 8) | (((ULONG)(n) & 0x3fff) << 16))
#define PACKET3_WRITE_DATA 0x37
#define WRITE_DATA_DST_SEL_MEM (5UL << 8)
#include "../amdgpu_gfx_mqd.h"
#include <asic_reg/gc/gc_12_0_0_offset.h>
struct AmdgpuDevice { int unused; };
static _Alignas(4096) ULONG buffer[65536 / 4];
static ULONG registers[0x5000], bank, submits, high_writes, flushes, posts;
static ULONG mode, fail_seq, delay, polls, shorten_flush;
static const UQUAD gpu_va = 0x12345678000ULL;
enum { OK, STALE, NO_ME, NO_PFP, WRONG_ME, BAD_RPTR, BAD_WB, BUSY,
       ME_LO, ME_HI, PFP_LO, PFP_HI };
static void bug(const char *fmt, ...) { (void)fmt; }
static ULONG gc0_r(struct AmdgpuDevice *ad, ULONG reg)
{ (void)ad; assert(reg < 0x5000); return registers[reg]; }
static ULONG gc1_r(struct AmdgpuDevice *ad, ULONG reg)
{ (void)ad; assert(reg == regGRBM_GFX_CNTL); return bank; }
static void gc0_w(struct AmdgpuDevice *ad, ULONG reg, ULONG value)
{
    (void)ad;
    if (reg == regCP_RB0_WPTR_HI) {
        assert(value == 0 && high_writes + 1 == submits);
        high_writes++;
        return;
    }
    assert(reg == regCP_RB0_WPTR && submits == high_writes);
    assert(value == 20 + submits * 10 && flushes == 4);
    assert(*(UQUAD *)((UBYTE *)buffer + 4104) == value);
    submits++; flushes = polls = 0;
    registers[regCP_RB0_WPTR] = value;
    registers[regCP_STAT] = 0x80038000;
}
static void CachePreDMA(APTR ptr, ULONG *length, ULONG flags)
{
    ULONG *p = ptr;
    assert(flags == DMA_ReadFromRAM && high_writes == submits);
    assert(flushes < 4);
    if (flushes < 2) {
        ULONG seq = submits + 1;
        ULONG want = (flushes ? 0x50460000UL : 0x4d450000UL) | seq;
        assert(p == buffer + (flushes ? 3071 : 2047) && *length == 12);
        assert(p[0] == 0x6ac0ffee && p[1] == ~want && p[2] == 0xc001cafe);
    } else if (flushes == 2) {
        ULONG seq = submits + 1;
        assert(p == buffer + 10 + submits * 10 && *length == 40);
        assert(p[0] == 0xc0033700 && p[1] == 0x500);
        assert(p[2] == (ULONG)(gpu_va + 8192) && p[3] == (ULONG)((gpu_va + 8192) >> 32));
        assert(p[4] == (0x4d450000UL | seq));
        assert(p[5] == 0xc0033700 && p[6] == 0x40000500);
        assert(p[7] == (ULONG)(gpu_va + 12288) && p[8] == (ULONG)((gpu_va + 12288) >> 32));
        assert(p[9] == (0x50460000UL | seq));
    } else {
        assert(p == buffer + 1026 && *length == 8);
    }
    flushes++;
    if (shorten_flush == flushes) *length -= 4;
}
static void CachePostDMA(APTR ptr, ULONG *length, ULONG flags)
{
    ULONG *p = ptr, fault = submits == fail_seq ? mode : OK;
    assert(flags == 0);
    assert((p == buffer + 1024 && *length == 4) ||
           ((p == buffer + 2047 || p == buffer + 3071) && *length == 12));
    posts++;
    if (!submits || p != buffer + 1024) return;
    assert(submits == high_writes);
    if (++polls <= delay) return;
    buffer[2048] = fault == STALE ? 0x600df00d :
        fault == NO_ME ? ~(0x4d450000UL | submits) :
        fault == WRONG_ME ? (0x4d450000UL | (submits - 1)) : (0x4d450000UL | submits);
    buffer[3072] = fault == STALE ? 0x600df00d :
        fault == NO_PFP ? ~(0x50460000UL | submits) : (0x50460000UL | submits);
    buffer[1024] = fault == BAD_WB ? 10 : registers[regCP_RB0_WPTR];
    registers[regCP_RB0_RPTR] = fault == BAD_RPTR ? 10 : registers[regCP_RB0_WPTR];
    registers[regCP_STAT] = fault == BUSY ? 0x80038000 : 0;
    if (fault == ME_LO) buffer[2047] ^= 1;
    if (fault == ME_HI) buffer[2049] ^= 1;
    if (fault == PFP_LO) buffer[3071] ^= 1;
    if (fault == PFP_HI) buffer[3073] ^= 1;
}
#include "gfx_repeat.inc"
static void reset(void)
{
    memset(buffer, 0xa5, sizeof(buffer));
    memset(registers, 0, sizeof(registers));
    registers[regCP_GFX_HQD_ACTIVE] = 1;
    registers[regCP_GFX_HQD_BASE] = (ULONG)(gpu_va >> 8);
    registers[regCP_GFX_HQD_BASE_HI] = (ULONG)(gpu_va >> 40);
    registers[regCP_RB0_RPTR] = buffer[1024] = 10;
    *(UQUAD *)((UBYTE *)buffer + 4104) = 10;
    bank = submits = high_writes = flushes = posts = mode = polls = shorten_flush = 0;
    fail_seq = 1; delay = 3;
}
static void intact(void)
{
    ULONG i;
    for (i = 0; i < 10; i++) assert(buffer[i] == 0xa5a5a5a5);
    for (i = 330; i < 1024; i++) assert(buffer[i] == 0xa5a5a5a5);
    /* Retained MQD, CSIB and all later pages must be untouched. */
    for (i = 4096; i < 65536 / 4; i++) assert(buffer[i] == 0xa5a5a5a5);
}
int main(void)
{
    struct AmdgpuDevice ad = {0};
    ULONG i;
    const ULONG bad_regs[] = { regCP_GFX_HQD_ACTIVE, regCP_GFX_HQD_BASE,
        regCP_GFX_HQD_BASE_HI, regCP_RB0_RPTR, regCP_STAT };
    reset();
    assert(gfx_v12_gfx_repeat_test(&ad, buffer, gpu_va));
    assert(submits == 32 && high_writes == 32 && polls == 4);
    assert(buffer[2048] == 0x4d450020 && buffer[3072] == 0x50460020);
    assert(buffer[1024] == 330 && registers[regCP_STAT] == 0);
    intact();
    for (i = STALE; i <= PFP_HI; i++) {
        reset(); mode = i;
        assert(!gfx_v12_gfx_repeat_test(&ad, buffer, gpu_va));
        assert(submits == 1 && high_writes == 1);
        assert(polls == (i >= ME_LO ? delay + 1 : GFX_REPEAT_POLLS));
        intact();
    }
    reset(); mode = WRONG_ME; fail_seq = 17;
    assert(!gfx_v12_gfx_repeat_test(&ad, buffer, gpu_va));
    assert(submits == 17 && high_writes == 17); intact();
    for (i = 0; i < sizeof(bad_regs) / sizeof(bad_regs[0]); i++) {
        reset(); registers[bad_regs[i]] ^= 1;
        assert(!gfx_v12_gfx_repeat_test(&ad, buffer, gpu_va));
        assert(!submits && !flushes && !posts);
    }
    reset(); bank = 1;
    assert(!gfx_v12_gfx_repeat_test(&ad, buffer, gpu_va) && !submits && !posts);
    reset(); buffer[1024] = 9;
    assert(!gfx_v12_gfx_repeat_test(&ad, buffer, gpu_va) && !submits && !flushes);
    reset(); buffer[1026] = 9;
    assert(!gfx_v12_gfx_repeat_test(&ad, buffer, gpu_va) && !submits && !flushes);
    for (i = 1; i <= 4; i++) {
        reset(); shorten_flush = i;
        assert(!gfx_v12_gfx_repeat_test(&ad, buffer, gpu_va));
        assert(!submits && !high_writes && flushes == i);
    }
    reset();
    assert(!gfx_v12_gfx_repeat_test(&ad, NULL, gpu_va));
    assert(!gfx_v12_gfx_repeat_test(&ad, buffer + 1, gpu_va));
    assert(!gfx_v12_gfx_repeat_test(&ad, buffer, 0));
    assert(!gfx_v12_gfx_repeat_test(&ad, buffer, gpu_va + 4));
    assert(!gfx_v12_gfx_repeat_test(&ad, buffer, 1ULL << 48));
    assert(!gfx_v12_gfx_repeat_test(&ad, buffer, (1ULL << 48) - 4096));
    assert(!submits && !flushes && !posts);
    puts("PASS: actual GFX reuse helper: 32 unique pairs, cache/packet ordering, bounds, stale/partial/guard/retirement/idle refusal, no retry");
    return 0;
}
