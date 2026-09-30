/* Native client of the already-installed/rebooted storage-v1 public ABI.
 * No driver objects/autoinit, register writes, GPU commands or display changes.
 * Exercises real VRAM storage and lifetime, not a host-side allocator model.
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
#define LOG(...) do { printf("[AmdgpuBoProbe] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuBoProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define NEED(c, s) do { if (!(c)) { LOG("FAIL %s", s); goto out; } \
                         LOG("PASS %s", s); } while (0)

struct Owned { ULONG handle, maps; APTR cpu; BOOL released; };
static struct AmdgpuDevice *device;

static BOOL allocate(struct Owned *o, UQUAD size, UQUAD alignment)
{
    return amdgpu_bo_alloc(device, size, alignment, AMDGPU_BO_DOMAIN_VRAM,
                          0, &o->handle) == 0 && o->handle;
}

static BOOL map(struct Owned *o)
{
    APTR cpu = NULL;
    if (amdgpu_bo_cpu_map(device, o->handle, &cpu) || !cpu) return FALSE;
    o->maps++;
    if (o->cpu && o->cpu != cpu) return FALSE;
    o->cpu = cpu;
    return TRUE;
}

static BOOL drop_owner(struct Owned *o)
{
    if (amdgpu_bo_free(device, o->handle)) return FALSE;
    o->released = TRUE;
    return TRUE;
}

static BOOL unmap(struct Owned *o)
{
    if (!o->maps || amdgpu_bo_cpu_unmap(device, o->handle)) return FALSE;
    o->maps--;
    if (!o->maps) o->cpu = NULL;
    return TRUE;
}

static BOOL cleanup(struct Owned *o)
{
    BOOL ok = TRUE;
    if (!o->handle) return TRUE;
    while (o->maps) {
        if (!unmap(o)) { ok = FALSE; break; }
    }
    if (!o->released && !drop_owner(o)) ok = FALSE;
    if (ok) memset(o, 0, sizeof(*o));
    return ok;
}

static BOOL pattern(APTR cpu, ULONG bytes, ULONG seed, BOOL write)
{
    volatile ULONG *p = cpu;
    static BOOL width_sampled;
    ULONG i;
    if (write) {
        for (i = 0; i < bytes / sizeof(*p); ++i)
            p[i] = seed ? (ULONG)(seed ^ (i * 0x9e3779b1UL)) : 0;
        __sync_synchronize();
    }
    for (i = 0; i < bytes / sizeof(*p); ++i) {
        /* ULONG is32 bits, C unsigned long is64 on this native target.
         * v1 truncated the store but compared against the untruncated UL
         * expression. The oracle must use the same explicit word width. */
        unsigned long wide = seed ? seed ^ (i * 0x9e3779b1UL) : 0;
        ULONG expected = (ULONG)wide, actual = p[i];
        if (write && !width_sampled && wide != expected) {
            LOG("WIDTH-SAMPLE word=%lu ULONG=%lu C-long=%lu wide=%lx expected32=%08lx actual32=%08lx",
                (unsigned long)i, (unsigned long)sizeof(ULONG),
                (unsigned long)sizeof(unsigned long), wide,
                (unsigned long)expected, (unsigned long)actual);
            width_sampled = TRUE;
        }
        if (actual != expected) {
            LOG("DATA-MISMATCH cpu=%p word=%lu seed=%08lx expected32=%08lx actual32=%08lx",
                cpu, (unsigned long)i, (unsigned long)seed,
                (unsigned long)expected, (unsigned long)actual);
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL disjoint(APTR a, APTR b, ULONG bytes)
{
    IPTR x = (IPTR)a, y = (IPTR)b;
    return x <= y ? y - x >= bytes : x - y >= bytes;
}

int main(void)
{
    struct Library *resident;
    struct AmdgpuGpuInfo gpu = {0};
    struct AmdgpuBoCaps caps = {0};
    struct AmdgpuBoInfo info = {0};
    struct Owned a = {0}, b = {0}, c = {0}, d = {0}, slots[64] = {{0}};
    APTR tmp = NULL, old_cpu = NULL;
    ULONG h = 0, old_handle = 0, i;
    LONG result = RETURN_FAIL;
    BOOL clean = TRUE;

    Forbid();
    resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd");
    Permit();
    if (!resident) { LOG("REFUSED absent driver; no load attempted"); return result; }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0); /* checked noexpunge resident */
    if (!AmdgpuBase) return result;
    /* Last new vector is29; include the complete negative table (30 slots).
     * Refuse cold40's shorter table before making ANY storage ABI call. */
    NEED(AmdgpuBase == resident && AmdgpuBase->lib_NegSize >= 30 * sizeof(APTR),
         "resident identity and storage vector extent");
    NEED(amdgpu_get_device_count() == 1 && (device = amdgpu_get_device(0)) != NULL,
         "one installed DUT device");
    amdgpu_query_gpu_info(device, &gpu);
    NEED(gpu.asic_id == 0x7590 && gpu.pci_rev == 0xc0 && gpu.gc_major == 12,
         "RX9060XT GC12 identity");
    NEED(amdgpu_query_bo_caps(device, &caps, sizeof(caps)) == 0 &&
         caps.abi_version == 1 && caps.features == AMDGPU_BO_CAP_CPU_MAP &&
         caps.domains == AMDGPU_BO_DOMAIN_VRAM && caps.max_handles == 64 &&
         caps.page_size == 4096 && caps.max_alignment == 65536,
         "truthful storage-v1 caps (no GPU/scanout/export)");
    LOG("BEGIN v2-word32 budget=%llu max=%llu handles=%lu",
        (unsigned long long)caps.budget, (unsigned long long)caps.max_allocation,
        (unsigned long)caps.max_handles);
    h = 123;
    NEED(amdgpu_bo_alloc(NULL, 4096, 0, 1, 0, &h) == -ENODEV && !h,
         "missing device rejected and handle cleared");
    NEED(amdgpu_bo_alloc(device, 0, 0, 1, 0, &h) == -EINVAL && !h,
         "zero size rejected");
    NEED(amdgpu_bo_alloc(device, ~(UQUAD)0, 0, 1, 0, &h) == -EINVAL && !h,
         "oversize cannot truncate or overflow");
    NEED(amdgpu_bo_alloc(device, 4096, 3, 1, 0, &h) == -EINVAL && !h,
         "non-power-of-two alignment rejected");
    NEED(amdgpu_bo_alloc(device, 4096, 131072, 1, 0, &h) == -EINVAL && !h,
         "unsupported alignment rejected");
    NEED(amdgpu_bo_alloc(device, 4096, 0, 0, 0, &h) == -ENOSYS && !h &&
         amdgpu_bo_alloc(device, 4096, 0, 1, 1, &h) == -ENOSYS && !h,
         "unsupported domains/flags rejected");
    tmp = (APTR)1;
    NEED(amdgpu_bo_cpu_map(device, 0, &tmp) == -ENOENT && !tmp,
         "invalid handle cannot map");

    NEED(allocate(&a, 5001, 65536) && map(&a) && map(&a), "two borrowed maps");
    NEED(!((IPTR)a.cpu & 65535) && pattern(a.cpu, 8192, 0, FALSE),
         "64KiB alignment and entire rounded allocation zeroed");
    NEED(amdgpu_bo_query_info(device, a.handle, &info, sizeof(info)) == 0 &&
         info.size == 8192 && info.alignment == 65536 && info.map_count == 2,
         "query rounded size and map references");
    NEED(pattern(a.cpu, 8192, 0xa5010042UL, TRUE), "write/read complete A");
    old_handle = a.handle;
    old_cpu = a.cpu;
    NEED(drop_owner(&a), "owner release defers while mapped");
    tmp = (APTR)1;
    NEED(amdgpu_bo_cpu_map(device, old_handle, &tmp) == -ENOENT && !tmp &&
         amdgpu_bo_free(device, old_handle) == -ENOENT,
         "released handle denies new maps and duplicate free");
    NEED(allocate(&b, 5001, 65536) && map(&b), "allocate B while A retained");
    NEED(disjoint(a.cpu, b.cpu, 8192) && pattern(b.cpu, 8192, 0, FALSE) &&
         pattern(b.cpu, 8192, 0xb6020053UL, TRUE) &&
         pattern(a.cpu, 8192, 0xa5010042UL, FALSE), "no alias after owner release");
    NEED(unmap(&a), "first unmap retains the second borrow");
    NEED(allocate(&d, 5001, 65536) && map(&d) &&
         disjoint(a.cpu, d.cpu, 8192) && disjoint(b.cpu, d.cpu, 8192) &&
         pattern(d.cpu, 8192, 0, FALSE) && pattern(d.cpu, 8192, 0xd8040075UL, TRUE) &&
         pattern(a.cpu, 8192, 0xa5010042UL, FALSE), "last borrow still owns storage");
    NEED(unmap(&a) && amdgpu_bo_cpu_unmap(device, old_handle) == -ENOENT,
         "final unmap retires storage once");
    NEED(allocate(&c, 5001, 65536) && map(&c), "allocation after retirement");
    NEED(c.handle != old_handle && (c.handle & 255) == (old_handle & 255),
         "reused registry slot has a new generation");
    LOG("STORAGE reuse cpu=%p previous=%p (allocator choice, not API guarantee)", c.cpu, old_cpu);
    NEED(pattern(c.cpu, 8192, 0, FALSE) && pattern(c.cpu, 8192, 0xc7030064UL, TRUE) &&
         pattern(b.cpu, 8192, 0xb6020053UL, FALSE) && pattern(d.cpu, 8192, 0xd8040075UL, FALSE),
         "reused storage cleared and neighbours preserved");
    tmp = (APTR)1;
    NEED(amdgpu_bo_free(device, old_handle) == -ENOENT &&
         amdgpu_bo_cpu_map(device, old_handle, &tmp) == -ENOENT && !tmp &&
         pattern(c.cpu, 8192, 0xc7030064UL, FALSE), "stale generation cannot affect C");
    NEED(cleanup(&a) && cleanup(&b) && cleanup(&c) && cleanup(&d), "borrowed-map cleanup");

    /* Bounded registry pressure, only512KiB of backing including padding. */
    for (i = 0; i < 64; i++) {
        if (!allocate(&slots[i], 4096, 0) || !map(&slots[i]) ||
            !pattern(slots[i].cpu, 4096, 0, FALSE) ||
            !pattern(slots[i].cpu, 4096, 0x81000000UL + i, TRUE)) {
            LOG("FAIL registry allocation/map/pattern at%lu", (unsigned long)i);
            goto out;
        }
    }
    h = 123;
    NEED(amdgpu_bo_alloc(device, 4096, 0, 1, 0, &h) == -ENOMEM && !h,
         "full registry fails without a handle");
    for (i = 0; i < 64; i++) {
        if (!drop_owner(&slots[i]) || !pattern(slots[i].cpu, 4096, 0x81000000UL + i, FALSE)) {
            LOG("FAIL retained registry pattern at%lu", (unsigned long)i); goto out;
        }
    }
    NEED(amdgpu_bo_alloc(device, 4096, 0, 1, 0, &h) == -ENOMEM && !h,
         "released but mapped slots are not recycled");
    for (i = 64; i > 0; i--)
        if (!cleanup(&slots[i - 1])) { LOG("FAIL registry cleanup"); goto out; }
    NEED(allocate(&a, 4096, 0) && map(&a) && pattern(a.cpu, 4096, 0, FALSE),
         "registry and storage recover after all borrowers leave");
    result = RETURN_OK;
out:
    if (device) {
        if (!cleanup(&a)) clean = FALSE;
        if (!cleanup(&b)) clean = FALSE;
        if (!cleanup(&c)) clean = FALSE;
        if (!cleanup(&d)) clean = FALSE;
        for (i = 64; i > 0; i--) if (!cleanup(&slots[i - 1])) clean = FALSE;
    }
    if (!clean) { LOG("FAIL cleanup; retained references require investigation"); result = RETURN_FAIL; }
    CloseLibrary(AmdgpuBase);
    AmdgpuBase = NULL;
    LOG("END result=%ld; native BO/CPU lifetime only, no GPU/3D claim", (long)result);
    return result;
}
