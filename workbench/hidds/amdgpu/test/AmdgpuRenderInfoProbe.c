/* Native resident-only render topology acceptance. Install/compare/reboot the
 * driver first. This client cannot read MMIO or initialize a private driver.
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
#define LOG(...) do { printf("[AmdgpuRenderInfoProbe] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuRenderInfoProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define NEED(c, s) do { if (!(c)) { LOG("FAIL %s", s); goto out; } \
                         LOG("PASS %s", s); } while (0)

static BOOL bytes_are(const void *data, ULONG size, UBYTE value)
{
    const UBYTE *p = data;
    while (size--) if (*p++ != value) return FALSE;
    return TRUE;
}

static BOOL asic_queries(struct AmdgpuDevice *ad)
{
    struct AmdgpuAsicInfo first, next;
    struct { struct AmdgpuAsicInfo info; ULONG guard[2]; } bounded;
    ULONG round;
    if (amdgpu_query_asic_info(ad, &first, sizeof(first))) return FALSE;
    LOG("ASIC flags=%lx chip=%lu external=%lu PCI=%lx strap[%lx]=%08lx "
        "ATOM=%lx master=%lx UMC=%lx GFX=%lx type=%lx bus=%lu timer10kHz=%lu "
        "boot10kHz=%lu/%lu", (unsigned long)first.flags, (unsigned long)first.chip_rev,
        (unsigned long)first.external_rev, (unsigned long)first.pci_rev,
        (unsigned long)first.strap_register_dw, (unsigned long)first.strap_value,
        (unsigned long)first.rom_header_version, (unsigned long)first.master_table_version,
        (unsigned long)first.umc_table_version, (unsigned long)first.gfx_table_version,
        (unsigned long)first.atom_vram_type, (unsigned long)first.memory_bus_width,
        (unsigned long)first.gpu_counter_refclk_10khz, (unsigned long)first.boot_engine_clock_10khz,
        (unsigned long)first.boot_memory_clock_10khz);
    if (first.abi_version != 1 || (first.flags & 7) != 7 || first.pci_rev != 0xc0 ||
        first.chip_rev != ((first.strap_value >> 24) & 0xf) ||
        first.external_rev != first.chip_rev + 0x40 || !first.memory_channels ||
        first.memory_channel_width_log2 > 16 ||
        first.memory_bus_width != (first.memory_channels << first.memory_channel_width_log2) ||
        !first.gpu_counter_refclk_10khz || first.gpu_counter_refclk_10khz > 0xffffffffUL / 10)
        return FALSE;
    for (round = 0; round < 256; round++) {
        if (amdgpu_query_asic_info(ad, &next, sizeof(next)) || memcmp(&first, &next, sizeof(first)))
            return FALSE;
    }
    memset(&bounded, 0xa5, sizeof(bounded));
    if (amdgpu_query_asic_info(ad, &bounded.info, sizeof(first) - 1) != -EINVAL ||
        !bytes_are(&bounded, sizeof(bounded), 0xa5)) return FALSE;
    if (amdgpu_query_asic_info(ad, &bounded.info, sizeof(bounded)) ||
        memcmp(&first, &bounded.info, sizeof(first)) ||
        !bytes_are(bounded.guard, sizeof(bounded.guard), 0xa5)) return FALSE;
    if (amdgpu_query_asic_info(ad, NULL, sizeof(next)) != -EINVAL) return FALSE;
    memset(&next, 0xa5, sizeof(next));
    if (amdgpu_query_asic_info(NULL, &next, sizeof(next)) != -ENODEV ||
        !bytes_are(&next, sizeof(next), 0)) return FALSE;
    memset(&next, 0xa5, sizeof(next));
    if (amdgpu_query_asic_info((struct AmdgpuDevice *)(IPTR)1, &next, sizeof(next)) != -ENODEV ||
        !bytes_are(&next, sizeof(next), 0)) return FALSE;
    return TRUE;
}

static BOOL firmware_queries(struct AmdgpuDevice *ad)
{
    struct AmdgpuFirmwareInfo first, next;
    struct { struct AmdgpuFirmwareInfo info; ULONG guard[2]; } bounded;
    ULONG type, round, stages;
    for (type = 0; type < AMDGPU_FIRMWARE_TYPES; type++) {
        if (amdgpu_query_firmware_info(ad, type, &first, sizeof(first))) return FALSE;
        stages = type == AMDGPU_FIRMWARE_MEC ? 7 : 3;
        LOG("firmware type=%lu version=%lu feature=%lu flags=%lx stages=%lx/%lx "
            "PSP=%08lx/%08lx/%08lx bytes=%lu crc=%08lx", (unsigned long)type,
            (unsigned long)first.version, (unsigned long)first.feature,
            (unsigned long)first.flags, (unsigned long)first.completed_stages,
            (unsigned long)first.required_stages, (unsigned long)first.psp_status[0],
            (unsigned long)first.psp_status[1], (unsigned long)first.psp_status[2],
            (unsigned long)first.source_bytes, (unsigned long)first.source_crc32);
        if (first.abi_version != 1 || first.type != type ||
            first.flags != (AMDGPU_FIRMWARE_HEADER_VALID | AMDGPU_FIRMWARE_STAGED) ||
            first.header_major != 2 || first.header_minor || first.ip_major != 12 ||
            first.ip_minor || first.header_bytes < 60 ||
            first.header_bytes > first.source_bytes || !first.instruction_bytes ||
            !first.data_bytes || first.required_stages != stages ||
            first.completed_stages != stages) return FALSE;
        for (round = 0; round < 256; round++) {
            if (amdgpu_query_firmware_info(ad, type, &next, sizeof(next)) ||
                memcmp(&first, &next, sizeof(first))) return FALSE;
        }
        memset(&bounded, 0xa5, sizeof(bounded));
        if (amdgpu_query_firmware_info(ad, type, &bounded.info, sizeof(first) - 1) != -EINVAL ||
            !bytes_are(&bounded, sizeof(bounded), 0xa5)) return FALSE;
        if (amdgpu_query_firmware_info(ad, type, &bounded.info, sizeof(bounded)) ||
            memcmp(&first, &bounded.info, sizeof(first)) ||
            !bytes_are(bounded.guard, sizeof(bounded.guard), 0xa5)) return FALSE;
    }
    memset(&next, 0xa5, sizeof(next));
    if (amdgpu_query_firmware_info(ad, AMDGPU_FIRMWARE_TYPES, &next, sizeof(next)) != -EINVAL ||
        !bytes_are(&next, sizeof(next), 0)) return FALSE;
    if (amdgpu_query_firmware_info(ad, 0, NULL, sizeof(next)) != -EINVAL) return FALSE;
    memset(&next, 0xa5, sizeof(next));
    if (amdgpu_query_firmware_info(NULL, 0, &next, sizeof(next)) != -ENODEV ||
        !bytes_are(&next, sizeof(next), 0)) return FALSE;
    memset(&next, 0xa5, sizeof(next));
    if (amdgpu_query_firmware_info((struct AmdgpuDevice *)(IPTR)1, 0, &next, sizeof(next)) != -ENODEV ||
        !bytes_are(&next, sizeof(next), 0)) return FALSE;
    LOG("PASS three staged CP records, immutable snapshots and firmware query bounds; "
        "raw responses retained, not an authentication/compute readiness assertion");
    return TRUE;
}

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad = NULL;
    struct AmdgpuGpuInfo gpu = {0};
    struct AmdgpuRenderInfo first = {0}, next;
    struct { struct AmdgpuRenderInfo info; ULONG guard[2]; } bounded;
    ULONG se, sa, count = 0, bits, round, cu_mask;
    LONG result = RETURN_FAIL;

    Forbid(); resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd"); Permit();
    if (!resident) { LOG("REFUSED no resident driver; no load attempted"); return result; }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    LOG("resident=%p opened=%p negative-bytes=%lu required=%lu", resident, AmdgpuBase,
        (unsigned long)AmdgpuBase->lib_NegSize, (unsigned long)(43 * sizeof(APTR)));
    /* NegSize can contain alignment padding: size alone cannot distinguish
     * vector42 from the old vector41 table rounded to336 bytes. Revision0.2
     * introduces that vector, and0.3 introduces ASIC vector43; refuse older
     * resident drivers before calling them.
     */
    NEED(AmdgpuBase == resident && AmdgpuBase->lib_NegSize >= 43 * sizeof(APTR) &&
         (AmdgpuBase->lib_Version > 0 || AmdgpuBase->lib_Revision >= 3),
         "resident identity, ASIC/firmware ABI revision and vector extent");
    NEED(amdgpu_get_device_count() == 1 && (ad = amdgpu_get_device(0)), "one installed device");
    amdgpu_query_gpu_info(ad, &gpu);
    NEED(gpu.asic_id == 0x7590 && gpu.pci_rev == 0xc0 && gpu.gc_major == 12,
         "RX9060XT GC12 identity");
    NEED(amdgpu_query_render_info(ad, &first, sizeof(first)) == 0 &&
         first.abi_version == AMDGPU_RENDER_INFO_ABI_VERSION &&
         (first.flags & AMDGPU_RENDER_INFO_READY), "validated boot-time topology available");
    LOG("GC=%lu.%lu flags=%lx SE=%lu SA/SE=%lu maxCU/SA=%lu activeCU=%lu SA=%lx RB=%lx "
        "addr=%08lx selector=%08lx/%08lx", (unsigned long)first.gc_table_major,
        (unsigned long)first.gc_table_minor, (unsigned long)first.flags,
        (unsigned long)first.num_shader_engines, (unsigned long)first.shader_arrays_per_engine,
        (unsigned long)first.max_cu_per_array, (unsigned long)first.active_cu_count,
        (unsigned long)first.active_sa_mask, (unsigned long)first.enabled_rb_mask,
        (unsigned long)first.gb_addr_config, (unsigned long)first.selector_before,
        (unsigned long)first.selector_after);
    NEED(first.num_shader_engines && first.num_shader_engines <= 8 &&
         first.shader_arrays_per_engine && first.shader_arrays_per_engine <= 2 &&
         first.max_cu_per_array && first.max_cu_per_array <= 32 &&
         first.enabled_rb_mask && first.gb_addr_config &&
         first.selector_before == first.selector_after, "bounded topology and restored boot selector");
    cu_mask = first.max_cu_per_array == 32 ? 0xffffffffUL : (1UL << first.max_cu_per_array) - 1;
    for (se = 0; se < first.num_shader_engines; se++) {
        for (sa = 0; sa < first.shader_arrays_per_engine; sa++) {
            bits = first.cu_bitmap[se % 4][sa + (se / 4) * 2];
            LOG("SE=%lu SA=%lu CU-mask=%08lx", (unsigned long)se, (unsigned long)sa,
                (unsigned long)bits);
            NEED(!(bits & ~cu_mask), "CU mask within discovery maximum");
            NEED(!!bits == !!(first.active_sa_mask &
                 (1UL << (se * first.shader_arrays_per_engine + sa))), "active SA agrees with CU mask");
            while (bits) { count++; bits &= bits - 1; }
        }
    }
    NEED(count == first.active_cu_count, "enabled CU population matches packed bitmap");
    LOG("RB/SE=%lu TCC=%lu wave=%lu LDS=%lu TCP=%lu GL1=%lu L2=%lu SQinst=%lu SQdata=%lu",
        (unsigned long)first.rb_pipes_per_engine, (unsigned long)first.num_tcc_blocks,
        (unsigned long)first.wavefront_size, (unsigned long)first.lds_size,
        (unsigned long)first.tcp_l1_size, (unsigned long)first.gl1c_size,
        (unsigned long)first.tcc_size, (unsigned long)first.instruction_cache_size,
        (unsigned long)first.scalar_cache_size);
    for (round = 0; round < 256; round++) {
        if (amdgpu_query_render_info(ad, &next, sizeof(next)) || memcmp(&first, &next, sizeof(first))) {
            LOG("FAIL immutable snapshot round=%lu", (unsigned long)round); goto out;
        }
    }
    LOG("PASS 256 identical cached queries; no runtime register selection");
    memset(&bounded, 0xa5, sizeof(bounded));
    NEED(amdgpu_query_render_info(ad, &bounded.info, sizeof(bounded.info) - 1) == -EINVAL &&
         bytes_are(&bounded, sizeof(bounded), 0xa5), "short buffer untouched");
    NEED(amdgpu_query_render_info(ad, &bounded.info, sizeof(bounded)) == 0 &&
         !memcmp(&first, &bounded.info, sizeof(first)) &&
         bytes_are(bounded.guard, sizeof(bounded.guard), 0xa5), "larger caller buffer tail preserved");
    NEED(amdgpu_query_render_info(ad, NULL, sizeof(first)) == -EINVAL, "null output rejected");
    memset(&next, 0xa5, sizeof(next));
    NEED(amdgpu_query_render_info(NULL, &next, sizeof(next)) == -ENODEV &&
         bytes_are(&next, sizeof(next), 0), "null device returns zeroed output");
    memset(&next, 0xa5, sizeof(next));
    NEED(amdgpu_query_render_info((struct AmdgpuDevice *)(IPTR)1, &next, sizeof(next)) == -ENODEV &&
         bytes_are(&next, sizeof(next), 0), "unknown device rejected without dereferencing it");
    NEED(firmware_queries(ad), "firmware provenance queries");
    NEED(asic_queries(ad), "ASIC revision, VBIOS provenance and immutable query bounds");
    result = RETURN_OK;
out:
    LOG("END result=%ld; topology only, not shader rendering or full Mesa support", (long)result);
    CloseLibrary(AmdgpuBase);
    return result;
}
