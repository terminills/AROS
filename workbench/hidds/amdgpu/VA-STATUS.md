# Native AMDGPU VA binding foundation

## Cold46 installed/native bringup and VA client pass

Source83ca36b82e; driver514704B SHA256
620584d20fac4186c68d9651bb5060d41ba4c880a9f9abdc15809a8f43c40b35.
Resident-only client75456B SHA256
70d7dd1aeb0e2162fc89eb93c2972a466c40729a4a67b9609404554aac4c2019.
Separate focused builds returned0; old506240B driver and independent Work
rollback compared native0, installed514704B candidate compared native0 at
0000474a238c7b7f before physical OFF23:55:49/ON23:56:45UTC September7.
Cold46 fresh va-bind-v1 marker00000169826e6bcf proves the installed code
loaded. Compute sentinel/EOP/write600dc0de/600df00d/600dbeef passed at
0000016405f4d13d,32uniqueME/PFP at00000166b859316e(max50polls), SDMA both
directions0/256 at00000167e5f2731e/00000167ed68e97f and hostbuffer0/512
at0000016800f68a47. These retained boot checks do NOT use the new public VAs.

MJPEG stopped delivering frames before driver replacement while native
NetShell/serial remained responsive. Direct H264 video works: raw clips,
packet timestamps and last-frame decodes retained. Host decode needed a
process-local packaged-library path because /opt/amdgpu libva lacks the
vaMapBuffer2 symbol required by installed FFmpeg; no host config changed.
Fresh682 at00:10:12UTC September8 shows the recurring SFS/DH0 requester
blocking networking/Wanderer reads. User-authorized Cancel resumed serial;
no driver/GUI/FS fix inferred from it. Subsequent VA client results follow.
Full evidence: JOB-0084-native-completion-20260906/va-bind-v1-provenance.md.

The resident-only VA client passed on cold46: aligned/exact reservations,
PTE readback and both-hub acknowledgments, two alias bindings, owner-free and
last-CPU-unmap lifetime retention, stale handles, and the 64-range limit.
END result=0 cleanup=PASS at 000004955c7eaf04; actual native return=0,
error=0, ioerr=0 at 0000049581854b73. Raw UART [693535576,693827190)
is cold46-va-binding-pass.uart.log, SHA256
a3a5530427e211b0d049d350f06ac917982efaaeb5fe4117586ef7c0aa0224f3.
Fresh capture 690 at 2026-09-08T00:21:23.821Z shows the native desktop and
Work icon after completion. Retained GTT passed END0/cleanup PASS and actual
native0 at 00000609ea4428d5. Copy passed four 131072-pixel checks with zero
bad pixels, two 512x256 SDMA guard completions, and native0 at
000006b4152cd0e7. Fresh 490/491/492 cold46 captures show the expected copy
stages and solid green rectangles afterward. Drawing retained 18 PASS/0 FAIL,
including 4x12 concurrent DMA, and native0 at 00000762cc71e36f. Fresh 691
shows white/blue/red blocks and four patterned DMA regions. Its legacy ARGB
pixelarray assertions mask a channel; this is not strict full-channel proof.
This is PTE publication and CPU-observable lifetime evidence, not GPU access
through these public VAs or confirmed GPU-reference retirement.

## Contract

JOB-0084, va-bind-v1. Native VA client and retained regressions passed; not complete
3D or a GPU execution result. Follows ABI-RFC's separate BO/VA/CS ownership.

Six appended vectors provide caps, aligned/exact VA reservation, query, free,
whole-GTT-BO bind and unbind. Existing BO ABI/layout and vector indices remain
unchanged. Ranges use the SAME serialized bump cursor as firmware/bitmap/GTT
private mappings, never overlap them and are never reused this boot. Size
rounding, address bounds and generation handles are checked before mutation.
VRAM mappings, partial BOs, rebinding and managed submission fail closed or
are not advertised; this is a deliberately limited first VA service.

Binding acquires a base-driver BO reference before publishing any PTE.
Owner free and the last CPU unmap cannot destroy a bound BO. New private GPU
references prevent unbind until the future CS path confirms retirement. No
public release method allows a client to claim GPU retirement. These private
GPU-reference hooks are NOT yet connected to submission or hardware-tested.
Legacy raw PM4 calls must not be used with these BOs as a substitute.

Each mapped page is DMA-resolved only within the owned payload, written and
read back exactly from VRAM. Bind/unbind also require both hub ACKs and the
MMHUB semaphore acquisition. Failed/partial publication pins the binding and
BO for a retry of checked unbind. Every attempted VA is single-use, including
failures; ACK alone is not a reusable-translation/coherency proof. Unbind
verifies every zero PTE before dropping the binding ref. Client death may
retain bounded backing/registry space. Repeated allocations can exhaust the
monotonic aperture despite available RAM; no full Mesa workload claim yet.

Native acceptance required: frozen committed source/binaries, installed-byte
comparison, physical reboot, fresh va-bind-v1 marker, resident-only
AmdgpuVaProbe PTE/binding/stale-handle/pressure checks, unchanged GTT storage
and copy/drawing regression clients. A successful client proves PTE
publication and CPU-observable binding lifetime, NOT DMA through the new VA,
GPU-reference retirement, general SMP, VA reuse, scanout or Mesa rendering.

The reference PTE layout and invalidate handshake are in AMD's Linux
gmc_v12_0.c and amdgpu_vm.h. Existing pre-wake legacy callers retain their
bounded behavior; the new binding tier checks the returned status instead
of silently accepting a missing ACK. No pointer, GUI or filesystem edits.
