# AMDGPU dynamic GART mapping guards

## map-guards-v1 — cold43 native bring-up and drawing/copy PASS

Source38e9fb9408 plus923d3e1920 built505216B, SHA256
f56c1f31a9226888e02554e129aceacdf00b389bfe4ba4a4ebed4714cf5f97ae.
Cold42 Work rollback502240B and installed candidate505216B each passed native
byte comparisons; candidate native0 at00000ae46c12e304. Physical OFF12000
completed16:27:34UTC and ON800 at16:28:09UTC, virtual media empty.

Cold43 loaded map-guards-v1 at00000152f32f14d4. Compute returned600dc0de with
EOP600df00d/WRITE600dbeef at000001632a0c161d; ME/PFP32unique pairs passed at
00000165dd4efdba. VRAM->SYSMEM and SYSMEM->VRAM each had0/256 bad dwords.
Dynamic unaligned HOSTBUF mapping2048B/offset0x358 returned0/512 bad dwords
at000001672652da6c; its fault status is0. GFXHUB invalidations ACK after wake;
SDMA initialization completed0000016760c78468. All11 logged dynamic mappings
have correct offset/page counts, disjoint monotonically increasing ranges,
and aperture bounds. This is a serial boot sample, not SMP/failure injection.

Frozen cold43-gart-native-bringup-pass.uart.log [661131460,664833358),3701898B,
SHA2565083c2e8a8750c27276b8f64b08cd607ff26965bedfe34c3c1705c9effc40bf8.
Full build/install/reboot evidence: registered JOB-0084-native-completion-20260906/
gart-map-guards-v1-provenance.md. After OVR0046/0047 renewals, cleared known
SFS requesters, closed completed startup Shell and retried screen reset;
AMDGPU1920x1080 at000004bbc9e8d9bd. Unchanged copy client40848B compared
IDENTICAL/native0, two512x256guardedcopies and four131072-pixel bad0 checks,
fresh490..492correct, native0/error0/ioerr0 at000006c848943a79.
Drawing18PASS/0FAIL including4task DMA and fresh650correct; actual native0/
error0/ioerr0 at00000721a65c967a. Not Mesa rendering, VA reuse, general GTT/VA,
submission retirement or full2D acceptance. Cursor failure remains deferred.

JOB-0084, 2026-09-07. Driver-local prerequisite for the Mesa GTT/VA path.
No public ABI, GUI, pointer, filesystem or shared-header changes.

Dynamic mapping previously wrote PTEs before advancing an unlocked bump cursor.
Concurrent callers could reserve the same range, partial DMA-resolution failures
left valid PTEs behind, and unmap accepted arbitrary unchecked VA/size ranges.
The service now initializes its semaphore/list before the serial device ladder,
serializes reservations/PTE edits/invalidations, checks wide geometry and CPU
range arithmetic, and validates the physical span/offset/address width for each
owned page fragment. Each successful mapping has an exact device/VA/size record;
unmap can only clear that record's bounded entries once.

Failed partial maps clear/read back/invalidate written entries and quarantine
the reserved VA range. Successful unmaps do not reclaim VA either. Fresh cold42
boot logs show GFXHUB ACKs after wake, correcting an overgeneralized historical
comment; this does NOT establish safe VA reuse or per-submission retirement.
The existing bounded hub-invalidation implementation is otherwise unchanged.

Current native x86 CachePreDMA is coherent/identity; these guards do not add
portable DMA pin/unpin, IOMMU mappings, reset recovery or task-death cleanup.
Callers still own backing storage and must retire GPU accesses before unmap/free.
Live mappings consume small records and monotonic aperture space; the service
does not expose general GTT/VA to clients yet. Module initialization is serial;
normal task-context dynamic callers use the semaphore, never interrupt context.

Required acceptance: focused driver build and immutable hash, preserve cold42
rollback, native installed-byte compare, physical OFF/dwell/ON, loaded marker,
retained ME/compute checks and native GART-backed SDMA upload/download with
known pixels. Build-only or RAM-loaded private driver probes are not acceptance.
Concurrency/failure-path observations and full Mesa submission remain separate
open gates; native success on the existing callers must not be overstated.

First focused build rejected an unavailable NewList declaration. Corrected to
the existing AROS NEWLIST macro used by this driver's PCI list initialization;
failed build-gart-map-guards-v1.log retained. No failed candidate was deployed.
