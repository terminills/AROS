#include <assert.h>
#include <stdio.h>
#include "dcn_state_regs.h"
#ifdef VERIFY_LINUX_DCN41
#include "asic_reg/dcn/dcn_4_1_0_offset.h"
#include "asic_reg/dcn/dcn_4_1_0_sh_mask.h"
_Static_assert(DCN_STATE_FLIP_PENDING ==
               HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK,
               "read-only flip pending mask");
#define VERIFY(name, segment, offset) \
    _Static_assert(reg##name == (offset), #name " offset"); \
    _Static_assert(reg##name##_BASE_IDX == (segment), #name " segment");
DCN_STATE_REGS(VERIFY)
#undef VERIFY
#endif
#define BOUNDS(name, segment, offset) \
    _Static_assert((segment) == 2, #name " valid segment"); \
    _Static_assert(DCN_STATE_DMU2 + (offset) < DCN_STATE_BAR_BYTES / 4, \
                   #name " in BAR5");
DCN_STATE_REGS(BOUNDS)
#undef BOUNDS
int main(void)
{
    assert(DCN_STATE_DMU2 + 0x0607 == 0x3ac7);
    assert(DCN_STATE_DMU2 + 0x0d19 == 0x41d9);
    assert(DCN_STATE_DMU2 + 0x1b89 == 0x5049);
    puts("PASS: fixed DCN snapshot offsets, segment2, BAR bounds");
    return 0;
}
