#include <assert.h>
#include <stdio.h>
#include "gfx_state_regs.h"
#ifdef VERIFY_LINUX_GFX12
#include "asic_reg/gc/gc_12_0_0_offset.h"
#define VERIFY(name, segment, offset) \
    _Static_assert(reg##name == (offset), #name " offset"); \
    _Static_assert(reg##name##_BASE_IDX == (segment), #name " segment");
GFX_STATE_REGS(VERIFY)
#undef VERIFY
#endif
#define BOUNDS(name, segment, offset) \
    _Static_assert((segment) == 0 || (segment) == 1, #name " valid segment"); \
    _Static_assert(((segment) ? GFX_STATE_GC1 : GFX_STATE_GC0) + (offset) < \
                   GFX_STATE_BAR_BYTES / 4, #name " in BAR5");
GFX_STATE_REGS(BOUNDS)
#undef BOUNDS
int main(void)
{
    assert(GFX_STATE_GC1 + 0x0987 == 0xa987);
    assert(GFX_STATE_GC1 + 0x4c80 == 0xec80);
    assert(GFX_STATE_GC0 + 0x0f41 == 0x21a1);
    puts("PASS: fixed GFX snapshot register offsets, segments, BAR bounds");
    return 0;
}
