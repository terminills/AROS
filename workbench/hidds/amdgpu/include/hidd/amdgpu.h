/*
 * amdgpu.hidd — PUBLIC Device/Query and bounded BO storage ABI.
 *
 * This is the driver side of the seam described in ABI-RFC.md: the subset of
 * libdrm_amdgpu's device/query surface that Mesa's ac_drm_* layer
 * (libamd_common/ac_aros_drm.c) consumes. Clients open amdgpu.hidd, enumerate
 * devices, and call these entry points; the device pointer is OPAQUE to them —
 * the full struct stays private to the driver (amdgpu_intern.h).
 *
 * Deliberately free of libdrm/Mesa ABI structs: results come back in
 * driver-owned shapes and the seam translates them into amdgpu_gpu_info /
 * drm_amdgpu_info_hw_ip / radeon_info on the Mesa side, where those headers
 * actually live. That keeps this header (and the driver) compilable standalone.
 *
 * Device/Query remains observe-only. The separately advertised BO tier below
 * allocates driver-owned storage; it does not imply GPU submission support.
 */
#ifndef HIDD_AMDGPU_H
#define HIDD_AMDGPU_H

#include <exec/types.h>

/* Opaque to clients; defined in amdgpu_intern.h. */
struct AmdgpuDevice;

/* HWIP blocks with a stable queryable meaning (subset of the discovery map). */
enum AmdgpuIpType {
    AMDGPU_IP_GC = 0,   /* graphics/compute */
    AMDGPU_IP_SDMA,
    AMDGPU_IP_MMHUB,
    AMDGPU_IP_PSP,
    AMDGPU_IP_SMU,
    AMDGPU_IP_DMU,      /* display */
    AMDGPU_IP_NBIO
};

/* Device snapshot, filled from probe-time identification + IP discovery. */
struct AmdgpuGpuInfo {
    UWORD   asic_id;            /* PCI device id */
    UBYTE   pci_rev;            /* PCI revision id */
    UBYTE   gfx_level;          /* enum amdgpu_gfx_level */
    const char *marketing_name;
    ULONG   vram_mb;            /* visible VRAM in MB (0 = not yet read) */
    UQUAD   vram_size;          /* bytes */
    UQUAD   vram_bar_size;      /* VRAM aperture bytes */
    /* IP versions (0.0.0 when the block was absent from discovery) */
    UBYTE   gc_major,    gc_minor,    gc_rev;
    UBYTE   sdma_major,  sdma_minor;
    UBYTE   mmhub_major, mmhub_minor;
    UBYTE   psp_major,   psp_minor;
    UBYTE   smu_major,   smu_minor;
    UBYTE   dmu_major,   dmu_minor;
};

/* --- enumeration: how a client gets a device handle --- */
ULONG amdgpu_get_device_count(void);
struct AmdgpuDevice *amdgpu_get_device(ULONG index);

/* --- Device/Query --- */
const char *amdgpu_get_marketing_name(struct AmdgpuDevice *ad);
void  amdgpu_query_gpu_info(struct AmdgpuDevice *ad, struct AmdgpuGpuInfo *out);
BOOL  amdgpu_query_hw_ip_version(struct AmdgpuDevice *ad, enum AmdgpuIpType ip,
                                 UBYTE *major, UBYTE *minor, UBYTE *revision);
ULONG amdgpu_query_heap_mb(struct AmdgpuDevice *ad, BOOL vram);
BOOL  amdgpu_read_mm_registers(struct AmdgpuDevice *ad, ULONG dword_offset,
                               ULONG count, ULONG *out);
BOOL  amdgpu_query_pci_bus_info(struct AmdgpuDevice *ad, UWORD *domain,
                                UBYTE *bus, UBYTE *dev, UBYTE *func);

/* --- Foundation device access for the 3D backends (radv / radeonsi) ---
 * amdgpu.hidd already woke the card, identified it, and mapped its register
 * aperture. The 3D backend must NOT re-discover or re-map — it takes the LIVE
 * device from here (the pseudo-library gets handed this). PCI objects are typed
 * APTR (= OOP_Object *) to keep this header free of oop.h; MMIO read/write go
 * through amdgpu's mapped aperture (amdgpu_rreg32/wreg32), so the backend needs
 * neither its own PCI enum nor mmio.library. */
APTR  amdgpu_get_pci_device(struct AmdgpuDevice *ad);   /* OOP_Object * PCIDevice */
APTR  amdgpu_get_pci_driver(struct AmdgpuDevice *ad);   /* OOP_Object * PCIDriver */
ULONG amdgpu_reg_read(struct AmdgpuDevice *ad, ULONG dword_offset);
void  amdgpu_reg_write(struct AmdgpuDevice *ad, ULONG dword_offset, ULONG val);
APTR  amdgpu_get_mmio_base(struct AmdgpuDevice *ad);    /* mapped register aperture VA */
ULONG amdgpu_get_mmio_size(struct AmdgpuDevice *ad);    /* aperture size in bytes */

/* Compute dispatch — the 3D backend hands a raw PM4 stream to amdgpu's MEC
 * compute queue (the reusable cs_submit path). amdgpu_compute_submit installs the
 * HQD with the PM4 present and kicks it, returning the new wptr (dwords) or 0 on
 * failure. fence_cpu/fence_va expose the queue's fence scratch (a CPU-readable
 * pointer and its GPU VA, both APTR-width) so the caller can target a WRITE_DATA
 * fence in its PM4 and poll completion. */
LONG  amdgpu_compute_submit(struct AmdgpuDevice *ad, const ULONG *pm4, ULONG ndw);
APTR  amdgpu_compute_fence_cpu(struct AmdgpuDevice *ad);
APTR  amdgpu_compute_fence_va(struct AmdgpuDevice *ad);

/* BO storage ABI v1. Native single-address-domain only: handles are checked
 * index+generation records, NOT transferable cross-domain capabilities/fds.
 * Driver owns backing storage; every cpu_map borrows one reference and MUST
 * be paired with cpu_unmap, never FreeVec/free. bo_free drops the owner ref,
 * disallows new maps/queries and defers reclamation until all maps are gone.
 * No automatic client-death reclamation yet: abandoned refs are retained.
 * No scanout ownership or import/export service is advertised here. Optional
 * VA/submission and the bounded 0.12 presentation copy are documented below.
 * Return0 on success, negative errno on failure; outputs are cleared on failure
 * when their pointer and (for structs) size are valid. Existing LVOs unchanged.
 */
#define AMDGPU_BO_ABI_VERSION 1
#define AMDGPU_BO_DOMAIN_VRAM 1UL
#define AMDGPU_BO_DOMAIN_GTT  2UL
#define AMDGPU_BO_CAP_CPU_MAP 1UL

struct AmdgpuBoCaps {
    ULONG abi_version, domains, features, page_size;
    ULONG max_handles, reserved;
    UQUAD max_allocation, budget, max_alignment; /* budget includes alignment padding */
};

struct AmdgpuBoInfo {
    UQUAD size, alignment;
    ULONG domain, map_count;
};

/* Memory accounting ABI1 (library0.4): one locked snapshot of the managed BO
 * registry, including backing retained after owner release. Before0.11 the
 * bound is a shared bring-up quota;0.11 reports the sum of independently
 * enforced VRAM/GTT bounds for compatibility. It is NOT the size of either
 * physical heap. Use query2 for separate domain capacities and availability.
 * This aggregate is not a promise of contiguous storage, free VA, or handles.
 */
#define AMDGPU_MEMORY_INFO_ABI_VERSION 1
struct AmdgpuMemoryInfo {
    ULONG abi_version, domains, page_size, max_handles;
    ULONG live_handles, released_handles, reusable_handles, reserved;
    UQUAD shared_budget, shared_used, shared_available, max_allocation;
    UQUAD vram_backing, gtt_backing, vram_payload, gtt_payload;
    UQUAD cpu_borrows, va_bindings;
};
LONG amdgpu_query_memory_info(struct AmdgpuDevice *ad, struct AmdgpuMemoryInfo *out,
                             ULONG out_size);

/* Library0.11: separate physical-map-backed VRAM and GART-limited GTT heaps.
 * All capacity/usage values are64-bit. total_vram is the usable hardware
 * range, visible_vram additionally respects the CPU BAR, and vram_budget
 * excludes firmware plus the entire independent display arena. Old query1
 * remains an aggregate accounting bound, not two additive physical heaps.
 * available is not a contiguous-allocation or future-success guarantee.
 * GART remaining includes its existing monotonic/no-reuse VA policy. */
#define AMDGPU_MEMORY_INFO2_ABI_VERSION 2
struct AmdgpuMemoryInfo2 {
    ULONG abi_version, domains, page_size, max_handles;
    ULONG live_handles, released_handles, reusable_handles, reserved;
    UQUAD total_vram, visible_vram, reserved_vram;
    UQUAD vram_budget, vram_used, vram_available;
    UQUAD gtt_budget, gtt_used, gtt_available, gart_remaining;
    UQUAD max_allocation, cpu_borrows, va_bindings;
};
LONG amdgpu_query_memory_info2(struct AmdgpuDevice *ad, struct AmdgpuMemoryInfo2 *out,
                              ULONG out_size);

/* Live GPU sensors from the SMU metrics table (SMU14 SmuMetrics_t). Read by
 * the driver under its own mailbox and VRAM-aperture locks, so tools must use
 * this instead of touching the MP1 mailbox. Clocks in MHz (cur = instantaneous,
 * avg = PMFW average), power in watts, temperatures in degrees C, activity in
 * percent. -ENODEV without an SMU, -EIO if the PMFW did not deliver a table. */
#define AMDGPU_SENSOR_INFO_ABI_VERSION 1
struct AmdgpuSensorInfo {
    ULONG abi_version, sample_counter;
    ULONG gfxclk_cur, socclk_cur, uclk_cur, fclk_cur;
    ULONG gfxclk_avg, uclk_avg, fclk_avg;
    ULONG gfx_activity, mem_activity;
    ULONG socket_power, board_power;
    ULONG temp_edge, temp_hotspot, temp_mem;
    ULONG fan_pwm, fan_rpm;
};
LONG amdgpu_query_sensors(struct AmdgpuDevice *ad, struct AmdgpuSensorInfo *out,
                          ULONG out_size);
/* Turn GFX clock gating (CGCG/CGLS/MGCG/FGCG, Linux gfx_v12_0 set) on or off
 * at run time. On by default after boot. 0 on success, -ENODEV, or -EIO if
 * the RLC did not enter safe mode (nothing changed). Since 0.14. */
LONG amdgpu_set_gfx_clock_gating(struct AmdgpuDevice *ad, ULONG enable);
/* Queue an IB on the VCN unified ring (VMID 0, trusted shared VM). The IB
 * and every buffer it references must stay bound until the fence completes.
 * *fence receives the sequence to pass to amdgpu_vcn_query_fence. 0 on
 * success, -EINVAL, or -ENODEV when VCN is not running. Since 0.15. */
LONG amdgpu_vcn_submit_ib(struct AmdgpuDevice *ad, UQUAD ib_va, ULONG ib_ndw, ULONG *fence);
/* *complete = 1 once the VCN has signalled fence. Since 0.15. */
LONG amdgpu_vcn_query_fence(struct AmdgpuDevice *ad, ULONG fence, ULONG *complete);

LONG amdgpu_query_bo_caps(struct AmdgpuDevice *ad, struct AmdgpuBoCaps *out,
                         ULONG out_size);
/* size rounds up to page_size; alignment0 means page_size, otherwise a power
 * of two, capped by max_alignment. Choose one advertised domain, never a mask.
 * GTT is driver-owned system RAM, privately GART mapped; CPU/backing alignment
 * is not a public GPU-VA alignment promise. Only flags0 is supported. No client
 * GPU access is enabled yet: VA/submission retirement is a separate service.
 * GART VA is monotonic until coherency/reuse acceptance; GTT allocation may
 * exhaust that aperture even after prior BOs are freed. On0.11 GTT and VRAM
 * have independent bounds; GTT includes owned alignment padding while VRAM
 * directly places an aligned range. */
LONG amdgpu_bo_alloc(struct AmdgpuDevice *ad, UQUAD size, UQUAD alignment,
                    ULONG domain, ULONG flags, ULONG *handle);
LONG amdgpu_bo_free(struct AmdgpuDevice *ad, ULONG handle);
LONG amdgpu_bo_query_info(struct AmdgpuDevice *ad, ULONG handle,
                         struct AmdgpuBoInfo *out, ULONG out_size);
LONG amdgpu_bo_cpu_map(struct AmdgpuDevice *ad, ULONG handle, APTR *cpu);
LONG amdgpu_bo_cpu_unmap(struct AmdgpuDevice *ad, ULONG handle);

/* Native presentation ABI v1 (library 0.12/vector 48). Copy an uncompressed
 * linear rectangle from a live public BO into an address in amdgpu.hidd's
 * display-bitmap VRAM arena. The caller supplies byte geometry and is
 * responsible for matching pixel formats; the driver validates both complete
 * ranges, retains the source across the synchronous SDMA operation, and never
 * exposes MC addresses. Source offset is relative to the BO payload. The
 * destination pointer must be the stable CPU address returned for an owned
 * display bitmap. Eligible 32-bit rectangles use one boot-verified SUB_WIND
 * packet; other layouts use bounded scanline packets. Return 0 on completed
 * fence, negative errno on rejection/failure. This is a bounded copy service,
 * not scanout ownership or an import/export ABI. */
#define AMDGPU_PRESENT_ABI_VERSION 1
LONG amdgpu_bo_present_rect(struct AmdgpuDevice *ad, ULONG bo_handle,
                            UQUAD src_offset, ULONG src_stride,
                            APTR dst_cpu, ULONG dst_stride,
                            ULONG row_bytes, ULONG rows);

/* VA binding ABI v1: a separate, optional service. Query domains; whole BO at
 * offset0, one immutable binding per range. Size is page-rounded, alignment0
 * means4KiB; requested0 chooses an address. Nonzero requests must be aligned
 * and in unused aperture space. Ranges are NEVER reused/rebound this boot.
 * A binding owns a BO reference, independently of CPU maps and owner free.
 * Unbind requires all driver-owned GPU references retired, exact PTE clearing
 * and both-hub invalidation acknowledgment; failure retains the binding.
 * Range free refuses a bound range. Abandoned references remain pinned.
 *
 * This is NOT a managed submission or scanout capability. Legacy raw-PM4
 * entry points do not register resource lifetimes and MUST NOT be used with
 * these BOs. The separate optional managed CS service acquires private GPU refs.
 * CPU/PTE acceptance alone does not prove GPU access, VA reuse or rendering.
 */
#define AMDGPU_VA_ABI_VERSION 1
#define AMDGPU_VA_STATE_RESERVED 0
#define AMDGPU_VA_STATE_BOUND 1
#define AMDGPU_VA_STATE_RETIRED 2
#define AMDGPU_VA_STATE_QUARANTINED 3
struct AmdgpuVaCaps {
    ULONG abi_version, domains, max_handles, page_size;
    UQUAD max_size, max_alignment, address_limit;
};
struct AmdgpuVaInfo {
    UQUAD address, size, alignment;
    ULONG state, bo_handle, gpu_refs, reserved;
};
LONG amdgpu_query_va_caps(struct AmdgpuDevice *ad, struct AmdgpuVaCaps *out, ULONG out_size);
LONG amdgpu_va_range_alloc(struct AmdgpuDevice *ad, UQUAD size, UQUAD alignment,
                          UQUAD requested, ULONG *handle, UQUAD *address);
LONG amdgpu_va_range_free(struct AmdgpuDevice *ad, ULONG handle);
LONG amdgpu_va_query_info(struct AmdgpuDevice *ad, ULONG handle,
                         struct AmdgpuVaInfo *out, ULONG out_size);
LONG amdgpu_bo_va_bind(struct AmdgpuDevice *ad, ULONG bo_handle, ULONG va_handle);
LONG amdgpu_bo_va_unbind(struct AmdgpuDevice *ad, ULONG va_handle);

/* Bounded native CS foundation, matching the BO/VA/context/fence ownership of
 * ac_drm. Trusted shared-VMID0 clients ONLY: no per-client VM or PM4 validation.
 * Clients list EVERY referenced binding, keep IB/data immutable until retirement,
 * and must not mix legacy raw submission/MMIO calls with this service. This is
 * not full Mesa rendering. One outstanding submit; library0.5 reuses retired
 * ring storage without producer-counter rollover. Respect queried limits.
 * Owner free is allowed while in flight; unbind/context free refuse it. Only
 * query_fence observing the driver's GPU fence can retire resource references.
 * A timeout, client death or query error never implies retirement.
 */
#define AMDGPU_CS_ABI_VERSION 1
#define AMDGPU_CS_TRUSTED_SHARED_VM 1
#define AMDGPU_CS_MAX_RESOURCES 8
struct AmdgpuCsCaps {
    ULONG abi_version, flags, max_contexts, max_resources;
    ULONG max_ib_dwords, max_inflight, max_submits_per_boot, reserved;
};
struct AmdgpuCsRequest {
    ULONG ib_handle, ib_offset, ib_dwords, resource_count;
    ULONG resources[AMDGPU_CS_MAX_RESOURCES];
    ULONG flags, reserved[3];
};
LONG amdgpu_query_cs_caps(struct AmdgpuDevice *ad, struct AmdgpuCsCaps *out, ULONG out_size);
LONG amdgpu_cs_ctx_create(struct AmdgpuDevice *ad, ULONG flags, ULONG *context);
LONG amdgpu_cs_ctx_free(struct AmdgpuDevice *ad, ULONG context);
LONG amdgpu_cs_submit(struct AmdgpuDevice *ad, ULONG context,
                      const struct AmdgpuCsRequest *request, UQUAD *sequence);
LONG amdgpu_cs_query_fence(struct AmdgpuDevice *ad, ULONG context, UQUAD sequence,
                           ULONG *complete);

/* Extended trusted-client submission, introduced by library0.8/vector45-46.
 * The v1 structures, vectors and queried limits above remain unchanged.
 * Size is exact for this version. Every referenced VA remains listed/retained;
 * this adds capacity, not PM4 validation, VM isolation or another queue.
 */
#define AMDGPU_CS_EXTENDED_ABI_VERSION 2
#define AMDGPU_CS_EXTENDED_MAX_RESOURCES 64
#define AMDGPU_CS_EXTENDED_MAX_IB_DWORDS 65536UL
struct AmdgpuCsRequest2 {
    ULONG abi_version, flags;
    ULONG ib_handle, ib_offset, ib_dwords, resource_count;
    ULONG resources[AMDGPU_CS_EXTENDED_MAX_RESOURCES];
    ULONG reserved[2];
};
LONG amdgpu_query_cs_caps2(struct AmdgpuDevice *ad, struct AmdgpuCsCaps *out, ULONG out_size);
LONG amdgpu_cs_submit2(struct AmdgpuDevice *ad, ULONG context,
                       const struct AmdgpuCsRequest2 *request, ULONG request_size,
                       UQUAD *sequence);

/* Immutable render topology, captured at driver initialization. This does not
 * advertise a working renderer. Only successful, validated discovery AND
 * GC12 harvest reads set READY. Queries never change a register selector.
 * Sizes/cache quantities retain the AMD GC discovery table's units; no PCI-ID
 * guessed topology, firmware version, clock or heap capacity is returned.
 * CU bitmap uses Linux's stable 4x4 packing: [se % 4][sa + (se / 4) * 2].
 * out_size must cover this version; a short buffer is left untouched.
 */
#define AMDGPU_RENDER_INFO_ABI_VERSION 1
#define AMDGPU_RENDER_INFO_READY 1
#define AMDGPU_RENDER_INFO_CACHE_V12 2
#define AMDGPU_RENDER_INFO_CACHE_V13 4
struct AmdgpuRenderInfo {
    ULONG abi_version, flags, gc_table_major, gc_table_minor;
    ULONG num_shader_engines, shader_arrays_per_engine, max_cu_per_array;
    ULONG rb_pipes_per_engine, num_tcc_blocks, num_gprs, max_gs_threads;
    ULONG gs_table_depth, gsprim_buffer_depth, parameter_cache_depth;
    ULONG double_offchip_lds_buffer, wavefront_size, max_waves_per_simd;
    ULONG max_scratch_slots_per_cu, lds_size, num_sc_per_array, num_packer_per_sc;
    ULONG num_gl2a, tcp_per_array, sdp_interfaces, num_tcps;
    ULONG tcp_per_wgp, tcp_l1_size, sqc_per_wgp, instruction_cache_size;
    ULONG scalar_cache_size, gl1c_per_array, gl1c_size, gl2c_per_gpu;
    ULONG tcp_size_per_cu, tcp_line_size, instruction_line_size;
    ULONG scalar_line_size, tcc_size, tcc_line_size;
    ULONG gb_addr_config, active_sa_mask, enabled_rb_mask, active_cu_count;
    ULONG cu_bitmap[4][4];
    ULONG selector_before, selector_after;
};
LONG amdgpu_query_render_info(struct AmdgpuDevice *ad,
                              struct AmdgpuRenderInfo *out, ULONG out_size);

/* Immutable provenance of the CP images actually staged during this boot.
 * STAGED means every required instruction/stack command completed its PSP
 * transport. It does NOT assert PSP authentication or a working compute queue:
 * raw PSP responses are retained separately, and managed engine readiness is
 * queried through the CS API. No runtime firmware-file reread occurs.
 */
#define AMDGPU_FIRMWARE_INFO_ABI_VERSION 1
#define AMDGPU_FIRMWARE_PFP 0
#define AMDGPU_FIRMWARE_ME 1
#define AMDGPU_FIRMWARE_MEC 2
#define AMDGPU_FIRMWARE_TYPES 3
#define AMDGPU_FIRMWARE_HEADER_VALID 1
#define AMDGPU_FIRMWARE_STAGED 2
struct AmdgpuFirmwareInfo {
    ULONG abi_version, type, flags, version, feature;
    ULONG source_bytes, header_bytes, header_major, header_minor;
    ULONG ip_major, ip_minor, source_crc32;
    ULONG instruction_bytes, data_bytes, required_stages, completed_stages;
    ULONG psp_status[3]; /* instruction, P0 stack, optional MEC P1 stack */
};
LONG amdgpu_query_firmware_info(struct AmdgpuDevice *ad, ULONG type,
                                struct AmdgpuFirmwareInfo *out, ULONG out_size);

/* Boot-time hardware identity and raw ATOM table fields for Mesa translation.
 * Clock values are in10kHz units. Boot clocks are NOT maximum operating clocks.
 * Each optional field group has its own validity bit; absent tables are never
 * replaced by PCI-ID/spec-sheet guesses. Runtime queries only copy the cache.
 */
#define AMDGPU_ASIC_INFO_ABI_VERSION 1
#define AMDGPU_ASIC_REVISION_VALID 1
#define AMDGPU_ASIC_UMC_VALID 2
#define AMDGPU_ASIC_TIMER_VALID 4
#define AMDGPU_ASIC_BOOT_CLOCKS_VALID 8
struct AmdgpuAsicInfo {
    ULONG abi_version, flags, strap_register_dw, strap_value;
    ULONG chip_rev, external_rev, pci_rev;
    ULONG rom_bytes, rom_header_version, master_table_version;
    ULONG umc_table_version, gfx_table_version, firmware_table_version;
    ULONG atom_vram_type, memory_channels, memory_channel_width_log2, memory_bus_width;
    ULONG gpu_counter_refclk_10khz, memory_refclk_10khz;
    ULONG boot_engine_clock_10khz, boot_memory_clock_10khz;
};
LONG amdgpu_query_asic_info(struct AmdgpuDevice *ad,
                            struct AmdgpuAsicInfo *out, ULONG out_size);

#endif /* HIDD_AMDGPU_H */
