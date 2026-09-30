# AMDGPU completion gates against current ADT Nouveau

JOB-0084, 2026-09-06. This is a source-contract comparison, not a native pass.

## Revisions and scope

ADT `upstream/master` was fetched and checked against the remote master:
`17981c45e5c2cb27c10498188a9821b66a21eea4`. Read with `git show` in
`/home/terminills/Documents/GitHub/AROS-main-adt-compare`; its working checkout
was not switched. The original comparison used AMDGPU `b5415bf0bb`; the current
cold25 candidate is `9cb35e1015` plus the pre-existing shared driver edits recorded
in JOB-0084 evidence. Those other-owner edits remain unchanged and uncommitted here.

The reference is a working implementation to compare contracts against, not
an infallible specification. Nouveau itself retains disposal FIXME comments.
Identical override lists are neither necessary nor sufficient: AMDGPU derives
its bitmap from ChunkyBM; Nouveau derives directly from Hidd_BitMap.

## Contract matrix

| Contract | ADT Nouveau reference | AMDGPU disposition / acceptance gate |
| --- | --- | --- |
| Gfx/display separation and enumeration | `nouveau.conf`, `nouveau_hiddclass.c`, `nouveau_displayclass.c`: separate gfx/display/bitmap classes, connector modes, preferred nominal size | Same class separation exists. Native monitor identity, published mode IDs and selected driver must be proven after each installed binary change. 1080p evidence alone does not validate every published mode. |
| Actual display mode changes | Connector/CRTC path and `HIDDNouveauSwitchToVideoMode` called by compositor | AMDGPU currently changes plane/recout within the inherited raster; `amdgpu_displayclass.c` explicitly refuses larger modes requiring OTG/PLL retiming. This is a real completeness limit, not a full timing modeset. Preserve working 1080p while completing this. |
| Bitmap format, pitch and lifetime | `nouveau_bitmapclass.c`: format validation, allocated BO pitch, failure cleanup, BO disposal | AMDGPU uses ChunkyBM allocation/format semantics, relocates displayable storage to VRAM, tracks GART/VRAM allocation and retires scanout storage. Gate: exact pitch/format, allocation failure, fragmentation, disposal while scanout changes, and repeated screen open/close on DUT. |
| CPU direct access versus queued GPU work | `nouveau_bitmapclass.c:634`: engine + bitmap locks, MAP_BUFFER; `nouveau_intern.h:283`: map and wait if gpu_dirty; release unlocks | Inherited ChunkyBM direct access returns the buffer; its release is empty. CursorFB adds cursor protection, not a general GPU lifetime contract. AMDGPU's synchronous SDMA fences must cover every producer, CPU reader, disposal and timeout path. Review these explicitly before claiming SMP/async safety; inheritance alone does not supply Nouveau's BO wait. |
| Accelerated CopyBox and fallbacks | `nouveau_hiddclass.c:777`: engine/multiple-bitmap locking, architecture dispatch, superclass fallback | AMDGPU gates format/storage/bounds, rejects DMA overlap, fences submissions, brackets public bitmap access for the software cursor. Cold19 found a double-evaluated decrement in release dispatch; fixed by 68b75b44cf. Gate: unchanged physical CopyProbe after replacement/reboot, source/destination cursor save-under, overlap, cross-surface copy, and a separately established safe timeout fallback. |
| Software and hardware cursor advertisement | Nouveau implements cursor shape/position/visibility and advertises sprite support consistently | AMDGPU deliberately uses software CursorFB and advertises zero hardware sprites. That is a valid fallback, not missing mouse support. Gate: visible shape/hotspot/edges/save-under and mode changes. Hardware sprite remains unproven and must not be advertised until physically demonstrated. |
| UpdateRect and screen stack | Nouveau UpdateRect forwards to compositor; ShowViewPorts forwards stack; compositor handles rect/position changes and validates offsets | Corresponding AMDGPU methods exist. Gate: native redraw, obscured/revealed windows, screen depth/drag, and failed plane updates preserving the old screen. No Wanderer/Zune patch is a substitute. |
| Panning / larger backing bitmap | Nouveau compositor validates LeftEdge/TopEdge then switches video mode or redraws stack | 9cb35e1015 owns offsets, filters inherited self-copy, resolves wrapper identity and programs a bounded source viewport under cursor exclusion. Cold25 unchanged PanProbe-v2 passes pixels and HDMI after native replacement/reboot. Positive-Y dragging/composition and failure recovery still need native acceptance. Cold22/cold23 failures remain preserved below. |
| Drawing beyond overridden methods | Nouveau explicitly provides alpha/template/pattern/line and direct-access methods | AMDGPU overrides a subset and inherits the rest. Gate: GfxHiddConformance classification and independent pixel oracles on driver-owned/displayable as well as generic objects; inherited passes are functional passes, not hardware-acceleration claims. |
| Gallium integration | Nouveau declares a Gallium class, checks interface version, provides CreatePipeScreen and DisplayResource | AMDGPU's current HIDD configuration exposes device/query/compute ABI but no equivalent Gallium class in that module. Check the separately maintained Mesa/Gallium consumer seam before calling this absent overall. A queried device or a 2D pass is not a Gallium/Vulkan/ME pass. |

## Native acceptance order

1. Freeze binary and source identity, upload using NetShell, compare the installed
   bytes against the frozen reference, retain the known baseline rollback.
2. Reboot the physical DUT before driver acceptance. Record uninterrupted UART,
   actual installed-driver loading, mode identity and a fresh HDMI capture.
3. Re-run Gfx2DConformance and the unchanged CopyProbe that exposed the fault.
   Record both failed and successful checkpoints; retain visible cursor evidence.
4. Run the already-installed GfxHiddConformance with strict alignment, then the
   native panning discriminator. Tests must not silently exercise generic/VESA
   objects when intended to cover AMDGPU-owned displayable storage.
5. Complete remaining mode, screen-stack, memory-pressure, CPU/GPU ordering and
   recovery gates in individual committed, replaced, rebooted iterations.
6. Leave Work mounted and Wanderer/local Shell available for human verification.

Full 2D remains the priority. No GUI/core/filesystem changes are authorized by
this comparison, and ME/Gallium/Vulkan completion must remain separate claims.

## Current evidence boundary

Cold38 repeat with corrected KVM capture rejects the apparent second fault:
TOP-RETURNED464/465 show both markers, matching active composite memory,
without any driver/probe change. Old helper captured after frame detection
had been re-enabled. UPDATED462 still lacks both markers in actual composite
memory despite correct backing stores: UpdateRect forwarding remains a real
open defect. Native0 is not full2D acceptance. See capture correction in
ME_DISPATCH_STATUS.md; original450..455 evidence remains caveated/preserved.

Cold38/source9f5cc47faa repeats native baseline on byte-verified/rebooted v10.
Separate StripCopyProbe-v1 passes two1920x192SDMA copies, seven368640pixel
oracles and all three fresh HDMI stages including nonuniform marker updates
and screen switches. Thus cold37 split marker loss is not reproduced as a
generic long-stride copy failure. Composition/UpdateRect remains unresolved;
TRIG-0951/REQ-0039 pending. V10 control-field preservation does not produce
visible hardware cursor. See leading cold38 ME_DISPATCH_STATUS.md for exact
hashes, native exits, the first-copy-stage pointer limitation and evidence.

Cold37 repeats copy/software-cursor/drawing passes on natively byte-compared,
physically rebooted cursor-v9. Its separate two-screen probe discovers a
presentation failure: drag192 geometry is correct and all eight4096-pixel
backing checks/coordinates pass, but live updates of both exposed screens do
not reach HDMI402. Bottom-front403 shows magenta; top-returned404 shows amber
but not lower magenta. Restored0 is correct. Actual native0 is NOT acceptance
of the failed scanout stages. AMDGPU UpdateRect currently omits Nouveau's
BitMapRectChanged forwarding. Narrow method handoffTRIG-0951 is requested;
no GUI/core/FS changes. See leading cold37 ME_DISPATCH_STATUS.md and registered
screenstack-probe-v1-provenance.md for binary identity, timestamps and failures.

Cold36 repeats the complete unchanged copy/software-cursor/drawing regression
gate on native byte-compared, physically rebooted cursor-v8 (fd8d35acac70).
Copy and drawing return 0/error=0/ioerr=0; drawing has 18 PASS, 0 FAIL,
including 4 tasks x12 disjoint DMA transfers, seq512/184 wraps without observed
expiry/capacity fault/CPU fallback. The separate exposure oracle also returns
native 0: all 131072 pixels match before/after a window cover at 1s and 11s,
with matching fresh HDMI. This is bounded exposure coverage, not a general
screen-stack or intermittent-regression fix. Idle register observations reject
disabled cursor/stuck update lock; active DLG follows OTG. Hardware cursor
visibility remains absent and advertisement stays disabled. See the leading
cold36 section of ME_DISPATCH_STATUS.md for exact source/binary/evidence identity.

Cold34 confirms the same SDMA regression gate with installed,byte-verified and
physically rebooted cursor-v6/b64a78a18a. CopyProbe twoDMA copies/four131072pixel
checks bad0; CursorProbe seven fresh-frame software shape/edge/screen stages;
drawing18PASS0FAIL incl4tasks x12DMA. All native0/error0/ioerr0;seq512/184wraps
without observed expiry/capacity fault/CPUfallback. Six4096-word cursor-image
readbacks match through SDMA, yet no hardware mirror is visible. GPU storage
is therefore proven but display cursor fetch/composition remains open. Main
pointer preference application is also unverified: configured white.png exists
and matches build asset, but default remains red. No driver-owned image bypass
or GUI patch is justified.320 confirms1080p Wanderer,Work,ready localShell4;
Info Work read/write0errors. See leading cold34 section in ME_DISPATCH_STATUS.md.

Cold32 (2026-09-07) advances the SDMA gate: source7686e5496c, frozen486864-byte
driver, native installed byte comparison and physical reboot. Two unchanged
CopyProbe runs completed all four512x256DMA copies/eight131072-pixel checks
with bad0; drawing18PASS0FAIL includes four tasks x12 DMA roundtrips. Ring
reached seq512/183wraps without submit timeout/capacity fault/CPUfallback.
This supersedes the earlier per-submit halt/rewind behavior, not its historical
failure evidence below. Safe engine reset recovery and soak remain open.

Cold33 repeats that SDMA gate after another byte-verified install and physical
reboot: cursor-v5/dd2dbb4e61,489144bytes. CopyProbe passes two512x256DMA copies
and four131072-pixel checks; drawing18PASS0FAIL includes four tasks x12 DMA
roundtrips. Native return0/error0/ioerr0 for both; seq512/169wraps and no submit
expiry/capacity fault/CPUfallback. Continuous-ring code was unchanged.

Cursor-v5's inherited color scales were already unity; direct post-frame
observations confirm pipe0/MPCC0/OPP0 routing. Neither explains the invisible
hardware mirror. A clean CursorProbe repeat visibly passes software custom
shape/hotspot/clipping/busy/screen restoration, native0. The first run's SFS
modal prevented clean shape coverage and is retained as limited evidence.
Opaque16x16 copy-stage captures also lack the hardware mirror; actual hardware
sprite advertisement remains disabled. Image288 confirms1080p Wanderer, Work
and ready localShell5; Info Work reports read/write and0errors. See the leading
cold33 section of ME_DISPATCH_STATUS.md for native hashes and limitations.

Negative-pan cold25 evidence stands; positive screen dragging,
true OTG/PLL timing changes, memory-pressure/lifetime coverage, full cursor
ownership handoff and recovery remain separate completion gates. No GUI/core
work is substituted for those driver obligations.

Cold25 supersedes the negative-panning failure below. Committed9cb35e1015,
469576-byte driver, was native-byte-compared then physically rebooted. The
unchanged PanProbe-v2 confirmed1984x1112 backing, all12 sampled pixels before/
panned/restored correct, accepted-64,-32, END0/native0. Hardware origin64,32 and
synchronized HDMI115/116 show the expected yellow block at visible0,0. Prior
1920x1080 returns. This closes the bounded negative-pan preservation/presentation
gate, not positive dragging or full modeset/ordering/2D completeness. See the
leading cold25 section of ME_DISPATCH_STATUS.md for hashes and failed-install
attempt provenance. The earlier no-expiry claim was an incorrect case-sensitive
filter result: uppercase TIMEOUT records show startup168/176dwords,248124us,
and during GHC328/336dwords,200001us, both RPTR4. Same loaded-binary reruns
passed drawing18/0/native0 and
CopyProbe four131072-pixel checks/bad0/native0, with matching source/destination/
clean-copy HDMI125/126/127. Strict GHC1850 completed with fail0, missing6,
native5 (284driver/1539generic/4public dispatch passes); not full completeness.
Correct pixels are not a clean acceleration or safe-timeout-recovery pass.
Capture129 and
Info Work/native0 confirm human-ready Wanderer, Shell5 and mounted read/write
Work. The test square is gone; the default red wedge remains software-rendered.
Hardware cursor shape/hotspot/clipping/screen-switch acceptance is still open.

Latest cold23: b5415bf0bb was built, installed, native-byte-compared and
physically rebooted before testing. Native mode00110000/1920x1080 returned.
Two Gfx2DConformance runs each reported18PASS0FAIL, but three measured SDMA
expiries occurred. Strict GHC completed1850checks, fail0,missing6,return5,
with two further expiries. The520dword submissions now publish528dwords;
RPTR4/WPTR840 persists. Fetch alignment alone does not fix the engine failure.

Unchanged CopyProbe passed both SDMA copies and all131072pixel source/copy/
save-under checks, result0, with no additional expiry. Synchronized HDMI
captures90/91/92 now cover the source pointer, destination pointer and clean
copied rectangles. This closes the previously missing cursor visual stages,
not every cursor contract. An attempted window reposition then caught the
screen bar and reproduced known panning corruption (capture95, retained UART).
Do not claim the damaged desktop as a pass. Cold24 physical recovery with the
same driver restored native1920x1080, Wanderer, Work and a ready local Shell
(capture108). Info Work: returned0 and reports mounted read/write,0errors.
Its startup still recorded SDMA expiries for328/336 and288/288dwords. This is
recovered human access, not clean acceleration or completed panning. The frozen
cold24 UART and current hashes are in ME_DISPATCH_STATUS.md. No GUI/core/FS
source was changed.

The following cold22 result is retained historical context:
Verified replacement/reboot of99edb6c3bd preserved native1080p,
the cursor-copy pixel/SDMA gate and strict GHC's1850checks/fail0. However,
Gfx2DConformance's18PASS0FAIL included a measured227890us SDMA expiry/fallback.
The short-poll-only hypothesis is insufficient. Full acceleration remains open.
Corrected PanProbe-v2 now reached actual1984x1112 backing and failed: accepted
-64,-32 offsets erased the four test colors, still wrong after restoring0,0.
Synchronized physical captures72/73 show blocks before and missing after.
The driver needs offset ownership, compositor notification and hardware source
viewport selection together; a mirrored-framebuffer self-copy is not a pan.
See the leading cold22 section in ME_DISPATCH_STATUS.md for hashes, exact
results and retained failures. Original Wanderer returned; no crash occurred.

The following cold19-cold21 evidence is retained historical context:

Cold19: native1080p and Gfx2DConformance 18 PASS / 0 FAIL, followed by a real
CopyProbe exception. That exception invalidated the new cursor-copy change,
not the preceding baseline result. 68b75b44cf fixes the confirmed OOP macro
double-evaluation; its 461512-byte installed image was compared on cold20.
Cold21 is the subsequent physical boot. After the SFS requesters were cancelled,
the native AMDGPU mode00110000/1920x1080 was selected. The unchanged installed
Gfx2DConformance reported18PASS0FAIL, DMA checkpoints through32 with fallback0,
and native return0. This remains the existing baseline test, not full format
conformance. The unchanged CopyProbe passed both SDMA copies with matching
cursor-guard-v1 completion markers; source-cursor-copy, source-preserved and
destination-cursor-restore each checked131072pixels with bad0. ENDresult0 and
RunCommand return0 followed without exception or restart.

Capture54 shows the destination-stage red software pointer on blue, despite
its copy-source filename. Capture55 shows restored Wanderer; it is not the
final rectangle hold. Source/final-stage visual coverage remains incomplete,
although UART positions and pixel checks are retained. Capture56 shows a ready
local Shell; Work remains mounted. No binaries were replaced during this run.

Frozen cold21-through-copyprobe-pass.uart.log preserves
[456276245,464832369),8556124bytes, SHA256
f0f30122404d95a88eb70020fcddcc072a6e32b388c541caac44d229686ba252.
The release-dispatch fault gate is now passed; the remaining contract matrix
and strict conformance/panning order are not yet accepted.

Logs, frozen binaries and screenshots:
`/home/terminills/Documents/GitHub/aros-opal-pc-x86_64-smp/qemu-test/artifacts/JOB-0084-native-completion-20260906/`.
