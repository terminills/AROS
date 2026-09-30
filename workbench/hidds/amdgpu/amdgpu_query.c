/*
 * amdgpu.hidd — Device/Query tier (observe-only).
 *
 * The driver side of the RFC's Query surface: report what Tier-1 identify and
 * Tier-1.5 IP-discovery already learned (device id, family, VRAM, per-IP
 * versions, PCI location) plus raw MMIO register reads. Nothing here allocates,
 * maps VRAM, or touches a BO. Render-info initialization additionally samples
 * banked topology, restoring its selector before clients can open the driver.
 * Public render queries only copy that immutable snapshot.
 *
 * The Mesa ac_drm seam (libamd_common/ac_aros_drm.c) translates these into
 * libdrm's amdgpu_gpu_info / drm_amdgpu_info_* wire structs; keeping the ABI
 * layout on that side lets this file stay free of the libdrm headers and
 * validate standalone via crosstools.
 */

#define DEBUG 1
#include <aros/debug.h>

#include <proto/oop.h>
#include <hidd/pci.h>
#include <errno.h>
#include <string.h>

#include "amdgpu_intern.h"
#include "amdgpu_query_internal.h"

/* GC12 topology follows AMD's Linux discovery.h/amdgpu_discovery.c and
 * gfx_v12_0.c get_sa/rb/wgp_active_bitmap, using gc_12_0_0 register definitions.
 * No user harvest mask is programmed. GRBM_GFX_INDEX is temporarily selected
 * only during the serial init ladder, before any client/display publication,
 * then restored exactly and checked. Runtime calls ONLY copy this cache.
 * Keep this out of the shared discovery/device structs while that code has
 * independent owners. The bounded cache refuses excess devices, not aliases.
 */
#define RENDER_CACHE_SLOTS 8
static struct {
    struct AmdgpuDevice *ad;
    struct AmdgpuRenderInfo info;
} render_cache[RENDER_CACHE_SLOTS];

static struct {
    struct AmdgpuDevice *ad;
    struct AmdgpuFirmwareInfo info[AMDGPU_FIRMWARE_TYPES];
} firmware_cache[RENDER_CACHE_SLOTS];

static struct {
    struct AmdgpuDevice *ad;
    struct AmdgpuAsicInfo info;
} asic_cache[RENDER_CACHE_SLOTS];

void amdgpu_firmware_info_reset(struct AmdgpuDevice *ad)
{
    ULONG i;
    if (!ad) return;
    for (i = 0; i < RENDER_CACHE_SLOTS; i++) {
        if (firmware_cache[i].ad == ad) break;
    }
    if (i == RENDER_CACHE_SLOTS) {
        for (i = 0; i < RENDER_CACHE_SLOTS && firmware_cache[i].ad; i++) {}
    }
    if (i == RENDER_CACHE_SLOTS) return;
    memset(firmware_cache[i].info, 0, sizeof(firmware_cache[i].info));
    firmware_cache[i].ad = ad;
}

void amdgpu_firmware_info_record(struct AmdgpuDevice *ad,
                                 const struct AmdgpuFirmwareInfo *info)
{
    ULONG i;
    if (!ad || !info || info->type >= AMDGPU_FIRMWARE_TYPES) return;
    for (i = 0; i < RENDER_CACHE_SLOTS; i++) {
        if (firmware_cache[i].ad == ad) {
            firmware_cache[i].info[info->type] = *info;
            return;
        }
    }
}

LONG amdgpu_query_firmware_info(struct AmdgpuDevice *ad, ULONG type,
                                struct AmdgpuFirmwareInfo *out, ULONG out_size)
{
    ULONG i;
    if (!out || out_size < sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (type >= AMDGPU_FIRMWARE_TYPES) return -EINVAL;
    if (!ad) return -ENODEV;
    for (i = 0; i < RENDER_CACHE_SLOTS; i++) {
        if (firmware_cache[i].ad == ad &&
            (firmware_cache[i].info[type].flags & AMDGPU_FIRMWARE_HEADER_VALID)) {
            *out = firmware_cache[i].info[type];
            return 0;
        }
    }
    return -ENODEV;
}

static ULONG render_le16(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8);
}

static ULONG render_le32(const UBYTE *p)
{
    return render_le16(p) | (render_le16(p + 2) << 16);
}

/* A validated common ATOM header and its complete declared extent. */
static const UBYTE *asic_atom_table(struct AmdgpuDevice *ad, ULONG offset, ULONG minimum)
{
    ULONG length;
    if (!offset || offset > ad->rom_size || ad->rom_size - offset < 4) return NULL;
    length = render_le16(ad->rom_base + offset);
    if (length < minimum || length > ad->rom_size - offset) return NULL;
    return ad->rom_base + offset;
}

static const UBYTE *asic_atom_data(struct AmdgpuDevice *ad, const UBYTE *master,
                                    ULONG index, ULONG minimum)
{
    ULONG field = 4 + index * 2;
    if (!master || field + 2 > render_le16(master)) return NULL;
    return asic_atom_table(ad, render_le16(master + field), minimum);
}

static void asic_info_init(struct AmdgpuDevice *ad)
{
    struct AmdgpuAsicInfo info = {0};
    const UBYTE *rom, *header, *master = NULL, *table;
    ULONG i, value, refclk;
    UQUAD reg;
    for (i = 0; i < RENDER_CACHE_SLOTS; i++) {
        if (asic_cache[i].ad == ad) return;
        if (!asic_cache[i].ad) break;
    }
    if (i == RENDER_CACHE_SLOTS) return;
    info.abi_version = AMDGPU_ASIC_INFO_ABI_VERSION;
    info.pci_rev = ad->revision;
    /* Exact NBIF6.3.1 get_rev_id register, NOT NBIO7.11's STRAP1 register.
     * Direct BAR reads only, using discovery BASE_IDX2; no index writes.
     */
    reg = (UQUAD)ad->nbio_seg[2] + 0x1c;
    if (ad->ip.nbio.valid && ad->ip.nbio.major == 6 && ad->ip.nbio.minor == 3 &&
        ad->ip.nbio.revision == 1 && ad->nbio_seg[2] && ad->rmmio &&
        reg <= 0x3fffffffULL && reg * 4 + 4 <= ad->rmmio_size) {
        value = amdgpu_rreg32(ad, (ULONG)reg);
        if (value != 0xffffffffUL && value == amdgpu_rreg32(ad, (ULONG)reg)) {
            info.strap_register_dw = (ULONG)reg; info.strap_value = value;
            info.chip_rev = (value >> 24) & 0xf;
            info.external_rev = info.chip_rev + 0x40; /* soc24 GC12.0.0 */
            info.flags |= AMDGPU_ASIC_REVISION_VALID;
        }
    }
    /* Firmware readback already belongs to the init ladder; use its bounded
     * host copy rather than reading the ROM BAR or running ATOM commands.
     */
    rom = ad->rom_base; info.rom_bytes = ad->rom_size;
    if (rom && ad->rom_size >= 0x4a && rom[0] == 0x55 && rom[1] == 0xaa) {
        header = asic_atom_table(ad, render_le16(rom + 0x48), 36);
        if (header && !memcmp(header + 4, "ATOM", 4)) {
            info.rom_header_version = ((ULONG)header[2] << 8) | header[3];
            /* atom.c uses ATOM_ROM_DATA_PTR=0x20 independently of the minor
             * header revision; atomfirmware.h marks this field position as
             * invariant. This DUT carries2.3. Accept that checked prefix,
             * retain the declared table bounds and separate master version.
             */
            if (info.rom_header_version == 0x202 || info.rom_header_version == 0x203)
                master = asic_atom_table(ad, render_le16(header + 32), 4);
            if (master) {
                info.master_table_version = ((ULONG)master[2] << 8) | master[3];
                if (info.master_table_version != 0x201) master = NULL;
            }
        }
    }
    /* atom_umc_info_v4_0: memory type is BYTE26, channels/width BYTE64/65. */
    table = asic_atom_data(ad, master, 24, 68);
    if (table) {
        info.umc_table_version = ((ULONG)table[2] << 8) | table[3];
        if (info.umc_table_version == 0x400 && table[64] && table[65] <= 16) {
            info.atom_vram_type = table[26];
            info.memory_channels = table[64];
            info.memory_channel_width_log2 = table[65];
            info.memory_bus_width = (ULONG)table[64] << table[65];
            info.memory_refclk_10khz = render_le32(table + 28);
            info.flags |= AMDGPU_ASIC_UMC_VALID;
        }
    }
    /* Navi+ uses the GFX RLC timer reference, overriding generic SMU refclk.
     * GC12 uses atom_gfx_info_v3_0 / equivalent v2.6 layout, +40 DWORD.
     */
    table = asic_atom_data(ad, master, 14, 44);
    if (table) {
        info.gfx_table_version = ((ULONG)table[2] << 8) | table[3];
        if (table[2] == 3 || info.gfx_table_version == 0x206) {
            refclk = render_le32(table + 40);
            if (refclk && refclk <= 0xffffffffUL / 10) {
                info.gpu_counter_refclk_10khz = refclk;
                info.flags |= AMDGPU_ASIC_TIMER_VALID;
            }
        }
    }
    table = asic_atom_data(ad, master, 4, 16);
    if (table) {
        info.firmware_table_version = ((ULONG)table[2] << 8) | table[3];
        if (table[2] == 3 && table[3] >= 1 && table[3] <= 5) {
            info.boot_engine_clock_10khz = render_le32(table + 8);
            info.boot_memory_clock_10khz = render_le32(table + 12);
            info.flags |= AMDGPU_ASIC_BOOT_CLOCKS_VALID; /* zero is meaningful */
        }
    }
    asic_cache[i].info = info; asic_cache[i].ad = ad;
    bug("[amdgpu-query] asic-info-v2 flags=%lx strap[%lx]=%08lx chip=%lu external=%lu "
        "PCI=%lx ATOM=%lx master=%lx UMC=%lx GFX=%lx type=%lx width=%lu "
        "timer10kHz=%lu bootclocks=%lu/%lu\n", (unsigned long)info.flags,
        (unsigned long)info.strap_register_dw, (unsigned long)info.strap_value,
        (unsigned long)info.chip_rev, (unsigned long)info.external_rev,
        (unsigned long)info.pci_rev, (unsigned long)info.rom_header_version,
        (unsigned long)info.master_table_version, (unsigned long)info.umc_table_version,
        (unsigned long)info.gfx_table_version, (unsigned long)info.atom_vram_type,
        (unsigned long)info.memory_bus_width, (unsigned long)info.gpu_counter_refclk_10khz,
        (unsigned long)info.boot_engine_clock_10khz, (unsigned long)info.boot_memory_clock_10khz);
}

LONG amdgpu_query_asic_info(struct AmdgpuDevice *ad,
                            struct AmdgpuAsicInfo *out, ULONG out_size)
{
    ULONG i;
    if (!out || out_size < sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!ad) return -ENODEV;
    for (i = 0; i < RENDER_CACHE_SLOTS; i++) {
        if (asic_cache[i].ad == ad) { *out = asic_cache[i].info; return 0; }
    }
    return -ENODEV;
}

static ULONG render_sum(const UBYTE *p, ULONG size)
{
    ULONG sum = 0;
    while (size--) sum += *p++;
    return sum & 0xffff;
}

static ULONG render_mask(ULONG n)
{
    return n == 32 ? 0xffffffffUL : (1UL << n) - 1;
}

static ULONG render_count(ULONG value)
{
    ULONG n = 0;
    while (value) { value &= value - 1; n++; }
    return n;
}

static BOOL render_discovery(struct AmdgpuDevice *ad, struct AmdgpuRenderInfo *r)
{
    const UBYTE *bin = ad->disc_bin, *table;
    ULONG size, tables, base, offset, length, major, minor, i, n;
    ULONG gc[38] = {0};
    static const ULONG fields[4] = {19, 22, 30, 38};

    if (!bin || ad->disc_size < 12 || render_le32(bin) != 0x28211407)
        return FALSE;
    size = render_le16(bin + 10);
    major = render_le16(bin + 4);
    bug("[amdgpu-query] discovery binary version=%lu size=%lu checksum=%04lx/%04lx\n",
        (unsigned long)major, (unsigned long)size,
        (unsigned long)(size >= 12 && size <= ad->disc_size ? render_sum(bin + 10, size - 10) : 0),
        (unsigned long)render_le16(bin + 8));
    if (size < 12 || size > ad->disc_size || major > 2 ||
        render_sum(bin + 10, size - 10) != render_le16(bin + 8))
        return FALSE;
    base = major == 2 ? 16 : 12;
    if (size < base) return FALSE;
    tables = major == 2 ? render_le16(bin + 12) : 6;
    if (tables < 2 || tables > (size - base) / 8) return FALSE;
    /* Table index 1 is GC, not the IP-discovery or harvest-info table. */
    offset = render_le16(bin + base + 8);
    length = render_le16(bin + base + 12);
    if (offset < base + tables * 8 || offset > size || length < 12 ||
        length > size - offset) return FALSE;
    table = bin + offset;
    bug("[amdgpu-query] GC table offset=%lu length=%lu header-size=%lu version=%lu.%lu checksum=%04lx/%04lx\n",
        (unsigned long)offset, (unsigned long)length, (unsigned long)render_le32(table + 8),
        (unsigned long)render_le16(table + 4), (unsigned long)render_le16(table + 6),
        (unsigned long)render_sum(table, length), (unsigned long)render_le16(bin + base + 10));
    if (render_sum(table, length) != render_le16(bin + base + 10) ||
        render_le32(table) != 0x4347 || render_le32(table + 8) != length)
        return FALSE;
    major = render_le16(table + 4); minor = render_le16(table + 6);
    if (major != 1 || minor > 3 || length < 12 + fields[minor] * 4)
        return FALSE;
    n = fields[minor];
    for (i = 0; i < n; i++) gc[i] = render_le32(table + 12 + i * 4);
    bug("[amdgpu-query] GC fields SE=%lu WGP=%lu+%lu RB/SE=%lu TCC=%lu wave=%lu SC/SE=%lu SA/SE=%lu\n",
        (unsigned long)gc[0], (unsigned long)gc[1], (unsigned long)gc[2],
        (unsigned long)gc[3], (unsigned long)gc[4], (unsigned long)gc[11],
        (unsigned long)gc[15], (unsigned long)gc[16]);
    /* These bounds also prove every bitmap shift and register-bank index. */
    if (!gc[0] || gc[0] > 8 || !gc[16] || gc[16] > 2 ||
        gc[1] > 16 || gc[2] > 16 || !(gc[1] + gc[2]) ||
        gc[1] + gc[2] > 16 || !gc[3] || gc[3] > 32 / gc[0] ||
        gc[3] % gc[16]) return FALSE;

    /* Wave/cache/scan-converter quantities are descriptive firmware fields,
     * not inputs to bank selection or bitmap shifts. Linux copies these raw
     * (and divides SC/SE by SA/SE), without requiring nonzero cache/SC fields
     * or treating wave_size as a literal 32/64 lane count. Preserve their wire
     * values here; do not reject valid topology on undocumented assumptions.
     * A Mesa consumer still has to interpret/validate the fields it uses.
     */

    r->gc_table_major = major; r->gc_table_minor = minor;
    r->num_shader_engines = gc[0];
    r->max_cu_per_array = 2 * (gc[1] + gc[2]);
    r->rb_pipes_per_engine = gc[3]; r->num_tcc_blocks = gc[4];
    r->num_gprs = gc[5]; r->max_gs_threads = gc[6];
    r->gs_table_depth = gc[7]; r->gsprim_buffer_depth = gc[8];
    r->parameter_cache_depth = gc[9]; r->double_offchip_lds_buffer = gc[10];
    r->wavefront_size = gc[11]; r->max_waves_per_simd = gc[12];
    r->max_scratch_slots_per_cu = gc[13]; r->lds_size = gc[14];
    r->num_sc_per_array = gc[15] / gc[16]; r->shader_arrays_per_engine = gc[16];
    r->num_packer_per_sc = gc[17]; r->num_gl2a = gc[18];
    r->tcp_per_array = gc[19]; r->sdp_interfaces = gc[20]; r->num_tcps = gc[21];
    r->tcp_per_wgp = gc[22]; r->tcp_l1_size = gc[23]; r->sqc_per_wgp = gc[24];
    r->instruction_cache_size = gc[25]; r->scalar_cache_size = gc[26];
    r->gl1c_per_array = gc[27]; r->gl1c_size = gc[28]; r->gl2c_per_gpu = gc[29];
    if (minor >= 2) r->flags |= AMDGPU_RENDER_INFO_CACHE_V12;
    if (minor >= 3) {
        r->tcp_size_per_cu = gc[30]; r->tcp_line_size = gc[31];
        r->instruction_cache_size = gc[32]; r->instruction_line_size = gc[33];
        r->scalar_cache_size = gc[34]; r->scalar_line_size = gc[35];
        r->tcc_size = gc[36]; r->tcc_line_size = gc[37];
        r->flags |= AMDGPU_RENDER_INFO_CACHE_V13;
    }
    return TRUE;
}

static BOOL render_harvest(struct AmdgpuDevice *ad, struct AmdgpuRenderInfo *r)
{
    /* Each pair is a DWORD offset and its discovered GC segment index. */
    static const ULONG regs[][2] = {
        {0x2200, 1}, /* GRBM_GFX_INDEX */
        {0x100f, 0}, /* CC_GC_SHADER_ARRAY_CONFIG */
        {0x5b90, 1}, /* GC_USER_SHADER_ARRAY_CONFIG */
        {0x13dd, 0}, /* CC_RB_BACKEND_DISABLE */
        {0x5b94, 1}, /* GC_USER_RB_BACKEND_DISABLE */
        {0x13de, 0}, /* GB_ADDR_CONFIG */
        {0x0fe9, 0}, /* GRBM_CC_GC_SA_UNIT_DISABLE */
        {0x5b92, 1}  /* GRBM_GC_USER_SA_UNIT_DISABLE */
    };
    ULONG reg[8], i, se, sa, disabled, rb_active, rb_per_sa, wgp, cu, selection;
    BOOL valid = TRUE;

    if (!ad->rmmio || !ad->gc_seg[0] || !ad->gc_seg[1]) return FALSE;
    for (i = 0; i < 8; i++) {
        UQUAD address = (UQUAD)ad->gc_seg[regs[i][1]] + regs[i][0];
        if (address > 0x3fffffffULL || address * 4 + 4 > ad->rmmio_size)
            return FALSE;
        reg[i] = (ULONG)address;
    }
    r->gb_addr_config = amdgpu_rreg32(ad, reg[5]);
    r->selector_before = amdgpu_rreg32(ad, reg[0]);
    if (!r->gb_addr_config || r->gb_addr_config == 0xffffffffUL ||
        r->selector_before == 0xffffffffUL) return FALSE;
    disabled = amdgpu_rreg32(ad, reg[6]) | amdgpu_rreg32(ad, reg[7]);
    r->active_sa_mask = render_mask(r->num_shader_engines * r->shader_arrays_per_engine) &
                        ~((disabled >> 8) & 0xffff);
    disabled = amdgpu_rreg32(ad, reg[3]) | amdgpu_rreg32(ad, reg[4]);
    rb_active = render_mask(r->num_shader_engines * r->rb_pipes_per_engine) &
                ~((disabled >> 4) & 0xf);
    rb_per_sa = r->rb_pipes_per_engine / r->shader_arrays_per_engine;
    for (i = 0; i < r->num_shader_engines * r->shader_arrays_per_engine; i++)
        if (r->active_sa_mask & (1UL << i))
            r->enabled_rb_mask |= render_mask(rb_per_sa) << (i * rb_per_sa);
    r->enabled_rb_mask &= rb_active;

    for (se = 0; se < r->num_shader_engines && valid; se++) {
        for (sa = 0; sa < r->shader_arrays_per_engine; sa++) {
            if (!(r->active_sa_mask & (1UL << (se * r->shader_arrays_per_engine + sa))))
                continue;
            selection = 0x40000000UL | (se << 16) | (sa << 8);
            amdgpu_wreg32(ad, reg[0], selection);
            if (amdgpu_rreg32(ad, reg[0]) != selection) { valid = FALSE; break; }
            disabled = amdgpu_rreg32(ad, reg[1]) | amdgpu_rreg32(ad, reg[2]);
            wgp = render_mask(r->max_cu_per_array / 2) & ~(disabled >> 16);
            cu = 0;
            for (i = 0; i < 16; i++) if (wgp & (1UL << i)) cu |= 3UL << (2 * i);
            r->cu_bitmap[se % 4][sa + (se / 4) * 2] = cu;
            r->active_cu_count += render_count(cu);
        }
    }
    /* All paths after the first selector write restore it, even failed reads. */
    amdgpu_wreg32(ad, reg[0], r->selector_before);
    r->selector_after = amdgpu_rreg32(ad, reg[0]);
    return valid && r->selector_before == r->selector_after &&
           r->active_sa_mask && r->enabled_rb_mask && r->active_cu_count;
}

void amdgpu_render_info_init(struct AmdgpuDevice *ad)
{
    struct AmdgpuRenderInfo r = {0};
    ULONG i;
    if (!ad || ad->deviceid != 0x7590 || !ad->ip.gc.valid ||
        ad->ip.gc.major != 12 || ad->ip.gc.minor != 0 || ad->ip.gc.revision != 0)
        return;
    asic_info_init(ad);
    for (i = 0; i < RENDER_CACHE_SLOTS; i++) {
        if (render_cache[i].ad == ad) return;
        if (!render_cache[i].ad) break;
    }
    if (i == RENDER_CACHE_SLOTS) return;
    if (!render_discovery(ad, &r)) {
        bug("[amdgpu-query] render-info-v1 REFUSED invalid/unsupported GC discovery\n");
        return;
    }
    if (!render_harvest(ad, &r)) {
        bug("[amdgpu-query] render-info-v1 REFUSED harvest/address configuration "
            "GC=%lu.%lu addr=%08lx selector=%08lx/%08lx SA=%lx RB=%lx CU=%lu\n",
            (unsigned long)r.gc_table_major, (unsigned long)r.gc_table_minor,
            (unsigned long)r.gb_addr_config, (unsigned long)r.selector_before,
            (unsigned long)r.selector_after, (unsigned long)r.active_sa_mask,
            (unsigned long)r.enabled_rb_mask, (unsigned long)r.active_cu_count);
        return;
    }
    r.abi_version = AMDGPU_RENDER_INFO_ABI_VERSION;
    r.flags |= AMDGPU_RENDER_INFO_READY;
    render_cache[i].info = r;
    render_cache[i].ad = ad;
    bug("[amdgpu-query] render-info-v1 READY GC=%lu.%lu SE=%lu SA/SE=%lu "
        "CU/SA=%lu activeCU=%lu RB=%lx addr=%08lx selector=%08lx/%08lx\n",
        (unsigned long)r.gc_table_major, (unsigned long)r.gc_table_minor,
        (unsigned long)r.num_shader_engines, (unsigned long)r.shader_arrays_per_engine,
        (unsigned long)r.max_cu_per_array, (unsigned long)r.active_cu_count,
        (unsigned long)r.enabled_rb_mask, (unsigned long)r.gb_addr_config,
        (unsigned long)r.selector_before, (unsigned long)r.selector_after);
}

LONG amdgpu_query_render_info(struct AmdgpuDevice *ad,
                              struct AmdgpuRenderInfo *out, ULONG out_size)
{
    ULONG i;
    if (!out || out_size < sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!ad) return -ENODEV;
    for (i = 0; i < RENDER_CACHE_SLOTS; i++) {
        if (render_cache[i].ad == ad) {
            *out = render_cache[i].info;
            return 0;
        }
    }
    return -ENODEV;
}

/* --- enumeration: how a client obtains an opaque device handle ---
   Devices are discovered once at init (amdgpu_pci_probe) and held on
   amdgpuBase.devices in probe order, so index is stable for the life of the
   driver. Returns NULL past the end rather than faulting. */
ULONG amdgpu_get_device_count(void)
{
    return amdgpuBase.device_count;
}

struct AmdgpuDevice *amdgpu_get_device(ULONG index)
{
    struct AmdgpuDevice *ad;
    ULONG i = 0;

    ForeachNode(&amdgpuBase.devices, ad) {
        if (i == index)
            return ad;
        i++;
    }
    return NULL;
}

const char *amdgpu_get_marketing_name(struct AmdgpuDevice *ad)
{
    if (ad != NULL && ad->name != NULL)
        return ad->name;
    return "AMD GPU (unknown)";
}

/* Pick the discovery-map slot for a queryable HWIP block. NULL if the block
   isn't one we resolve, so callers report "absent" rather than a bogus 0.0.0. */
static const struct AmdgpuIpBase *ip_slot(struct AmdgpuDevice *ad,
                                          enum AmdgpuIpType ip)
{
    switch (ip) {
    case AMDGPU_IP_GC:    return &ad->ip.gc;
    case AMDGPU_IP_SDMA:  return &ad->ip.sdma0;
    case AMDGPU_IP_MMHUB: return &ad->ip.mmhub;
    case AMDGPU_IP_PSP:   return &ad->ip.psp;
    case AMDGPU_IP_SMU:   return &ad->ip.smu;
    case AMDGPU_IP_DMU:   return &ad->ip.dmu;
    case AMDGPU_IP_NBIO:  return &ad->ip.nbio;
    default:              return NULL;
    }
}

BOOL amdgpu_query_hw_ip_version(struct AmdgpuDevice *ad, enum AmdgpuIpType ip,
                                UBYTE *major, UBYTE *minor, UBYTE *revision)
{
    const struct AmdgpuIpBase *slot;

    if (ad == NULL)
        return FALSE;
    slot = ip_slot(ad, ip);
    if (slot == NULL || !slot->valid)
        return FALSE;

    if (major)    *major    = slot->major;
    if (minor)    *minor    = slot->minor;
    if (revision) *revision = slot->revision;
    return TRUE;
}

void amdgpu_query_gpu_info(struct AmdgpuDevice *ad, struct AmdgpuGpuInfo *out)
{
    if (ad == NULL || out == NULL)
        return;

    out->asic_id        = ad->deviceid;
    out->pci_rev        = ad->revision;
    out->gfx_level      = ad->gfx_level;
    out->marketing_name = amdgpu_get_marketing_name(ad);

    out->vram_mb        = ad->vram_mb;
    out->vram_size      = (UQUAD)ad->vram_mb << 20;
    out->vram_bar_size  = (UQUAD)ad->vram_bar_size;

    /* IP versions straight from the discovery map (0.0.0 when absent). */
    out->gc_major    = ad->ip.gc.valid    ? ad->ip.gc.major    : 0;
    out->gc_minor    = ad->ip.gc.valid    ? ad->ip.gc.minor    : 0;
    out->gc_rev      = ad->ip.gc.valid    ? ad->ip.gc.revision : 0;
    out->sdma_major  = ad->ip.sdma0.valid ? ad->ip.sdma0.major : 0;
    out->sdma_minor  = ad->ip.sdma0.valid ? ad->ip.sdma0.minor : 0;
    out->mmhub_major = ad->ip.mmhub.valid ? ad->ip.mmhub.major : 0;
    out->mmhub_minor = ad->ip.mmhub.valid ? ad->ip.mmhub.minor : 0;
    out->psp_major   = ad->ip.psp.valid   ? ad->ip.psp.major   : 0;
    out->psp_minor   = ad->ip.psp.valid   ? ad->ip.psp.minor   : 0;
    out->smu_major   = ad->ip.smu.valid   ? ad->ip.smu.major   : 0;
    out->smu_minor   = ad->ip.smu.valid   ? ad->ip.smu.minor   : 0;
    out->dmu_major   = ad->ip.dmu.valid   ? ad->ip.dmu.major   : 0;
    out->dmu_minor   = ad->ip.dmu.valid   ? ad->ip.dmu.minor   : 0;

    D(bug("[amdgpu] query_gpu_info: %s asic=%04x rev=%02x gfx_level=%u "
          "VRAM=%uMB GC=%u.%u.%u SDMA=%u.%u\n",
          out->marketing_name, out->asic_id, out->pci_rev, out->gfx_level,
          (unsigned)out->vram_mb, out->gc_major, out->gc_minor, out->gc_rev,
          out->sdma_major, out->sdma_minor));
}

/* Heap size in MB. VRAM = discovered visible VRAM; GTT (system-memory heap) is
   not sized until the memory tier stands up, so report 0 (honest "unknown")
   rather than guess. */
ULONG amdgpu_query_heap_mb(struct AmdgpuDevice *ad, BOOL vram)
{
    if (ad == NULL)
        return 0;
    return vram ? ad->vram_mb : 0;
}

/* Bounded raw MMIO register read (libdrm read_mm_registers). dword_offset and
   count are in 32-bit register units; amdgpu_rreg32 clamps against the mapped
   aperture, so an out-of-range tail reads back 0. */
BOOL amdgpu_read_mm_registers(struct AmdgpuDevice *ad, ULONG dword_offset,
                              ULONG count, ULONG *out)
{
    ULONG i;

    if (ad == NULL || out == NULL || ad->rmmio == NULL)
        return FALSE;

    for (i = 0; i < count; i++)
        out[i] = amdgpu_rreg32(ad, dword_offset + i);
    return TRUE;
}

BOOL amdgpu_query_pci_bus_info(struct AmdgpuDevice *ad, UWORD *domain,
                               UBYTE *bus, UBYTE *dev, UBYTE *func)
{
    IPTR v;

    if (ad == NULL || ad->pciDevice == NULL)
        return FALSE;

    /* AROS PCI is single-domain. */
    if (domain) *domain = 0;

    if (bus) {
        OOP_GetAttr(ad->pciDevice, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_Bus, &v);
        *bus = (UBYTE)v;
    }
    if (dev) {
        OOP_GetAttr(ad->pciDevice, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_Dev, &v);
        *dev = (UBYTE)v;
    }
    if (func) {
        OOP_GetAttr(ad->pciDevice, amdgpuBase.pciDeviceAB + aoHidd_PCIDevice_Sub, &v);
        *func = (UBYTE)v;
    }
    return TRUE;
}

/* --- Foundation device access for the 3D backends (radv / radeonsi) ---
 * amdgpu.hidd already woke the card + mapped its register aperture. Hand the LIVE
 * device out so the backend does its OWN PCI enum and MMIO through neither a
 * re-scan nor mmio.library. */
APTR amdgpu_get_pci_device(struct AmdgpuDevice *ad)
{
    return ad ? (APTR)ad->pciDevice : NULL;
}

APTR amdgpu_get_pci_driver(struct AmdgpuDevice *ad)
{
    return ad ? (APTR)ad->pciDriver : NULL;
}

ULONG amdgpu_reg_read(struct AmdgpuDevice *ad, ULONG dword_offset)
{
    return ad ? amdgpu_rreg32(ad, dword_offset) : 0;
}

void amdgpu_reg_write(struct AmdgpuDevice *ad, ULONG dword_offset, ULONG val)
{
    if (ad)
        amdgpu_wreg32(ad, dword_offset, val);
}

/* The live, already-mapped register aperture (VA + byte size). amdgpu.hidd
 * mapped this when it woke the card; hand it to the 3D backend so radv's winsys
 * uses THIS mapping instead of re-mapping the BAR via mmio.library. */
APTR amdgpu_get_mmio_base(struct AmdgpuDevice *ad)
{
    return ad ? (APTR)ad->rmmio : NULL;
}

ULONG amdgpu_get_mmio_size(struct AmdgpuDevice *ad)
{
    return ad ? ad->rmmio_size : 0;
}

/* --- Compute dispatch for the 3D backend (radv cs_submit) ---
 * Thin wrappers over the gfx-tier compute-queue primitive (amdgpu_gfx.c). The
 * backend passes a raw PM4 stream; amdgpu ensures the queue is up, installs the
 * HQD with the work present and kicks it (doorbell-less MMIO install). Returns
 * the new wptr (dwords) or 0 on failure. Fence helpers expose the queue's fence
 * scratch (CPU pointer + GPU VA) so the caller can build a WRITE_DATA fence and
 * poll it. */
/* Backed by the PROVEN one-shot install (gfx_v12_compute_submit_oneshot), NOT the
 * reusable gfx_v12_compute_submit (g_cq path, 0/all-history). This is what the radv
 * winsys's cs_submit lands on (aros_radv_amdgpu_submit -> here). */
extern ULONG gfx_v12_compute_submit_oneshot(struct AmdgpuDevice *ad, const ULONG *pm4, ULONG ndw);

LONG amdgpu_compute_submit(struct AmdgpuDevice *ad, const ULONG *pm4, ULONG ndw)
{
    if (!ad || !pm4 || !ndw)
        return 0;
    return (LONG)gfx_v12_compute_submit_oneshot(ad, pm4, ndw);
}

APTR amdgpu_compute_fence_cpu(struct AmdgpuDevice *ad)
{
    (void)ad;
    return (APTR)gfx_v12_compute_fence_cpu();
}

APTR amdgpu_compute_fence_va(struct AmdgpuDevice *ad)
{
    (void)ad;
    return (APTR)(IPTR)gfx_v12_compute_fence_va();
}

/* --- radv cs_submit BRIDGE (ac_drm_cs_submit_raw2 -> here) ---
 * radv builds a compute IB in a GART-mapped BO and hands us its VA + dword size;
 * we run it on the compute queue via an INDIRECT_BUFFER on the proven one-shot
 * install (gfx_v12_compute_submit_ib), then the caller polls the completion fence
 * (amdgpu_compute_ib_fence_cpu == IB_DONE when the IB returns). Declared locally:
 * amdgpu_intern.h is under another agent's scope lease. */
extern ULONG gfx_v12_compute_submit_ib(struct AmdgpuDevice *ad, UQUAD ib_va, ULONG ib_ndw);
extern volatile ULONG *gfx_v12_compute_ib_fence_cpu(void);

LONG amdgpu_compute_submit_ib(struct AmdgpuDevice *ad, UQUAD ib_va, ULONG ib_ndw)
{
    if (!ad || !ib_va || !ib_ndw)
        return 0;
    return (LONG)gfx_v12_compute_submit_ib(ad, ib_va, ib_ndw);
}

APTR amdgpu_compute_ib_fence_cpu(struct AmdgpuDevice *ad)
{
    (void)ad;
    return (APTR)gfx_v12_compute_ib_fence_cpu();
}
