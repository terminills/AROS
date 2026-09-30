/* Driver-owned BO storage, first native single-address-domain tier.
 *
 * ABI-RFC.md: backing storage belongs to this driver; CPU maps are borrowed.
 * VRAM BOs use validated hardware ranges excluding firmware and the whole
 * independent display arena. Only each owned BO is CPU-mapped, not all VRAM.
 * The separate VA service retains binding and driver-owned GPU references.
 * Managed GPU submission is a separate optional CS service. Library 0.12 adds
 * one bounded synchronous BO-to-owned-display copy, but still no scanout
 * ownership, import/export or task-death reclamation. Refs retain storage even after free;
 * an abandoned client can leak its bounded quota, never reclaim live storage.
 * Submit/scanout services MUST add their own references before enabling
 * GPU use. A positive allocation result is not rendering readiness.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/kernel.h>
#include <aros/kernel.h>
#include <exec/memory.h>
#include <hidd/pci.h>
#include <hidd/amdgpu.h>
#include <errno.h>
#include <string.h>

#include "amdgpu_bo.h"
#include "amdgpu_intern.h"
#include "amdgpu_gart.h"
#include "amdgpu_gfx_support.h"
#include "amdgpu_vram.h"

#define BO_SLOTS 64
#define BO_INDEX_MASK 0xffUL
#define BO_GENERATION_MAX 0xffffffUL
#define BO_PAGE 4096UL
#define BO_MAX_ALIGNMENT (2UL << 20)
/* Individual AROS allocation/map lengths remain ULONG; cumulative capacity
 * and usage must not truncate on a card larger than4GiB. */
#define BO_LENGTH_LIMIT ((0xffffffffULL - BO_MAX_ALIGNMENT - BO_PAGE) & ~(UQUAD)(BO_PAGE - 1))

struct BoSlot {
    APTR cpu;
    APTR ram_allocation;
    UQUAD backing_mc, payload_mc, gart_va;
    ULONG backing_size, size, alignment, generation, maps, bindings, copies, domain;
    BOOL released;
};

struct VaSlot {
    struct AmdgpuGartRange *range;
    UQUAD address;
    ULONG size, alignment, generation, state, bo_handle, gpu_refs;
};

static struct SignalSemaphore bo_lock;
static struct HWData *bo_hw;
static struct BoSlot bo_slots[BO_SLOTS];
static struct VaSlot va_slots[BO_SLOTS];
static UQUAD bo_used, bo_vram_used, bo_gtt_used, bo_gtt_budget, bo_max_size;
static const struct AmdgpuVramMap *bo_map;
static BOOL bo_ready;

void amdgpu_bo_service_init(struct HWData *hw)
{
    if (bo_ready) return;
    InitSemaphore(&bo_lock);
    bo_hw = hw;
    bo_map = amdgpu_vram_seal(hw);
    if (bo_map) {
        UQUAD remaining;
        bo_max_size = bo_map->usable < BO_LENGTH_LIMIT ? bo_map->usable : BO_LENGTH_LIMIT;
        bo_max_size &= ~(UQUAD)(BO_PAGE - 1);
        amdgpu_gart_capacity(hw->ad, &bo_gtt_budget, &remaining);
    }
    /* Published once before module initialization returns. No lazy-init race. */
    bo_ready = TRUE;
    bug("[amdgpu-bo] map-backed-v1 slots=%u VRAM=%llu GTT=%llu max=%llu; "
        "independent display arena, retained CS refs, no VA reuse\n", BO_SLOTS,
        (unsigned long long)(bo_map ? bo_map->usable : 0),
        (unsigned long long)bo_gtt_budget, (unsigned long long)bo_max_size);
}

static BOOL device_valid(struct AmdgpuDevice *ad)
{
    /* Compare identity without dereferencing an unrecognized device pointer. */
    return bo_ready && bo_hw && bo_map && ad && ad == bo_hw->ad &&
        bo_map->valid && bo_map->sealed;
}

/* Caller owns bo_lock. It may enter pool_lock or gart_map_lock, never the
 * inverse. Bitmap/plane/GART code does not enter this service or bo_lock. */
static struct BoSlot *lookup(ULONG handle)
{
    ULONG index = handle & BO_INDEX_MASK;
    struct BoSlot *bo;
    if (!index || index > BO_SLOTS) return NULL;
    bo = &bo_slots[index - 1];
    return bo->cpu && bo->generation == (handle >> 8) ? bo : NULL;
}

static void destroy(struct BoSlot *bo)
{
    /* All CPU maps, VA bindings and synchronous presentation copies must have
     * retired. A binding itself cannot retire while any managed GPU reference
     * remains. */
    __sync_synchronize();
    if (bo->domain == AMDGPU_BO_DOMAIN_GTT) {
        /* The private GART VA is consumed only by driver-owned operations.
         * Managed submits retain bindings; present copies retain copies. */
        amdgpu_gart_unmap(bo_hw->ad, bo->gart_va, bo->size);
        FreeMem(bo->ram_allocation, bo->backing_size);
        bo_gtt_used -= bo->backing_size;
    } else {
        HIDD_PCIDriver_UnmapPCI(bo_hw->ad->pciDriver, bo->cpu, bo->backing_size);
        bo_vram_used -= bo->backing_size;
    }
    bo_used -= bo->backing_size;
    bo->cpu = NULL;
    bo->ram_allocation = NULL;
    bo->backing_mc = bo->payload_mc = bo->gart_va = 0;
    bo->backing_size = bo->size = bo->alignment = bo->maps = bo->bindings = bo->copies = 0;
    bo->domain = 0;
    bo->released = FALSE;
    /* Retain generation. At its maximum the slot is retired, never wrapped. */
}

LONG amdgpu_query_bo_caps(struct AmdgpuDevice *ad, struct AmdgpuBoCaps *out,
                         ULONG out_size)
{
    if (!out || out_size < sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!device_valid(ad)) return -ENODEV;
    out->abi_version = AMDGPU_BO_ABI_VERSION;
    out->domains = AMDGPU_BO_DOMAIN_VRAM;
    if (ad->gart_ready && bo_gtt_budget) out->domains |= AMDGPU_BO_DOMAIN_GTT;
    out->features = AMDGPU_BO_CAP_CPU_MAP;
    out->page_size = BO_PAGE;
    out->max_handles = BO_SLOTS;
    out->max_allocation = bo_max_size;
    out->budget = bo_map->usable + bo_gtt_budget;
    out->max_alignment = BO_MAX_ALIGNMENT;
    return 0;
}

LONG amdgpu_query_memory_info(struct AmdgpuDevice *ad, struct AmdgpuMemoryInfo *out,
                             ULONG out_size)
{
    struct AmdgpuMemoryInfo info = {0};
    ULONG i;
    LONG result = 0;
    if (!out || out_size < sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    info.abi_version = AMDGPU_MEMORY_INFO_ABI_VERSION;
    info.domains = AMDGPU_BO_DOMAIN_VRAM;
    if (ad->gart_ready && bo_gtt_budget) info.domains |= AMDGPU_BO_DOMAIN_GTT;
    info.page_size = BO_PAGE;
    info.max_handles = BO_SLOTS;
    info.shared_budget = bo_map->usable + bo_gtt_budget;
    info.max_allocation = bo_max_size;
    for (i = 0; i < BO_SLOTS; i++) {
        const struct BoSlot *bo = &bo_slots[i];
        if (!bo->cpu) {
            if (bo->generation < BO_GENERATION_MAX) info.reusable_handles++;
            continue;
        }
        info.live_handles++;
        if (bo->released) info.released_handles++;
        info.cpu_borrows += bo->maps;
        info.va_bindings += bo->bindings;
        if (bo->domain == AMDGPU_BO_DOMAIN_VRAM) {
            info.vram_backing += bo->backing_size;
            info.vram_payload += bo->size;
        } else if (bo->domain == AMDGPU_BO_DOMAIN_GTT) {
            info.gtt_backing += bo->backing_size;
            info.gtt_payload += bo->size;
        } else {
            result = -EIO;
        }
    }
    info.shared_used = info.vram_backing + info.gtt_backing;
    /* Never turn an accounting inconsistency into optimistic free capacity. */
    if (info.shared_used != bo_used || info.shared_used > info.shared_budget ||
        info.vram_backing != bo_vram_used || info.gtt_backing != bo_gtt_used ||
        bo_vram_used > bo_map->usable || bo_gtt_used > bo_gtt_budget)
        result = -EIO;
    if (!result) {
        info.shared_available = info.shared_budget - info.shared_used;
        *out = info;
    }
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_query_memory_info2(struct AmdgpuDevice *ad, struct AmdgpuMemoryInfo2 *out,
                              ULONG out_size)
{
    struct AmdgpuMemoryInfo2 info = {0};
    UQUAD vram = 0, gtt = 0, total, remaining;
    ULONG i;
    LONG result = -EIO;
    if (!out || out_size < sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    info.abi_version = AMDGPU_MEMORY_INFO2_ABI_VERSION;
    info.domains = AMDGPU_BO_DOMAIN_VRAM;
    info.page_size = BO_PAGE; info.max_handles = BO_SLOTS;
    info.total_vram = bo_map->total; info.visible_vram = bo_map->visible;
    info.reserved_vram = bo_map->visible - bo_map->usable;
    info.vram_budget = bo_map->usable; info.gtt_budget = bo_gtt_budget;
    info.max_allocation = bo_max_size;
    for (i = 0; i < BO_SLOTS; i++) {
        const struct BoSlot *bo = &bo_slots[i];
        if (!bo->cpu) {
            if (bo->generation < BO_GENERATION_MAX) info.reusable_handles++;
            continue;
        }
        info.live_handles++;
        if (bo->released) info.released_handles++;
        info.cpu_borrows += bo->maps; info.va_bindings += bo->bindings;
        if (bo->domain == AMDGPU_BO_DOMAIN_VRAM) vram += bo->backing_size;
        else if (bo->domain == AMDGPU_BO_DOMAIN_GTT) gtt += bo->backing_size;
        else goto out;
    }
    if (vram != bo_vram_used || gtt != bo_gtt_used || vram + gtt != bo_used ||
        vram > info.vram_budget || gtt > info.gtt_budget) goto out;
    info.vram_used = vram; info.vram_available = info.vram_budget - vram;
    info.gtt_used = gtt;
    if (bo_gtt_budget && amdgpu_gart_capacity(ad, &total, &remaining) && total == bo_gtt_budget) {
        UQUAD free_ram = AvailMem(MEMF_ANY);
        info.domains |= AMDGPU_BO_DOMAIN_GTT;
        info.gart_remaining = remaining;
        info.gtt_available = bo_gtt_budget - gtt;
        if (info.gtt_available > remaining) info.gtt_available = remaining;
        if (info.gtt_available > free_ram) info.gtt_available = free_ram;
    } else if (bo_gtt_budget) goto out;
    *out = info;
    result = 0;
out:
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_bo_alloc(struct AmdgpuDevice *ad, UQUAD size, UQUAD alignment,
                    ULONG domain, ULONG flags, ULONG *handle)
{
    struct BoSlot *bo = NULL;
    APTR cpu = NULL, ram_allocation = NULL;
    UQUAD mc = 0, gart_va = 0;
    ULONG rounded, align, backing_size, padding, i;
    LONG result = -ENOMEM;
    if (!handle) return -EINVAL;
    *handle = 0;
    if (!device_valid(ad)) return -ENODEV;
    if ((domain != AMDGPU_BO_DOMAIN_VRAM && domain != AMDGPU_BO_DOMAIN_GTT) || flags)
        return -ENOSYS;
    if (domain == AMDGPU_BO_DOMAIN_GTT && !ad->gart_ready) return -ENODEV;
    if (!size || size > bo_max_size || alignment > BO_MAX_ALIGNMENT ||
        (alignment && (alignment & (alignment - 1)))) return -EINVAL;
    /* Bounds above make rounding safe at both UQUAD and ULONG width. */
    rounded = ((ULONG)size + BO_PAGE - 1) & ~(BO_PAGE - 1);
    align = alignment < BO_PAGE ? BO_PAGE : (ULONG)alignment;
    /* VRAM placement aligns the actual range, retaining reusable holes rather
     * than charging a second copy of the requested alignment. System RAM
     * still owns its AllocMem alignment padding. */
    backing_size = rounded + (domain == AMDGPU_BO_DOMAIN_GTT ? align - 1 : 0);
    ObtainSemaphore(&bo_lock);
    if (domain == AMDGPU_BO_DOMAIN_GTT) {
        if (bo_gtt_used > bo_gtt_budget || backing_size > bo_gtt_budget - bo_gtt_used) goto out;
    } else if (bo_vram_used > bo_map->usable || backing_size > bo_map->usable - bo_vram_used) goto out;
    for (i = 0; i < BO_SLOTS; i++) {
        if (!bo_slots[i].cpu && bo_slots[i].generation < BO_GENERATION_MAX) {
            bo = &bo_slots[i];
            break;
        }
    }
    if (!bo) goto out;
    if (domain == AMDGPU_BO_DOMAIN_GTT) {
        ram_allocation = AllocMem(backing_size, MEMF_ANY);
        if (!ram_allocation) goto out;
        padding = (align - ((IPTR)ram_allocation & (align - 1))) & (align - 1);
        if ((IPTR)ram_allocation > ~(IPTR)0 - (backing_size - 1) ||
            padding > backing_size - rounded) {
            FreeMem(ram_allocation, backing_size);
            result = -EINVAL;
            goto out;
        }
        cpu = (APTR)((IPTR)ram_allocation + padding);
        memset(cpu, 0, rounded);
        __sync_synchronize();
        gart_va = amdgpu_gart_map(ad, cpu, rounded);
        if (!gart_va) {
            FreeMem(ram_allocation, backing_size);
            goto out;
        }
        /* The internal VA is page-aligned only, not a public VA reservation.
         * CPU/backing alignment does not promise a future requested GPU VA. */
    } else {
        struct AmdgpuVramRange busy[BO_SLOTS];
        struct Library *KernelBase;
        UQUAD offset, physical;
        ULONG count = 0, live_index;
        /* Preserve i: it identifies the selected slot in the public handle. */
        for (live_index = 0; live_index < BO_SLOTS; live_index++) {
            const struct BoSlot *live = &bo_slots[live_index];
            if (!live->cpu || live->domain != AMDGPU_BO_DOMAIN_VRAM) continue;
            busy[count].start = live->backing_mc - bo_map->mc_base;
            busy[count].end = busy[count].start + live->backing_size;
            count++;
        }
        if (!amdgpu_vram_find(bo_map, rounded, align, busy, count, &offset)) goto out;
        mc = bo_map->mc_base + offset;
        physical = (UQUAD)ad->vram_bar_phys + offset;
        if ((mc & (align - 1)) || (physical & (align - 1))) {
            result = -EINVAL;
            goto out;
        }
        cpu = HIDD_PCIDriver_MapPCI(ad->pciDriver, (APTR)(IPTR)physical, rounded);
        if (!cpu || cpu == (APTR)-1) goto out;
        if (((IPTR)cpu & (align - 1)) || (IPTR)cpu > ~(IPTR)0 - (rounded - 1)) {
            HIDD_PCIDriver_UnmapPCI(ad->pciDriver, cpu, rounded);
            result = -EINVAL; goto out;
        }
        KernelBase = OpenResource("kernel.resource");
        if (!KernelBase || KrnMapGlobal(cpu, (APTR)(IPTR)physical, rounded,
                    MAP_Readable | MAP_Writable | MAP_WriteCombine)) {
            HIDD_PCIDriver_UnmapPCI(ad->pciDriver, cpu, rounded);
            goto out;
        }
        padding = 0;
        memset(cpu, 0, rounded);
        __sync_synchronize();
        bug("[amdgpu-bo] VRAM-owned offset=%llx MC=%llx CPU=%p bytes=%lu\n",
            (unsigned long long)offset, (unsigned long long)mc, cpu, (unsigned long)rounded);
    }
    bo->cpu = cpu;
    bo->ram_allocation = ram_allocation;
    bo->backing_mc = mc;
    bo->payload_mc = domain == AMDGPU_BO_DOMAIN_VRAM ? mc + padding : 0;
    bo->gart_va = gart_va;
    bo->domain = domain;
    bo->backing_size = backing_size;
    bo->size = rounded;
    bo->alignment = align;
    bo->maps = 0;
    bo->copies = 0;
    bo->released = FALSE;
    bo->generation++;
    bo_used += backing_size;
    if (domain == AMDGPU_BO_DOMAIN_VRAM) bo_vram_used += backing_size;
    else bo_gtt_used += backing_size;
    *handle = (bo->generation << 8) | (i + 1);
    result = 0;
out:
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_bo_free(struct AmdgpuDevice *ad, ULONG handle)
{
    struct BoSlot *bo;
    LONG result = -ENOENT;
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    bo = lookup(handle);
    if (bo && !bo->released) {
        bo->released = TRUE;
        if (!bo->maps && !bo->bindings && !bo->copies) destroy(bo);
        result = 0;
    }
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_bo_query_info(struct AmdgpuDevice *ad, ULONG handle,
                         struct AmdgpuBoInfo *out, ULONG out_size)
{
    struct BoSlot *bo;
    LONG result = -ENOENT;
    if (!out || out_size < sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    bo = lookup(handle);
    if (bo && !bo->released) {
        out->size = bo->size;
        out->alignment = bo->alignment;
        out->domain = bo->domain;
        out->map_count = bo->maps;
        result = 0;
    }
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_bo_cpu_map(struct AmdgpuDevice *ad, ULONG handle, APTR *cpu)
{
    struct BoSlot *bo;
    LONG result = -ENOENT;
    if (!cpu) return -EINVAL;
    *cpu = NULL;
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    bo = lookup(handle);
    if (bo && !bo->released) {
        result = -EOVERFLOW;
        if (bo->maps != ~(ULONG)0) {
            bo->maps++;
            *cpu = bo->cpu;
            result = 0;
        }
    }
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_bo_cpu_unmap(struct AmdgpuDevice *ad, ULONG handle)
{
    struct BoSlot *bo;
    LONG result = -ENOENT;
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    bo = lookup(handle);
    if (bo && bo->maps) {
        __sync_synchronize();
        bo->maps--;
        if (bo->released && !bo->maps && !bo->bindings && !bo->copies) destroy(bo);
        result = 0;
    }
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_bo_present_rect(struct AmdgpuDevice *ad, ULONG bo_handle,
                            UQUAD src_offset, ULONG src_stride,
                            APTR dst_cpu, ULONG dst_stride,
                            ULONG row_bytes, ULONG rows)
{
    struct BoSlot *bo;
    UQUAD src_span, dst_span, src_mc, dst_mc, dst_offset;
    IPTR dst, pool;
    ULONG done = 0;
    LONG result = -EINVAL;

    if (!device_valid(ad)) return -ENODEV;
    if (!dst_cpu || !rows || !row_bytes || row_bytes > src_stride ||
        row_bytes > dst_stride)
        return -EINVAL;

    /* ULONG products fit UQUAD without wrap. Validate the complete strided
     * source and destination spans before acquiring a DMA reference. */
    src_span = (UQUAD)(rows - 1) * src_stride + row_bytes;
    dst_span = (UQUAD)(rows - 1) * dst_stride + row_bytes;
    dst = (IPTR)dst_cpu;
    pool = (IPTR)bo_hw->vram_pool_cpu;
    if (!pool || dst < pool) return -ERANGE;
    dst_offset = (UQUAD)(dst - pool);
    if (dst_offset >= bo_hw->vram_pool_size ||
        dst_span > (UQUAD)bo_hw->vram_pool_size - dst_offset)
        return -ERANGE;
    if (bo_hw->vram_pool_mc > ~(UQUAD)0 - dst_offset)
        return -ERANGE;
    dst_mc = bo_hw->vram_pool_mc + dst_offset;

    ObtainSemaphore(&bo_lock);
    bo = lookup(bo_handle);
    if (!bo || bo->released) {
        result = -ENOENT;
        goto unlock;
    }
    if (src_offset >= bo->size || src_span > (UQUAD)bo->size - src_offset) {
        result = -ERANGE;
        goto unlock;
    }
    src_mc = bo->domain == AMDGPU_BO_DOMAIN_VRAM ? bo->payload_mc : bo->gart_va;
    if (!src_mc || src_mc > ~(UQUAD)0 - src_offset) {
        result = -EIO;
        goto unlock;
    }
    if (bo->copies == ~(ULONG)0) {
        result = -EOVERFLOW;
        goto unlock;
    }
    src_mc += src_offset;
    bo->copies++;
    result = 0;
unlock:
    ReleaseSemaphore(&bo_lock);
    if (result) return result;

    /* The 32-bit native-present case is one verified SUB_WIND packet. It
     * accepts either a VRAM MC address or a GART VA. Any ineligible or failed
     * packet falls through to the format-agnostic scanline path below. */
    if (amdgpu_sdma_copy_rect_vram32(ad, src_mc, src_stride, 0,
            dst_mc, dst_stride, 0, row_bytes, rows))
        done = rows;

    /* COPY_LINEAR accepts either a VRAM MC address or a GART VA. The helper's
     * historical name says VRAM because its first caller copied two display
     * bitmaps; the packet itself is address-domain agnostic. */
    while (done < rows) {
        ULONG count = rows - done;
        if (count > AMDGPU_SDMA_COPY_RECT_MAX_ROWS)
            count = AMDGPU_SDMA_COPY_RECT_MAX_ROWS;
        if (!amdgpu_sdma_copy_rect_vram(ad,
                src_mc + (UQUAD)done * src_stride, src_stride, 0,
                dst_mc + (UQUAD)done * dst_stride, dst_stride, 0,
                row_bytes, count)) {
            result = -EIO;
            break;
        }
        done += count;
    }

    ObtainSemaphore(&bo_lock);
    bo = lookup(bo_handle);
    if (!bo || !bo->copies) {
        /* The private reference makes slot reuse impossible; this is a driver
         * invariant failure, not a client-visible lifetime race. */
        result = -EIO;
    } else {
        bo->copies--;
        if (bo->released && !bo->maps && !bo->bindings && !bo->copies)
            destroy(bo);
    }
    ReleaseSemaphore(&bo_lock);
    return result;
}

static struct VaSlot *va_lookup(ULONG handle)
{
    ULONG index = handle & BO_INDEX_MASK;
    struct VaSlot *v;
    if (!index || index > BO_SLOTS) return NULL;
    v = &va_slots[index - 1];
    return v->range && v->generation == (handle >> 8) ? v : NULL;
}

LONG amdgpu_query_va_caps(struct AmdgpuDevice *ad, struct AmdgpuVaCaps *out, ULONG out_size)
{
    if (!out || out_size < sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!device_valid(ad) || !ad->gart_ready) return -ENODEV;
    out->abi_version = AMDGPU_VA_ABI_VERSION;
    out->domains = AMDGPU_BO_DOMAIN_GTT | AMDGPU_BO_DOMAIN_VRAM;
    out->max_handles = BO_SLOTS;
    out->page_size = BO_PAGE;
    out->max_size = bo_gtt_budget < bo_max_size ? bo_gtt_budget : bo_max_size;
    out->max_alignment = BO_MAX_ALIGNMENT;
    out->address_limit = 512UL << 20; /* exclusive VMID0 GART upper bound */
    return 0;
}

LONG amdgpu_va_range_alloc(struct AmdgpuDevice *ad, UQUAD size, UQUAD alignment,
                          UQUAD requested, ULONG *handle, UQUAD *address)
{
    ULONG i, rounded, align;
    LONG result = -ENOMEM;
    if (handle) *handle = 0;
    if (address) *address = 0;
    if (!handle || !address) return -EINVAL;
    if (!device_valid(ad) || !ad->gart_ready) return -ENODEV;
    if (!size || size > bo_max_size || size > bo_gtt_budget || alignment > BO_MAX_ALIGNMENT ||
        (alignment && (alignment & (alignment - 1)))) return -EINVAL;
    align = alignment < BO_PAGE ? BO_PAGE : (ULONG)alignment;
    rounded = ((ULONG)size + BO_PAGE - 1) & ~(BO_PAGE - 1);
    if (requested >= (512UL << 20) || (requested & (align - 1)) ||
        requested > (512UL << 20) - rounded) return -EINVAL;
    ObtainSemaphore(&bo_lock);
    for (i = 0; i < BO_SLOTS; i++) {
        struct VaSlot *v = &va_slots[i];
        if (v->range || v->generation == BO_GENERATION_MAX) continue;
        v->range = amdgpu_gart_reserve(ad, rounded, align, requested, address);
        if (!v->range) break;
        v->address = *address;
        v->size = rounded;
        v->alignment = align;
        v->state = AMDGPU_VA_STATE_RESERVED;
        v->bo_handle = v->gpu_refs = 0;
        v->generation++;
        *handle = (v->generation << 8) | (i + 1);
        result = 0;
        break;
    }
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_va_range_free(struct AmdgpuDevice *ad, ULONG handle)
{
    struct VaSlot *v;
    LONG result = -ENOENT;
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    v = va_lookup(handle);
    if (v) {
        result = -EBUSY;
        if (!v->bo_handle && !v->gpu_refs) {
            amdgpu_gart_release_range(v->range);
            v->range = NULL;
            result = 0;
        }
    }
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_va_query_info(struct AmdgpuDevice *ad, ULONG handle,
                         struct AmdgpuVaInfo *out, ULONG out_size)
{
    struct VaSlot *v;
    LONG result = -ENOENT;
    if (!out || out_size < sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    v = va_lookup(handle);
    if (v) {
        out->address = v->address;
        out->size = v->size;
        out->alignment = v->alignment;
        out->state = v->state;
        out->bo_handle = v->bo_handle;
        out->gpu_refs = v->gpu_refs;
        result = 0;
    }
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_bo_va_bind(struct AmdgpuDevice *ad, ULONG bo_handle, ULONG va_handle)
{
    struct BoSlot *bo;
    struct VaSlot *v;
    LONG result = -ENOENT;
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    bo = lookup(bo_handle);
    v = va_lookup(va_handle);
    if (!bo || bo->released || !v) goto out;
    result = -ENOSYS;
    if (bo->domain != AMDGPU_BO_DOMAIN_GTT && bo->domain != AMDGPU_BO_DOMAIN_VRAM) goto out;
    result = -EINVAL;
    if (v->size != bo->size) goto out;
    result = -EBUSY;
    if (v->state != AMDGPU_VA_STATE_RESERVED) goto out;
    result = -EOVERFLOW;
    if (bo->bindings == ~(ULONG)0) goto out;
    /* Take ownership BEFORE any PTE publication, including failed transactions. */
    bo->bindings++;
    v->bo_handle = bo_handle;
    v->state = AMDGPU_VA_STATE_QUARANTINED;
    result = -EIO;
    if (bo->domain == AMDGPU_BO_DOMAIN_GTT ?
        amdgpu_gart_bind_range(ad, v->range, bo->cpu) :
        amdgpu_gart_bind_vram_range(ad, v->range, bo->payload_mc)) {
        v->state = AMDGPU_VA_STATE_BOUND;
        result = 0;
    }
out:
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_bo_va_unbind(struct AmdgpuDevice *ad, ULONG va_handle)
{
    struct BoSlot *bo;
    struct VaSlot *v;
    LONG result = -ENOENT;
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    v = va_lookup(va_handle);
    if (!v || !v->bo_handle || !(bo = lookup(v->bo_handle))) goto out;
    result = -EBUSY;
    if (v->gpu_refs) goto out;
    result = -EIO;
    if (!amdgpu_gart_unbind_range(ad, v->range)) goto out;
    v->state = AMDGPU_VA_STATE_RETIRED;
    v->bo_handle = 0;
    bo->bindings--;
    if (bo->released && !bo->maps && !bo->bindings && !bo->copies) destroy(bo);
    result = 0;
out:
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_va_gpu_acquire(struct AmdgpuDevice *ad, ULONG handle, UQUAD *address, ULONG *size)
{
    struct VaSlot *v;
    struct BoSlot *bo;
    ULONG off, length;
    LONG result = -ENOENT;
    if (!address || !size) return -EINVAL;
    *address = 0;
    *size = 0;
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    v = va_lookup(handle);
    if (v && v->state == AMDGPU_VA_STATE_BOUND && (bo = lookup(v->bo_handle))) {
        result = -EOVERFLOW;
        if (v->gpu_refs != ~(ULONG)0) {
            /* Owned page-sized intervals; no assumed physical contiguity.
             * The caller promises no CPU mutation until GPU retirement. */
            /* Local VRAM is a WC BAR borrow, not system RAM to translate or
             * CLFLUSH through CachePreDMA. Its UC PTE and full fence preserve
             * the CPU-to-GPU publication ordering without inventing RAM PFNs. */
            __sync_synchronize();
            for (off = 0; bo->domain == AMDGPU_BO_DOMAIN_GTT && off < v->size; off += BO_PAGE) {
                length = BO_PAGE;
                CachePreDMA((UBYTE *)bo->cpu + off, &length, DMA_ReadFromRAM);
                if (length != BO_PAGE) { result = -EIO; goto acquired; }
            }
            v->gpu_refs++;
            *address = v->address;
            *size = v->size;
            result = 0;
        }
    }
acquired:
    ReleaseSemaphore(&bo_lock);
    return result;
}

LONG amdgpu_va_gpu_release(struct AmdgpuDevice *ad, ULONG handle)
{
    struct VaSlot *v;
    struct BoSlot *bo;
    ULONG off, length;
    LONG result = -ENOENT;
    if (!device_valid(ad)) return -ENODEV;
    ObtainSemaphore(&bo_lock);
    v = va_lookup(handle);
    if (v && v->gpu_refs && (bo = lookup(v->bo_handle))) {
        /* This runs only after the private real EOP/RPTR/idle retirement.
         * Local VRAM UC writes are now complete; order subsequent BAR reads. */
        __sync_synchronize();
        for (off = 0; bo->domain == AMDGPU_BO_DOMAIN_GTT && off < v->size; off += BO_PAGE) {
            length = BO_PAGE;
            CachePostDMA((UBYTE *)bo->cpu + off, &length, 0);
            if (length != BO_PAGE) { result = -EIO; goto released; }
        }
        v->gpu_refs--; result = 0;
    }
released:
    ReleaseSemaphore(&bo_lock);
    return result;
}
