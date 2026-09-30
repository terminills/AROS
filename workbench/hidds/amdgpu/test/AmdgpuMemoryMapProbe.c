/* Run only against the installed, physically rebooted library0.11 or later.
 * Verifies real CPU access to driver-owned VRAM; NOT proof of3D rendering. */
#define DEBUG 1
#define __INLINE_AMDGPU_STACKCALL__ 1
#include <aros/debug.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/amdgpu.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct Library *AmdgpuBase;
#define COUNT 20
#define BYTES (2UL << 20)
#define LOG(...) do { printf("[AmdgpuMemoryMapProbe] " __VA_ARGS__); printf("\n"); \
    bug("[AmdgpuMemoryMapProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define NEED(x, s) do { if (!(x)) { LOG("FAIL %s", s); goto out; } LOG("PASS %s", s); } while (0)

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad = NULL;
    struct AmdgpuMemoryInfo2 base, next;
    struct { struct AmdgpuMemoryInfo2 info; UQUAD guard; } bounded;
    ULONG handles[COUNT] = {0};
    APTR ptrs[COUNT] = {0};
    ULONG i, page, result = RETURN_FAIL;
    BOOL have_base = FALSE, owner_released = FALSE;
    Forbid(); resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd"); Permit();
    if (!resident) { LOG("REFUSED no resident driver; no load attempted"); return result; }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    LOG("resident=%p opened=%p revision=%lu.%lu negative-bytes=%lu", resident, AmdgpuBase,
        (unsigned long)AmdgpuBase->lib_Version, (unsigned long)AmdgpuBase->lib_Revision,
        (unsigned long)AmdgpuBase->lib_NegSize);
    NEED(AmdgpuBase == resident && AmdgpuBase->lib_NegSize >= 47 * sizeof(APTR) &&
        (AmdgpuBase->lib_Version || AmdgpuBase->lib_Revision >= 11), "resident0.11 and appended vector47");
    NEED(amdgpu_get_device_count() == 1 && (ad = amdgpu_get_device(0)), "one resident device");
    NEED(!amdgpu_query_memory_info2(ad, &base, sizeof(base)), "hardware-map memory snapshot");
    have_base = TRUE;
    LOG("total=%llu visible=%llu reserved=%llu usableVRAM=%llu GTT=%llu used=%llu/%llu",
        (unsigned long long)base.total_vram, (unsigned long long)base.visible_vram,
        (unsigned long long)base.reserved_vram, (unsigned long long)base.vram_budget,
        (unsigned long long)base.gtt_budget, (unsigned long long)base.vram_used,
        (unsigned long long)base.gtt_used);
    NEED(base.abi_version == 2 && !base.reserved && base.page_size == 4096 &&
        base.visible_vram <= base.total_vram && base.reserved_vram < base.visible_vram &&
        base.vram_budget == base.visible_vram - base.reserved_vram &&
        base.vram_available == base.vram_budget - base.vram_used &&
        base.gtt_available <= base.gtt_budget - base.gtt_used,
        "independent heaps and exact reserved-range subtraction");
    NEED(base.vram_budget > (1ULL << 32) && base.vram_available >= (UQUAD)COUNT * BYTES &&
        base.reusable_handles >= COUNT, "DUT usable VRAM exceeds4GiB, no tiny bootstrap quota");
    memset(&bounded, 0xa5, sizeof(bounded));
    NEED(amdgpu_query_memory_info2(ad, &bounded.info, sizeof(base) - 1) == -EINVAL &&
        bounded.guard == 0xa5a5a5a5a5a5a5a5ULL && bounded.info.abi_version == 0xa5a5a5a5UL,
        "short output rejected without overwrite");
    NEED(!amdgpu_query_memory_info2(ad, &bounded.info, sizeof(bounded)) &&
        bounded.guard == 0xa5a5a5a5a5a5a5a5ULL, "larger output tail preserved");
    for (i = 0; i < COUNT; i++) {
        NEED(!amdgpu_bo_alloc(ad, BYTES, BYTES, AMDGPU_BO_DOMAIN_VRAM, 0, &handles[i]),
             "real aligned2MiB VRAM allocation");
        NEED(!amdgpu_bo_cpu_map(ad, handles[i], &ptrs[i]) && !((IPTR)ptrs[i] & (BYTES - 1)),
             "borrowed aligned CPU BAR mapping");
        for (page = 0; page < BYTES / sizeof(ULONG); page += 4096 / sizeof(ULONG)) {
            volatile ULONG *p = ptrs[i];
            if (p[page] != 0) {
                LOG("FAIL new payload not zero at BO%lu page%lu", (unsigned long)i, (unsigned long)page);
                goto out;
            }
            p[page] = 0x6d000000UL ^ (i << 16) ^ page;
        }
    }
    __sync_synchronize();
    NEED(!amdgpu_query_memory_info2(ad, &next, sizeof(next)) &&
        next.vram_used == base.vram_used + (UQUAD)COUNT * BYTES &&
        next.gtt_used == base.gtt_used && next.live_handles == base.live_handles + COUNT,
        "40MiB simultaneous VRAM, no duplicate alignment charge or GTT debit");
    for (i = 0; i < COUNT; i++) {
        volatile ULONG *p = ptrs[i];
        for (page = 0; page < BYTES / sizeof(ULONG); page += 4096 / sizeof(ULONG))
            if (p[page] != (0x6d000000UL ^ (i << 16) ^ page)) {
                LOG("FAIL readback alias at BO%lu page%lu", (unsigned long)i, (unsigned long)page); goto out;
            }
    }
    LOG("PASS distinct patterns on every page of20 simultaneously retained BOs");
    NEED(!amdgpu_bo_free(ad, handles[0]), "drop first owner while CPU borrow retains backing");
    owner_released = TRUE;
    NEED(!amdgpu_query_memory_info2(ad, &next, sizeof(next)) &&
        next.vram_used == base.vram_used + (UQUAD)COUNT * BYTES &&
        next.released_handles == base.released_handles + 1, "released owner remains charged");
    result = RETURN_OK;
out:
    if (ad) {
        for (i = 0; i < COUNT; i++) {
            if (ptrs[i] && amdgpu_bo_cpu_unmap(ad, handles[i])) result = RETURN_FAIL;
            if (handles[i] && !(i == 0 && owner_released) && amdgpu_bo_free(ad, handles[i])) result = RETURN_FAIL;
        }
        if (have_base && (amdgpu_query_memory_info2(ad, &next, sizeof(next)) ||
            next.vram_used != base.vram_used || next.gtt_used != base.gtt_used ||
            next.live_handles != base.live_handles || next.cpu_borrows != base.cpu_borrows)) {
            LOG("FAIL cleanup did not restore baseline"); result = RETURN_FAIL;
        }
    }
    CloseLibrary(AmdgpuBase);
    LOG("END result=%lu; native mapped VRAM only, not3D rendering", (unsigned long)result);
    return result;
}
