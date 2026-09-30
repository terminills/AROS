# Native managed graphics submission foundation

Cold56 update (2026-09-08): source7746773c47 driver0.8 installed, independently
compared546816bytes and physically power-cycled. Residentbase1069bcd00;
32/32ME/PFP initialization retained. Unchanged CSv2 passed264writes/ringreuse
native0. Extendedv1 passed64refs/65536DWORDIBx3/189GPUwrites,2MiBalignment,
owner-drop/EOP/cleanup/native0 at0000057c83adc267. RetainedVRAMv1 fourdomain
pairs,GTTv2,VAv3 andMemoryv1 allnative0. Adapterv10 fourVRAM/GTTpairs and
65536DWIBs alsoactualnative0 at000008b6a3c30fe8. Separate frozenraw logs in
JOB-0084-native-completion-20260906 preserve each result. Retained CopyProbe
passed both actual SDMA guard completions and4x131072pixels bad0; fresh
physical captures490/491/492 were viewed. Drawing passed18/18 including
DMA4x12 concurrency, capture836 viewed, actualnative0/error0 at
0000149bee9c97c3. Both KVM ExecuteCommand launches left residualIoErr209,
not0; their actual command return is0. Raw cold56-copy-retained.uart.log
SHA256 d04ab49377bb2d3d536f26a5d9ca869c20b57ee864cc918250edad0186df53ea
and cold56-drawing-and-compare-v3.uart.log
SHA256 bf1f7ef638075b18e4e5661ec73f11de10270a64304993eb63d5b50c53e15be7.
No actual RadeonSI render/visible3D claim.
The historical uninstalled statement below describes the pre-cold56 state.

Cold55 full retained regression acceptance of installed0.7 now complete:
memory/GTT returnednative0, copy4x131072bad0 with both cursor-guard completions,
threefreshholds visuallychecked, drawing18PASS0FAIL andnative0 at00000d060eb66bf3.
Raw cold55-retained-memory-gtt-copy-drawing.uart.log SHA256
e9cec89939c2a48d2efd32f6575ffb043fcd91c14d2baaa27bb0fc6863a00a86;
nativeexit follows in cold55-drawing-native-exit.uart.log SHA256
2fa68882e778c914e56f2c3238589b393acbb2e15f99b1e7ef6b74adb7b4b91b.
The split exists because an incorrectly encoded KVM SPACE packet did not
arrive; corrected KVM.press sent the proper report. Not a GPU failure.
Candidate0.8 fullcompile/linkexit0 with no diagnostics, frozen
amdgpu-cs-extended-v1.hidd546816B SHA256
3369d9828f8634465fb2dcaebd7a5340982709d943f950e07115b402266daff8.
Extendedclient65808B SHA256
535a657efad929c9f51994efc44ef337b96e1a7df947e063653493a9889f29e1.
Still uninstalled/unaccepted; preserve0.7 rollback before physicalcold56.

Candidate0.8 adds NEW vectors45(query_cs_caps2) and46(cs_submit2) for
64resources and65536-DWORD IBs. The original64-byte v1 request, eight-entry
array, vectors, ABI1 limits and1024-DWORD check stay unchanged. V2 is a fixed
288-byte ABI2 request with an explicit exact-size argument. Both routes use
the same context generations, one queue, retained resource references and
privateEOP/guard/ring-idle retirement. This is trusted PM4, not isolation.
The ring still stores only its12-DWORD IB/EOP packet; longer external IBs do
not enlarge or overwrite ring storage. BO/VA alignment increases to2MiB for
the actual4MiB GFX12 attribute ring, with exact padding ownership and existing
16MiB quota/8MiB payload limits untouched. No desktop pool expansion.
Resident extended client tests v1 rejection boundaries, v2 size/version/count/
duplicate/reserved limits,2MiB CPU+GPU alignment,64retained resources and
three65536-DWORD realIBs writing63distinct destinations each. Final round
drops allocation owners before real retirement; guards and exact accounting
must recover. Per-element successes are not debug-logged; failures identify
index/round and successful rounds have a bounded summary. Build/native tests
pending; driver must be installed, independently byte-compared and rebooted.

Cold55 ring acceptance: byte-verified DEVS0.7 installed, physical OFF/ON,
resident unchangedCSv2 passed8lifetime+256reuse GPUwrites. WPTR1026/2058/3078
crossed three boundaries with split2/10/6; exactEOP64, payloads, guards and
cleanup all passed. Actualnative0/error0/ioerr0 at000005cc27ac66a8. Raw
cold55-ring-reuse-v3-pass.uart.log SHA256
9ab85ab3d422d515994635b4590ced3198d9e38c6c5cb273ba19b4e54dd7eb9d.
This fixes cold54's first-wrap false-pending result. VRAM/domain tests also
passed below; retained Mesa/2D regressions remain pending. No visible3D yet.

Candidate0.7 corrects ring-position retirement after a read-only snapshot of
the still-live cold54 failure: CP_RB0_RPTR=2, WPTR=1026, CP_STAT=0,
RB/HQD_CNTL=00f00789, HQD_ACTIVE=1, selector0. Snapshot native0 at
000008545216851a. A32-bit read-pointer type does not imply a monotonic MMIO
counter; the previous equality check was wrong. The correction requires the
MMIO ring position, writeback position modulo1024, unchanged fullWPTR/high0,
valid originalHQD control and idle, PLUS the exact private64-bit EOP token and
guards before any ref release. Position equality alone never signals a fence.
One pending64 diagnostic per slow submission records each operand without a
per-poll log flood. Existing256-write native client remains unchanged.
This build also contains the separately committed0.6 VRAM mapping, which was
never deployed: cold55 must accept ring reuse first, then the new domain-pair
and VA-v2 clients, then retained memory/GTT/copy/drawing. Mesa's GTT-only
capability checks require a separately tested adapter update before VRAM is
advertised there. No0.7 acceptance until byte-verified install and reboot.

403068fd46 actual full driver compilation/module link exit0, no diagnostics.
Frozen amdgpu-ring-reuse-v3.hidd546072B SHA256
f630b420ca220210cf46ee3f38e162fa1807051766057a124ac854693705017e.
build-ring-reuse-v3-driver.log preserves the build; no resident0.7 test yet.

Cold54 library0.5 b729d199d9 was byte-verified, installed and physically rebooted.
Its ring-wrap acceptance FAILED: eight lifetime rounds and49 reuse writes
retired, then token58/WPTR1026/slot1014/split2 crossed the first storage boundary.
The GPU wrote90610032 with intact guards, but completion remainedfalse for250
polls. Actual native return20/error0/ioerr0 at00000686a2f15fc0. References stay
quarantined. The outer cleanupPASS does not supersede nested quarantineFAIL.
Raw cold54-ring-reuse-v2-fail.uart.log SHA256
d7d30797bfdbbfe0aa15ba6bbe121490cd62394e08d5f4c818338c8f16096008.
The0.6 VRAM candidate inherits this path and remains UNINSTALLED. Read-only
register snapshot9ec71acfa2 will distinguish pointer semantics from a stalled
packet before a correction is selected. No physical success inferred from
Linux's pointer width or from payload alone. Accepted0.4 rollback is preserved
as Work:JOB-0084-rollback-cold53-memory-net.hidd.

Library0.5 original ring-storage reuse candidate design (failure above):
after actual EOP/guard/RPTR/writeback/idle retirement, the same serialized
graphics queue can overwrite retired slots in its1024-dword ring. Producer
WPTR stays monotonic; only the storage index is masked. A twelve-DWORD IB/EOP
packet crossing the page boundary is copied and DMA-synchronized as two
bounded spans; it never writes into RPTR storage at+4096. No queue reset,
remapping, new engine or concurrent producer is introduced. Stop before
32-bit producer rollover; queried boot capacity is(0xffffffff-330)/12 rather
than57. CS ABI1/vector layouts and single-inflight ownership are unchanged.
Reference: Linux gfx_v12_0_ring_get_rptr_gfx/get_wptr_gfx/set_wptr_gfx and
amdgpu_ring_write_multiple; GFX12 support_64bit_ptrs=true,32-bit RPTR, only
storage indices use buf_mask. Native acceptance must corroborate this on DUT.

AmdgpuCsProbe v2 requires resident0.5 and capacity>=264 before submission,
retains the eight owner-release/generation/pressure rounds, then performs256
unique second-page writes using two stable owned bindings. Every round checks
actual base retirement, payload guards and released GPU refs before CPU reuse.
Three rounds also reject overwrite/unbind/context destruction while pending.
This must cross at least three ring boundaries with matching publish split/
epoch and private EOP/RPTR/writeback logs. No render or3D claim from this test.
Frozen v1 client remains separate for the current cold53/library0.4 memory
acceptance cycle; candidate0.5 requires its own commit/build/byte-verified
install/physical reboot and retained regression tests.

Sequential focused driver and probe compile/link exited0 with no diagnostics.
Frozen amdgpu-ring-reuse-v2.hidd543616B SHA256
720ea9e230b24b5fa1ad374adaf8618bd1bd9731eff382338e76ac1ed3be62b3;
AmdgpuCsProbe-v2-ring-reuse80816B SHA256
c58d4a9fe9f0da1c3cd7526040c6b61a94a04cc28f23bd02fac7b30b21c68203.
Logs build-ring-reuse-v2-driver.log and build-ring-reuse-v2-probe.log preserve
actual compilation/linkage. Build lock released dirty; foreign edits untouched.

JOB-0084, managed-gfx-v1: cold47 bounded native CS acceptance PASS.

User acceptance checkpoint: visible hardware-rendered 3D on the DUT (Mesa
demos are in scope). Plumbing and regression passes are intermediate evidence,
not a checkpoint or stopping point. Commits remain recovery/bisect records.

Cold47 first client run refused before GPU submission: its extent check
incorrectly required 41 pointer slots although generated CS vectors end at
base[-40]. Corrected to 40 slots with actual resident/opened bases and negative
size logged. Failure returned20 with cleanupPASS at UART0000082c19065066.
This was a client preflight defect, not evidence of a GPU submission failure.
The installed afbb0809f0 driver was unchanged for the corrected client retest.

Corrected client681ffa04eb passed on physical cold47: resident/opened both
0x1069b2220, negative bytes320/required320. Eight distinct public second-page
GTT writes (90600001..90600008) matched; all private EOP64 tokens and guards
matched, hardware RPTR/writeback/WPTR advanced342..426 with idle0. Owner-free
BOs stayed pinned until retirement, premature unbind/context free were refused,
and stale context generations and capacity were checked. END result0 cleanup
PASS at000009a14e6943d2; actual native return0/error0/ioerr0 at000009a174b662f4.
Frozen raw evidence: cold47-cs-managed-pass.uart.log, SHA256
f74701ae790e845db77ef90f5fc597d48480f7beeb948991f7dc21b01656e667,
in JOB-0084-native-completion-20260906. Retained regressions are being rerun;
Mesa connection and visible hardware-rendered3D remain the active next work.

First focused build of e9947f5578 failed: the new CS module dereferenced HWData
without including its defining gfx_support header. Fixed explicitly; no driver
was deployed from the failed build. The failed log is retained as
build-managed-gfx-v1-driver.log in the native-completion evidence set.

This builds on cold46's accepted BO/VA lifetimes and retained ME/PFP queue.
Five appended vectors expose capability query, generation-tagged context
creation/free, single-IB submission and fence query. Existing vectors retain
their indices. This is NOT routed or advertised to Mesa yet.

The service pins every declared GTT VA binding before publication and checks
the IB's offset/extent against its actual binding. Duplicate/missing resource
and invalid requests are rejected before publication. CPU DMA synchronization
uses owned page-sized intervals. Clients must declare all referenced resources
and stop changing IB/data until retirement: this is a trusted shared-VMID0
contract, NOT PM4 validation, cross-client isolation or hostile-input support.
Do not mix legacy raw PM4/MMIO clients with this service.

The existing graphics queue is published to the service only after the retained
32 unique ME/PFP pairs pass. Commands append after those packets in the same
ring. No reset, remap or wrap: at most 57 submissions per boot, one in flight,
16 context records and 8 resource bindings per submit. Context sequences are
independent monotonic counters; the private hardware fence has a globally
unique token. A context generation cannot query another generation's fence.

The GPU executes INDIRECT_BUFFER then RELEASE_MEM (GL2 writeback, EOP event,
64-bit sequence) into a private guarded slot distinct from MQD and CSIB. Fence
query requires matching token, unchanged guards, hardware RPTR and writeback
at the submitted pointer, and idle. Only then does the driver release GPU
references. Timeout/error/client death retains BOs, VAs and context; no reset
recovery is offered. Unpublished submission failure unwinds references. A
failed reference release poisons later submissions and keeps affected storage.

Reference packet semantics: Linux gfx_v12_0.c ring_emit_ib_gfx/ring_emit_fence,
nvd.h and soc24_enum.h. Graphics IB has no compute-only VALID bit. The old
compute bridge was inspected but not modified or used for this first service.

Acceptance must be installed-byte comparison -> physical reboot -> fresh
managed-gfx-v1 resident -> AmdgpuCsProbe (8 unique second-page GTT writes,
private hardware EOP plus owner-release/unbind/context/fence lifetime checks),
then retained VA/GTT/copy/drawing clients. A compile or CPU-only check cannot
establish hardware acceptance. Full Mesa rendering, VRAM VA, ring wrap,
scheduler fairness, task-death reclamation, reset recovery and general SMP
remain unfinished. No pointer, GUI, filesystem or foreign dirty file edits.
