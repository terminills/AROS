#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef uintptr_t IPTR;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define DMA_ReadFromRAM 2
#include "../amdgpu_gfx_csb.h"
#include <asic_reg/gc/gc_12_0_0_offset.h>
#include <asic_reg/gc/gc_12_0_0_sh_mask.h>
#define RLC_SRM_CNTL__SRM_ENABLE RLC_SRM_CNTL__SRM_ENABLE_MASK
#define RLC_SRM_CNTL__AUTO_INCR_ADDR RLC_SRM_CNTL__AUTO_INCR_ADDR_MASK
struct cs_extent_def { const unsigned int *extent; unsigned int reg_index, reg_count; };
enum { SECT_NONE, SECT_CONTEXT };
struct cs_section_def { const struct cs_extent_def *section; unsigned int id; };
#include <clearstate_gfx12.h>
struct AmdgpuDevice { int unused; };
static ULONG registers[0x5000], written[4], write_count, reject_reg, flushed;
static ULONG shorten_flush;
static void bug(const char *fmt, ...) { (void)fmt; }
static ULONG gc1_r(struct AmdgpuDevice *ad, ULONG reg)
{ (void)ad; assert(reg < 0x5000); return registers[reg]; }
static void gc1_w(struct AmdgpuDevice *ad, ULONG reg, ULONG value)
{
    (void)ad;
    assert(flushed && write_count < 4);
    written[write_count++] = reg;
    if (reg != reject_reg) registers[reg] = value;
}
static void CachePreDMA(void *ptr, ULONG *length, ULONG flags)
{
    assert(ptr && *length == 300 && flags == DMA_ReadFromRAM && !write_count);
    flushed++;
    if (shorten_flush) *length = 128;
}
#include "gfx_csb.inc"
static void reset(void)
{
    memset(registers, 0, sizeof(registers));
    registers[regRLC_SRM_CNTL] = 2;
    write_count = flushed = shorten_flush = 0;
    reject_reg = UINT32_MAX;
}
int main(void)
{
    _Alignas(4096) ULONG buffer[1024];
    ULONG expected[75], i = 1, e, r;
    UQUAD va = 0x12345678000ULL;
    struct AmdgpuDevice ad = { 0 };
    expected[0] = 6;
    for (e = 0; gfx12_cs_data[0].section[e].extent; e++) {
        const struct cs_extent_def *ext = &gfx12_cs_data[0].section[e];
        expected[i++] = ext->reg_count;
        expected[i++] = ext->reg_index;
        for (r = 0; r < ext->reg_count; r++) expected[i++] = ext->extent[r];
    }
    assert(e == 6 && i == 75);
    for (i = 0; i < 75; i++) {
        memset(buffer, 0xa5, sizeof(buffer));
        assert(!gfx12_build_csb(buffer, i));
        for (r = 0; r < 1024; r++) assert(buffer[r] == 0xa5a5a5a5);
    }
    assert(!gfx12_build_csb(NULL, 75));
    assert(gfx12_build_csb(buffer, 75) == 75);
    assert(!memcmp(buffer, expected, sizeof(expected)));
    assert(buffer[75] == 0xa5a5a5a5);
    reset();
    assert(gfx_v12_init_csb(&ad, buffer, va));
    assert(write_count == 4 && written[0] == regRLC_CSIB_ADDR_HI &&
           written[1] == regRLC_CSIB_ADDR_LO && written[2] == regRLC_CSIB_LENGTH &&
           written[3] == regRLC_SRM_CNTL);
    assert(registers[regRLC_CSIB_ADDR_HI] == (ULONG)(va >> 32));
    assert(registers[regRLC_CSIB_ADDR_LO] == (ULONG)va);
    assert(registers[regRLC_CSIB_LENGTH] == 75 && registers[regRLC_SRM_CNTL] == 3);
    for (i = 0; i < 4; i++) {
        ULONG regs[] = {regRLC_CSIB_ADDR_LO, regRLC_CSIB_ADDR_HI, regRLC_CSIB_LENGTH, regRLC_SRM_CNTL};
        reset(); registers[regs[i]] = 1;
        assert(!gfx_v12_init_csb(&ad, buffer, va));
        assert(!write_count && !flushed);
        reset(); reject_reg = regs[i];
        assert(!gfx_v12_init_csb(&ad, buffer, va));
        assert(write_count == (i == 3 ? 4 : 3));
        assert(!(registers[regRLC_SRM_CNTL] & 1));
    }
    reset(); shorten_flush = 1;
    assert(!gfx_v12_init_csb(&ad, buffer, va) && !write_count);
    reset();
    assert(!gfx_v12_init_csb(&ad, NULL, va));
    assert(!gfx_v12_init_csb(&ad, buffer + 1, va));
    assert(!gfx_v12_init_csb(&ad, buffer, 0));
    assert(!gfx_v12_init_csb(&ad, buffer, va + 4));
    assert(!gfx_v12_init_csb(&ad, buffer, 1ULL << 48));
    assert(!write_count && !flushed);
    puts("PASS: CSIB upstream byte equality, bounds, cache/write ordering and refusal paths");
    return 0;
}
