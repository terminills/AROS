/* Managed memory accounting on an installed, physically rebooted driver.
 * No private driver, MMIO, raw command stream, or hardware reinitialization.
 */
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
#define LOG(...) do { printf("[AmdgpuMemoryInfoProbe] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuMemoryInfoProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define NEED(c, s) do { if (!(c)) { LOG("FAIL %s", s); goto out; } \
                         LOG("PASS %s", s); } while (0)

static BOOL bytes_are(const void *data, ULONG size, UBYTE value)
{
    const UBYTE *p = data;
    while (size--) if (*p++ != value) return FALSE;
    return TRUE;
}

static BOOL snapshot(struct AmdgpuDevice *ad, struct AmdgpuMemoryInfo *info)
{
    return !amdgpu_query_memory_info(ad, info, sizeof(*info)) &&
        info->abi_version == AMDGPU_MEMORY_INFO_ABI_VERSION &&
        info->shared_used == info->vram_backing + info->gtt_backing &&
        info->shared_used <= info->shared_budget &&
        info->shared_available == info->shared_budget - info->shared_used &&
        info->vram_payload <= info->vram_backing && info->gtt_payload <= info->gtt_backing &&
        info->released_handles <= info->live_handles &&
        info->live_handles + info->reusable_handles <= info->max_handles && !info->reserved;
}

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad = NULL;
    struct AmdgpuBoCaps caps;
    struct AmdgpuMemoryInfo baseline, charged, next;
    struct { struct AmdgpuMemoryInfo info; ULONG guard[2]; } bounded;
    ULONG gtt = 0, vram = 0, gmaps = 0, vmaps = 0, round, bad = 0;
    BOOL gowner = FALSE, vowner = FALSE, bad_owner = FALSE, have_baseline = FALSE;
    APTR cpu;
    LONG result = RETURN_FAIL, rejected;

    Forbid(); resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd"); Permit();
    if (!resident) { LOG("REFUSED no resident driver; no load attempted"); return result; }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    LOG("resident=%p opened=%p revision=%lu.%lu negative-bytes=%lu", resident, AmdgpuBase,
        (unsigned long)AmdgpuBase->lib_Version, (unsigned long)AmdgpuBase->lib_Revision,
        (unsigned long)AmdgpuBase->lib_NegSize);
    NEED(AmdgpuBase == resident && AmdgpuBase->lib_NegSize >= 44 * sizeof(APTR) &&
        (AmdgpuBase->lib_Version > 0 || AmdgpuBase->lib_Revision >= 4),
        "resident identity, memory ABI revision0.4 and vector44 extent");
    NEED(amdgpu_get_device_count() == 1 && (ad = amdgpu_get_device(0)), "one resident device");
    NEED(!amdgpu_query_bo_caps(ad, &caps, sizeof(caps)) && snapshot(ad, &baseline),
        "coherent managed storage snapshot");
    have_baseline = TRUE;
    NEED(baseline.domains == caps.domains && baseline.page_size == caps.page_size &&
         baseline.max_handles == caps.max_handles && baseline.shared_budget == caps.budget &&
         baseline.max_allocation == caps.max_allocation &&
         baseline.domains == (AMDGPU_BO_DOMAIN_VRAM | AMDGPU_BO_DOMAIN_GTT),
         "quota and limits agree with storage ABI; both domains share one quota");
    LOG("quota=%llu used=%llu available=%llu VRAM=%llu GTT=%llu handles=%lu/%lu",
        (unsigned long long)baseline.shared_budget, (unsigned long long)baseline.shared_used,
        (unsigned long long)baseline.shared_available, (unsigned long long)baseline.vram_backing,
        (unsigned long long)baseline.gtt_backing, (unsigned long)baseline.live_handles,
        (unsigned long)baseline.max_handles);
    for (round = 0; round < 256; round++)
        if (!snapshot(ad, &next) || memcmp(&baseline, &next, sizeof(next))) {
            LOG("FAIL idle snapshot changed round=%lu", (unsigned long)round); goto out;
        }
    LOG("PASS 256 stable idle snapshots");
    memset(&bounded, 0xa5, sizeof(bounded));
    NEED(amdgpu_query_memory_info(ad, &bounded.info, sizeof(next) - 1) == -EINVAL &&
         bytes_are(&bounded, sizeof(bounded), 0xa5), "short buffer untouched");
    NEED(!amdgpu_query_memory_info(ad, &bounded.info, sizeof(bounded)) &&
         !memcmp(&baseline, &bounded.info, sizeof(baseline)) &&
         bytes_are(bounded.guard, sizeof(bounded.guard), 0xa5), "larger output tail preserved");
    NEED(amdgpu_query_memory_info(ad, NULL, sizeof(next)) == -EINVAL, "null output rejected");
    memset(&next, 0xa5, sizeof(next));
    NEED(amdgpu_query_memory_info(NULL, &next, sizeof(next)) == -ENODEV &&
         bytes_are(&next, sizeof(next), 0), "null device yields empty output");
    memset(&next, 0xa5, sizeof(next));
    NEED(amdgpu_query_memory_info((struct AmdgpuDevice *)(IPTR)1, &next, sizeof(next)) == -ENODEV &&
         bytes_are(&next, sizeof(next), 0), "unknown device rejected without dereference");

    NEED(!amdgpu_bo_alloc(ad, 8193, 65536, AMDGPU_BO_DOMAIN_GTT, 0, &gtt), "GTT aligned allocation");
    gowner = TRUE;
    NEED(!amdgpu_bo_alloc(ad, 4097, 0, AMDGPU_BO_DOMAIN_VRAM, 0, &vram), "VRAM allocation");
    vowner = TRUE;
    NEED(snapshot(ad, &charged) && charged.gtt_payload == baseline.gtt_payload + 12288 &&
         charged.vram_payload == baseline.vram_payload + 8192 &&
         charged.gtt_backing == baseline.gtt_backing + 12288 + 65535 &&
         charged.vram_backing == baseline.vram_backing + 8192 +
             ((AmdgpuBase->lib_Version || AmdgpuBase->lib_Revision >= 11) ? 0 : 3840) &&
         charged.live_handles == baseline.live_handles + 2 &&
         charged.reusable_handles + 2 == baseline.reusable_handles,
         "rounded payload and exact owned alignment padding charged once");
    NEED(!amdgpu_bo_cpu_map(ad, gtt, &cpu), "first GTT borrow"); gmaps++;
    NEED(!amdgpu_bo_cpu_map(ad, gtt, &cpu), "second GTT borrow"); gmaps++;
    NEED(!amdgpu_bo_cpu_map(ad, vram, &cpu), "VRAM borrow"); vmaps++;
    NEED(!amdgpu_bo_free(ad, gtt), "release GTT owner"); gowner = FALSE;
    NEED(!amdgpu_bo_free(ad, vram), "release VRAM owner"); vowner = FALSE;
    NEED(snapshot(ad, &next) && next.shared_used == charged.shared_used &&
         next.live_handles == charged.live_handles &&
         next.released_handles == baseline.released_handles + 2 &&
         next.cpu_borrows == baseline.cpu_borrows + 3,
         "owner release does not under-report retained backing or handles");
    NEED(!amdgpu_bo_cpu_unmap(ad, gtt), "drop one GTT borrow"); gmaps--;
    NEED(snapshot(ad, &next) && next.shared_used == charged.shared_used &&
         next.cpu_borrows == baseline.cpu_borrows + 2, "remaining borrow retains the entire charge");
    NEED(!amdgpu_bo_cpu_unmap(ad, gtt), "drop last GTT borrow"); gmaps--; gtt = 0;
    NEED(snapshot(ad, &next) && next.gtt_backing == baseline.gtt_backing &&
         next.gtt_payload == baseline.gtt_payload && next.vram_backing == charged.vram_backing &&
         next.live_handles == baseline.live_handles + 1 &&
         next.released_handles == baseline.released_handles + 1,
         "last GTT borrow retires only its own domain's charge");
    NEED(!amdgpu_bo_cpu_unmap(ad, vram), "drop last VRAM borrow"); vmaps--; vram = 0;
    NEED(snapshot(ad, &next) && !memcmp(&baseline, &next, sizeof(next)), "all accounting returns to baseline");
    bad = 0xa5a5a5a5UL;
    rejected = amdgpu_bo_alloc(ad, caps.max_allocation + 1, 0, AMDGPU_BO_DOMAIN_GTT, 0, &bad);
    bad_owner = !rejected && bad;
    NEED(rejected == -EINVAL &&
         !bad && snapshot(ad, &next) && !memcmp(&baseline, &next, sizeof(next)),
         "rejected allocation leaves empty output and unchanged accounting");
    result = RETURN_OK;
out:
    if (ad) {
        if (bad_owner && amdgpu_bo_free(ad, bad)) result = RETURN_FAIL;
        if (gowner && amdgpu_bo_free(ad, gtt)) result = RETURN_FAIL;
        if (vowner && amdgpu_bo_free(ad, vram)) result = RETURN_FAIL;
        while (gmaps) { if (amdgpu_bo_cpu_unmap(ad, gtt)) result = RETURN_FAIL; gmaps--; }
        while (vmaps) { if (amdgpu_bo_cpu_unmap(ad, vram)) result = RETURN_FAIL; vmaps--; }
        if (have_baseline && (!snapshot(ad, &next) || memcmp(&baseline, &next, sizeof(next)))) {
            LOG("FAIL cleanup accounting differs from baseline"); result = RETURN_FAIL;
        }
    }
    CloseLibrary(AmdgpuBase);
    LOG("END result=%ld; managed memory accounting only, not shader rendering or allocation guarantee",
        (long)result);
    return result;
}
