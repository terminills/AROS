/* Native read-only client of the installed AMDGPU public Device/Query ABI.
 * Refuses to load an absent driver. The current noexpunge module must already
 * be resident after a physical boot; opening that resident only takes a library
 * reference. No private driver objects, register reads/writes, BO allocation,
 * GPU submission or hardware initialization are linked into this program.
 * This is ABI/enumeration evidence, not Mesa rendering acceptance.
 */
#define DEBUG 1
#define __INLINE_AMDGPU_STACKCALL__ 1
#include <aros/debug.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/amdgpu.h>
#include <stdio.h>

/* Explicit base and generated inline calls: do NOT link -lamdgpu/autoinit,
 * which could open the driver before main's already-resident guard. */
struct Library *AmdgpuBase;
#define LOG(...) do { printf("[AmdgpuApiQuery] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuApiQuery] " __VA_ARGS__); bug("\n"); } while (0)

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad;
    struct AmdgpuGpuInfo info = { 0 };
    ULONG count, heap;
    UBYTE major = 0, minor = 0, revision = 0;
    UWORD domain = 0xffff;
    UBYTE bus = 0xff, device = 0xff, function = 0xff;
    LONG result = RETURN_FAIL;

    Forbid();
    resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd");
    Permit();
    if (!resident) {
        LOG("REFUSED: driver is not already resident; no OpenLibrary attempted");
        return result;
    }
    /* noexpunge is the checked module contract in amdgpu.conf. No intervening
     * expunge can turn this already-resident open into an on-disk load. */
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    if (AmdgpuBase != resident || AmdgpuBase->lib_NegSize < 12 * sizeof(APTR)) {
        LOG("FAIL: library identity or Device/Query vector extent");
        goto out;
    }
    count = amdgpu_get_device_count();
    LOG("BEGIN v1 resident-only version=%u.%u vectors_bytes=%u count=%lu",
        AmdgpuBase->lib_Version, AmdgpuBase->lib_Revision,
        AmdgpuBase->lib_NegSize, (unsigned long)count);
    if (count != 1) {
        LOG("REFUSED: expected exactly one device on the JOB-0084 DUT");
        goto out;
    }
    ad = amdgpu_get_device(0);
    if (!ad || amdgpu_get_device(count) != NULL) {
        LOG("FAIL: enumeration bounds");
        goto out;
    }
    amdgpu_query_gpu_info(ad, &info);
    if (info.asic_id != 0x7590 || info.pci_rev != 0xc0 || info.gc_major != 12) {
        LOG("REFUSED: unexpected device=%04x revision=%02x GC=%u.%u.%u",
            info.asic_id, info.pci_rev, info.gc_major, info.gc_minor, info.gc_rev);
        goto out;
    }
    heap = amdgpu_query_heap_mb(ad, TRUE);
    if (!amdgpu_query_hw_ip_version(ad, AMDGPU_IP_GC, &major, &minor, &revision) ||
        !amdgpu_query_pci_bus_info(ad, &domain, &bus, &device, &function)) {
        LOG("FAIL: GC or PCI query");
        goto out;
    }
    LOG("DEVICE %s id=%04x revision=%02x pci=%04x:%02x:%02x.%u GC=%u.%u.%u",
        info.marketing_name ? info.marketing_name : "(unnamed)",
        info.asic_id, info.pci_rev, domain, bus, device, function,
        major, minor, revision);
    LOG("MEMORY vram_mb=%lu heap_mb=%lu vram_bytes=%llu aperture_bytes=%llu",
        (unsigned long)info.vram_mb, (unsigned long)heap,
        (unsigned long long)info.vram_size, (unsigned long long)info.vram_bar_size);
    if (!heap || heap != info.vram_mb ||
        info.vram_size != ((UQUAD)heap << 20) ||
        major != info.gc_major || minor != info.gc_minor || revision != info.gc_rev ||
        amdgpu_get_device_count() != count || amdgpu_get_device(0) != ad) {
        LOG("FAIL: inconsistent cached queries or device identity");
        goto out;
    }
    result = RETURN_OK;
out:
    CloseLibrary(AmdgpuBase);
    AmdgpuBase = NULL;
    LOG("END result=%ld; query ABI only, no submission/rendering claim", (long)result);
    return result;
}
