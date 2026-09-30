# amdgpu.hidd ↔ Mesa seam: `ac_drm_*` wiring contract

Design authority: `.acd/decisions/ACD-amdgpu-mesa-vendoring-layout.md` (+ parent
`ACD-amdgpu-driver-stack-split.md`). This file is the *implementation blueprint*
for wiring the one bridge between pristine Mesa (fetched as an AROS port) and
`amdgpu.hidd` (the sole device owner).

## The rule

The ONLY path from Mesa into AROS is the `ac_drm_*` / libdrm_amdgpu ABI,
implemented in `workbench/libs/mesa/libamd_common/ac_aros_drm.c`, routed into
`amdgpu.hidd` public LVOs. Upstream RADV/radeonsi winsyses are written against
`ac_drm_*` already; make it real and the port's own drivers drive our device.
Today every `ac_aros_drm.c` entry returns `-ENODEV` and the old
`hidds/radeonsi` monolith bypasses it — that monolith retires once this is live.

## Mapping: `ac_drm_*`  →  `amdgpu.hidd` LVO

| `ac_drm_*` entry            | Backing amdgpu LVO                          | State |
|-----------------------------|---------------------------------------------|-------|
| `ac_drm_device_initialize`  | `amdgpu_get_device_count` + `amdgpu_get_device` | exists |
| device/heap/IP queries      | `amdgpu_query_gpu_info` / `amdgpu_query_hw_ip_version` / `amdgpu_query_heap_mb` | exists |
| MMIO base/size              | `amdgpu_get_mmio_base` / `amdgpu_get_mmio_size` | exists |
| `ac_drm_cs_submit` (PM4)    | `amdgpu_compute_submit(ad, pm4, ndw)` → mapped MES queue | exists |
| `ac_drm_cs_query_fence_status` | `amdgpu_compute_fence_cpu` / `amdgpu_compute_fence_va` | exists |
| `ac_drm_bo_alloc`           | `amdgpu_bo_alloc(ad, size, align, domain, flags) → handle` | **GAP** |
| `ac_drm_bo_va_op` / `_raw`  | `amdgpu_bo_va_map(ad, handle, offset, size, va, flags, op)` | **GAP** |
| `ac_drm_bo_cpu_map` / unmap | `amdgpu_bo_cpu_addr(ad, handle)` (GART/VRAM CPU view) | **GAP** |
| `ac_drm_cs_ctx_create2` / free | single implicit ctx first; per-ctx later    | stub-ok |
| syncobj create/wait/signal  | map onto the compute fence first; real syncobjs later | stub-ok |

## Shortest path to "GPU ran our shader"

`amdgpu_compute_submit` already takes raw PM4 to the mapped compute queue, so a
FIRST compute dispatch needs no BO/VA plumbing: allocate a shader + args in an
already-mapped GART region, build a PM4 `DISPATCH_DIRECT` IB, submit, poll
`amdgpu_compute_fence_*`. This is the milestone gate before wiring the full
winsys.

## Migration steps (each committable)

1. **Seam, submit half** — implement `ac_aros_drm.c` `device_initialize`, the
   query set, `cs_submit` (→ `amdgpu_compute_submit`), and
   `cs_query_fence_status` (→ `amdgpu_compute_fence_*`). Prove with a hand-built
   PM4 compute dispatch. *(= first RADV-adjacent dispatch.)*
2. **Container gaps** — add the two GAP LVOs to `amdgpu.conf`:
   `amdgpu_bo_alloc` and `amdgpu_bo_va_map` (+ `amdgpu_bo_cpu_addr`), backed by
   the existing GART/GPUVM (`amdgpu_gart_map`/`_unmap`, GPUVM allocator). Then
   fill `ac_drm_bo_*` / `ac_drm_bo_va_op`.
3. **Flip the winsys** — `radv_amdgpu_winsys_public.h`
   `radv_amdgpu_winsys_create` returns the real upstream `radv_amdgpu_winsys`
   (port `src/amd/vulkan/winsys/amdgpu/`) instead of the failure stub.
4. **Retire the monolith** — delete `hidds/radeonsi` `aros_radv_winsys.c` +
   `radeonsi_vk_entry_stubs.c` + the audit's pure-dup files.

## Constraints

- Keep the amdgpu LVO surface minimal and shaped to `ac_drm_*` — it is the
  stable contract that lets the Mesa port and the container version apart.
- Never edit a port source file for AROS reasons; adaptation lives in
  `aros_compat` / `lib*` recipes / this seam.
- `ac_drm_cs_submit` boot-variance: the mapped-queue ACK is ~1/6 today
  ([[radv-vulkan-bringup-ladder]]); the seam must surface submit failure cleanly
  (return `-EIO`, let the winsys/caller retry) rather than hang.
