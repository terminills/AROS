# Graphics ME bring-up: 2026-09-05

## Cold43 GART map guards: native bring-up, drawing and copy PASS

Driver923d3e1920 (505216B) installed-byte compared then physically power-cycled.
Loaded map-guards-v1 marker, compute600dc0de/EOP600df00d,32unique ME/PFP pairs,
SDMA host-memory roundtrips and unaligned dynamic HOSTBUF0/512 all pass.
GART-STATUS.md records exact UART/hashes and limits. Serial boot's11 mappings
are bounded/disjoint; this is not a concurrency/failure-path or Mesa proof.
No cursor/GUI changes. After renewed guards and known SFS/requester recovery,
native1920x1080, two guarded512x256SDMAcopies/fourpixelchecks and drawing18/18
passed with fresh HDMI; native0 at000006c848943a79 and00000721a65c967a.
No general VA/retirement/Mesa/full2D claim; cursor failure remains deferred.

## Cold42 UpdateRect stack acceptance PASS

Isolated87d8951259 forwards displayable bitmap UpdateRect to the existing
compositor outside the framebuffer lock. Driver502240B was natively compared
before physical OFF/dwell/ON. Cold42 native1080p and unchanged stack observer
prove immediate magenta/amber updates in both active composite and fresh HDMI,
all six stages correct, actual native0/error0/ioerr0 at0000043ab3a11db3.
See UPDATERECT-STATUS.md and registered updaterect-v1-provenance.md. Retained
ME32/compute600dc0de boot checks also pass. Subsequent unchanged SDMA copy,
drawing18PASS/0FAIL and panning checks returned native0 with fresh HDMI.
Software cursor returned native10 (aggregate sentinel FAIL); cause unproven,
raw evidence retained, investigation deferred by user direction for 3D work.
No Mesa/full2D claim. Foreign ARGB and other driver hunks remain unchanged.

## Cold41 bounded BO lifetime and retained baseline PASS

Driver sourcebecb861f80 adds six appended VRAM/CPU storage vectors, without
changing the existing ME/compute interfaces. Frozen driver501864B SHA256
56cd626f02a09b3afae879ed191113dc55ba58163e635e624f16c6fcded0712e was byte-
compared on the DUT before completed physicalOFF/dwell/ON. Cold41 loaded the
storage-v1 marker; ME32unique and compute600dc0de still passed, and native
1920x1080 was selected. No further driver replacement in this cycle.

Native public BO client v1 first returned20 due to a32/64-bit test-oracle
promotion error, retained with raw failed UART. Client-only correction
aec01e6df3 then passed after native74968-byte comparison; WIDTH-SAMPLE directly
confirmed the diagnosis and correct VRAM words. Owner/map lifetime, no-alias,
zeroed generation reuse, stale-handle rejection,64-slot pressure/recovery and
cleanup pass; actual native0/error0/ioerr0 at000005ee8e5010db. Acceptance is
committed c5d2894e23; see BO-STORAGE-STATUS.md for limits and failure history.

Unchanged baseline checks on this same driver all returned native0/error0/ioerr0:

- Drawing18PASS0FAIL incl4task DMA:000005669c98184c.
- Two guarded512x256 SDMA copies, four131072-pixel bad0 checks and fresh
  HDMI490..492:0000069b01accf1d.
- Seven resident bitmap blocks,16588800B interior reuse atCPU70018ed000,
  all1024-pixel sentinels bad0/noalias/cleanup:000007133451a89e.
- Seven software-pointer stages and sentinel with fresh HDMI500..506:
  000007b624984b58. This is NOT hardware-fetch evidence.

Raw logs/hashes and build/install/reboot/capture provenance are in
JOB-0084-native-completion-20260906/bo-storage-v1-provenance.md. Known SFS
requesters were cleared via KVM without filesystem/GUI edits; native1080p
Wanderer/Work remain. Strict GHCv40 --strict-align also completed1850checks:
284driver/1539generic/4public passes, fail0/missing6/skipped12/manual5,
uncovered0/displayable1. Actual native5/error0/ioerr0 at000009123ffa187e, not
all-pass: the same six ProcessPixelArray noops belong to cybergraphics.library
and did not call the driver. No timeout/fallback messages in the frozen
1987199B run (SHA256df938dd96baf8929416aa51547df74084b0fc9b6119c026868bc6b7c51f80fc9).
Info Work returned0:1726.5G,20.0Mused,0errors,read/write. Capture621 at12:24:42UTC
shows native1080p Wanderer/Work and a ready local Shell5 for human verification;
NewShell native1/error0/ioerr0 at000009810134df33. Full2D,
UpdateRect(REQ0039 pending), hardware pointer, true timing
modesets, GPU VA/generalGTT, submission/fence retirement, Mesa and soak are
NOT closed by these bounded results. Foreign bitmap/Atom/MMIO/discovery/
intern hunks remain byte-identical to their saved baseline.

## Cold40 post-repair baseline checks PASS

After installed-byte verification and physical reboot of4f07513c32, unchanged
native tests passed without another driver change:
- Interior VRAM reuse and all1024-pixel sentinels: native0 (8b1f1d27d3).
- Public resident-only Device/Query client: native0 (b6dff74c07).
- Two guarded512x256 SDMA copies, four131072-pixel checks bad0; captures490..492
  show source/destination pointer and clean copied pixels. Native0 at
  00000582e1900e93.
- Seven software-pointer stages500..506 show default/custom/busy images,
  hotspot/edge clipping and screen restoration; sentinelPASS, native0 at
  0000062cbdb7bdf4. Public sprite flag1 is NOT hardware-fetch evidence.
- C:Gfx2DConformance18PASS/0FAIL, including176x176 DMA roundtrip and4tasks x12
  disjoint transfers. Capture512; actual native0 at000006acd8f9caeb after
  interactive exit. Observed upload/download fallback counters remain0.

All actual native exits above have error0/ioerr0. Details and immutable UART
slices are in cold40-2d-regression-provenance.md in the registered
JOB-0084-native-completion-20260906 artifact set. Source foreign ARGB/MMIO/
discovery/Atom/intern hunks remain byte-identical to the preserved baseline.
No GUI/core/filesystem changes. Hardware cursor is explicitly deferred.
Wider strict conformance/stack/pan still pending on cold40; known UpdateRect
refresh, true timing modesets and recovery/soak gates remain open. This is
not full2D or Mesa rendering acceptance. Next3D lane: checked driver-owned
BO/VA lifetime and per-submission retirement through the existing Mesa seam.

## Cold40 native public Device/Query client PASS

Resident-only AmdgpuApiQuery-v1 (source9161035242) was natively compared
IDENTICAL44568, return0/error0/ioerr0 at000004bfbd3a8046 before execution.
It used public generated inline vectors against the already-running cold40
driver, with no private initialization code or driver reload. One RX9060XT
(7590:c0), PCI0000:0c:00.0, GC12.0.0, VRAM16304MiB and16GiB BAR aperture were
reported consistently. Actual native0/error0/ioerr0 at000004e74dae83e9.
The first public-client integration gate passes; BO/VA/submit services and
the existing Mesa adapter remain separate uncompleted gates. No Mesa render
claim.509-cold40-public-query-pass.jpg at08:18:57UTC shows the preserved
1080p Wanderer/Work/software pointer after the query.

## Cold40 native allocator regression PASS

Unchanged VramReuseProbe-v1 natively compared IDENTICAL42552, return0/error0/
ioerr0 at0000033412deaf0a, observed before execution. Driver source4f07513c32
was installed, byte-compared and physically rebooted (cold39 then same-binary
cold40 recovery). Probe runs on selected AMDGPU00110000/1920x1080, not VESA.

Seven resident8294400-byte blocks filled the bounded pool; initial system
fallback is expected. Released adjacent interior pair0 atCPU70018ed000 while
higher ranges remained live. Combined16588800-byte request now returns EXACT
CPU70018ed000 in VRAM: INTERIOR-REUSE PASS at00000355d69b20ce. All1024-pixel
sentinels pass, no alias; cleanup releases owned blocks and subsequent small
allocation is resident with correct pixels. Native0/error0/ioerr0 at
000003562063ddd1. Cold38's unchanged probe returned5 for failed reuse.

This closes the demonstrated interior-fragmentation defect, not full2D/3D,
SMP allocator stress or general invalid-free recovery. Drawing/cursor and
public Device/Query client regressions follow; cold39 failed startup remains
separate and unassigned.50808:09:17 shows native1080p Wanderer/Work/pointer.

## Historical cold39 checkpoint: acceptance was pending (superseded above)

Interior-coalescing source4f07513c32 built496400bytes, SHA256
38f2f737e5bae48b8295a91f6fdbdf78462431be18b3548603cda84c4639f0b8.
Installed native byte comparison0at000012754bc04298 preceded physical cold39.
Cold39 loaded the new marker and passed boot ME32/32/compute sentinel, but
later startup became unresponsive at IPrefs; UART stopped07:46:24UTC during
an ELF read, before any native allocator test. Cause remains unproven.
No changed free-path execution was observed. Failed4159904-byte boot capture
and HDMI484..486 retained. SAME binary is recovering as cold40, ON07:51:57UTC
following completed12sOFF and over30s fully off. No further driver change.

User permits hardware-cursor deferral; software pointer stays. Concrete
Mesa integration inventory is committed in MESA-INTEGRATION-STATUS.md
(4b158af457); native resident-only Device/Query client9161035242 compiled,
but has not run. Neither new client nor driver build is a native pass.

## Native VRAM reuse defect and next repair (2026-09-07)

Cold38/cursor-v10 native AmdgpuVramReuseProbe-v1 was byte-compared IDENTICAL
42552 bytes, native0/error0/ioerr0 at00000f2c816f731a before execution.
The probe filled only the bounded64MiB driver bitmap pool, then freed adjacent
interior CPU70030dfc00 and70038c8c00 ranges (8294400 bytes each), leaving higher
allocations alive. A16588800-byte request incorrectly fell back to system RAM
CPU109616840 despite the contiguous hole: INTERIOR-REUSE FAIL at00000f50807c43a1.
All1024-pixel sentinel checks passed, no alias, cleanup completed and a final
small allocation was resident. Actual native5/error0/ioerr0 at00000f50c6a6bd8e.
This is allocator fragmentation, not display corruption or OS RAM exhaustion.

Frozen UART[619846612,620367364),520752bytes,SHA256
941454720eebbdd29d7d49483e00d56ca34a659114da69d74ee80e66749f0bc7;
479-cold38-vram-reuse-running.jpg actually shows the restored idle1080p
Wanderer/Work/Shell AFTER completion at07:19:38UTC (filename retained).

The repair in amdgpu_gfx_support.c joins adjacent interior free ranges
transitively under the existing pool lock, then reclaims the tail or inserts
one merged entry. It validates bounds/overlapping frees before mutation.
Active-scanout deferred release and quarantine rules are unchanged.
New boot identity: allocator=interior-coalesce-v1. Build/deploy/physical cold39
reboot and unchanged native tests remain required; no pass claimed yet.

User explicitly permits deferring the invisible hardware cursor for Mesa26/3D
integration. Keep the verified software pointer and native1080p. UpdateRect
live split refresh remains separate; REQ0039/TRIG0951 pending. Old Claude
bitmap leases are released, but foreign ARGB hunks must stay untouched.

## Capture correction: returned split screen passes without a driver change

The tentative second driver/DCN fault below was rejected. NanoKVM's
StopFrameDetect endpoint disables detection, sleeps, RE-ENABLES detection,
then returns. capture-frame-v3 incorrectly awaited it before streaming.
Corrected capture-frame-v4 streams DURING the temporary disable window,
discards cached frames and requires at least 3s of streaming. Both helpers
and the misleading images remain preserved. Reference:
https://github.com/sipeed/NanoKVM/blob/main/server/service/stream/mjpeg/frame-detect.go

The SAME installed ScreenStackObserve-v2 was re-compared IDENTICAL45048/
native0 at00000ba818942156, observed before enqueueing this run; no driver or
probe binary changed. Seven460..466 captures at07:01:37..07:02:27 UTC show:
TOP-RETURNED464 and later465 both contain upper magenta and lower amber,
matching stable active MC80028f6c00 samples before/after hold. Other geometry/
front/restored stages agree. Native0/error0/ioerr0 at00000c225d165fdb.
No plane/cache patch is justified by the old upper-marker omission.

UPDATED462 still omits both markers AND CPU composite memory remains
blue/green despite correct backing stores. Missing UpdateRect forwarding
remains the real open defect; REQ0039/TRIG0951 remains pending. Full2D is not
done. Recheck older absent-feature/cursor claims with corrected captures.

Source-only cursor clock audit also rejects an extra missing DCCG divide:
Linux f377d0025eb0 dccg401_get_dccg_ref_freq returns xtalin unchanged;
hubbub2_get_dchub_ref_freq applies the already-used GLOBAL_TIMER divide2.
Current100MHz crystal/50MHz hub reference matches that contract; no edit.

## Earlier cold38 inference, superseded above: two apparent split failures

ScreenStackObserve-v2 (source aaef1d4c3f, 45048 bytes, SHA256
d926430cc021e18f279f2338f36e723da5aa01c084318db2ea8628aac63eca19)
adds fixed read-only active-plane samples to the unchanged six-stage public
screen sequence. Work/RAM uploads acknowledged; native comparison returned
0/error0/ioerr0 at 0000093c3e06648f, before observer entry. Procedural lapse:
the run was queued before the model inspected the comparison verdict, although
serialized execution completed comparison first. Future runs must wait for
the observed verdict before enqueueing. No driver replacement or initialization.

All eight 4096-pixel backing checks and coordinates pass; observer native
0/error0/ioerr0 at 00000978c0b46bed is NOT presentation acceptance.
Fresh 450..455 (06:47:42 through 06:48:31 UTC) reproduce two failures:

- UPDATED: backing markers exist, but active composite MC80028f6c00 still
  reads blue/green at (300,32)/(300,224), both before and after the 10s hold.
  HDMI452 agrees: both markers absent. Missing UpdateRect notification remains
  a concrete contract defect; REQ0039/TRIG0951 remains pending, not edited.
- TOP-RETURNED: the SAME stable active MC reads magenta00dd33bb and
  amber00eebb33 at those positions, before and after the hold, but HDMI454
  shows only amber. Thus an additional memory-to-HDMI discrepancy remains;
  UpdateRect alone must not be assumed to repair it. No speculative cache,
  plane or GUI workaround was added. CPU aperture is not DCN-fetch proof.
- TOP-COVERS, DRAG192, BOTTOM-FRONT and RESTORED0 match both observations.

Strict GfxHiddConformance-v40 --strict-align also completed this cold boot:
1850 checks, 284 driver/1539 generic/4 public passes, fail0/missing6/skipped12/
manual5/uncovered0/displayable1; native5/error0/ioerr0 at 000008c4f44333d6.
The same six missing library operations do not dispatch a driver method:
COLOR2GREY, NEGATIVE, NEGFADE, TINTFADE, GRADIENT, SHIFTRGB. Not a full pass.

456 at 06:53:54 UTC restores 1080p Wanderer, Work and a ready local Shell5.
Supplemental UART [616505988,619059370),2553382 bytes, SHA256
5d9d96c081fd49ffa70083e4b739525116fd49926f102d10d7daaacfb41095a2.
Continuous UART/NetShell remain live. No foreign bitmap hunks were changed.

## Cold38: v10 native baseline and patterned-strip discriminator

Installed v10/source9f5cc47faa,495968bytes,SHA256
0335f4c28f983c8449a31c560d7a6ad14758c475434edd57fe6af88dc60b84f7.
Native byte comparison0 preceded12sOFF/completed05:57:57/>31sdwell/800msON
around05:58:39UTC. UART boundary608490625; v10 prepared and native00110000/
1920x1080 confirmed. Known SFS read requesters cleared through KVM/NetShell;
no premature reboot. ME/PFP32/32unique pairs/final330/max48polls and compute
600dc0deattempt1 repeat, not Mesa acceptance.

V10 preserves actual inheritedCM84 (inversion/alpha modulation), not zero;
CUR_CONTROL preserved0. White hardware mirror remains absent. Masked-field
repair is not a hardware visibility fix; software cursor ownership remains.
Copy two512x256guardedSDMA/four131072pixelsbad0,native0at000003fa1197748f.
Source-hold430 lacks a discernible pointer; destination431/after432 show it,
so copy's all-stage pointer claim is explicitly limited. Independent seven
CursorProbe stages420..426 show correct software shape/hotspot/clipping/busy/
screen restoration,sentinelPASS,native0at000004c05ead1c95. Drawing18PASS0FAIL
incl176x176DMA/4tasks x12disjoint,seq512/184wraps,native0at0000053bc9e4ee54.
No observed submit timeout/capacityfault/SDMA-failed fallback.

New standalone StripCopyProbe-v1 source0c94f6eb16+d855ce5067,39408bytes,SHA256
621fe8fe5317920d5d014bb8954a58eb9dec5bde322b1685efe67f89a80683eb. Bothuploads
acknowledged; nativeIDENTICAL39408/return0at0000058f7029886c. Seven368640pixel
checksbad0,two1920x192guardedSDMAcopies,stride7680,destinationy0/256. Fresh
44006:28:56/44106:29:05/44206:29:15 show both magenta/amber markers correctly
and preserve them across screen switching. Native0at000005ccc19ed65b. General
nonuniform long-stride copy failure is not reproduced. This narrows cold37's
split failure toward composition/update/ordering; it does not fix it or prove
universal cache coherence. No driver was initialized/replaced by the probe.

44306:32:43 shows1080p Wanderer/Work/readyShell5. Work1726.5G,18.0Mused,0errors,
read/write; Info native0. ContinuousUART remains live. Frozen cold38 interval
[608490625,616505988),8015363bytes,SHA256
5a10c164bc87aa55e54e493b63aa42fd4adbdf3bc173eef317f6c14a9689ef67.
Evidence set JOB-0084-native-completion-20260906 is registered live with captions.
UpdateRect TRIG-0951/REQ-0039 remains pending; no blocked method/foreign-hunk,
GUI/core/FS edits. Full2D and hardware cursor remain incomplete.


## Cursor-v10 candidate while the split-update handoff is pending

TRIG-0951 requests the non-overlapping bitmap UpdateRect method; REQ-0039
requests the corresponding narrow console guard extension. Neither is treated
as self-approval. The proven split-screen failure remains the next functional
repair; no edit is made to the blocked method or Claude's alignment hunks.

Independent cursor correction: Linux f377d0025eb0 hubp32 cursor attributes,
hubp401 init/position and dpp401 attributes/position use masked REG_UPDATE.
The native adapter overwrote entire CUR_CONTROL and CM_CUR0_CONTROL words.
V10 preserves fields outside the exact masks1f030715/7b and records inherited
values on bounded shape publications. CM bit2 is inversion, bit7 alpha
modulation, bit16 read-only pending; no speculative extra enable bit is set.
Unreadable control words stop further cursor writes. V9 private opaque-white
image, source/software ownership, dimensions/pitch/mode/position and all other
cursor/plane/timing settings remain unchanged. If the preserved writable bits
are zero, that explicitly rejects the lost-inherited-field visibility theory.
This corrects the programming contract, NOT a claimed hardware cursor fix.
Build/install/native-byte-compare/physical-reboot/unchanged tests required.

## Cold37: regressions pass; live split-screen presentation fails

Source b3d155da0d, cursor-v9,495368bytes,SHA256
6a212c499029a368738666cfef0834bc99d8db84220bc0429a887a1ce51e4fef.
Native installed byte comparison0/error0/ioerr0 preceded physical12sOFF,
completed response,>22sfullyoff,800msON at04:57UTC. UART boundary597456993;
v9 prepared confirmed. Unchanged ME/PFP initial and32/32unique reuse pairs
again pass,final330/max48polls; compute600dc0deattempt1. Not fullMesa acceptance.

Copy two512x256SDMA/four131072-pixel checksbad0,native0at000003fef0942350.
Cursor seven380-386 fresh stages demonstrate software default/customL/hotspot/
edges/busy/screen restoration,sentinelPASS,native0at000004fb12aced5d. Drawing
18PASS0FAIL including4tasks x12DMA,seq512/184wraps,actualnative0afterkeyexit
at0000062fa328da8d. All three returnerror0/ioerr0; no observedSDMAexpiry/
capacityfault/CPUfallback. Hardware mirror remains invisible. V9's private
opaque-white4096word GPU image matches hash1ceeecc5/bad0 while originalsource
is untouched (default78alpha159,busy155alpha159,custom100opaque). This rejects
source color/transparency as the sole cause, not fetch/composition. Software
ownership/advertisement0 retained; calibration must be removed before handoff.

Supplemental ScreenStackProbe-v1/76431e4540,39256bytes,SHA256
9a26f5591071f6a4e267fce2c3edf4294b2222f245943cdb7ee7706e289fa16b was
uploaded and natively compared0at0000067c450d855a, then run on bootedv9.
All eight4096-pixel backing checks and requested coordinates pass; native
return0/error0/ioerr0at000006dda6c31042. But fresh402 at05:33:33UTC shows BOTH
marker writes absent from the split raster.403 reveals magenta onbottomfront;
404topreturned shows amber but still lacks lower magenta.400/401/405 confirm
fulltop,drag192,restored0 geometry. Native0 is emphatically NOT a visual pass.

AMDGPU BitMap UpdateRect515 does not notify its compositor; it only performs
legacy refresh ifdata->disp. ADT Nouveau UpdateRect893 forwards
BitMapRectChanged. TRIG-0951 requests narrow handoff for this method outside
Claude's preserved ARGB hunks. If forwarding does not restore the lower
marker after stack rebuild, that remains a separate driver failure. No GUI/
core/FS changes or probe repair calls. Newly exposed coverage, not proof that
cursor-v9 introduced this composition fault.

406 at05:36:52UTC shows1080pWanderer,Work,ready localShell5. InfoWork native0:
1726.5G,17.9Mused,0errors,read/write. NewShell1/error0/ioerr0at00000754f528f662,
creation visually confirmed. ContinuousUART1876587 andNetShell remain live.
Frozen cold37-v9-native-results.uart.log[597456993,605523411),8066418bytes,
SHA256dd28ae716ba25248de42b9235bfd371bf2bbe47ccb2daf72eaf7aa9a702823b8.
All artifacts/captions in registered JOB-0084-native-completion-20260906.
Full2D remains open; next focus is proven live composition failure.

## Cursor-v9 candidate: isolate the private image from fetch/composition

Cold36's separate native read-only observer v2 confirms request expansion=55
(CRQ=1, matching AMD DML), cursor scheduling=300, TTUs=08001587/1587, and
3DLUT control/status=0. It returns 0/error=0/ioerr=0 at 00000a2d9607f30f.
No missing-CRQ initialization fix is justified; v8 register setup is unchanged.

V9 changes only the private x+64 hardware image contents to fully opaque white,
retaining the existing dimensions/pitch/format/position/register setup and
transparent padding. Original AROS/prefs image alpha distribution and first
nontransparent word are logged without modifying the source or software cursor.
SDMA verifies all 4096 words against the calibration image. This distinguishes
bad source alpha/content from missing DCN fetch/composition on a fresh reboot;
it is a temporary diagnostic, not a real main-pointer replacement or a fix.
Hardware advertisement stays zero and CursorFB retains normal ownership.
No shared timing/clock/plane/GUI/core/FS changes. Build, native byte comparison,
physical reboot and unchanged probes are required before any runtime verdict.

## Cold36: native v8 regression preserved; active pipeline advances

2026-09-07: installed cursor-v8/b9ae76dcd1,494640bytes,
SHA256fd8d35acac70fe9ac6542164ec523c2f80264fa79f774f80b87b6decbc22e325.
Both old-v7 and installed-v8 bytes compared natively, actual0/error0/ioerr0.
Physical12s-OFF/completed/>8s-dwell/800ms-ON at03:57UTC, virtual media empty;
UART1876587 uninterrupted, boundary587631123. V8 prepared marker confirmed;
startup endcli0, NetShell connected04:11:44UTC, owned00110000/1920x1080 selected.
Known SFS read requesters cancelled before affected probes; noFS/GUI/core fixes.

V8 pairs three request-status reads with independent OTG active positions before
logging. Five shapes collect all three phases; shape2 collects1/3 within100ms
and reports incomplete/readable1/polls1996. DLGVTG matchesOTG311/581/851 with
VBLANK0. The pipeline does advance; neither this nor GPU image readbacks prove
cursor fetch/composition. All fresh copy/cursor images still lack an x+64
hardware mirror. Software owns the pointer; hardware advertisement stays0.

Unchanged native CopyProbe: two512x256SDMA guarded copies,four131072pixel
checksbad0, native0/error0/ioerr0. CursorProbe: seven fresh350-356 stages show
software default/custom/hotspot/clipping/busy/screen restoration, sentinelPASS,
native0/error0/ioerr0. Drawing18PASS0FAIL including176x176DMA and4tasks x12DMA,
seq512/184wraps, no observed SDMAexpiry/capacityfault/CPUfallback; actualnative0
after focused key exit atUART0000052aaaac092a. ME/PFP initial pair and32/32
unique reuse pairs again pass with guards/final330/max48polls; compute600dc0de
onattempt1. These bounded tests are not full2D/soak/reset/nativeMesa acceptance.

371 at04:25:08UTC shows clean desktop exposure after drawing closure, unlike
cold35's gray rectangle. Do not attribute the difference to v8 or call the
intermittent visual issue fixed. Separate native exposure oracleb6d250c777 and
fixed read-only idle/pending observer5e73049e25 were built without changing the
driver binary. Both were then installed, natively byte-compared and run on v8.

DcnCursorRead v1: three idle samples retain CUR_CONTROL=04000305 and CM=31;
OTG_PIPE_UPDATE_STATUS=0, cursor lock=0, active DPP0 pending bits=0. Frames
1a550/1a598/1a5e3 advance. Native return=0/error=0/ioerr=0 at
000005c95cd5516c. This rejects idle-disable and stuck-update-lock hypotheses;
it does not demonstrate cursor fetch. MPC pending 2220 belongs to inactive pipes.

AmdgpuExposeProbe v1: 131072 cover pixels match, then all 131072 restored pixels
match the saved oracle at both 1s and 11s after closing the cover window, with
no repair writes. Matching hashes 3b849dc5; native return=0/error=0/ioerr=0 at
000006af19ab2448. Fresh 365/366/367 at 04:31:47/04:31:58/04:32:08 UTC agree.
This closes one bounded exposure gate, not the intermittent cold35 gray issue.

364 at 04:36:53 UTC shows native 1080p Wanderer, Work and ready local Shell5.
Info Work: read/write, 0 errors, 1726.5G, native return=0/error=0/ioerr=0.
NewShell returned 1/error=0/ioerr=0 at 0000077e97117089; creation and the ready
prompt are visually verified. Continuous UART and idle NetShell remain active.
Supplemental UART cold36-v8-supplemental-human.uart.log spans
[594938771,596182769), 1243998 bytes, SHA256
b4cf3cac6c7fb033763b50fac67377cfa1572d3b2169176f750721bee809e3e3.
Frozen UART cold36-v8-native-regressions.uart.log[587631123,594938771),7307648B,
SHA25677832addea9019efff12caf11f263ae6d3e9958acc4f9505e6e60a1fecdadbb5.
Full build/install/boot/captions and negative evidence are in the registered
JOB-0084-native-completion-20260906 set, cursor-mirror-v8-provenance.md.

## Current distinction: ME execution passes; full native integration is separate

Rechecked the installed, physically booted cold34 UART on2026-09-07 at the
user's request. The initial engine test independently clears ME and PFP memory
targets, submits both writes, then observes ME=600df00d and PFP=600df00d,
RPTR/writeback0a and CP_STAT0. This is a real command side effect, not a ready
bit or an SDMA-only inference. The earlier CSIB/SRM setup fix is present; this
pass is not caused by the pending cursor-v7 candidate. Historical ME-WEDGE
wording and persistent EXCEPTION_STATUS=c122 must not override passing writes.

The user confirms Mesa26 is already ported and much of the AROS glue exists.
After full2D, the task is repairing/wiring that existing native AMDGPU path and
validating it, not porting Mesa again. Initial ME execution does not by itself
establish completed Gallium/Vulkan integration or sustained rendering safety.
Evidence: cold34-v6-boot-copy.uart.log, engine-test line33547.

## Cursor-v7 candidate: initialize DLG mission mode, observe request state

Cold35 result (2026-09-07): installed492992-byte91c125b91311 driver was compared
natively before physical12s-OFF/completed/>8s-wait/800ms-ON at03:14UTC.
UART boundary579323841, mirror-v7 prepared confirmed. Both ME/PFP initial
fences and32/32 unique follow-up pairs pass again, intact guards, final330.
Native owned1920x1080 selected after boot Shell endcli0 and SFS recovery.

DLG was ALREADY00000100; changed0 rejects the missing-init explanation.
Six4096-word SDMA cursor-image readbacks match; no hardware mirror in the
fresh copy or seven cursor stages. Correction: five of six request observations
haveVBLANK1 (VTG0 or1); one atUART000003ac3d395ff2 hasVTG0x3b2/VBLANK0,
status03b21f1f/000003ad/080e1e0a. The previous "allVTG1/VBLANK1" summary
overgeneralized the majority; the raw evidence was retained. DLG count therefore
does advance. The next discriminator pairs request reads with independent OTG
positions; unpaired observations do not justify changing working shared timing.

CopyProbe two512x256DMA/four131072pixel checksbad0; CursorProbe software
shape/hotspot/clipping/busy/screen transitions pass. Bothnative0/error0/ioerr0.
Drawing18PASS0FAIL including176x176DMA and4tasks x12disjoint transfers; actual
complete/native0/error0/ioerr0 after focused key exit. SDMAseq512/184wraps,
no observed SDMAexpiry/capacityfault/CPUfallback. Hardwareadvertisement remains0.
Evidence330-336,340-343 and cursor-mirror-v7-provenance.md in the native
completion set. This preserves the tested baseline, not full2D completion.

Next candidate cursor-v8 adds read-only, explicitly ordered request samples
at three active OTG raster windows for the first six shapes. It collects before
logging, bounds all windows together to100ms plus a finite poll ceiling, and
records the OTG position before/after each request-status group. Missing phase
windows are reported, not treated as a cursor fix or failure. Unreadable MMIO
stops further cursor writes. No shared clock/deadline/route/plane geometry,
GUI/core/FS, test-binary or hardware-advertisement changes. This is an evidence
discriminator, not a claimed visibility fix, and requires another native
byte-verified replacement, physical reboot and unchanged probes.

Cold34 below proves GPU image storage while the hardware mirror remains absent.
Linux f377d0025eb0 hubp401_program_deadline and hubp32_init explicitly program
HUBPREQ_DEBUG_DB=0x100 for normal DLG operation. The native adapter did not
initialize this state. V7 records its inherited value, applies that exact AMD
initialization once after the existing owned/unity-pipe validation, and checks
readback. Already0x100 means no write and rejects this hypothesis. Unreadable
or rejected mode disables the mirror; no later cursor writes follow. No clock,
route, plane geometry, GUI/core/FS or test-binary change.

For the first8 images, direct sequential post-frame observations record DLG
VTG/chunk status, cursor urgency/self-refresh, recovery/flush, pstate, blank/
prefetch/globalTTU values. These are bounded direct reads, not indirect debug
selectors or W1C writes; they do not establish actual cursor fetch. Software
CursorFB remains owner and hardware advertisement stays0. Candidate requires
build,byte-verified native install,physical cold35 and unchanged visual/DMA
probes before any acceptance. It is NOT yet a visibility fix.

## Cold34 cursor-v6: GPU image verified, hardware mirror still absent

Source02e86cae09/b64a78a18a;491192bytes SHA256
fc220daa0eec08c33458abb0b2a6c3ab72047a8e3687161ec80fbf79e3ab265c.
Native old-v5/new-v6 FileCompare both return0/error0/ioerr0. Physical12s-off,
completed response, >8s off,800ms-on at02:14UTC; virtual media empty, continuous
UART1876587 retained. Cold34 boundary571007925; mirror-v6 prepared observed.
Boot Shell1 endcli0; native owned00110000/1920x1080 selected. SFS requesters
cancelled before each affected probe, no GUI/core/FS changes or hot-load proof.

All six observed4096-word image readbacks match, including default, opaque,
custom and busy shapes. Hashes e817ddc5,1f8bddc5,7183ddc5,f7cdddc5 agree with
expected pixels. This verifies SDMA-addressed cursor storage, NOT DCN fetch or
composition. Post-frame control04000305,CM31,power24/state0; pending clears.
No separately visible hardware mirror at x+64 in the fresh-frame captures.

Unchanged native CopyProbe: two512x256guardedDMA copies,four131072pixel checks
bad0,END0/native0/error0/ioerr0. Captures310/311/312 at02:42:18/28/38UTC show
software pointer on source/destination then clean green rectangles/no stamps.
Unchanged CursorProbe: seven synchronized300-306 at02:46:16 through02:47:16UTC
demonstrate default/custom asymmetricL,hotspot/clipping,busy,and both screen
transitions in software. Native0/error0/ioerr0. SFS modal318 cancelled before
probe began; no modal in these seven captured stages. Hardware advertisement0.
Unchanged Gfx2DConformance18PASS0FAIL incl176x176DMA and4tasks x12roundtrips;
319 at02:48:50UTC. Native complete/return0/error0/ioerr0 after focused key exit.
Continuous-ring seq512/184wraps; no observed submit expiry/capacity fault/CPU
fallback. This is regression coverage, not soak/reset recovery or full2D.

320 at02:54:33UTC shows1080p Wanderer,Work,ready localShell4. Info Work native0:
NVMEZFS1,1726.5G,17.2Mused,0errors,read/write,ZFS0. NetShell remains connected.
User flagged main pointer preferences: boot IPrefs attempts Normal/white.png
and Busy/zzz.png, but observed default remains16x16 red. Pulled main32x32 PNG
is valid and byte-identical to build asset. Driver SetCursorShape consumes
Intuition-supplied bitmap/hotspot; no hard-coded driver main shape. Preference
application remains unverified and is separate from absent hardware mirror.

Frozen UART cold34-v6-boot-copy[571007925,577980326),6972401bytes SHA256
9bfb4b61d2b1af056d6d94ea7a1e3a54b8f9b9ba1da8a520208c35bcbdacea1f;
cold34-v6-cursor-drawing-human[577980326,578553227),572901bytes SHA256
739a981b64c4fff00c68c6fdf1cf1d1c364830f3e9fe26caceb6857f46b4555f.
Evidence set JOB-0084-native-completion-20260906. Capture-v3 discards initial
cached MJPEG frames;292 is explicitly stale cold33 desktop during cold34 boot,
not v6 proof. Foreign dirty driver hunks remain byte-identical.

## Historical cursor-v6 candidate: separate image storage from display fetch

Cold33's negative hardware result below is retained. CURSOR_REQ_MODE was also
checked against Linux f377d0025eb0 hubp401_set_unbounded_requesting: the required
bit2 is already set in04000305, so no request-mode change is warranted.

V6 adds bounded driver-local GPU readback of the private64x64/256-byte-pitch
cursor image before publication. All4096 words, including transparent padding,
are independently compared through the existing synchronous SDMA download path;
failure retains the software owner and disables this mirror. Readback scratch
is permanently retained with the pinned cursor state, not a short-lived stack
buffer. Display->SDMA lock order matches guarded bitmap transfers; no pool or
framebuffer lock is held. Existing unretired-DMA containment is unchanged.
At most16 shapes are checked. Bounded actual hide/show enable observations help
distinguish callback state from missing display fetch/composition. No route,
clock, GUI/core/FS or test-binary changes. GPU readback is memory evidence, NOT
hardware cursor acceptance. Required next step remains byte-verified native
replacement, physical reboot and synchronized cursor/copy/DMA regression tests.

## Cold33 cursor-v5: scale/routing ruled out; native regressions preserved

Source dd2dbb4e61, frozen489144bytes SHA256
9fac8d890230f1524715c5a98164792b1c5662b70839b6f21dbf410e171ba30d.
Native FileCompare verified old486864 then new489144, both return0/error0/ioerr0.
Physical12s-off/completed-response/>8s-wait/800ms-on at01:12UTC; virtual media
empty, continuous UART recorder1876587 retained, cold33 starts559690716.
New mirror-v5 prepared and continuous-ring-v1 markers prove fresh loading.
Boot EndCL/ioerr205 was dropped KVM input, not driver failure. Full endcli
returned0; known SFS read requesters cancelled. IPrefs Retry selected owned
00110000/1920x1080; no GUI/core/FS changes or hot-driver acceptance.

The inherited color multipliers were ALREADY00003c00/00003c00. Explicit unity
initialization is therefore NOT the cursor visibility fix on this DUT. Direct
post-frame snapshot: DMU2=034c0,DMU3=09000,HUBP=000f000a,clock=00f11111,
VM=0,return=00e40000,DPP=70000010,scale3c00/3c00,matrix0. MPCC0 top0/bottomf/
opp0/controlffff0461/status2/OUT0=4100; MPCC1-3 disconnected/disabled, OUT=f.
Pipe0 routing is correct; no justification to rewrite the active route.
CM pending clears after real frames; power24/status0, control04000305. These
are configuration observations, NOT evidence that cursor pixels were fetched.

Unchanged CopyProbe-v1: two512x256 guardedSDMA copies/four131072pixel checks
bad0, native return0/error0/ioerr0. Images270/271 show opaque16x16red software
pointer over source/destination;272 bothrectangles green/no pointerstamp.
Hardware mirror absent even with alpha256. Ring progressedseq256/74wraps.

First CursorProbe native0 but custom/busy shapes not demonstrated: an SFS
modal remained on the underlying screen (260-266). After clearing it, the
unchanged probe repeated native0 with seven synchronized frames280-286:
custom transparent asymmetricL, hotspot-4,-4, clipping, busy pointer and both
screen transitions visibly work in software. Driver shape logs alpha100/
offset-4,-4 and busyalpha155 corroborate the repeat. Hardware mirror still
absent, so hardware advertisement/ownership MUST remain disabled.

Unchanged drawing18PASS0FAIL includes176x176DMA and4tasks x12roundtrips,
HDMI287 at01:39:59UTC. Ring reachedseq512/169wraps; no submit expiry/capacity
fault/CPUfallback. Two late389/397us completions crossed the old poll count,
not the200ms deadline. Native complete/return0/error0/ioerr0 confirmed after
key exit; the helper returning alone was not used as the completion criterion.
Two physical boots now preserve the continuous-ring regression gate; this is
not soak/production reset recovery, full timing modesets or full2D completion.

Image288 at01:46:45UTC confirms1920x1080 Wanderer, mounted Work and ready local
Shell5. Info Work returned0/error0/ioerr0:NVMEZFS1,1726.5G,17.2Mused,0errors,
read/write,ZFS0. NewShell returned1/error0/ioerr0 and its actual prompt is visible.
No test remains running; NetShell is connected and continuous UART stays live.

Frozen UART boot/cursor [559690716,566672173),6981457bytes SHA256
4636def5075031c8199eca4a7ccaaccba03de4705c357cb5fcac6ca3939a35a4.
Copy [566672173,566902228),230055bytes SHA256
9c18b7a785a56bec88e270699b9cbfc83acc1a0d0357e50a2c2f7d80c709f1f4.
Clean cursor repeat [566902228,567031133),128905bytes SHA256
e3cb98a776a0048cb00d0e517e63aa3fa76d0623ec5b3fc627047cc701d725ec.
Drawing/human [567031133,567363730),332597bytes SHA256
ffe82c6ea79cdea32c61fbcf80dbf81e8334c3f8c3d22b8c49b0bcd775f236b8.
All under JOB-0084-native-completion-20260906. Next cursor investigation must
distinguish fetch from composition with driver-local evidence; do not repeat
the ruled-out scale/power/pending/routing hypotheses or patch GUI symptoms.

## Historical cursor mirror-v5 candidate (before cold33)

The continuous-ring cold32 checkpoint below is retained unchanged. Cursor-v5
adds the missing explicit FP16 unity multiplier/zero bias for the cursor G and
R/B channels, following Linux f377d0025eb0 dcn10_set_cursor_sdr_white_level and
dpp401_set_optional_cursor_attributes. It records inherited values before the
write and fails closed to the existing software owner if readback rejects it.
This is a concrete initialization omission, NOT yet a proven visibility cause.

The first shape observations now include a bounded direct read-only HUBP/DPP
and four-MPCC/OPP routing snapshot after actual frame boundaries. No selector,
route, clock or main-plane writes are added. It does not prove pixels fetched.
Legacy mirror-v4 scheduling/power logs describe unchanged shared logic; the
prepared marker and added observations identify v5. All existing test binaries
remain unchanged. Required acceptance is a native byte-verified installed driver,
physical cold boot, synchronized HDMI cursor stages and native copy/concurrency
regression checks. No hot-load, host unit-test or register-only acceptance.

## Cold32: continuous-ring native copy and concurrency pass (2026-09-07)

Source7686e5496c built486864bytes; frozen SHA256
a9a7f792f4a9f79c9774b0c5b23c1355d1e47da08e97db68a08a65887467e1dd.
Native FileCompare confirmed old v4 first, then new DEVS IDENTICAL486864,
RunCommand0/error0/ioerr0. Physical12s-off/completed-response/extra8.5s/800ms-on;
virtual media empty. Continuous UART starts551382786, recorder1876587 retained.
Fresh continuous-ring-v1 marker and sequences confirm the installed binary.

After full startup, EndCLI0 and IPrefs Retry selected owned1920x1080. One KVM
EndCLI attempt lost its final I (EndCL, ioerr205); the successful command is
separately logged. Known SFS read requesters were cancelled; no FS/GUI changes.

Two unchanged CopyProbe-v1 runs: native mode00110000, eight131072-pixel checks
bad0, four completed512x256 SDMA cursor-guard copies, both END0 and native
return0/error0/ioerr0. Six synchronized HDMI images230-232/240-242 agree: solid
rectangles/no pointer stamp, software pointer visible. Hardware mirror remains
absent; cursor-v4 unchanged. Repeat progresses seq256/79physical ring wraps.

Unchanged C:Gfx2DConformance:18PASS0FAIL, including176x176DMA round-trip and
four tasks x12 disjoint DMA round-trips; native complete/return0/error0/ioerr0
after key exit. Image250 corroborates the native result. Ring reached at least
seq512/183wraps. No SDMA submit timeout, capacity fault, unretired-stop or CPU
fallback in the boot/copy/concurrency evidence. Logged completions after the
old poll-count boundary were385-402us, NOT200ms deadline expiries.

This is one successful physical boot, not soak/repeated-boot or reset recovery
proof. Unused SUBWIN diagnostic pixel failures and PSP/ME blocker remain;
production scanline copies pass. Hardware cursor remains a separate full2D gate.
Fail-stop containment is still not production engine-reset recovery.

Frozen cold32-sdma-v1-boot-copy.uart.log [551382786,558364528),6981742bytes,
SHA25628af560f764c8f7f7e9783fd950a59d12f6cd22ce7ba9f03366408ffd71037f5.
Frozen cold32-sdma-v1-repeat-copy.uart.log [558364528,558589758),225230bytes,
SHA256e854553a0092247652cbe5bb79d2f4309b140e4e346a058ab47e61329a3434b9.
All native logs/captures under JOB-0084-native-completion-20260906; human desktop
restoration and remaining drawing slice are recorded in that set's STATUS.md.
Frozen drawing/human [558589758,558922254),332496bytes SHA256
89ed895b739bcd4ee2ad1de340eb5c65850fdc2f21921fdea7a1ba18dfe6b4e8.
Image251 at00:47:10UTC confirms1080p Wanderer/Work/ready localShell4. Info Work
native0/error0/ioerr0:NVMEZFS1,1726.5G,17.2Mused,0errors,read/write,ZFS0.
NetShell remains connected/idle; no active test, sole UART recorder retained.

## Historical continuous-ring-v1 candidate plan (before cold32)

Cold31's two200ms expiries exposed the remaining per-submit halt/rewind path.
The candidate removes all per-submit RPTR/WPTR resets, including the separate
boot blit. Initial bring-up alone programs zero, under MINOR_PTR_UPDATE.
64-bit cumulative byte pointers and capacity-checked split writes wrap only
the4KiB VRAM array; each submission has a distinct completion sequence at
fence+8 after the unchanged caller packets, padded to16dwords. Source/ring
stores are fenced and posted MMIO drained before forward WPTR publication.
Reference is local Linux f377d0025eb0 sdma_v7_0.c and amdgpu_ring.c.

No unretired timeout authorizes CPU fallback or buffer/scratch release: it logs
the exact sequence/pointers/status and parks the submitting task, retaining
caller/lock ownership until physical reboot. This is fail-stop containment,
NOT complete reset recovery. A capacity failure before publication can return
FALSE because all previous data fences retired; future submissions are disabled.
No GUI/core/FS changes. Cursor mirror-v4 and collaborator dirty hunks untouched.
Acceptance requires frozen native install/byte comparison/physical reboot,
fresh continuous-ring marker, repeated copy/fill pixel checks AND sequence/wrap
progress without expiry/capacity fault/fallback, plus1080p Wanderer/Work/Shell.
No native improvement claimed until that cycle completes.

## Cold31: corrected power control, cursor still absent (2026-09-06)

Source425088c812 built485112bytes, SHA256
1565b5b3b1316a678f4852db3930d4e59ef838978d32c6628cb259d009e2fa71.
Native comparison confirmed installed current-v3 first, then v4 IDENTICAL485112,
RunCommand0/error0/ioerr0/prompt. Physical12s-off/completed/additional8s/800ms-on,
no virtual media, continuous UART boundary543187300. Fresh mirror-v4 prepared;
EndCLI0, known SFS cancellations and IPrefs Retry selected owned1920x1080.

Correct power RMW20->24, status0 before and after; stereo0 untouched. Actual
two-frame wait advanced, CM00000031 (pending CLEAR), control04000305,
address00000080:01100000, size00100010. Clock50MHz/reference versus measured
148599632/148565991Hz; ratio176429, TTU08001589/00001589. Default alpha78 and
opaque alpha256 both programmed. All three synchronized copy captures200-202
still lack the hardware mirror. The incorrect power-register address was real,
but correcting it did NOT resolve hardware cursor visibility. CM pending seen
immediately after writes was not evidence of a permanently unlatched update.

CopyProbe-v1 four131072-pixel checks bad0, END0/native0/error0/ioerr0, BUT not
clean accelerated acceptance: only first guarded512x256 copy completed. SDMA
328/336dwords timed out RPTR4/WPTR540, then520/528 timed out RPTR4/WPTR840;
second copy fell back atrow64. Both expiries200000us. Default shape replaced
the opaque test image mid-SOURCE hold, so201 is a default-pointer capture,
not an opaque-pointer stage. Preserve timeout/fallback despite correct pixels.

Previously installed CursorProbe-v1 (native-compared39744 before cold29) then
ran unchanged: all seven API/coordinate holds, background sentinel and native
return0/error0/ioerr0. Captures203-209 show software default, custom L with4px
hotspot, clipped corners, busy image, prior screen and returned default. No
hardware mirror where x+64 is in bounds; bottom-right intentionally clips that
mirror out entirely and cannot test its visibility. No SDMA timeout appeared in
this separate API slice; that does not undo the preceding copy expiries.

The test's printed advertised_hw_sprite=1 label is misleading: public
GetDisplayInfoData always ORs DIPF_IS_SPRITES for software emulation
(rom/graphics/getdisplayinfodata.c:207-210). It is NOT a query of the driver's
HIDD hardware-sprite contract, which remains FALSE/0/0. No core/GUI change.

Frozen cold31-cursor-v4-copy.uart.log [543187300,550091994),6904694bytes,
SHA2565e3077f6519ebb52dfa3c9dda709393200ec1a9969b89529163595d2e3e14c93.
Frozen cold31-cursor-v4-api.uart.log [550091994,550324820),232826bytes,
SHA2568067223e8b57dcd320553c864373bfc6c29388c5f0f8615db126ae1b71052472.
All captures/logs registered in JOB-0084-native-completion-20260906.

Remaining cursor discriminator is actual HUBP/DPP/MPC routing/fetch state,
not another claim based only on control readback. SDMA per-submit ring re-arm
and unsafe timeout retirement remain separate full2D gates; no full2D or
hardware-pointer completion is claimed. The dormant legacy amdgpu_cursor.c
also contains wrong power offsets; it is not invoked by this adapter and was
not silently modified during this binary's acceptance cycle.

## Cold30: measured cursor timing accepted, hardware image absent (2026-09-06)

Source bf8261a615/d3f00eb714 built483512bytes, SHA256
9db494ee05dd73455afadaee165aecd49b76200d1bb27ccc4895e285857ef6f8,
installed-byte-compared, physically rebooted, selected owned1920x1080.
DCE4.5 crystal100MHz, live divider2/ref50MHz; two measured pixel rates
148580941/148595894Hz with2200x1125 totals yield ratioQ19=176422.
Cursor normal/prefetch TTU readbacks08001589/00001589 match requestQ10=5513.
This time timing was actually programmed, but HDMI180/181/182 still shows NO
hardware mirror at x+64, including the opaque test image. No cursor acceptance.

Unchanged installed CopyProbe-v1 ran via local Shell5: four131072-pixel checks
bad0, both guarded512x256 copies complete, END0, native return0/error0/ioerr209.
All three synchronized captures completed; no SDMA submission timeout in this
boot slice. Separate PSP autoload timeout remains, so no full driver claim.
Native NetShell exhausted retries before network readiness; inspected eth0 and
the actual default route, then relaunched the existing C:NetShell successfully.
No networking, GUI, filesystem or test-binary changes.

Frozen cold30-cursor-v3-native.uart.log [534928702,542269423),7340721bytes,
SHA2569853a90d1892206441fd3753207ce908e67a011a5d5b66b8f05c04d3e4965aed.
UART and all captures are in JOB-0084-native-completion-20260906.

## Cursor mirror-v4: correct memory-control address (candidate)

Audit against Linux f377d0025eb0 dcn_4_1_0_offset.h/sh_mask.h found CUR_POWER
067f is actually CURSOR_STEREO_CONTROL. The previous write4 hit a reserved
stereo bit and never changed cursor memory power. Actual MEM_PWR_CTRL is0681,
MEM_PWR_STATUS0682. Other cursor addresses, segment indices and used masks
were checked against the same DCN4.1 headers, including CM matrix0cf6.
This is a definite implementation error, NOT yet the proven missing-image cause.

Candidate changes only cursor power addressing and observation: preserve
unrelated fields, clear FORCE/set DIS, require exact field readback, and log
control/status/stereo. Stereo is no longer written. After publishing each shape,
observe CM pending/power/address/size/position after two bounded real frames.
The working measured timing and software owner/zero HW advertisement remain.
Needs fresh build/install/native comparison/physical reboot and visual tests.

## Cursor mirror-v3 candidate: independently measured timing (2026-09-06)

Read-only bounded parser obtains DCE reference clock from the existing VBIOS
copy (master2.1/DCE4.1..4.5). Linux dccg401_get_dccg_ref_freq and
hubbub2_get_dchub_ref_freq supply the live enabled HUBBUB divider rules;
unsupported tables/clock values retain software ownership. Two independent
two-frame intervals derive the actual pixel rate from EClock and OTG totals;
require agreement within1%, unchanged timing and a250ms/fixed-poll bound.
Only cursor-specific TTU/position scheduling uses this derived ratio. The
inconsistent inherited HUBP deadlines remain unchanged and logged, not trusted.
No clock/plane/scaler/GUI/FS write is added. Native acceptance still requires
another installed-byte comparison, physical reboot and synchronized tests.

## Cold29: timing consistency gate withheld hardware fetch (2026-09-06)

Candidate01b9c06901 built479832bytes, frozen SHA256
69501b14f3eedd268402094ce84d76872c7f53d04bebd3f5af233a3d6c8f075f,
native-byte-compared against installed DEVS driver and physically power-cycled.
Fresh boot logged mirror-v2 prepared and selected owned1920x1080 after EndCLI0,
known SFS cancellations and native-coordinate IPrefs Retry. No GUI/FS changes.

The cursor gate withheld programming: ratioQ19=0x14e5e, htotal2200,
horizontal cyclesQ8=0x1e641 (124481), expected91950; prefetch28080000,
scaler0 and viewport/recout04380780. The two inherited timing fields disagree;
this does NOT prove either is current or that the missing TTU caused v1 failure.
No cursor TTU scheduling write or hardware fetch acceptance occurred in v2.

Unchanged CopyProbe-v1: prior native mode00110000/1920x1080, all four131072-pixel
checks bad0, both guarded512x256 copies complete, END0 and native return0.
HDMI161/162 shows the software opaque pointer over source/destination without
a hardware mirror. The host capture helper reached its180-second overall limit
after SFS launch delays: AFTER capture is MISSING; retain that evidence gap.
No SDMA submission timeout appeared in this cold29 slice (the separate startup
PSP autoload timeout remains); this is not sustained acceleration acceptance.

Frozen UART [527505347,534505051),6999704bytes, SHA256
f19affbbb970c9ca3f126667c12be31370dd088db2a4652aaf20a291fa776434,
cold29-cursor-v2-native.uart.log in the native-completion evidence set.
Next driver-local correction derives reference clock from validated VBIOS DCE
info and the live HUBBUB timer divider, and pixel clock from timed OTG frames.
Do not simply remove the failed consistency check or guess a clock constant.

## Cursor mirror-v2: live-timing scheduling candidate (2026-09-06)

Driver-local continuation of the cold28 missing hardware mirror. For the
existing RGB-unity plane only, cross-check the live HUBP reference/pixel ratio
(Q19) against reference cycles per horizontal total (Q8). Derive cursor delivery
and prefetch request periods (Q10), program cursor TTU with QoS8, and derive the
position-dependent fetch offset. Linux f377d0025eb0 dml2_0 delivery-time formulas
and hubp401 setup/position are the references. Unsupported scaling/prefetch,
missing/inconsistent timing or rejected readback retains the software pointer;
no guessed clock, plane/scaler change or hardware-sprite advertisement is added.

This is a candidate, not a native result. It must be built, frozen, installed,
byte-compared and physically rebooted before the unchanged copy test and
synchronized physical cursor captures. No host fixture substitutes for that
cycle. Cold28 failures and the frozen prior drivers remain available.

## Cold28: real-shape cursor mirror still absent (2026-09-06)

Committed1f8ba05401 was built as477320bytes, frozen SHA256
fa77c7a909d93c9a1238ed9c5b3f3af9eb3dc3c4138236f6d1f4416bd33a717a,
native-byte-compared in DEVS:Drivers/amdgpu.hidd, then physically rebooted.
The prior viewport-v1 rollback was separately transferred and native-compared
on Work. Preserved collaborator driver hunks remain unstaged and unchanged.

Cold26 stopped at a Work read before AMDGPU init; cold27 stopped in early device
startup, also before AMDGPU. Matching UART/HDMI and both failed logs are retained;
neither last line establishes a cause. Same-binary physical recovery cold28
passed both points and logged mirror-v1 prepared plus viewport-v1. Original boot
console EndCLI returned0; known SFS requesters were cancelled and IPrefs Retry
selected owned native1920x1080. No GUI/core/filesystem source or binary changed.

The unchanged installed CopyProbe-v1 confirmed native mode00110000/1920x1080,
both guarded512x256 completions, all four131072-pixel checks bad0, END0 and
native return0. Synchronized HDMI140/141/142 shows the software pointer over
source, destination and clear copied rectangles respectively. The expected
hardware mirror at x+64 is ABSENT in all three. A separate screen-setup SDMA
timeout is retained:328input/336published dwords, RPTR4, WPTR540, elapsed200002us.
Correct copied pixels do not establish clean acceleration or timeout retirement.

Mirror diagnostics: actual16x16 image with78nonzero-alpha pixels for the default
pointer and256for the opaque test cursor; control04000305, CM00010031. Cursor
TTU registers are both00000000; MPC00004100 and update-lock00000000. The shape
returned to default during the source hold, recorded rather than hidden. This
still supplies both an opaque source-stage failure and a default-shape failure.
No retirement TIMEOUT was logged. Keep software ownership and zero hardware
sprite advertisement; register readback has NOT proved GPU fetch/composition.

Frozen cold28-cursor-mirror-copy.uart.log:[519017449,525959129),6941680bytes,
SHA256f3c0f15de198cc5cbcff50a348c49b2abb0ff786890fa481229087b99f07da08,
registered in JOB-0084-native-completion-20260906 with all failed boot evidence.
Next bounded lead is cursor TTU scheduling: Linux hubp401_program_deadline and
hubp401_setup program both CUR0 TTU registers, unlike this current adapter.
Zero observed values are an implementation gap, NOT established root cause.
Any clock/rate derivation needs valid live timing/scaler inputs, not a guessed
constant or a change to the working main plane.

Source-only test c4b08d185e subsequently compiled/linked natively as39744bytes,
SHA2569c0c39c217ac90ccb7f09d4ecc24f204628aa8874708201e0f874037e422ff1d.
Frozen AmdgpuCursorProbe-v1 is NOT installed/run; it is not evidence of a pass.

Same installed/rebooted candidate then passed unchanged Gfx2DConformance:
18PASS0FAIL, native return0 after the documented key exit. A second SDMA timeout
occurred during this run:328/336dwords, RPTR4, WPTR540, elapsed200000us. Thus
cold28 has two measured SDMA submission expiries, not a clean acceleration run.
The existing masked-ARGB oracle remains limited. Capture155 shows the completed
test;156 shows restored1920x1080 Wanderer, ready local Shell4 and software cursor.
Info Work returned0: NVMEZFS1,1726.5G,17.1M used,0errors,read/write ZFS0.
NewShell returned1/error0 and its real ready prompt is visually confirmed.
Continuous UART and connected NetShell remain live. All five preserved dirty
driver diffs compare byte-identical to the pre-candidate snapshot; none staged.

## Cursor mirror-v1 candidate (2026-09-06)

Driver-only diagnostic stage, not a hardware-cursor completion claim. The real
AROS SetCursorShape image is extracted as little-endian BGRA32/native AARRGGBB
with64-pixel pitch into separately reserved VRAM. The existing software pointer
and zero-HW-sprite advertisement remain unchanged. A secondary hardware image
at display x+64 will distinguish hardware fetch/composition in synchronized
physical captures. No legacy hard-coded cursor initializer is called.

The exact Navi44/pipe0 path is gated on current AMD-owned scanout. Shape changes
disable both cursor blocks and wait two observed frames before storage reuse;
a bounded200ms failure retains storage and disables the diagnostic. Image WC
stores are fenced before publication. Attributes/position/enable are published
under the OPP0 cursor update lock, with real-size chunk selection and clipped
hotspots. No plane/scaler/modeset, GUI, core or filesystem changes. TTU registers
are observed, not guessed. References: Linux f377d0025eb0 hubp32/hubp401/dpp401/
mpc1 cursor routines and ADT Nouveau17981c45e5c2 shape conversion.

Native build, frozen install/byte comparison, physical reboot and unchanged
CopyProbe-v1 with simultaneous UART/HDMI must precede any verdict. If the x+64
image is absent, retain that failure and the working primary software pointer;
do not flip the HW-cursor advertisement on readback evidence alone.

## Cold25: negative pan preserved AND physically presented (2026-09-06)

9cb35e1015 is committed. Its469576-byte viewport-v1 driver has SHA256
83ba982b323a169b150516850ce0e8f8dfc28194f7c2d849c257db0dc2571ae6.
Native installed baseline comparison passed465040/return0. The guarded Copy
backup attempt failed5/error115 (bad number), so the script Quit20 before
replacement. NetShell transfer plus native comparison then proved the Work:
rollback465040 and new installed driver469576 identical, both return0.
Physical12s-off, completed-off wait greater than8s, then800ms-on started cold25
at UART byte506474215. The continuous recorder was never stopped.

Fresh UART logged viewport-v1 and monitor00110000. The original console reached
1> before EndCLI (return0); IPrefs Retry and known SFS requester cancellations
selected owned1920x1080/32bpp scanout. NetShell reconnected and Shell5 was created.
No GUI/core/filesystem source or binary was changed for this iteration.

The UNCHANGED, previously installed PanProbe-v2 ran first to isolate the repaired
gate. It confirmed mode00110000/depth24 and actual1984x1112 screen/backing/stride.
All four colors passed before, at accepted(-64,-32), and after restoring(0,0):
12 sampled pixel checks, END0, native RunCommand0. Driver selected hardware
origin64,32 without moving the backing base, then restored0,0. Synchronized
capture115 shows red/green above blue/yellow;116 shows only the expected yellow
64x32 block at visible origin0,0. Thus this is both storage preservation and a
physical presentation pass, not merely accepted coordinates. Prior1920x1080
screen returned. CORRECTION at17:50UTC: the earlier case-sensitive log filter
missed uppercase TIMEOUT. Startup included one measured SDMA timeout,168 input/
176 published dwords, RPTR4, elapsed248124us. The native panning pixel and HDMI
results stand, but the earlier no-expiry claim in f61614bd70 is withdrawn.

Frozen cold25-through-pan-pass.uart.log covers[506474215,513614935),7140720bytes,
SHA256 0d51dbf739dd39efd9f8b686e4b05638b3393694898708f40c1cc685f689d6fe,
in registered JOB-0084-native-completion-20260906.

Same-boot regression runs are now complete, without replacing any driver or
native probe binary. Gfx2DConformance reported 18 PASS / 0 FAIL, then exited via
its documented key path with native return0 (capture124). Its existing masked
ARGB oracle is not exhaustive alpha validation. CopyProbe-v1 completed both
guarded512x256 copies and all four131072-pixel checks with bad0, END0/native0.
Synchronized captures125/126/127 show its deliberate16x16 test cursor over the
source, over the destination, and both copied rectangles clean afterward.

Strict GfxHiddConformance-v40 completed1850 checks: driver dispatch284,
generic1539, public4, fail0, missing6, skipped12, manual5. Native return5 is
retained: this is not all-pass or full acceleration/2D acceptance. Case-
insensitive audit found a second SDMA timeout during GHC:328 input/336 published
dwords, RPTR4, elapsed200001us. Correct pixels include available CPU fallbacks;
this is not a clean acceleration run. Both current failures and the mistaken
no-expiry hypothesis remain recorded. Drawing and CopyProbe intervals have no
additional timeout records. Safe timeout recovery/ordering remains unresolved.

Info Work: returned0; NVMEZFS1 is1726.5G,16.1M used,0errors, read/write ZFS0.
Capture129 shows native1920x1080 Wanderer, mounted Work and a ready local Shell5,
with the red software pointer and no requester. The square test cursor was
cleared by CopyProbe. The built-in Intuition default is a red wedge; hardware
cursor advertisement remains disabled until real shape/hotspot/clip/screen-
switch acceptance. No GUI/core pointer styling change is part of this fix.

Frozen cold25-2d-copy-ghc-human.uart.log covers[513614935,515948618),2333683bytes,
SHA256 96642cd8fa45b83176a5232298605bd653bc108b831a16caa2fe4fe3358e0a61.
Continuous UART and NetShell remain live. Positive screen dragging, full
modesetting, cursor completeness and CPU/GPU ordering remain open. Historical
host fixture compatibility was committed as e5f5d6a779 after this native pass;
no host fixture execution was used for DUT acceptance.

## Viewport-v1 candidate: awaiting native cold25 (2026-09-06)

REQ0034 is granted as OVR0038 through Sep7 16:38:53UTC; OVR0036 and
OVR0037 cover the driver lane and physical KVM recovery. The narrow bitmap
Root.Get/Set handoff permits this repair without staging Claude's ARGB hunks.

The coupled driver change owns LeftEdge/TopEdge, filters those tags out of the
ChunkyBM superclass setter, resolves CursorFB wrappers through a private native
identity attribute, and uses a non-notifying compositor setter. Position changes
hold compositor then display exclusion; the pointer stays hidden through nested
plane selection and logical rollback. Positive Y retains the Nouveau composite
path; negative offsets select a bounded HUBP source viewport with unchanged
storage base. Direct scanout refresh no longer shifts its own backing pixels.
Same-base viewport changes additionally wait for two OTG frame-count advances;
that is a latch opportunity, not proof of the displayed pixels.

Targeted native driver compilation/link passed; generated module start/end and
all affected class/support objects rebuilt. This is NOT runtime acceptance.
Next: freeze/provenance, native file comparison and replacement, physical
off/wait/on, new viewport-v1 boot marker, unchanged native PanProbe-v2 with UART
and HDMI captures. Cold22's negative pixel oracle must improve, not merely its
reported coordinates. Positive drag, repeated switches, cursor, copy and drawing
remain native gates. The independent SDMA expiries remain unresolved.
No MUI/Zune/Wanderer, core or filesystem source was changed. Historical host
plane fixtures are not being used as DUT acceptance for this candidate.

## Cold24 recovery: native desktop available (2026-09-06)

The physical recovery after cold23's screen-drag corruption is complete. No
driver or test binary changed between cold23 and cold24. Fresh UART identifies
fetch-align-v1, and virtual media is detached. Ending only the original boot
console returned0; IPrefs Retry plus known SFS requester cancellations restored
AMDGPU scanout into the owned VRAM bitmap at1920x1080/32bpp. Wanderer remains
running. Capture108 shows its icons, Work, the software pointer and a ready
local Shell5 at explicit CON:700/180/640/480 geometry, without dragging.

Native Info Work: returned0 and reports NVMEZFS1,1726.5G,13.6M used,0errors,
read/write,ZFS0,Work. This is a live mount/status check, not a new filesystem
integrity or persistence test. NetShell remains connected at its prompt and
the continuous UART recorder remains running for human access.

Recovery is NOT a clean acceleration pass: startup/mode restoration logged
three measured SDMA expiries, input/published328/336,288/288,328/336dwords,
RPTR4, with elapsed200000,224679,252712us respectively. Therefore the remaining
failure is not limited to520dword input. SFS requesters were cancelled, not
fixed; the first scaled Cancel click missed below the button, and its corrected
click and later repeated requesters are retained rather than hidden.

Frozen cold24-recovered-human-desktop.uart.log covers[492684945,499774252),
7089307bytes, SHA256
d861741ae2dd8d76f493761564d2b86773552fb0e70b61263b99ac73cfec4626.
It is in JOB-0084-native-completion-20260906 with capture108 and all earlier
failed captures. The primary coherent panning repair still awaits the narrow
bitmap Root.Get/Set handoff (REQ0034/TRIG0919); Claude's ARGB hunks remain
byte-identical and uncommitted here. Read-only SDMA follow-up is recorded in
sdma-after-fetch-alignment.md. No GUI/core/filesystem source edits were made.

## Cold23: aligned ring still expires; cursor visuals now covered (2026-09-06)

b5415bf0bb was built as465040bytes, SHA256
af0b638f9efc762c9266e28174b6cfcce0791dcb6122e5a8af5d4a0374e09451,
installed and native-byte-compared, then physically rebooted. Three native
FileCompare calls and Copy returned0; the last comparison printed IDENTICAL
465040bytes. Final status Echo returned seek219/10; that separate failure is
retained rather than described as a clean install-script exit. Cold23 starts
at UART byte483344529, with the same recorder and virtual media detached.
The fresh loaded-driver marker identifies fetch-align-v1 and EClock1193180Hz.
After ending only the original boot console, IPrefs Retry and familiar SFS
requester cancellations, scanout selected native00110000/1920x1080.

The baseline command was queued twice while startup was blocked on SFS. Both
unchanged Gfx2DConformance runs returned0 and reported18PASS0FAIL. Three actual
SDMA expiries occurred: input520dwords, published528, RPTR00000004,
WPTR00000840, RB_CNTL00801014. Measured elapsed times were268248,238114 and
236717us (legacy382,393,383us); wait_cap0 throughout. Thus fetch-size padding
alone does NOT fix the failure. Correct pixels include CPU fallback; safe
timeout retirement and full acceleration remain unproven.

Unchanged CopyProbe-v1 then passed both512x256SDMA copies, cursor-guard-v1
completion1/2, all131072pixel source/copy/save-under checks, ENDresult0 and
native return0. No additional expiry or exception appeared during CopyProbe.
The new host-only UART-triggered capture helper caught all three real stages:
90 shows the16x16red pointer on the green source at96,160;91 shows it on the
blue destination at800,160;92 shows both rectangles solid green without a
pointer stamp, with the pointer relocated1600,800. This closes the missing
source/destination visual coverage; it is not a full cursor/driver release gate.

Immutable cold23-through-copy-pass.uart.log covers[483344529,490662389),
7317860bytes, SHA256
b616a2d180c777e86e6363a728a339640dc9a0a425b6a2818776a38976e827ae.
Evidence is in JOB-0084-native-completion-20260906 with captioned captures and
source/binary provenance. Unchanged strict GHCv40 then completed1850checks:
284driver dispatch passes,1539generic,4public; fail0,missing6,skipped12,
manual5,displayable1,uncovered0 (including display/interface coverage).
GHC ENDreturn5 and native RunCommand return5 both appeared. The six missing
public operations never invoke the driver. Two further SDMA expiries occurred
during GHC with the same520/528dword,RPTR4,WPTR840,RB_CNTL801014 tuple;
elapsed200003/209756us, legacy391/384us, cap0. These functional checks do not
establish clean acceleration. The SDMA re-arm/quiescence/fence-lifetime path
remains the next isolated engine investigation; no diagnosis is asserted yet.
Panning was not changed or re-claimed as fixed; coherent bitmap ownership and
viewport work remains pending REQ0034/TRIG0916/0919. No GUI/core/FS edits.

After GHC completed and NetShell returned to its prompt, an attempted KVM
window reposition caught the screen bar instead. This was an unintended screen
gesture, not a planned passed test. Capture95 shows repeated Workbench rows and
cursor stamps. UART refresh#1 uses dst=7001118600, framebuffer/src=7001100000
for rect0,13..1920,1080; refresh#4 shifts dst=700113c000 for rect0,32..1920,1080.
This reproduces the known direct-backing panning corruption. One further SDMA
expiry reports520/528dwords, RPTR4/WPTR840, elapsed200000us, legacy390us.
No binary changed; this observation does not establish a new alignment-caused
regression. The failure capture must not be labelled a ready human desktop.

Frozen cold23-ghc-strict.uart.log covers[490662389,492650487),1988098bytes,
SHA256 b025d7c4d25f007f1eded27264ed924ec3cca465e076b16119cf93bfdbe5c6e5.
Frozen cold23-screen-drag-corruption.uart.log covers[492650487,492684944),
34457bytes, SHA256
123d7e8ead9a2a68202480772aec59601a10735a49815ea99ae30e432bc028e1.
Both are retained in the same evidence set beside failed HDMI capture95.

Recovery cold24 uses the unchanged installed driver: physical12s power-off,
wait9s plus off-state checks, then800ms power-on. Off-state UART stopped at
492684945bytes; its recorder was never restarted. The old NetShell listener
was stopped only while the DUT was off; cold24 has a new listener. Off-state
capture96 timed out with no frame and is not a valid image. Cold24 begins at
byte492684945. Its boot and human-desktop restoration are still in progress;
this is recovery, not another candidate acceptance run. Open the local Shell
at explicit CON geometry rather than using another unverified KVM drag.

## Ring fetch-alignment candidate (2026-09-06)

The cold22 520-dword expiry has an independently verifiable ring-contract
mismatch: Linux f377d0025eb0 `sdma_v7_0_ring_funcs.align_mask = 0xf`, and
`amdgpu_ring_commit` pads to that fetch size before publishing the write pointer.
The common AMDGPU submission path now pads the VRAM ring to 16 dwords, without
reading past or growing any caller's fixed-size packet array. A 520-dword input
publishes 528 dwords. Bounds are checked before touching the ring. The marker is
`fetch-align-v1`; timeout/late-retirement records include both input and published
lengths. The early standalone ring diagnostics are unchanged.

This is a source candidate, NOT a proven explanation of the timeout. The
200ms measured deadline, per-submit HALT/reset sequence, fence token and CPU
fallback are deliberately unchanged for discrimination. Required acceptance:
targeted build, frozen artifact, native byte comparison, physical reboot, then
unchanged installed 2D/CopyProbe/strict GHC with retained UART and HDMI evidence.
No host unit-test result substitutes for this sequence.

Panning remains a separate confirmed driver failure. Root.Get/Set coordination
requests TRIG0912/0915/0916 have not received a response; narrow 24h REQ0034
was filed for that file only. Claude's dirty ARGB GetImage/PutImage hunks must
remain untouched and uncommitted by this lane. Do not ship a partial viewport
fix which still allows the inherited mirrored-framebuffer self-copy.

## Cold22 measured timeout and native panning failure (2026-09-06)

99edb6c3bd deadline-v1 driver464568bytes, SHA256
afad537df9c1113522ac85c1c3b78cd33e51acc5ca5130b35f4d3e514f330a0b,
and5823776485 PanProbe-v2 were installed and native-byte-compared on cold21,
then physically rebooted to cold22. Virtual media was detached and UART never
stopped. The candidate marker reports EClock1193180Hz. After known SFS requester
cancellations and ending only the original boot console, native AMDGPU
00110000/1920x1080 was selected and NetShell connected. No binaries changed
between that reboot and these tests.

Unchanged Gfx2DConformance reported18PASS0FAIL, but concurrent4x12 DMA stress
also produced a real expiry: dwords520,RPTR4,WPTR820,RB_CNTL801014,
legacy_us431,elapsed_us227890,wait_cap0. Correct pixels involved fallback.
Thus a short CPU poll alone does not explain the failure. The measured-deadline
change is useful instrumentation, not a fix verdict or safe-timeout guarantee.
The unchanged CopyProbe then passed both SDMA copies, guards1/2, all131072pixel
checks and native return0, without another timeout. Captures70/71 missed the
pointer holds and must not be described as source/destination cursor coverage.
Unchanged strict GHCv40 completed1850checks:284driver,1539generic,4public,
fail0,missing6,skipped12,manual5,displayable1,uncovered0; native return5.
No additional timeout or late-retirement marker occurred during that GHC run.
The six missing public-library operations explicitly did not invoke the driver.

PanProbe-v2 exercised actual1984x1112/depth24 backing. All four initial pixels
passed. UART-triggered HDMI captures72/73 show the four colored blocks before,
then their absence after the accepted-64,-32 pan, rather than yellow at origin.
All four pixel values became202020 and remained wrong after restoring0,0.
ENDresult10, native return10, then Wanderer restored. An SDMA expiry also
occurred during pan: dwords520/RPTR4/WPTR820,legacy429us,elapsed200001us,cap0.
This is a real driver/presentation failure, distinct from v1's setup rejection.

Source trace for the next coherent driver-only correction: generic BM.Root.Set
calls Display.UpdateFB when offset tags change on a visible bitmap; AMDGPU's
Root.Set then invokes a shifted refresh again. Both can act on the direct
scanout's own backing. The driver plane still selects viewport origin0,0,
and no compositor position notification was logged. ADT Nouveau's Root.Set
instead owns offsets, validates and notifies its compositor. Merely preventing
one copy, changing a GUI caller, or updating coordinates is not a completed pan.
TRIG0916 requests narrow Root.Set coordination while preserving Claude's dirty
ARGB GetImage/PutImage hunks unchanged.

Separate SDMA lead, not tested or changed: local Linux f377d0025eb0
sdma_v7_0_ring_funcs.align_mask=0xf and amdgpu_ring_commit pad to the64-byte
fetch size, while this driver's builders pad only to8dwords. Common timeout
length520 is not16dword-aligned. Keep this candidate separate from panning and
from the still-questionable per-submit HALT/reset/constant-fence reuse path.

Immutable cold22-through-pan-failure.uart.log covers[469260276,478491596),
9231320bytes, SHA256
08e4831be9111251f9e5c65aae1bdcfc0745579996fb1cd1567a82f874c44e67.
All files, captures and the marker-driven host capture helper are in
qemu-test/artifacts/JOB-0084-native-completion-20260906. Work stays mounted;
NewShell was requested for human access. No GUI/core/filesystem source changes.

## Cold21 strict run exposes SDMA timeout; deadline candidate (2026-09-06)

The unchanged installed GHCv40 --strict-align completed1850checks on
hidd.gfx.amdgpu:284driver-dispatch passes,1539generic passes,4public passes,
0pixel failures,6missing public ProcessPixelArray operations,12skips,5manual,
0uncovered primary/displayable/display-interface methods. Native return5.
The six missing operations explicitly report that the driver was not invoked.
This is NOT an acceleration pass: during mixed-submission stress, the first
SDMA timeout logged RPTR00000004/WPTR00000820/RB_CNTL00801014 at dwords520,
then CopyBox fell back at row64/128. A later copy succeeded before another
timeout. GHC correctly labels engine execution UNVERIFIED despite correct pixels.
Frozen cold21-ghc-strict.uart.log:2020569bytes, SHA256
05e16b2ba17fe49291f01ab64b889a2a61bc735d87aba351b59377d088c08aae.

The following PanProbe-v1 returned20 before its first pixel/hold stage. UART
shows a custom bitmap and a distinct screen backing allocation; the probe's
pointer-identity guard therefore prevented a pan. OpenScreen legitimately
rejects a non-compositable custom bitmap and allocates a replacement. This is
a probe setup failure, not a measured panning failure. Its allocation/fill also
logged SDMA timeouts. Frozen cold21-pan-setup-exit.uart.log:126793bytes, SHA256
39ff8d3cd88de212723a81b59de8a53b52f13ef78da69df58bd7207d6d1616bc.
Native Wanderer/local Shell remain available; no crash or reboot occurred.

Driver candidate: replace the CPU-speed-dependent200000 cached-fence poll
timeout with a measured200ms EClock deadline. Preserve the old loop boundary
and log its elapsed time only after late completion/expiry, avoiding serial
output during the wait. One timer reference is retained for the resident ring's
lifetime; unavailable timing prevents common-path submission. A finite cap also
bounds a stopped clock. The old ring re-arm sequence remains unchanged to keep
this a discriminator: native late-retirement vs measured-timeout evidence must
decide whether short polling explains this failure. It is not a proven fix,
and expiry is not proof of engine quiescence or safe fallback/storage lifetime.
No GUI/core/FS patches. Build/install/reboot precede all candidate testing.

## Release correction passed on physical cold21 (2026-09-06)

68b75b44cf was built as a 461512-byte driver, SHA256
f7b17779f0b3da25ca3073790f3a1b49e76426493b1f80e1945dc6f340723d37,
installed and byte-compared on cold20, then physically rebooted into cold21.
No binary changed between that reboot and these acceptance tests. Cancelling
the familiar SFS requesters allowed startup to continue; closing the original
boot console and accepting IPrefs Retry selected native AMDGPU mode00110000,
1920x1080. This was not an in-memory replacement test.

The unchanged installed Gfx2DConformance reported18PASS0FAIL, including DMA
roundtrip and4x12 concurrent disjoint-region checks; DMA checkpoints through32
reported fallback0. It returned0 after the completed window was closed.
This is the existing baseline gate, not full format or driver conformance.

The unchanged Work:JOB-0084-AmdgpuCopyProbe-v1 then passed both512x256 SDMA
copies, with cursor-guard-v1 complete markers#1 and#2. Source-cursor-copy,
source-preserved and destination-cursor-restore each checked131072pixels with
bad0. ENDresult0 and native RunCommand return0 followed without an exception
or restart. This closes the specific cold19 release-dispatch fault gate.
Capture54 shows the destination-stage16x16red pointer on the blue rectangle;
despite its filename, it is not the source stage. Capture55 is restored
Wanderer, not the final rectangle hold. Source and final holds have UART and
pixel evidence but were missed visually. Capture56 shows a ready local Shell;
Work remains mounted. No GUI/core/filesystem source was changed.

Frozen cold21-through-copyprobe-pass.uart.log covers byte interval
[456276245,464832369),8556124bytes, SHA256
f0f30122404d95a88eb70020fcddcc072a6e32b388c541caac44d229686ba252.
All artifacts are in JOB-0084-native-completion-20260906; cold19 failed evidence
is retained. Strict GfxHiddConformance, physical panning, overlap/cross-surface
coverage, timing modesets, ordering/lifetime and recovery remain separate gates
in NOUVEAU-COMPLETION-GATES.md. ME/Gallium/Vulkan are not accepted by this run.

## Native cursor-copy fault and release correction (2026-09-06)

Candidate4f32063555 was installed/byte-compared on the DUT, then cold-booted
as cold19. All15HDMI modes registered; the native1920x1080 desktop was selected
after the SFS requester and IPrefs reset were resolved. Installed
Gfx2DConformance completed18PASS0FAIL, DMA upload/download checkpoints1..32
with fallback0. This preserved the baseline, not complete2D acceptance.

The installed CopyProbe verified prior mode00110000/1920x1080 and all131072
source pixels, then entered its first512x256SDMA CopyBox. It faulted before
the guard-completion marker: primary#GP, secondary null-IP page fault in the
exception path, Software Failure80000004, then reboot. No agent power command
caused that restart. Raw cold19-through-copyprobe-fault.uart.log preserves
[440446733,447490606),7043873bytes, SHA256
221ec871a11fb3fb5ccb30fe3429ec2dfe3c23beb73d4e6143a929660a517a15.

Confirmed source/code-generation defect: OOP_DoMethod evaluates its object
argument twice (rom/oop/include/oop.h). Passing access_bm[--held] generated
two decrements, underflowing the one-object release into an out-of-bounds
dispatch. Capture31 already shows the subsequent boot, not passing copy output.
Fix: choose a side-effect-free local bitmap, decrement once, dispatch the local.
This changes only the AMDGPU release loop. A rebuilt/installed candidate and
another reboot are required before repeating the identical native probe.
No native pass is claimed for the release correction at this checkpoint.

## CopyBox cursor bracket candidate (2026-09-06)

The accelerated driver CopyBox path previously read/wrote VRAM without the
public bitmap direct-access bracket. CursorFB therefore could not remove the
source pointer or refresh destination save-under across SDMA. The driver now
obtains both original bitmap objects (not unwrapped/coerced storage), keeps
access through all row submissions and fences, then releases in reverse order.
An identical bitmap is obtained once. Unsupported/changing direct layouts and
SDMA failures release every successful obtain before superclass fallback.

`AmdgpuCopyProbe` is a native public-API discriminator: a private native1080p
screen, two disjoint512x256 rectangles, a visible16x16 pointer, source-under-
cursor and destination-under-cursor copies, pointer relocation, and whole-area
RGB verification. Matching `CopyBox cursor-guard-v1 complete` UART markers and
visible-pointer captures are required; pixel checks alone do not prove SDMA.
The prior public screen is retained/restored; no GUI/core changes are involved.

Driver461480bytes SHA256
1930555b3983f8068b85a1f20d2ae65b5d610689cbb7b2343677674bc23b8204;
probe40848bytes SHA256
279f16609dcc0a3cde0539f4df1280c4a5b361d1b884aa436c63f483a5069051.
Both compile/link successfully, but have NOT been installed/reboot-tested at
this source checkpoint. No host unit tests were used as native acceptance.
The first build invocation incorrectly ran two mmake top-level targets in
parallel, racing genmf's shared temporary makefile; separate sequential target
invocations succeeded. The failed build log is retained alongside the retries
in JOB-0084-native-completion-20260906. Existing unrelated dirty hunks remain.

## Native completion continuation: panning discriminator (2026-09-06)

Cold17 remains the fixed installed 1080p/18-of-18 baseline. The new
`AmdgpuPanProbe` uses only public screen/graphics APIs: retain the prior public
screen, create a 1984x1112 backing bitmap for its exact 1920x1080 mode, paint
independent color sentinels, pan by -64,-32, restore0,0, and verify that panning
never changes stored pixels. It restores the prior screen before closing/freeing
the private one. Coordinate and memory checks are not physical-fetch proof;
UART plane observations and a correctly decoded capture remain separate gates.

The native target compiles/links (38120 bytes, SHA256
b503abb5f502543635b32187a6e3544d27ca84069beee6605b605790a709b6f1).
It has NOT been installed or run. Every binary replacement requires a reboot
before testing. No driver or GUI candidate is deployed by this checkpoint.

Source inspection: the current plane transaction always writes viewport start0;
bitmap Root::Set delegates offset changes to the mirrored-framebuffer base path,
then performs another refresh without notifying the driver compositor. These
are panning leads, not a new native verdict. TRIG-0912 requests ownership
resolution for the existing collaborator ARGB conversion hunks before edits to
the non-overlapping bitmap position handlers. The dirty hunks remain untouched.

The preserved GHCv40 Work copy still needs full verification. Installed minigzip
successfully read/compressed it into a RAM file, returning0. Native NetShell read
all96125 compressed bytes to EOF, but the host received only8192; that gzip is
incomplete and is retained, not a valid hash or driver failure. No filesystem,
common-gfx, Zune or Wanderer correction belongs to this discriminator.

## Full2D priority and transaction candidate (2026-09-05)

The user explicitly prioritizes a fully usable2D driver over further ME probes.
ME command submission does not depend on migrating the desktop framebuffer.
Full2D work was authorized; leaving the display handoff incomplete was the
failure. Do not expand Wanderer guards to mask driver symptoms. Hold the GUI
binaries fixed across driver comparisons, restore native1080p and the fallback
pointer, and validate drawing/blits/scrolling and desktop stability.

Native boot13 read-only DcnStateRead-flip returned0: requested and earliest-in-use
MC8000000000 agree, FLIP_PENDING=0, viewport1024x768/pitch1023, AutoCal1,
SCL_MODE1, recout/MPC1920x1080. No new GPU driver or ME probe ran that boot.
Boot14 power-on19:07:57Z followed the approved long-off/wait8+/short-on cycle;
Work had active references and refused a clean export, so this is a recovery,
NOT a clean shutdown. Continuous UART and boot13-full.uart.log preserve it.

The new private amdgpu_plane transaction engine coordinates linear RGB format,
VMID0/noDCC, actual pixel pitch, viewport, manual AutoCal-off recout, MPC and
unity-ratio float line-buffer configuration under OTG and surface update locks.
It writes address HIGH then LOW, checks the requested state, releases owned
locks and requires no pending flip plus a consistent earliest-in-use address.
Pre-release failures restore the saved fields/address; unverified rollback or
post-release failure reports UNCERTAIN and requires retaining scanned storage.
No writes follow an all-ones MMIO read. Polls have clock and iteration bounds.

Actual-source tests pass normally and with ASan/UBSan, including every field's
rollback/mismatch, dead MMIO at each normal read position, torn samples,
never-latched address and a stopped clock. All15 field offsets/masks are checked
against Linux f377d0025eb0 DCN4.1 headers. The first test compile caught a macro
parameter colliding with a member name; the next caught the viewport START
fields being16bits rather than14. Both failed logs are retained and corrected.
The native Navi44 adapter and serialized allocation/retirement integration now
compile successfully (plane-native-first-build.log). The build regenerated and
compiled amdgpu_start.c after the HWData layout change. All five actual-source
suites (plane, display preflight, VRAM retirement, preparation and refresh) pass
normally and under ASan/UBSan (plane-retirement-tests.log). Active allocations
are deferred on free until a verified new scanout; uncertain outcomes quarantine
the pool and block stale refresh/clear. Wide MC free bounds are checked before
truncation. b605cfb8e8 records this integration.

Driver-only Show/Visible/cursor publication now compiles natively. A separate
display semaphore serializes hardware selection through logical publication;
OOP Visible/base-cursor methods run outside framebuffer/pool locks, preventing
cursor-to-refresh lock inversion. The software cursor is temporarily hidden
while CursorFB and base Show bind the confirmed surface; requested visibility
is restored only after a non-uncertain result. Direct Show no longer returns a
failed candidate as if it were selected. Empty viewport stacks detach cursor and
visible references before screen disposal; physical storage remains pinned.
Six actual-source suites pass normal/ASan/UBSan plus compositor toggle/redraw
regressions (display-publish-tests.log, display-publish-native-build.log).
Candidate20ec7903f2 (459784 bytes, SHA256
63c24b364ba10ee50a10a43e313cc7890c307274ff441cd917842974ed66c2c9)
was opened from RAM during boot14. Registration succeeded, but the hardware
remained on the 1024x768 firmware scanout. This is NOT installed-driver acceptance:
OpenLibrary may reuse a resident basename despite a different path. The user
requires installation, a cold reboot and native 2D tests. Common gfx/Wanderer
remain fixed during this driver comparison.

The cold15 deployment set under test/dut preserves the installed driver and
Startup-Sequence before replacement, verifies contents with a read-only
AmdgpuFileCompare helper, and adds a removable S:DUT-AMDGPU hook before IPrefs.
The hook opens only DEVS:Drivers/amdgpu.hidd, before Wanderer starts. Library-open
success still does not prove scanout selection, cursor, drawing or stability.
Helper host checks pass for full candidate equality and rejected differing,
empty, missing, unreadable and missing-argument inputs; native compilation passes.

Boot14's baseline download stalled after8192 host bytes; the last UART Read
completed4096 bytes successfully. No installed replacement had occurred. Closing
the host bridge and trying local-Shell recovery did not restore progress.
Boot15 is a baseline recovery boot, powered on at20:50:14Z after the authorized
long-off/wait8+/short-on cycle, UART boundary386956391. The cold15 deployment-set
name is not a claim that boot15 already ran the candidate. Native verification
and the following cold candidate boot were pending at that checkpoint.

Installed-copy milestone (boot15): installer return0, explicit IDENTICAL459784
bytes for RAM:amdgpu.hidd versus DEVS:Drivers/amdgpu.hidd. The712-byte hook and
1446-byte Startup-Sequence also matched. Backups in
DH0:Developer/Debug/JOB-0084-cold15-20260905 are amdgpu.before434368bytes and
Startup-Sequence.before1234bytes; AmdgpuProbe40632bytes is installed there too.
UART records backup/loader comparisons returning0, although their expected
stdout lines were absent.

An unnecessary additional read-only comparison using a new -noposixc helper
faulted in its first fopen(RAM:AmdgpuProbe): GP13 then PF14/code15, followed by
an unexpected reboot. This was BEFORE AMDGPU loaded in boot15. It is not a new
driver fault or a clean cold-cycle pass. The failing helper is retained, is not
installed at startup, and must not be rerun. TRIG-0910 asks the userland owner
about runtime/sysroot ABI transitions; no userland fix is undertaken here.
Boot15 immutable UART spans[386956391,396068271),SHA256
61ca6d77509ba5bbb6d975dc1f1d6b530d2fd74759efd4921e0c811f3d9feb2f.

The user confirmed boot15 ran VESA and supplied ScreenMode evidence:
only00100000/VESA:1024x768, no AMDGPU display. Quiet UART/NetShell and short KVM
capture timeouts did NOT establish a whole-machine hang. A55-second KVM capture
returned a frame. Preserve that correction rather than calling it a GPU wedge.

Freeze additional driver fixes while closing the installed/reboot/native cycle.
Every binary change needs installation and reboot before testing. Unit-test
passes and RAM-resident library opens are not DUT display acceptance. Required:
actual AMDGPU display selection,1920x1080 scanout, pointer, drawing/blits,
scrolling and usable Wanderer, with Work mounted. Automatically restarted boot16
is being observed; it is not a controlled cold-power-cycle success. Continuous
UART and all failed/successful artifacts remain in the JOB-0084 evidence set.

### Installed native display milestone, boot16 (2026-09-05 21:49 UTC)

The installed candidate opened from DEVS:Drivers/amdgpu.hidd after reboot and
registered15 HDMI modes. Registration still left VESA1024x768 active. Existing
ScreenMode preferences then selected00110000/HDMI-A-1:1920x1080; Use wrote only
the session preference. EndCLI returned0 and closed the old console; the normal
Intuition screen-reset requester was retried without changing GUI binaries.

The native driver created a1920x1080 displayable bitmap in VRAM and logged
switch_mode1920x1080@32bpp, MC8001100000, centred0,0. This success log follows
amdgpu_plane_apply returning ACTIVE: the transaction checked plane fields,
released its locks and confirmed no pending flip plus the new earliest-in-use
address. This is installed/native plane acceptance, not merely enumeration.
Properly decoded physical captures show native-sized Workbench, a moving red
software pointer and the new900x550 DUT Verification local shell. Its physical
Echo AMDGPU_BOOT16_NATIVE_1080_CHECK returned0 in UART.

No driver/library/executable changed and no unit test ran during this native
sequence. Every subsequent binary replacement still requires another reboot.
Boot16 remains a fault/restart, not a controlled cold-power persistence pass.
The1080p preference is not saved to ENVARC. Sustained2D, blits/scrolling, stable
Wanderer and the next cold boot are NOT yet accepted. Fresh SFS DH0 read-error
requesters repeatedly delay startup and file loads; cancelling an observed
failed read lets progress resume but is not a filesystem fix. Work has not
been unmounted, reformatted or repaired during this iteration.

Capture provenance correction (TRIG-0911): kvm.py's raw HTTP stream can embed
chunk framing inside a saved JPEG (CRLF16139CRLF at offset1988 in
boot16-desktop-mode-check.jpg). These decode as grey/green incomplete images.
curl HTTP dechunking plus byte-exact first-JPEG extraction gives valid captures;
no DUT binary or pixels are modified. The host ffmpeg attempt failed with
libavutil.so.60/vaMapBuffer2 and was abandoned without a host-library change.
Keep the invalid originals, but do not use them as driver rendering evidence.

Evidence: qemu-test/artifacts/JOB-0084-wanderer-recovery-20260905/ in the native
build tree; boot16-http-current.jpg, boot16-1080-depth-check.jpg, netshell-cold16.log.
Immutable boot16-through-native1080.uart.log covers[396068271,403707412),
7639141bytes, SHA256
f20bf87fcb1b407cb67c210f5802332d66827638e3c34a09da277da0df74a6d5.
The sole continuous UART reader remains live; later activity is in that log.

### Installed native 2D checkpoint, boot16 (2026-09-05 22:19 UTC)

With the same installed/rebooted driver and active00110000 display, the existing
C:Gfx2DConformance completed18 PASS and0 FAIL. The checks cover independent
RGB pixel readback, solid fill, ClipBlit, six red-over-blue alpha values, a
176x176 image roundtrip, four workers doing12 disjoint image roundtrips each,
and an obtained-red-pen RectFill. SDMA upload/download counters reported
successful checkpoints1,2,4,8,16,32 with fallback0. Physical, HTTP-dechunked
capture04-gfx2d-conformance.jpg shows white/blue/red and the independent red
fill/raw-pixel/pen panels correctly. These are actual DUT results, not host
unit tests or a RAM-loaded replacement driver. No executable changed in this
test cycle.

The read-only installed DcnStateRead reported viewport/recout1920x1080,
pitch register0x77f, primary address0x8001100000, and OTG master lock0. This
older diagnostic does not sample pending/in-use fields; the earlier driver
plane transaction remains the evidence for those fields.

The larger installed GfxHiddConformance did NOT run: reading its version hit
another SFS DH0 read-error requester and was cancelled. The small probe's
pen4 is genuinely black in the current palette, not a failed red oracle. Its
whole-window clear and malformed lower-panel geometry also limit its visual
oracle. Do not interpret either the SFS failure or test-layout defects as an
AMDGPU regression.

Evidence set: qemu-test/artifacts/JOB-0084-native-2d-20260905-2205 in the native
build tree, registered live with JOB-0084. Immutable
boot16-through-2d-conformance.uart.log covers[405409820,406908867),1499047bytes,
SHA2561c545b17a85842ed2c0058aa553542af7f6e753729b7ee448ed395cd70cd2573.
Full GHC coverage, repeated stability, all modes, cursor damage tests and a
controlled cold boot with saved1080p preferences remain open. This checkpoint
does not establish complete2D or SMP acceptance. Work remains mounted.

### Controlled cold17: same native 2D result (2026-09-05)

After preserving boot16 evidence, power-off12000ms completed successfully;
an explicit8second wait followed observed completion, then power-on800ms
completed at22:31:25UTC. Virtual media was empty. Continuous UART was not
restarted; cold17 starts at byte410660852.

The installed driver opened successfully, registered monitor00110000 and its
15 modes, and again left the existing VESA boot console unchanged. The saved
62byte ENVARC screen-mode preference selected00110000/depth24/default native
dimensions. Its pulled readback matched the active preference exactly,
SHA2563aca49260c2fe3267ff0c48614a9ff375a4bb610a9db666c960dfbdf6a769139.
Previously that saved preference was absent; no old setting was overwritten.

This was NOT unattended startup: the original console prevented Intuition's
screen reset. EndCLI returned0 for that console only; repeated SFS read
requesters also needed cancelling. Wanderer was not stopped or modified.
Once those prerequisites cleared, IPrefs created the native1920x1080 AMD
bitmap and the driver confirmed its plane transaction with switch_mode,
CPU7001100000/MC8001100000,32storage bits, centre0,0. The physical capture
then showed native-sized desktop text, software pointer and local NewShell.

On this fresh boot, the unchanged installed C:Gfx2DConformance again completed
18 PASS and0 FAIL. SDMA upload/download checkpoints1,2,4,8,16,32 again showed
success and fallback0. Capture17-cold17-conformance.jpg corroborates the
colour panels, completed test and local shell behind it. No driver/library
binary or test executable was replaced between this boot and that test.

The broader GHCv40 matrix remains unexecuted. A preserved405248byte artifact
was staged on Work before this reboot. Installed ZFS rejected creation in a
subdirectory, so the unique root filename Work:JOB-0084-GfxHiddConformance-v40
was used instead. Upload acknowledged405248bytes, but full pull verification
stopped at208896bytes on the host despite native reads reaching EOF. That
prefix matches the source, not a full-file identity proof. Retain it as a
failed transfer; do not claim GHC acceptance or modify filesystem/GUI code
to hide these independent blockers.

Evidence set remains JOB-0084-native-2d-20260905-2205. Immutable
boot17-through-2d-conformance.uart.log covers[410660852,417796685),7135833bytes,
SHA2565f98399f31a135926ded2fa578cf4d211f2e622326da7e394e0a1e6c9b62dc55.
The two-boot18/0 result restores a concrete2D baseline, not full2D, unattended
boot, all-mode, sustained-stability, SMP, Gallium or Vulkan acceptance.

## Human verification and restoration contract (2026-09-05)

The user has physically checked the DUT and reports regressions relative to the
earlier usable driver state. Their acceptance criterion is a complete driver:
temporary regressions during active iteration are permissible, leaving them as
the final state is not. Keep Wanderer, a local shell and Work ZFS available for
human verification. Keep continuous serial evidence accessible. A diagnostic
result does not replace desktop or acceleration acceptance.

Known regression ledger:

| Change / observation | Reason or evidence | Required restoration / verification |
| --- | --- | --- |
| Previous 1080p display is now 1024x768 firmware scanout | 2a2000e58b deliberately deferred init-time scanout changes to avoid taking the existing screen away during ME probes; native boot11 confirms GOP viewport retained, GHC identifies VESA | Safe AMD-owned 1920x1080 desktop, correct pitch/format, cursor and visible human verification; not merely driver registration |
| DUT was left in the diagnostic shell without starting Wanderer | Test orchestration omission, not a required ME limitation | Launch the installed desktop and leave a usable local shell; user has since run NewShell, Info and ScreenMode |
| Work volume disappeared after GPU probes | Work was cleanly exported with Mount SHUTDOWN for crash isolation, but not restarted afterward; NVMEZFS1 DOS device node remains | Restart the existing handler/import, verify Work visible/readable, and leave it mounted for human checks for now; no partitioning or formatting |
| Other regressions reported by human | User reports multiple regressions; their individual symptoms have not yet been enumerated | Record and reproduce each as reported; do not equate this list with complete regression coverage |

Existing ME/PFP cold-boot/reuse and compute evidence must remain passing while
display restoration proceeds. Full 2D, sustained submission, SMP, Gallium and
Vulkan remain separate open gates. Host-only fixes must be labelled as such
until tested on the physical DUT. Keep changes scoped and committed so other
agents can bisect them.

Human-session UART evidence: human-session-native11.uart.log, 1528702 bytes,
continuous-log interval [354486419,356015121), SHA256
a86833e65d23e1b0c1991aaa39eac721ff6d4fc7d4f1503305c4dead654bb8b7 in the
JOB-0084-gfx-repeat-native-20260905 artifact directory. Original last write
2026-09-05T16:32:54.681719253Z. It contains the Wanderer launcher, human
NewShell, Info return0 and ScreenMode return0. A launcher returning does not
mean Wanderer exited. No successful Work reimport is present in this interval;
the user was given C:List NVMEZFS1: to restart the retained device handler.
The user subsequently clarified that the DUT is unmanned and available for
agent iteration/recovery; human activity is periodic verification, not a
keyboard-ownership approval gate. Preserve logs and leave the desktop and Work
available between tests. Recover a failed session promptly instead of waiting
for manual intervention.

## Native boot12 recovery and refresh-format isolation

Wanderer later faulted in muimaster `_zune_window_message+0x37`, reading 0x4b0
through a null MUI owner; neither new AMDGPU display candidate had been deployed.
No controllable shell was recovered, so OVR-0035 long-off/wait>=8sec/short-on
recovery restarted the DUT at17:45:15Z. Startup completed and Work imported
successfully (pool GUID cfb474d31eeedd84, TXG6); it remains mounted. This is
not evidence of permanent hardware damage or an AMDGPU root cause.

Zune containment commits bdaf75c26a/d49e9a63f4 resolve events from the live
application window family. Host normal/sanitized tests and targeted native
build pass; native installation and validation pending. Raw UART and build
failures are preserved in JOB-0084-wanderer-recovery-20260905.

Separately, ordinary AMDGPU refresh no longer performs hidden live plane
format/stride programming. History5f8815244c correctly calls for real depth
selection, but doing only that write from a redraw bypasses the modeset lock
and buffer transaction. Hardware format selection remains in the compositor
modeset; the existing RGB565 conversion is only for mismatched source/active
scanout formats, not a replacement for completing that modeset. Actual refresh
tests pass normal/ASan/UBSan for RGB565 conversion, padded same-format copy,
unsupported conversion refusal and unchanged hardware metadata. The harness
exposes no plane-programming service. Native build/deployment pending.

This is a bounded correction, not complete display restoration: active buffer
geometry, atomic plane/scaler transaction, latch/retirement acknowledgement and
bitmap/cursor lifetime remain open. Do not deploy this as proof of complete2D.

## Framebuffer extent repair (source / host validation)

The software framebuffer now has explicit fb_width/fb_height independent of the
published/native raster width/height. Initial GOP adoption and private VRAM
preparation initialize these extents. The existing switch hook updates the
pointer, MC address, extent, pitch, pixel size and advertised storage span under
its framebuffer mutex together; it leaves the published raster unchanged.
This still uses the hook's existing post-address success convention and does
NOT add hardware latch acknowledgement or resolve its rollback/lifetime gates.

Refresh validates both advertised storage extents and clips translated wide
coordinates against source storage, display bounds and the selected framebuffer
before forming pointers. Identity direct-scanout updates do no self-DMA. Clear
uses the selected framebuffer extent, not the larger advertised raster. Host
tests exercise the actual preparation, switch, extent, refresh and clear code;
normal and ASan/UBSan runs pass, including padded/smaller surfaces and invalid
extents. The first preparation test failed because its extraction included the
new adjacent helper. Its runner now stops at the function's closing brace and
honors CFLAGS; CFLAGS-only invocations of the old runner were not sanitizer
evidence. Native build and deployment of this new extent change are pending.

These checks bound advertised dimensions; they do not prove allocation
ownership, fix caller-side rectangle overflow before this function, or solve
general non-identity alias/overlap and caller-owned bitmap retirement.

JOB-0084, physical B550 DUT / RX 9060 XT. Graphics ME now has FOUR independent
cold-boot memory-fence passes after valid CSIB/SRM initialization (boots8/9/10/11),
and boots10/11 also pass32 unique ME/PFP pairs without reset/remap/wrap.
Compute has eleven patched cold-boot shader passes on attempt1, with nine bounded
two-submit queue-reuse passes after the HQD drain fix. This is not sustained
graphics, SMP, complete2D, Gallium or full RADV validation. See
COMPUTE_DISPATCH_STATUS.md for earlier runs; failed hypotheses are retained below.

## Native boot11: graphics repeat and deferred display registration PASS

Commit2a2000e58b changes display preparation, not command submission: reserve
owned VRAM while preserving the existing GOP plane, and map the entire aligned
pool. Host actual-helper tests and ASan/UBSan pass, including unchanged original
framebuffer, native clear, same-geometry seed, full pool bounds and mapping
failure. The harness offers no scanout/viewport/cursor mutation functions.
The exact incremental driver build passed with a ULONG sentinel conversion
warning; collaborator dirty inputs were preserved. Driver445160bytes SHA256
f932f34910535f168c113194c6f476699b6d7922440bfa2770328dc71048ba82.

Cold power-on14:25:11Z, UART boot boundary345172293 and probe351428998.
Startup completed and Work exported/Mount0. Networking was enabled to stage
both candidates through NetShell; the connection quit before the probe but the
stack remained enabled. Thus this differs from boots8/9/10's network-off runs.
Staged path DH0:Developer/Debug/JOB-0084-defer-20260905/amdgpu.hidd; installed
baseline and old candidates unchanged. Full transfer ACK, Copy0 and native size
verified, but no on-DUT full binary hash roundtrip is claimed.

Both initial ME/PFP fences600df00d;32/32 unique pairs pass, guards intact,
RPTR/WB/WPTR330, CP_STAT0, max48polls. Shader600dc0de/EOP600df00d/write600dbeef
on attempt1 and A1/B2 reuse pass again. SDMA linear and scanline copies retire
correctly; experimental SUBWIN mismatches remain failures, not hidden passes.
Registration returns0 with monitor00110000. New private1920x1080 buffer is at
MC8000800000; actual post-registration scanout stays8000000000,1024x768,master1.
Probe0 and script0 shell resume. This proves deferred registration does not
steal the firmware scanout; it does NOT prove an AMD-owned screen is displayed.

Evidence buildroot qemu-test/artifacts/JOB-0084-gfx-repeat-native-20260905/
deferred-native11.uart.log302990bytes captured15:09:24.836167411Z, last output
15:08:33.885486344Z; SHA256
93c9c25dfdcb90e02a967776ea26fefa8d5f22cd005711ef9d60f99a664b9866.

GHCv39 on boot10 identified VESA, not AMDGPU, and stopped when it reported via
printf while holding CursorFB direct access. Checkpoint158657ef34 retained the
existing harness; fixdb09a00a77 releases access before reporting and marks v40.
Native GHCv40 --strict-align completes on boot11 against the installed common
stack, crash-tests off. Both direct-access results pass and all1850 checks reach
the summary and return10; no deadlock. Counts:29VESA-driver dispatch passes,
1793generic,4public,1FAIL,6MISSING,12SKIPPED,5MANUAL. The failure is displayable
unaligned odd-modulo ARGB32 PutImage; missing cases are public ProcessPixelArray
no-ops. This is not a clean result or AMD2D acceptance. Full raw
ghc-v40-vesa-native11.uart.log2424520bytes captured15:18:03.640420508Z, SHA256
3c257a0f3e77065f5cb2e50d890eb3f7fc729318f41a462c7bb00518b377865f.

### Read-only display discriminator and pitch fix; live modeswitch still gated

Standalone DcnStateRead a3c00276b0 links no AMDGPU init code and accepts no
addresses or write arguments. Fixed1002:7590/c0, memory decode already enabled,
512KiB BAR5 and DMU2=34c0 from prior discovery are required. All18 direct
configuration offsets/base indices compare exactly to Linuxf377d0025eb0.
Native38512byte executable SHA256
8f147b5164775b953f265542213a3f07154a8d91c91124688b00ef8396ee99a8 uploaded via
NetShell ACK38512, Copy0, then returned0. No GPU/config writes or plane lock.

Boot11 still has firmware plane MC8000000000,viewport1024x768,format8.
PITCH register000003ff confirms pixel stride minus one, agreeing with Linux's
hubp401_program_size. AutoCal1 is enabled, SCL_MODE1 and boundary-mode1; recout
and MPC size remain1920x1080. Manual recout is therefore not yet established:
Linux dpp401_dscl_set_recout explicitly depends on AutoCal being off. No manual
scaler/plane update has been attempted on this boot.

Raw dcn-state-native11.uart.log67608bytes, captured15:24:24.728281149Z, SHA256
427c09a5daa5d986f55917e160ae20938e8b7c7f40851e7b2c9a1c865070860a in the same
evidence directory. Sequential raw register reads are not visible-pixel proof.

Fix3effa82b89 corrects viewport pitch-1 and refuses zero/overflow geometry or a
stride smaller than visible width BEFORE any MMIO. Actual production-helper
host tests reject the old encoding and pass normal+ASan/UBSan: all four pipes,
1024/1920 widths,1366 width with1376-pixel aligned stride, min/max fields,
invalid-device/bounds no-write checks, and readback-mismatch refusal. It does
not repair the caller's current width-as-stride assumption or provide plane
rollback. Commit4178c2e2be separately removes the ULONG sentinel build warning.
The new exact driver build passed with no compiler warnings in either rebuilt
source file. Immutable candidate445192bytes SHA256
e465682b938cfc12497538614388300c282c728a0206a4ce45cbc8abbc145ff8 is in
pitch-encoding-candidate/; shared build lock released dirty. Neither change is
deployed on the DUT; boot11 still has the tested2a2000e58b driver resident.
Final ten-suite host regression sweep passes, including CSIB/MQD/register
comparisons against the local Linux headers. Captured command-invocation errors
(missing/wrong Linux include arguments) are retained separately from the verified
passing log; they were not source failures. Viewport sanitizer tests also pass.

Remaining prerequisites for an AMD-owned screen test are failure-atomic plane
and scaler programming, actual bitmap pitch/full-span validation, active-plane
metadata/copy geometry, and compositor retention when a switch fails. At
f73f63e758 the code still ignored the switch result before disposing its old
composite and continued plane writes after failed lock acquisition. These are
source-proven gaps, not reasons to mislabel the VESA run as AMDGPU acceptance.

### Display preflight and failed-lock repair (host verified, not deployed)

The switch hook now serializes plane updates with the framebuffer semaphore
and requires successful hardware lock acquisition before any plane write. The
bounded lock helper refuses an existing/unknown lock, cancels its own timed-out
request when MMIO still responds, and makes no further writes after all-ones
device loss. Actual-helper tests cover all four pipes, immediate/last-poll
success, timeout, pre-existing request/status, absent device and invalid input.

The OOP-facing hook now reads actual bitmap BytesPerRow and uses its pixel
stride (including padding), validates dimensions/format without truncating
IPTR attributes, bounds the entire advertised bitmap storage inside the pool,
and rejects CPU/MC overflow, an unaligned base or a surface outside the DCN
48-bit address range. This is pool-range validation, not a per-allocation
lifetime proof. Production-hook tests pass normal and ASan/UBSan for padded
1366/1376 stride, both supported byte depths, exact pool end, malformed inputs,
full-height overrun, failed-lock zero plane writes and later-helper refusal.
The existing viewport tests also pass. No native run of these changes yet.

Test-development correction retained: an initial hypothesis that ModeID was
32-bit was disproved by rom/hidds/gfx/include/gfx.h: HIDDT_ModeID is IPTR.
There was no demonstrated GetAttr stack overwrite; the fixture and source were
corrected before commit, and wide ModeIDs remain valid. Likewise the address
low register has a full 32-bit field, not an eight-bit truncation mask; the
256-byte alignment check enforces our bitmap allocator's contract. The high
register's 16-bit field is the verified reason for the 48-bit bound.

This increment does NOT provide register rollback, restore AutoCal/scaler
prerequisites, verify surface latching, publish safe active-plane copy geometry,
or fix compositor bitmap lifetime. Those remain required before a live AMD
screen transition. The resident DUT driver and human session are unchanged.

The preflight increment is committed as a02e2588e2; exact native-smp
hidd-amdgpu build PASS. Candidate446984bytes SHA256
a1f94f22b705451d4c0461ea497d24472c6c9c0584b6c4464e784477fd3571c4 is preserved
in display-preflight-candidate/, not transferred. Build lock released dirty
because collaborator source changes and incremental dependencies remain.
Old lock source fails the timeout-cancellation test, old display source fails
the required semaphore test, and a width-as-stride mutation fails the pitch
oracle. The initial old-display test invocation stopped at unused-fixture
warnings; the adjusted runtime discriminator is separately retained.

### Compositor rejection retention (host verified, not deployed)

ToggleCompositing now returns success/failure internally. It prepares a
candidate without disposing the old composite, restores published pointers
before the switch call, and commits replacement/disposes the old owned
composite only after the hook reports success. Allocation failure and switch
rejection keep the old composite and mark pending redraw blocked. Both redraw
entry points honour that flag; callers stop after rejection, and later retry
can clear it. Mode lookup and stack-node allocation failures are also checked.

Production-toggle tests pass normal and ASan/UBSan: failed allocation, rejected
direct/composited transitions, retained cached composite, successful publication
order, unchanged-mode no-op and blocked retry. Actual redraw functions perform
zero queries/copies/fills with blocked geometry; enabled controls still render.
Mutations that dispose the old composite on rejection or remove the redraw
guards both fail their runtime assertions. A pre-existing signedness warning
found by the redraw harness was corrected to use signed rectangle coordinates;
the failed first compile log is retained separately from verified results.

Limits: this does not rebuild/restore a rejected bitmap stack, retain a
caller-owned screen that its owner has destroyed, signal failure through the
public void compositor method, or prove hardware retirement. The current
switch hook returns FALSE before programming the candidate address; disposing
a rejected NEW candidate relies on that contract. Before allowing post-address
failure, verified rollback/retirement must replace this assumption. Even TRUE
still lacks latch/retirement verification, so old-buffer retirement is not yet
native-safe acceptance. Plane/scaler rollback, active-copy geometry and cursor
handoff remain open; do not deploy these partial repairs as a complete driver.

Compositor repair committed00b0ff9771. Exact native hidd-amdgpu build PASS,
including every class rebuilt for the internal compositor layout change, with
no compiler warnings in the rebuilt files. Candidate448160bytes SHA256
ae1bd5c4c544c7e7957302b47c4fe69780d8c1d6cbc83c1c3c146b6d0e378122 is preserved
in compositor-retention-candidate/. It has NOT been transferred or loaded.
The first documentation commit was blocked on an invalid decision Status enum;
corrected to the allowed confirmed value scoped to construction only, without
bypassing the hook. Build started during that documentation correction; the
code did not change during compilation. Build lock released dirty.

## Native boot9: exact CSIB candidate repeats the first graphics pair

Unchanged source527d61a800, driver441376bytes SHA256
8710f147669670b78ff96cdb3eb5ecfbb1eff129cfa7543598b13afbaf249ed3.
Cold power-on2026-09-05T13:16:39Z; UART boot boundary332650307 and probe
boundary336741262. Full startup completed; Work export and Mount return0
preceded the probe, networking off. Both ME/PFP fences600df00d, RPTR/WB10,
CP_STAT0, ME PC1137->1276. Compute shader passed on attempt1, A1/B2 compute
reuse passed, SDMA retired with0/256 mismatches. Probe0 and shell resumed.
This is a second independent graphics cold pass, not queued graphics reuse.

Evidence: buildroot qemu-test/artifacts/JOB-0084-gfx-repeat-native-20260905/
me-csib-native9.uart.log,303217bytes, captured13:37:13.724496777Z,
SHA2566d28605e0bfb1385249ccd02066446e88bb5ce7c2fb458779f28353493cac453.

## Bounded graphics reuse c0b1143491: host and native boot10 PASS

After the initial pair, append32 distinct ME/PFP WRITE_DATA pairs to the same
queue without reset, remapping or wrap (final WPTR330 of1024dwords). Poison both
targets and set neighboring guards before each submission. Both fresh values,
unchanged guards, RPTR/WB retirement and CP_STAT0 must agree before the next
pair. Refuse initial bank/queue/retirement mismatch, incomplete cache flush or
timeout; never retry an ambiguous packet. This is a single-producer diagnostic,
not a public submission API or sustained/SMP validation.

Actual-source host harness passes normal and ASan/UBSan tests: exact packet and
cache ordering,32 unique pairs, stale/partial/wrong values, both guards on each
target, delayed completion, missing retirement/writeback/idle, bad initial queue,
address bounds, partial flush, and failure at pair17 without further submission.
CSIB/MQD/MES-map/alignment/HQD-drain/read-only-register regressions also pass.
Exact native-smp hidd-amdgpu target built successfully. Existing collaborator
driver edits are preserved in the evidence patch; this is not a clean release
tree. Native test uses a separate diagnostic directory, not the installed driver.

Cold boot10 power-on13:58:34Z, boot boundary339063609, probe343154023. Work
clean export/Mount0 preceded the network-off probe. Both first fences600df00d
and all32 follow-up pairs passed. Pair1 ME4d450001/PFP50460001, RPTR/WB/WPTR20;
pair32 ME4d450020/PFP50460020, RPTR/WB/WPTR330. Guards intact and CP_STAT0 on
every accepted pair, max49 polls. Compute shader and A1/B2 reuse passed again;
SDMA0/256 mismatch with retired fence; Probe0 and script0 shell resumed.

Driver444936bytes SHA256
48f8100400f43414966824caa78bdfac5fe7b5824f5b1b5940e68b654ea28191.
Same evidence directory, gfx-reuse-native10.uart.log304130bytes,
captured2026-09-05T14:11:20.708933074Z (last native output14:10:22.900230552Z),
SHA256216c45f3c7a35cb1a0e7681040c5af0fa58a6a953285696a5d4e5218bb31e4e4.
One32-pair pass is not ring-wrap, sustained or SMP validation. No generic
graphics submission API or full rendering pipeline has been established.
The next run uses staged GHCv39 --strict-align against the installed common
stack, crash-tests off, with driver ownership and hardware effects reported
separately; it is not the old frozen-package/five-round acceptance claim.

## Native boot4: requested firmware bases do not become actual bases

Diagnostic commits335f4bf8e4 and75d450d546 add read-only ME snapshots, including
ME's own exception register (older diagnostics read only PFP's).

- Requested ME IC02109d70 / DC0213cc00; immediate readbacks still show PSP
  IC3:f895d000 / DC3:f9700000. They remain unchanged through MES and graphics.
- The direct-load requests are misaligned, but there is no evidence they became
  the executing firmware backing. Aligning those requests alone is not a
  demonstrated solution to the current ME failure.
- ME_EXC is zero through MES, then0000c122 after the graphics test. This records
  the MISALIGNED_ADDR flag and instruction-address fieldc12. PFP has the same
  value while successfully writing its fence; the register alone is not proof
  of a fatal fault or the source of the missing ME write.
- ME fence0; PFP600df00d. Shader and queue reuse still pass.

Native source75d450d546; driver SHA256
fec772677fa2ba309159ea2708bce72e5a8a33c30c4fa09b8fcb3fa67a07afc8.
Raw me-state-native4.uart.log:301991bytes, captured08:21:31.737408425Z,
SHA2565b511597816f738c820f431cd281ec7b45423ffd1b82697af7300386410298af.
Cold-boot boundary303510320, probe boundary307610515 in continuous UART.

## Native boot5: omitting the late reset does not fix the fence

Commit99a9cdb077 removes the experimental ME-only reset/prime after queue setup.
Initial firmware-start resets, queue programming and the two packets remain
unchanged. New snapshots narrow where the exception first appears.

- ME_EXC is zero immediately before unhalt andc122 immediately afterward,
  before packet submission. It therefore predates the omitted late reset.
- ME PC is1137 before the packets andbc83 after. Do not describe this as a
  core frozen at one instruction. Both fixed-PC and moving-PC samples need a
  command-side-effect oracle.
- ME fence remains0, PFP600df00d, RPTR10. Reset removal is not the ME fix.
- Shader600dc0de / EOP600df00d / write600dbeef on attempt1; reuseA1/B2 pass.
- Probe returned0 and the shell resumed; library-open success is not a graphics
  pass. Work was exported, networking off, installed baseline unchanged.

Driver435776bytes SHA256
f8b2a5df6760b13423d8e8be8a1212e9f851c66e3c063c8270a8e0a27d9904b9.
Raw me-no-reset-native5.uart.log:303210bytes, captured08:47:08.975199102Z,
SHA256091f2ab422d06757981220171f0c016885b4292b7db0acc615c448f7c2c159c7.
Cold-boot boundary309610241, probe boundary313710924. Power-on08:33:21Z.

## MES graphics mapping candidate and host checks

Encoders563acacc14 and integration53aa6590b0 implement the missing graphics
legacy-queue handoff. This is a separate candidate, not part of boots4 or5:

1. Use aligned graphics queue/MQD storage and distinct RPTR/WPTR slots.
2. Enable PFP/ME before MES mapping, as in Linux's async graphics sequence.
3. Append GFX ADD_QUEUE and QUERY_SCHEDULER_STATUS to the already acknowledged
   scheduler ring; preserve the compute mapping and prior frames.
4. Require both completion values1 and bounded scheduler retirement, then
   require the expected active graphics HQD, base and controls before submitting.
5. Keep the existing ME/PFP memory-effect comparison; no late reset and no
   forced deactivation/manual HQD fallback on a failed or ambiguous mapping.

The MQD uses Linux's0xa01 quantum and0x00f00709 controls, not only their enable
bits. Reference: local Linuxf377d0025eb0, gfx_v12_0_gfx_mqd_init,
mes_v12_0_map_legacy_queue, mes_v12_0_submit_pkt_and_poll_completion.

Host tests pass: exact layout checks against Linux v12_structs.h and
mes_v12_api_def.h, reserved fields, high addresses/alignment/fail-closed encoding,
and the actual production handoff helper's ACK/query/retirement ordering,
timeouts, error reply, unconsumed-ring refusal and no ambiguous replay. Existing
allocation/drain regressions also pass. The two new tests additionally pass
AddressSanitizer/UBSan. These tests do not execute on the GPU; the first native
mapping result follows.

## Native boot6: actual graphics queue mapped; fence withheld by readback guard

The committed53aa6590b0 candidate ran on a fresh NVMe boot with networking off
and Work cleanly exported. ADD_QUEUE and QUERY each returned the64-bit value1;
scheduler RPTR and WPTR both reached256. Graphics HQD readback is:

- ACTIVE00000001, BASE00000000:00021000: expected active queue and ring address.
- MQD00000000:02104000; legacy CP_RB0_BASE00021000: expected descriptor and alias.
- CNTL00f00789, whereas the submitted MQD requested00f00709.

The control difference is exactly0x80, named TMZ_MATCH in Linux's
gc_12_0_0_sh_mask.h. The exact-equality guard rejected it and intentionally
returned before writing either graphics test packet. This is therefore a native
graphics-mapping milestone, not an ME fence pass or a fresh ME fence failure.
No hardware protection bit was cleared. Next: determine which control fields
are descriptor identity versus mapping/hardware-updated state, correct the
readback check only as justified, then run the native memory-effect test.

Shader600dc0de with both fences passed on attempt1, and reusableA1/B2 passed
again before graphics setup. Probe returned0 and the shell resumed. These do
not establish sustained compute after mapping, SMP or graphics rendering.

Power-on09:00:45Z, boot boundary315712082, probe boundary319802414.
Driver438232bytes SHA256
596063dddf4a1e7a5a0b164c1185cb5643267ef78aff863cb4df56c60028bcb8.
Raw me-mes-map-native6.uart.log:301604bytes, captured09:13:00.071210563Z,
SHA25638d58003577164b22c5f8781f1af4eae576ead6aba82e545a900932889797a82.
Installed driver, partitions and continuous serial remain unchanged.

## Next diagnostic: bounded post-map readback acceptance

Commit46667422aa accepts only CNTL00f00709 or00f00789 for the exact
non-TMZ VMID0 diagnostic MQD, and adds actual MQD-address/VMID checks. It changes
neither the submitted MQD controls nor the live protection registers. Linux
gfx_v12_0_gfx_mqd_init explicitly sets TMZ_MATCH for TMZ queues, so the flag is
not described as universally read-only. Its post-MES addition on this DUT is
observed, not yet a general firmware-semantics contract. The narrow allowance
lets the memory-effect experiment proceed without broad masking or clearing
the bit; it is not a production secure-queue policy.

Host tests accept both exact observed controls and reject each other one-bit
variation, including TMZ_STATE, as well as a changed requested descriptor.
Linux mask/layout comparison, existing allocation/drain/MES handoff tests, and
ASan/UBSan descriptor tests pass. Its native result follows.

## Native boot7: mapping accepted, ME memory write still absent

Fresh boot of46667422aa, power-on10:05:32Z, boot boundary321845336 and probe
boundary325940738. Work clean export and C:Mount return0 preceded the probe;
networking remained off. ADD_QUEUE/QUERY both1, scheduler RPTR/WPTR256, and
the same active GFX queue/address/control readbacks passed the bounded guard,
including MQD identity and VMID0. No protection control was rewritten.

Actual packets now ran: ME fence0, PFP600df00d, RPTR/writeback10,
CP_STAT80038000. ME PC1137 before andbc83 after; ME_EXC remainsc122.
ME_HEADER_DUMPc0001200 is the last-recorded CLEAR_STATE header (opcode12),
not either WRITE_DATA37 header submitted by the diagnostic. It is a lead,
not proof of where execution is blocked. Linux initializes the RLC clear-state
buffer even on PSP boots; our corresponding CSIB setup is absent. Inspect the
live CSIB registers before making that gap the next fix.

This disproves mapping alone as a sufficient fix for the missing ME write.
Shader600dc0de/EOP600df00d/write600dbeef and reusableA1/B2 passed again;
SDMA copy had0/256 mismatches. Probe returned0 and the shell resumed. These
are seven cold-boot shader passes and five bounded reuse passes, not full
graphics, sustained or SMP validation.

Driver438792 bytes SHA256
dfb2749073d1b43221847ad65ef8789a9bd0b37a33f5200cedcb60caacc7ce26.
Raw me-readback-native7.uart.log303589 bytes, captured10:18:09.045256390Z,
SHA25634378895f44759c5621bb78564b55f8b092be3db0ca4600758da8b5d24af3601.

## Live boot7 CSIB discriminator and next cold candidate

Read-only GfxStateRead3fdd3ba3b8 ran after networking was enabled for transfer,
without a reboot or driver reinitialization. It links no driver setup/discovery
code, performs no PCI enable or register/selector writes, and refuses anything
other than the verified1002:7590 revc0/512KiB BAR5 DUT geometry. GC bases are
from this DUT's captured IP discovery, not a portable PCI-ID mapping.

- CSIB address HI/LO and LENGTH are all0. RLC_CNTL1, RLC_SRM_CNTL2: RLC running,
  automatic address increment set but SRM enable clear.
- CP_STAT80038000, ME_PCbc83, ME_EXCc122 and interrupt0 remain observable.
- The late ME_HEADER_DUMP read isdef0def0, not the earlierc0001200. Current bank
  was not sampled or changed; this is not proof of a current CLEAR_STATE stall.
- Tool returned0. Native raw gfxstateread-native7.uart.log60552bytes, capture
  10:36:29.035850730Z, SHA256
  319944669bce68779b5ad932d4f563d2080ab2bf60fe9f4eb9232c456ea8556d.

Missing CSIB/SRM setup is now observed, but its causal role remains unproven.
Candidate527d61a800 builds Linux GFX12's75-dword,6-extent clear-state table in
an unused retained page at graphics-allocation offset32768. Before CP RS64
configuration it checks for a null CSIB and disabled SRM, flushes the buffer,
writes HI/LO/LENGTH, requires exact readback, then enables SRM with auto-increment.
It will not replace an existing CSIB or enable SRM after an unconfirmed address.
No queue, packet, or reset-sequence change is bundled with this experiment.

Host tests compare every dword with local Linuxf377d0025eb0 clearstate_gfx12.h,
test capacity/canary bounds and actual-helper flush/write ordering, bad addresses,
existing-state refusal and each rejected register write. ASan/UBSan and existing
MQD/MES/alignment/drain tests pass. Initial alignment-test extraction failed when
the new layout assertions were placed inside its isolated allocator slice;
moving those assertions beside the CSIB helper resolved this harness dependency.
The failed log is retained. The first native candidate result follows.

## Native boot8: first ME memory-effect PASS after CSIB/SRM setup

Cold power-on10:48:19Z, boot/probe UART boundaries328185487/332285866.
Startup completed; Work export and C:Mount returned0 before the probe, and
networking remained off. Source527d61a800 was staged separately; installed
DEVS driver and disk layout remain unchanged.

- CSIB was null and SRM2 before setup. Requested and actual address02108000,
  length75,6clusters match; requested and actual SRM3 match.
- Same MES ADD/QUERY ACK1, scheduler RPTR/WPTR256, active graphics MQD/ring
  identity and accepted controls as boot7. No packet/readback-policy change.
- ME_fence600df00d AND PFP_fence600df00d, RPTR/writeback10, CP_STAT0.
  This is the actual memory side effect missing from all earlier ME tests.
- ME PC1137->1276 instead of1137->bc83; last headerc0000e00 instead ofc0001200.
- ME_EXCc122 persists on this SUCCESS, so it is not a fatal-fault oracle by
  itself. Do not let older diagnostic wording override the command effects.
- Shader out600dc0de/EOP600df00d/write600dbeef on attempt1; reuseA1/B2 pass.
  SDMA0/256 mismatches, fence retired. Probe returned0 and shell resumed.

This single cold-boot comparison supports missing valid CSIB/SRM initialization
as the ME-blocking setup gap. It tests the two required setup steps together;
it does not isolate CSIB versus SRM individually or establish repeated/sustained
graphics reliability. Next gates are repeated graphics submissions/cold boots,
2D conformance, then Gallium/Vulkan integration and SMP coverage. Queue mapping
alone was insufficient (boot7); complete GPU-stack readiness is not claimed.

Driver441376bytes SHA256
8710f147669670b78ff96cdb3eb5ecfbb1eff129cfa7543598b13afbaf249ed3.
Raw me-csib-native8.uart.log304709bytes, captured11:01:23.616617913Z, SHA256
7197d2edadd0a27608b30ecd50d675e92c7721bcee518e900e1897778b243072.
Numbered result index: me-csib-native8.markers.log in the same evidence set.

## Evidence access and limits

Native build root:
/home/terminills/Documents/GitHub/aros-opal-pc-x86_64-smp

All new UART slices, marker indexes, build logs, hashes and staging records:
qemu-test/artifacts/JOB-0084-me-native-20260905/

Continuous user-accessible UART (do not stop it):
qemu-test/logs/runs/JOB-0087/dut-v16-physical-20260903/dut-serial.log

Each run is registered in ACD. Staging proves full upload acknowledgement,
successful native copies and native sizes; no complete DUT/host binary hash
readback is claimed. KVM streaming is unavailable independently of these tests,
so there is no visual-display validation. Preserve the failed hypotheses and
raw logs. No partition, filesystem format, or firmware-flash changes belong to
this GPU iteration.
