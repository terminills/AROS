# Managed BO memory accounting (JOB-0084)

## Cold68: first visible hardware 3D accepted

On 2026-09-09 at 04:34:15 UTC, fresh physical HDMI capture 1048 showed the
RGB triangle rendered by actual Mesa 26 RadeonSI/ACO on the RX 9060 XT.
This meets the user's first-visible-3D checkpoint, not full driver or
application qualification. Installed driver 0.11 was physically rebooted
and passed the map/accounting tests described below before this scene ran.

The 40158096-byte triangle v5 was independently transferred twice, fully
compared natively and SHA-256 verified before execution:
d60b9be7c9490cf575c7ce0652cc02fac90f6c234c69cf6d35d243980e23de66.
The real renderer identified PCI 7590, GFX12, ACO, DRM 0.1 and the native
graphics queue, with no forced family, no-op mode or software fallback.
Its real Gallium draw passed the native private-EOP-backed fence at
00000a74921f3bb2. Completed color-target readback passed at 00000a74bacbf5cd:
FNV32 a5a1fad1, 24520 shaded pixels, center BGRA 128,64,63,255,
corner BGRA 64,0,0,255, stride 1280. The interpolated-color/outside-pixel
checks passed before the native window opened and the VISIBLE marker at
00000a74f69bc5fb. Independent HDMI confirms those GPU-rendered pixels.

GPU-rendered and checked pixels are CPU-copied into the native window by
WritePixelArray. This does not claim zero-copy presentation, full OpenGL or
Vulkan integration, general application readiness, or performance qualification.
The scene is deliberately left open for human verification; teardown and
native process return have not yet been tested in this run.

Evidence set JOB-0084-native-completion-20260906:
1048-cold68-first-hardware-triangle.png/.h264/.json, captured_at
2026-09-09T04:34:15.014Z; cold68-triangle-v5-first-pixels.uart.log SHA-256
a06a8fa9dd198594a333a4996dce1a44c0b74f9a199f1bf4cea99fbadb9da5be.
Preflight raw cold68-triangle-v5-preflight.uart.log SHA-256
5a7d018d564fd45b1aa1f7ea699b3541edfe3182972f7f88d396d504e52880a6.
Both are bounded copies of the uninterrupted original physical UART log.
ACD registered the first-pixels evidence immediately; the user was notified.

## Map-driven0.11: native allocation accepted before the first 3D replay

The user rejected continued quota doubling on the16GiB DUT. The undeployed
0.10 candidate a7dd9b1d10 is superseded, not accepted. Version0.11 now retains
the existing GMC window and PSP primary/extended reservation readings in an
immutable map. It validates ATOM firmware/protected/training regions, excludes
the legacy256..771MiB driver firmware/ring workspace, runtime DB/discovery and
the entire original64MiB display arena plus preceding framebuffer/cursor/GOP
storage. The usable range is bounded by the minimum of NBIO memory size,
GMC window and CPU BAR, not inferred from a16GiB BAR alone.

VRAM BOs select free aligned ranges top-down, excluding every retained BO and
reservation. Each BO alone is CPU-mapped; no eager16GiB BAR map is made. Its
aligned payload is the exact VRAM charge; GTT still owns AllocMem padding.
Aggregate counters are64-bit. The8MiB per-object bring-up limit is replaced
by the actual usable range and checked ULONG map-length bound; GPU binding
additionally respects the existing512MiB GART address space.64 live handles
and the current non-reusing VA policy remain explicit limitations, not hidden
by a claim of unlimited allocation. Abandoned or unretired objects stay owned.

New appended vector47, amdgpu_query_memory_info2, reports separate VRAM and
GTT capacities/usage. GTT availability also respects remaining GART space and
free system memory. Query1 remains a compatibility aggregate accounting bound;
Mesa on0.11 must use query2, never advertise that sum independently as each
physical heap. The Mesa shim preserves its old-driver path for prior versions.

No display mode, cursor, ownership, GUI, kernel or filesystem logic changes.
The new resident-only MemoryMapProbe requires an installed/rebooted0.11,
checks detected usable VRAM above4GiB on this DUT,40MiB of simultaneous2MiB
aligned allocations, distinct page patterns, retained-owner accounting and
cleanup. Allocation traces report actual upper-VRAM offsets. These are native
memory checks only; first3D still requires actual draw/readback/HDMI evidence.
Updated MemoryInfoProbe retains old padding expectations for old drivers and
checks exact aligned VRAM charges on0.11.

Source3bae7b6231 and Mesa shimc1b72b80d9 built successfully. Actual
DEVS:Drivers/amdgpu.hidd559312B compared in full and hashed natively to
76fb0d1b165cefe0fced3a8f117d02ce43c97bc9c0027d46f98900e5caf42b18.
The prior0.9 driver remains a verified rollback backup. Physical OFF12000,
more than8seconds fully off, then ON800 produced cold68; no private HIDD.

The booted map validated total=visible17095983104 bytes, subtracting three
merged exclusions to expose15929966592 bytes of usable VRAM. Separate GTT
capacity502267904 bytes. ME/PFP32/32 passed. Resident and opened library
bases both0x10698f100, revision0.11, actual negative size384 bytes (47vectors
plus library alignment). MemoryMapProbe retained twenty2MiB BOs at actual
offsets0x3daa00000 through0x3d8400000, verified distinct data at every4KiB
page,40MiB exact charge, owner-release retention and return to baseline.
END0/native0 at000003d79e45ef42/000003d7c401dd36. MemoryInfoProbe passed
all snapshots/domain/bounds/retention/release checks, END0/native0 at
000003dbbe4fe17c/000003dbe4e227a5. Both native returns error0/ioerr0.
Evidence cold68-native-map-probes-pass.uart.log SHA256
3a13739dd22e955eeb3b3b2e70b0986d70916f8d9fdc5650a11b8278452dfa8c,
in JOB-0084-native-completion-20260906, with map-driven-v1-provenance.md.

This accepts real post-reboot allocation/accounting above4GiB, not shader
rendering or a full16GiB stress test. The relinked real RadeonSI/ACO triangle
v5 is being independently staged/verified for actual GPU draw and readback.
First visible hardware3D remains the user's next checkpoint.

## Cold66 draw/readback capacity failure;0.10 candidate

The integrity-accepted Mesa26 v4 scene on physical cold66 creates the real
RX9060XT RadeonSI/ACO screen, context, inline framebuffer, VS/FS state and
vertex input, then dispatches its draw. The driver's private-EOP fence retires
ctx0x104/sequence1 and releases submission references. This is not proof of
rendered pixels: readback fails and no scene END or return has been observed.
Four VRAM and four GTT allocation retries report ret=-12 with identical
budget33554432,used31488507,available2065925; VRAM backing27261440 plus
GTT backing4227067,11handles,0released,8maps,7bindings. The requested2MiB,
2MiB-aligned BO needs4194048 VRAM or4194303 GTT backing bytes. The quota gate
therefore rejects it before the underlying allocator, including during draw.
Raw cold66-triangle-v4-fence-readback-fail.uart.log [1041329482,1048436254),
SHA2563c5ffaf0283ffdfcdcd9170e9791a1db0941c32c4730e3085c14df2340b0749f;
frame1027 independently shows the desktop without a triangle.

Library0.10 increases the shared aperture pool from64 to128MiB and derives
the managed VRAM/GTT quota as half that pool (64MiB). This keeps at least
half the pool outside managed-BO ownership even if every BO is VRAM; it is
not a guarantee of free or contiguous display storage. Limits remain64slots,
8MiB per allocation,2MiB alignment, with unchanged padding accounting,
permission/retention rules and ABI. Native1080p pool [17,145)MiB remains below
the first fixed firmware reservation at256MiB; the bounded4096x2160 layout
also ends below197MiB for the existing firmware-offset-zero handoff. No new
support for arbitrary firmware framebuffer placements is claimed.
No display ownership, modes, cursor, GUI, kernel or filesystem logic changes.

This is an unaccepted source candidate until focused build, real installed
byte/SHA verification, physical reboot, resident0.10/capacity verification,
retained native driver tests and the unchanged v4 draw/readback/HDMI replay.
The cold66 cleanup stall remains separate and must not be called resolved.

## Historical0.9 quota change

2026-09-08 cold60 actual RadeonSI v3 confirms the original16MiB quota blocks
the first color BO before drawing: native allocation ret=-12, query0,
budget16777216, used14712315, available2064901. VRAM backing10485248 plus
GTT backing4227067 exactly equals usage;7 live handles, none owner-released.
The requested2097152-byte/2097152-aligned VRAM BO owns4194048 backing bytes,
which cannot fit the remaining allowance. All four upstream retries report
the same snapshot. Raw cold60-triangle-v3-quota-diagnosis.uart.log
[930588441,937305567), SHA256
4b51bfde809cba24fd9c45b6b6703e09c81bc5b1400d8d4f424eb8e586229eb4.

Library0.9 candidate raises the shared charged-backing budget to32MiB, keeping
max8MiB allocation, max2MiB alignment,64 handles, padding accounting and all
retention/permission rules unchanged. Thus at most half of the existing64MiB
VRAM pool can be consumed by managed BOs, even if every allocation is VRAM.
There are no separate additive VRAM/GTT quotas and no promise of contiguous
capacity. No display ownership, scanout, pointer, GUI or filesystem change.
The snapshot ABI and vector44 remain unchanged and report the actual limit.
Source bae5fa9f4e built as546816B driver, SHA256
72badc728f5c4a2918cbdd89cdf27c3f83d2294cd2a300a27d97fd4d0e415beb.
After interrupted deployment, corrected native Copy and independent
FileCompare-v3 verified all546816 bytes against the complete RAM reference
before physical cold62 reboot. Rejected input/transfer attempts are retained
in bo-quota32-v1-provenance.md, not hidden by the successful comparison.

Cold62 resident==opened0x1069e6f50/revision0.9/368 negative bytes verified.
Actual quota33554432 with used0/available33554432 and0/64 handles; all memory
snapshot, padding, retained-borrow and cleanup checks PASS. Actual native
return0/error0/ioerr0 at000003aaf3a5a059;32/32 ME/PFP boot checks also PASS.
Raw cold62-installed-quota32-memory-acceptance.uart.log
[946272939,953580659), SHA256
b6cf804e9b3217f6c2f354daac23f7496bc66ac4ec461adddf46648db044f368.
This accepts the installed/rebooted memory accounting change only. Actual
RadeonSI draw/fence/readback/physical presentation remain pending; no visible3D.

Historical0.4 acceptance and its original16MiB policy follow.

Source0a41b6c778/library0.4 base query accepted on physical cold53. Vector44
appends without changing earlier vectors or structures. Driver DEVS/RAM543152
compared identical with native0 at000009977913055f before longOFF/35+s dwell/
shortON. Retained32/32 ME/PFP, both SDMA host directions and ASICflagsf passed.
Client Work/RAM67152 compared identical with native0 at000003c1fa7091f7.
Resident/opened bases both1069a7a10, revision0.4 and352 negative bytes verified.
All snapshot/bounds/alignment/domain/owner-release/borrow/cleanup assertions
passed. END0 at000003ff060606a4; actual native0/error0/ioerr0 at
000003ff5d2275c1. Frozen cold53-memory-info-client-pass.uart.log SHA256
52408e259ca5386a23d37edc3d7a279c81914fc76a52b1ca8a09b2c22f432b76.
Mesa v6 memory translation also passed actual native0 at00000492b777386b,
including pending real-GPU backing charges and exact retirement cleanup.
Raw cold53-ac-drm-memory-pass.uart.log SHA256
3ae6afc724aa979b1f4130e3a7a54af27d0c31876c469c26fb4844c8ba5e2c07.
Retained CS/VA/GTT/copy/drawing tests passed actual native0. Drawing18PASS0FAIL
returned at0000077fc7473d37; three copy holds visually verified. Raw
cold53-retained-cs-va-gtt-copy-drawing.uart.log SHA256
d8b05495f37dbe0edf46b208cc8e7a4a00f4efebf9f8e77a84154c3b0f3545f7.

Sequential focused driver/probe compile and link exit0, no diagnostics.
Frozen amdgpu-memory-info-v1.hidd543152B SHA256
21fee1ca9577ace6dfc0a0931902de1388f00f9c58034a84efbe37ca00995bae;
AmdgpuMemoryInfoProbe-v1 67152B SHA256
2cd67fd8065f22dcef01c4910eb936c0154ff3d53a80ce9f5b59dbc5f8f7b8e8.
These frozen artifacts are the byte-compared cold53 base acceptance pair.

One read-only snapshot holds the existing BO registry semaphore and reports
the shared16MiB charged-backing quota, available quota, each domain's payload
and backing, retained/released handles, reusable slots, CPU borrows and VA
bindings. Backing retained after owner release remains counted. Per-domain
usage sums to shared usage; disagreement with the allocator's counter fails
with EIO and an empty output. No allocation, pool policy or MMIO changes.

Quota is not physical VRAM/RAM capacity and is not a promise of contiguous
storage, VA space or a successful future allocation. VRAM and GTT do not have
separate additive16MiB budgets. The desktop's shared pool is outside this
registry; these figures describe the managed BO service only. GPU references
retain VA bindings, which retain backing; a client cannot mark GPU retirement.

AmdgpuMemoryInfoProbe requires resident0.4/vector44 before calling this API.
It checks ABI bounds and256 idle snapshots, exact page/alignment accounting
across VRAM/GTT, owner-free with borrowed mappings, domain-specific final
release and restoration of the baseline, plus rejected allocation immutability.
It links no private driver and submits no GPU commands. Acceptance still
requires commit/build/freeze, installed byte comparison, physical reboot,
resident-only execution and retained native driver regression tests.

Mesa MEMORY/heap translation440d9a71c2 is accepted as frozen v6 on
cold53. Full renderer enablement remains subsequent work. First
visible hardware-rendered3D remains the user's checkpoint.
