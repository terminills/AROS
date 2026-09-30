# Native Mesa integration checkpoint — 2026-09-07

## Current storage work (bounded native lifetime PASS)

The first bounded driver BO storage tier is implemented in amdgpu_bo.c and
described in BO-STORAGE-STATUS.md: driver-owned paired VRAM backing, checked
generation handles and borrowed CPU-map retirement. Six vectors are appended;
existing ones do not move. The native public client links no driver/private
initialization objects. This does not expose GPU VA, submission or scanout.
Sourcebecb861f80 passed installed-byte verification, physical cold41 boot and
the corrected public-clientaec01e6df3 lifetime gates (native0 at000005ee8e5010db).
The initial v1 client width-oracle failure and corrective evidence are retained
in BO-STORAGE-STATUS.md. Drawing18/0, guarded SDMA copy, interior bitmap VRAM
reuse and seven-stage software-pointer baseline rechecks also pass with actual
native exits and fresh HDMI evidence. No GPU execution or SMP BO soak is claimed. The
source-wiring table below is the earlier audit, not the current BO API inventory.

User permits deferring the invisible hardware cursor while progressing toward
3D. Preserve the working software cursor and 1080p desktop. This does not mark
the separate UpdateRect, timing-modeset or recovery gates complete.

## Confirmed versus not demonstrated

Cold38, installed/rebooted cursor-v10, completed 32/32 unique guarded ME/PFP
writes and the 600dc0de compute-shader sentinel. These are bounded driver-side
tests, not Mesa rendering, a reusable concurrent submission API or a soak test.
Cold40 physically booted the independent interior VRAM coalescing repair
4f07513c32 and passed its unchanged native reuse regression (8b1f1d27d3).
That repair does not change any Mesa/compute interface.

Native public client9161035242 passed on cold40 without a driver reload or
private driver objects. The installed client was byte-compared IDENTICAL44568,
native0/error0/ioerr0 at000004bfbd3a8046, before execution. It opened only the
already-resident noexpunge module, checked the same library base, and queried
one RX9060XT/Navi44 (7590:c0), PCI0000:0c:00.0, GC12.0.0, VRAM16304MiB and
16GiB BAR aperture. Bounds and repeated cached identity/size checks passed;
actual native0/error0/ioerr0 at000004e74dae83e9. This proves the exercised
public Device/Query vectors, not every query Mesa requires, BO services or
rendering. The existing Mesa adapter remains unwired.

## Current source wiring (read at 4f07513c32)

| Layer | Actual source state | Required next gate |
| --- | --- | --- |
| Public driver service | amdgpu.conf exports device/query and compute-submit/fence entries; no BO/VA registry or graphics-submit entry | Native clients must use one device owner; add bounded, versioned services without reinitializing the card |
| Mesa DRM adapter | libamd_common/ac_aros_drm.c returns -ENODEV/-1/NULL; its mmakefile currently leaves AMD_COMMON_AROS_SOURCES empty | Compile and wire the existing adapter, preserving the side-effect-free VirtIO rejection |
| RADV winsys selection | libradv/mmakefile.src still compiles the legacy hidds/radeonsi/aros_radv_winsys; compatibility radv_amdgpu_winsys_create returns initialization failure | Connect the upstream winsys to the real adapter; do not add another winsys or duplicate bring-up |
| Legacy submit reference | aros_radv_cs_submit delegates to compute but ignores its result, IP type and waits/signals; inactive legacy ring reports success without submitting | Reference only; none of these success returns establish GPU execution or synchronization |
| Driver mapping substrate | amdgpu_gart_map advances gart_next_page; unmap clears PTEs but does not reclaim the VA interval; no map/unmap serialization in these functions | Checked allocation/mapping, lifetime, reclaim and completion must precede exposing arbitrary Mesa BO traffic |
| RDNA4 OpenGL | Existing split decision selects RADV plus Zink for GFX10.3+; libzink recipe exists | Vulkan execution/presentation first, then actual GL rendering through Zink; do not extend the legacy RadeonSI generation lane |

The public compute entry currently calls gfx_v12_compute_submit_oneshot,
limited to 1024 dwords. It installs a compute queue and reports submitted
dwords, not a retired fence. The separate indirect-buffer path returns a
submission size and exposes one shared magic-value completion location.
Neither is a ready general concurrent graphics/compute submission contract.
Do not translate a positive write pointer into VK_SUCCESS/completion without
the required ordering, per-submission retirement and error propagation.

## Existing design remains authoritative

Follow ABI-RFC.md's settled July23 contracts and the two ACD split/vendoring
decisions: driver-owned storage, paired alloc/free, borrowed CPU mappings,
index+generation handles, separate BO/fence/scanout references, checked VA and
alignment, and no fd-flavored sharing invented for the single-domain gate.
Mesa metadata retains its own allocator family. No edits to pristine port
sources, no parallel Mesa tree, and no premature deletion of the legacy
reference implementation.

Next implementation sequence: establish truthful public device/query service;
provide checked BO/VA/lifetime services; provide serialized per-submission
execution and retirement (including the graphics ring); connect the existing
Mesa adapter and upstream winsys; prove offscreen output, presentation and
Zink/GL on the physical DUT. Each driver binary requires installed-byte
verification, a physical reboot and preservation of the 2D baseline.

Keep all raw failures and partial results in JOB-0084. An enumeration string,
compile, accepted submission or the old "radv path LIVE" log is not a rendered
triangle. The current native evidence is in ME_DISPATCH_STATUS.md and the
JOB-0084-native-completion-20260906 registered artifact set.
