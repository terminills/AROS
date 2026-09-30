/* Exercise the production drain helper with a register protocol model.
 * This verifies ordering/fail-closed behavior, not physical GPU execution. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t ULONG;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define GRBM_GFX_CNTL_MEID_SHIFT 2
enum { regGRBM_GFX_CNTL, regCP_HQD_ACTIVE, regCP_HQD_DEQUEUE_REQUEST,
       regCP_HQD_PQ_RPTR };

struct AmdgpuDevice {
    ULONG selected;
    unsigned active, requested, reads, drain_reads, inactive_after;
    unsigned requests, clears, rptr_resets, bank_writes;
};

static void gc1_w(struct AmdgpuDevice *ad, ULONG reg, ULONG value)
{
    assert(reg == regGRBM_GFX_CNTL);
    assert(value == 0 || value == (1UL << GRBM_GFX_CNTL_MEID_SHIFT));
    ad->selected = value;
    ad->bank_writes++;
}

static ULONG gc0_r(struct AmdgpuDevice *ad, ULONG reg)
{
    assert(ad->selected == (1UL << GRBM_GFX_CNTL_MEID_SHIFT));
    assert(reg == regCP_HQD_ACTIVE);
    ad->reads++;
    if (ad->requested) {
        ad->drain_reads++;
        if (ad->inactive_after && ad->drain_reads >= ad->inactive_after)
            ad->active = 0;
    }
    return ad->active;
}

static void gc0_w(struct AmdgpuDevice *ad, ULONG reg, ULONG value)
{
    assert(ad->selected == (1UL << GRBM_GFX_CNTL_MEID_SHIFT));
    if (reg == regCP_HQD_DEQUEUE_REQUEST) {
        assert(value <= 1);
        if (value) {
            assert(ad->active);
            ad->requested = 1;
            ad->requests++;
        } else {
            assert(!ad->active); /* Never cancel a drain before inactivity. */
            ad->requested = 0;
            ad->clears++;
        }
    } else {
        assert(reg == regCP_HQD_PQ_RPTR && value == 0);
        assert(!ad->active && !ad->requested);
        ad->rptr_resets++;
    }
}

static void bug(const char *format, ...) { (void)format; }
#include "cq_drain.inc"

int main(void)
{
    struct AmdgpuDevice ad;
    unsigned delay;

    memset(&ad, 0, sizeof(ad));
    assert(cq_quiesce(&ad));
    assert(ad.requests == 0 && ad.reads == 1);
    assert(ad.clears == 1 && ad.rptr_resets == 1);
    assert(ad.selected == 0 && ad.bank_writes == 2);

    for (delay = 1; delay <= 5; delay++) {
        memset(&ad, 0, sizeof(ad));
        ad.active = 1;
        ad.inactive_after = delay;
        assert(cq_quiesce(&ad));
        assert(ad.requests == 1 && ad.drain_reads == delay);
        assert(ad.clears == 1 && ad.rptr_resets == 1);
        assert(ad.selected == 0 && ad.bank_writes == 2);
        /* Reinstall, then drain another completed submission. */
        ad.active = 1;
        ad.drain_reads = 0;
        assert(cq_quiesce(&ad));
        assert(ad.requests == 2 && ad.clears == 2 && ad.rptr_resets == 2);
        assert(ad.selected == 0 && ad.bank_writes == 4);
    }

    memset(&ad, 0, sizeof(ad));
    ad.active = 1;
    ad.inactive_after = CQ_DRAIN_POLLS;
    assert(cq_quiesce(&ad)); /* Last permitted observation can succeed. */
    assert(ad.drain_reads == CQ_DRAIN_POLLS && ad.rptr_resets == 1);

    memset(&ad, 0, sizeof(ad));
    ad.active = 1; /* Hardware never acknowledges the request. */
    assert(!cq_quiesce(&ad));
    assert(ad.active && ad.requested && ad.requests == 1);
    assert(ad.drain_reads == CQ_DRAIN_POLLS && ad.reads == CQ_DRAIN_POLLS + 1);
    assert(ad.clears == 0 && ad.rptr_resets == 0);
    assert(ad.selected == 0 && ad.bank_writes == 2);
    puts("PASS: inactive, delayed drain, repeated handoff, boundary and timeout safety");
    return 0;
}
