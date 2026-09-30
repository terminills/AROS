/* Resident-only native VA/PTE and binding-lifetime acceptance. Does not load
 * a private driver, submit raw PM4, or pretend CPU readback is GPU execution. */
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
#define LOG(...) do { printf("[AmdgpuVaProbe] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuVaProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define NEED(c, s) do { if (!(c)) { LOG("FAIL %s", s); goto out; } \
                         LOG("PASS %s", s); } while (0)

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad = NULL;
    struct AmdgpuGpuInfo gpu = {0};
    struct AmdgpuVaCaps caps = {0};
    struct AmdgpuVaInfo info = {0};
    ULONG bo = 0, other = 0, vr = 0, va[64] = {0}, old = 0, h = 0, i;
    UQUAD addr = 0, first = 0, second = 0, requested = 0;
    APTR cpu = NULL, tmp = NULL;
    BOOL released = FALSE, clean = TRUE;
    LONG result = RETURN_FAIL;

    Forbid();
    resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd");
    Permit();
    if (!resident) { LOG("REFUSED absent driver; no load attempted"); return result; }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    NEED(AmdgpuBase == resident && AmdgpuBase->lib_NegSize >= 36 * sizeof(APTR),
         "resident identity and VA vector extent");
    NEED(amdgpu_get_device_count() == 1 && (ad = amdgpu_get_device(0)), "one installed device");
    amdgpu_query_gpu_info(ad, &gpu);
    NEED(gpu.asic_id == 0x7590 && gpu.pci_rev == 0xc0 && gpu.gc_major == 12,
         "RX9060XT GC12 identity");
    NEED(amdgpu_query_va_caps(ad, &caps, sizeof(caps)) == 0 &&
         caps.abi_version == 1 && (caps.domains & AMDGPU_BO_DOMAIN_GTT) &&
         !(caps.domains & ~(AMDGPU_BO_DOMAIN_GTT | AMDGPU_BO_DOMAIN_VRAM)) &&
         caps.page_size == 4096 && caps.max_alignment >= 65536 && caps.max_handles == 64,
         "bounded GTT VA capability; no submission claim");
    LOG("BEGIN v2; native PTE readback and binding lifetimes, NOT GPU execution");
    h = 123; addr = 123;
    NEED(amdgpu_va_range_alloc(ad, ~(UQUAD)0, 0, 0, &h, &addr) == -EINVAL && !h && !addr,
         "oversized VA request rejected and outputs cleared");
    NEED(amdgpu_va_range_alloc(ad, 4096, 3, 0, &h, &addr) == -EINVAL && !h && !addr,
         "invalid alignment rejected");
    NEED(amdgpu_va_range_alloc(ad, 4096, 4096, caps.address_limit, &h, &addr) == -EINVAL &&
         !h && !addr, "outside-aperture request rejected");
    NEED(amdgpu_va_range_alloc(ad, 4096, 4096, 1, &h, &addr) == -EINVAL && !h && !addr,
         "unaligned exact request rejected");
    NEED(amdgpu_va_range_alloc(ad, 5001, 65536, 0, &va[0], &first) == 0 &&
         first && !(first & 65535), "fresh64KiB aligned range");
    NEED(amdgpu_va_query_info(ad, va[0], &info, sizeof(info)) == 0 &&
         info.address == first && info.size == 8192 && info.alignment == 65536 &&
         info.state == AMDGPU_VA_STATE_RESERVED && !info.bo_handle && !info.gpu_refs,
         "exact rounded reservation metadata");
    NEED(amdgpu_va_range_alloc(ad, 8192, 65536, first, &h, &addr) == -ENOMEM && !h && !addr,
         "overlapping exact reservation rejected");
    requested = first + 65536;
    NEED(amdgpu_va_range_alloc(ad, 8192, 65536, requested, &va[1], &second) == 0 &&
         second == requested, "exact fresh address honored");
    NEED(amdgpu_bo_alloc(ad, 8192, 65536, AMDGPU_BO_DOMAIN_GTT, 0, &bo) == 0 &&
         amdgpu_bo_cpu_map(ad, bo, &cpu) == 0, "owned GTT and CPU borrow");
    for (i = 0; i < 2048; i++) ((volatile ULONG *)cpu)[i] = 0x5a700000UL ^ i;
    __sync_synchronize();
    NEED(amdgpu_bo_alloc(ad, 8192, 4096, AMDGPU_BO_DOMAIN_VRAM, 0, &vr) == 0,
         "VRAM temporary allocated");
    if (caps.domains & AMDGPU_BO_DOMAIN_VRAM) {
        NEED(amdgpu_va_range_alloc(ad, 8192, 4096, 0, &va[2], &addr) == 0 &&
             amdgpu_bo_va_bind(ad, vr, va[2]) == 0 &&
             amdgpu_va_query_info(ad, va[2], &info, sizeof(info)) == 0 &&
             info.state == AMDGPU_VA_STATE_BOUND && info.bo_handle == vr,
             "advertised VRAM binding has exact PTE ownership; no GPU execution claim");
        NEED(amdgpu_bo_va_unbind(ad, va[2]) == 0 && amdgpu_va_range_free(ad, va[2]) == 0,
             "VRAM temporary binding retired"); va[2] = 0;
    } else {
        NEED(amdgpu_bo_va_bind(ad, vr, va[0]) == -ENOSYS,
             "unadvertised VRAM VA fails closed");
    }
    NEED(amdgpu_bo_free(ad, vr) == 0, "VRAM temporary released"); vr = 0;
    NEED(amdgpu_bo_va_bind(ad, bo, va[0]) == 0 && amdgpu_bo_va_bind(ad, bo, va[1]) == 0,
         "two fresh bindings of same GTT; exact PTE readback plus both-hub ACK");
    NEED(amdgpu_bo_va_bind(ad, bo, va[0]) == -EBUSY &&
         amdgpu_va_range_free(ad, va[0]) == -EBUSY, "bound range cannot rebind or free");
    NEED(amdgpu_bo_free(ad, bo) == 0, "owner free retains two bindings and CPU map");
    released = TRUE;
    tmp = (APTR)1;
    NEED(amdgpu_bo_cpu_map(ad, bo, &tmp) == -ENOENT && !tmp,
         "released BO cannot acquire new public owner refs");
    for (i = 0; i < 2048; i++)
        if (((volatile ULONG *)cpu)[i] != (0x5a700000UL ^ i)) {
            LOG("FAIL retained binding payload word=%lu", (unsigned long)i);
            goto out;
        }
    LOG("PASS retained binding payload2048words");
    NEED(amdgpu_bo_cpu_unmap(ad, bo) == 0, "last CPU borrow dropped; only bindings retain BO");
    cpu = NULL;
    NEED(amdgpu_bo_alloc(ad, 8192, 4096, AMDGPU_BO_DOMAIN_GTT, 0, &other) == 0 &&
         (other & 255) != (bo & 255), "bound owner-free BO slot not recycled");
    NEED(amdgpu_bo_free(ad, other) == 0, "independent temporary released"); other = 0;
    NEED(amdgpu_bo_va_unbind(ad, va[0]) == 0 &&
         amdgpu_va_query_info(ad, va[0], &info, sizeof(info)) == 0 &&
         info.state == AMDGPU_VA_STATE_RETIRED && !info.bo_handle,
         "first exact unbind retires PTEs but preserves other binding");
    NEED(amdgpu_bo_alloc(ad, 8192, 4096, AMDGPU_BO_DOMAIN_GTT, 0, &other) == 0 &&
         (other & 255) != (bo & 255), "remaining binding still retains BO slot");
    NEED(amdgpu_bo_va_bind(ad, other, va[0]) == -EBUSY, "retired VA cannot be rebound");
    NEED(amdgpu_bo_free(ad, other) == 0, "second temporary released"); other = 0;
    old = bo;
    NEED(amdgpu_bo_va_unbind(ad, va[1]) == 0, "last binding retires owner-free backing");
    bo = 0; released = FALSE;
    NEED(amdgpu_bo_alloc(ad, 8192, 4096, AMDGPU_BO_DOMAIN_GTT, 0, &other) == 0 &&
         other != old && (other & 255) == (old & 255), "BO slot reusable only after final binding");
    NEED(amdgpu_bo_cpu_unmap(ad, old) == -ENOENT && amdgpu_bo_free(ad, old) == -ENOENT,
         "retired BO generation cannot touch replacement");
    NEED(amdgpu_bo_free(ad, other) == 0, "replacement released"); other = 0;
    old = va[0];
    NEED(amdgpu_va_range_free(ad, va[0]) == 0 && amdgpu_va_range_free(ad, va[1]) == 0,
         "unbound range records released"); va[0] = va[1] = 0;
    NEED(amdgpu_va_range_free(ad, old) == -ENOENT, "stale VA handle rejected");
    NEED(amdgpu_va_range_alloc(ad, 8192, 65536, first, &h, &addr) == -ENOMEM && !h && !addr,
         "released GPU address remains quarantined this boot");
    for (i = 0; i < 64; i++)
        NEED(amdgpu_va_range_alloc(ad, 4096, 0, 0, &va[i], &addr) == 0,
             "VA registry pressure allocation");
    NEED(amdgpu_va_range_alloc(ad, 4096, 0, 0, &h, &addr) == -ENOMEM && !h && !addr,
         "65th VA handle refused without corruption");
    result = RETURN_OK;
out:
    if (ad) {
        if (cpu && amdgpu_bo_cpu_unmap(ad, bo)) clean = FALSE;
        for (i = 0; i < 64; i++) if (va[i]) {
            if (!amdgpu_va_query_info(ad, va[i], &info, sizeof(info)) && info.bo_handle)
                if (amdgpu_bo_va_unbind(ad, va[i])) clean = FALSE;
            if (amdgpu_va_range_free(ad, va[i])) clean = FALSE;
        }
        if (bo && !released && amdgpu_bo_free(ad, bo)) clean = FALSE;
        if (other && amdgpu_bo_free(ad, other)) clean = FALSE;
        if (vr && amdgpu_bo_free(ad, vr)) clean = FALSE;
    }
    if (!clean) result = RETURN_FAIL;
    LOG("END result=%ld cleanup=%s", (long)result, clean ? "PASS" : "FAIL-retain-quarantine");
    CloseLibrary(AmdgpuBase);
    return result;
}
