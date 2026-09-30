/* Native client of the installed/rebooted GTT storage extension.
 * No private driver objects, autoinit, register writes or GPU submission. */
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
#define LOG(...) do { printf("[AmdgpuGttProbe] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuGttProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define NEED(c, s) do { if (!(c)) { LOG("FAIL %s", s); goto out; } \
                         LOG("PASS %s", s); } while (0)

struct Owned { ULONG handle, maps; APTR cpu; BOOL released; };
static struct AmdgpuDevice *device;

static BOOL allocate(struct Owned *o, ULONG size, ULONG align, ULONG domain)
{
    return amdgpu_bo_alloc(device, size, align, domain, 0, &o->handle) == 0;
}

static BOOL map(struct Owned *o)
{
    APTR cpu = NULL;
    if (amdgpu_bo_cpu_map(device, o->handle, &cpu)) return FALSE;
    o->maps++;
    if (!cpu || (o->cpu && cpu != o->cpu)) return FALSE;
    o->cpu = cpu;
    return TRUE;
}

static BOOL drop(struct Owned *o)
{
    if (amdgpu_bo_free(device, o->handle)) return FALSE;
    o->released = TRUE;
    return TRUE;
}

static BOOL unmap(struct Owned *o)
{
    if (!o->maps || amdgpu_bo_cpu_unmap(device, o->handle)) return FALSE;
    if (!--o->maps) o->cpu = NULL;
    return TRUE;
}

static BOOL cleanup(struct Owned *o)
{
    if (!o->handle) return TRUE;
    while (o->maps) if (!unmap(o)) return FALSE;
    if (!o->released && !drop(o)) return FALSE;
    memset(o, 0, sizeof(*o));
    return TRUE;
}

static BOOL pattern(APTR cpu, ULONG bytes, ULONG seed, BOOL write)
{
    volatile ULONG *p = cpu;
    ULONG i;
    for (i = 0; write && i < bytes / sizeof(*p); i++)
        p[i] = seed ? (ULONG)(seed ^ (i * 0x9e3779b1UL)) : 0;
    __sync_synchronize();
    for (i = 0; i < bytes / sizeof(*p); i++) {
        ULONG want = seed ? (ULONG)(seed ^ (i * 0x9e3779b1UL)) : 0;
        if (p[i] != want) {
            LOG("MISMATCH cpu=%p word=%lu got=%08lx want=%08lx", cpu,
                (unsigned long)i, (unsigned long)p[i], (unsigned long)want);
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL disjoint(APTR a, APTR b, ULONG bytes)
{
    IPTR x = (IPTR)a, y = (IPTR)b;
    return x < y ? y - x >= bytes : x - y >= bytes;
}

int main(void)
{
    struct Library *resident;
    struct AmdgpuGpuInfo gpu = {0};
    struct AmdgpuBoCaps caps = {0};
    struct AmdgpuBoInfo info = {0};
    struct Owned a = {0}, b = {0}, v = {0}, reused = {0}, slots[64] = {{0}};
    ULONG h = 0, old = 0, i;
    APTR tmp = NULL;
    LONG result = RETURN_FAIL;
    BOOL clean = TRUE;

    Forbid();
    resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd");
    Permit();
    if (!resident) { LOG("REFUSED absent driver; no load attempted"); return result; }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    NEED(AmdgpuBase == resident && AmdgpuBase->lib_NegSize >= 30 * sizeof(APTR),
         "resident identity and unchanged storage vector extent");
    NEED(amdgpu_get_device_count() == 1 && (device = amdgpu_get_device(0)),
         "one installed DUT device");
    amdgpu_query_gpu_info(device, &gpu);
    NEED(gpu.asic_id == 0x7590 && gpu.pci_rev == 0xc0 && gpu.gc_major == 12,
         "RX9060XT GC12 identity");
    NEED(amdgpu_query_bo_caps(device, &caps, sizeof(caps)) == 0 &&
         caps.abi_version == 1 && caps.domains == (AMDGPU_BO_DOMAIN_VRAM | AMDGPU_BO_DOMAIN_GTT) &&
         caps.features == AMDGPU_BO_CAP_CPU_MAP && caps.max_handles == 64 &&
         caps.page_size == 4096 && caps.max_alignment >= 65536,
         "GTT+VRAM caps, CPU storage only");
    LOG("BEGIN v1 budget=%llu max=%llu", (unsigned long long)caps.budget,
        (unsigned long long)caps.max_allocation);
    h = 123;
    NEED(amdgpu_bo_alloc(device, 4096, 0, 3, 0, &h) == -ENOSYS && !h,
         "domain mask rejected and output cleared");
    NEED(amdgpu_bo_alloc(device, ~(UQUAD)0, 0, AMDGPU_BO_DOMAIN_GTT, 0, &h) == -EINVAL && !h,
         "GTT oversized request cannot truncate");
    NEED(amdgpu_bo_alloc(device, 4096, 3, AMDGPU_BO_DOMAIN_GTT, 0, &h) == -EINVAL && !h,
         "GTT invalid alignment rejected");
    NEED(allocate(&a, 5001, 65536, AMDGPU_BO_DOMAIN_GTT) && map(&a) && map(&a),
         "GTT allocation with two borrows");
    NEED(!((IPTR)a.cpu & 65535) && pattern(a.cpu, 8192, 0, FALSE),
         "GTT64KiB alignment and entire rounded allocation zeroed");
    NEED(amdgpu_bo_query_info(device, a.handle, &info, sizeof(info)) == 0 &&
         info.domain == AMDGPU_BO_DOMAIN_GTT && info.size == 8192 &&
         info.alignment == 65536 && info.map_count == 2, "GTT metadata exact");
    NEED(pattern(a.cpu, 8192, 0xa5000042, TRUE), "GTT complete pattern A");
    NEED(allocate(&v, 8192, 4096, AMDGPU_BO_DOMAIN_VRAM) && map(&v) &&
         pattern(v.cpu, 8192, 0, FALSE) && pattern(v.cpu, 8192, 0xb6000053, TRUE) &&
         disjoint(a.cpu, v.cpu, 8192) && pattern(a.cpu, 8192, 0xa5000042, FALSE),
         "VRAM remains independent while GTT live");
    old = a.handle;
    NEED(drop(&a), "GTT owner release retains borrows");
    tmp = (APTR)1;
    NEED(amdgpu_bo_cpu_map(device, old, &tmp) == -ENOENT && !tmp &&
         amdgpu_bo_free(device, old) == -ENOENT, "released handle rejects new refs");
    NEED(unmap(&a) && allocate(&b, 5001, 65536, AMDGPU_BO_DOMAIN_GTT) && map(&b) &&
         disjoint(a.cpu, b.cpu, 8192) && pattern(a.cpu, 8192, 0xa5000042, FALSE) &&
         pattern(b.cpu, 8192, 0, FALSE), "last GTT borrow prevents alias/reclamation");
    NEED(unmap(&a) && amdgpu_bo_cpu_unmap(device, old) == -ENOENT,
         "final GTT borrow retires once");
    NEED(allocate(&reused, 5001, 65536, AMDGPU_BO_DOMAIN_GTT) && map(&reused) &&
         reused.handle != old && (reused.handle & 255) == (old & 255) &&
         pattern(reused.cpu, 8192, 0, FALSE), "retired slot gets new generation and zeroed GTT");
    NEED(cleanup(&a) && cleanup(&b) && cleanup(&v) && cleanup(&reused),
         "both domain allocations released");
    NEED(allocate(&a, 8UL << 20, 4096, AMDGPU_BO_DOMAIN_GTT) && map(&a) && drop(&a),
         "large GTT borrow retains budget after owner free");
    h = 123;
    NEED(amdgpu_bo_alloc(device, 8UL << 20, 4096, AMDGPU_BO_DOMAIN_VRAM, 0, &h) == -ENOMEM && !h,
         "shared budget prevents VRAM allocation while GTT retained");
    NEED(unmap(&a) && cleanup(&a) &&
         allocate(&v, 8UL << 20, 4096, AMDGPU_BO_DOMAIN_VRAM) && cleanup(&v),
         "final GTT borrow returns budget to VRAM");
    for (i = 0; i < 64; i++)
        NEED(allocate(&slots[i], 4096, 4096,
             i & 1 ? AMDGPU_BO_DOMAIN_VRAM : AMDGPU_BO_DOMAIN_GTT), "mixed registry allocation");
    h = 123;
    NEED(amdgpu_bo_alloc(device, 4096, 4096, AMDGPU_BO_DOMAIN_GTT, 0, &h) == -ENOMEM && !h,
         "mixed64slot pressure rejected without alias");
    for (i = 0; i < 64; i++) NEED(cleanup(&slots[i]), "mixed registry cleanup");
    NEED(allocate(&reused, 4096, 4096, AMDGPU_BO_DOMAIN_GTT) && map(&reused) &&
         pattern(reused.cpu, 4096, 0, FALSE), "GTT recovers after registry pressure");
    result = RETURN_OK;
out:
    if (!cleanup(&a)) clean = FALSE;
    if (!cleanup(&b)) clean = FALSE;
    if (!cleanup(&v)) clean = FALSE;
    if (!cleanup(&reused)) clean = FALSE;
    for (i = 0; i < 64; i++) if (!cleanup(&slots[i])) clean = FALSE;
    if (!clean) result = RETURN_FAIL;
    LOG("END result=%ld cleanup=%s", (long)result, clean ? "PASS" : "FAIL");
    CloseLibrary(AmdgpuBase);
    return result;
}
