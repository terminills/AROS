# Native Mesa aligned-allocation replay — JOB-0084

2026-09-11, source baseline 7947bcf8f7. This records a candidate and failed
attempts, not the user-required stock Mesa 3D-pixels acceptance milestone.

## Evidence and repair

Cold90 initialized the native RX9060XT/ACO provider, failed framebuffer
attachment0 resource creation, then faulted in util_sparse_array_get+0x1ff
from glGenLists. The latter consumed a noncanonical pointer. These remain
separate observations; the framebuffer failure is not yet attributed.

Commit 966adfdb9ad52b71de7e07de5d79cf4fcfa7ed1c already fixes the AROS
os_malloc_aligned contract. Its hosted Chromium reproduction likewise failed
in sparse-array metadata decoding. The native Ports header and sparse_array.o
still used plain AllocVec, ignoring alignment. The old object calls
os_malloc_aros from _util_sparse_array_node_alloc. This is a confirmed missing
build input, not yet proof that the complete native failure is repaired.

Propagated that committed header change without touching the shared dirty
Mesa patch, build-source stubs, display driver, GUI, filesystem or boot config.
TRIG-1039 informs the shared Mesa owner. The source mmakefile has a pre-existing
dirty fetch no-op; a normal dependency walk does not reapply the port patch.

## Build and capture results

Artifact root: aros-opal-pc-x86_64-smp/qemu-test/artifacts/
JOB-0084-native-completion-20260906 (under the GitHub workspace).

- Bringup91-before-build.json inventories 958 existing header-dependent objects.
- Bringup91-mesa-native-rebuild.log: stopped the initial simultaneous MetaMake
  traversals to avoid overlapping dependency work and unrelated includes fetch.
- Bringup91-rebuild-leaves.log: rejected no-op; missing explicit TARGET meant
  leaf conditionals did not build anything. Unchanged binary hashes caught it.
- Bringup91-rebuild-leaves-r2.log: explicit TARGET, sequential native library
  leaves, six compiler jobs within each; completed 12:40:14Z. Public Mesa and
  provider were actually relinked. Sparse-array object now contains paired
  os_malloc_aligned_aros/os_free_aligned_aros. No whole-OS rebuild is claimed.
- Source launcher commits c695b2b9d7/7947bcf8f7 explicitly select SYS_Output and
  SYS_Error. First helper build's seven LONG format warnings were corrected;
  Bringup91-build-r2.log completed with format warnings promoted to errors.
- Physical DUT launcher self-test passed: DOS stderr and C stdout/stderr
  sentinels reached external UART, result=0, IoErr=0, close_ok=1. UART stamps
  000069b0eca3fc71 through 000069b191cccbd8. This is logging validation only.
- Old NetShell command output file T:nsout remains locked (Open error202)
  after the cold90 gears crash. Command prompts alone did not prove execution.
  The helper self-test ran through the local physical shell instead.

Candidate normal-OS files (not yet native accepted):

| File | Bytes | SHA256 |
| --- | ---: | --- |
| mesa3dgl26-0.library | 32569680 | 9f46573ef23935fca88e265da6c0cb7ffef0d4b1abca16a0bddd10d2c21ac804 |
| radeonsi_native.hidd | 40152424 | 3f1056d4453b3cce047272817333b3705f1b687ee6cd7e3a5eecfc030e1b3b6a |
| AmdgpuRunLogged-v1 | 323128 | 2167fce327cd5f7b915433e6166f7dd52c8ff7a7b3db4408cc22cbaec3966fc3 |

Bringup91-install requires full native hashes before replacement, preserves
both old installed libraries as -before-cold91 backups, checks those backups,
and reads back/hashes the normal installed replacements. Copy BUF4096 is in
512-byte blocks (2MiB), not bytes. A fresh physical boot is mandatory before
the next first GL client. The interrupted RAM transfer is not an installation.

## Recovery and complete existing ownership fixes

Cold90 RAM upload stopped at 12:45:55Z, UART byte1572306039. The local-shell
Echo and pointer check produced no visible response (HDMI1472 at12:49:05Z).
Froze the complete post-resume UART span as
Bringup91-cold90-recovery-transfer.uart.log (2349884 bytes,
SHA256 3605aa1c53231242ea50e04f9e132eb46faf7655994a2a7fcd65e82a1d163846).
No normal files had been replaced. Authorized physical recovery used a
12000ms power hold, more than8s fully off, then800ms on. Original UART recorder
remained untouched. Cold91 is a recovery/deployment boot, not candidate testing.

Further read-only audit found three additional committed port fixes missing
from the native generated C sources: 83b3e5f225 (SpirV extension ownership),
66530eb094 (zombie sampler/shader ownership), and ccc69513ae (tmp_draws
allocation/reallocation/free pairing). Propagated their exact changes to four
generated Mesa files and rebuilt the affected libmesa/public library before
installation. GLAPI Exec-task TLS from5eb49c7af1 was already present; unchanged.
This avoids deliberately shipping known invalid context-cleanup pairings.

Bringup91-rebuild-leaves-r3.log completed13:01:21Z. Candidate-v1 remains
preserved and unused. Candidate-v2 public Mesa is32570656 bytes, SHA256
5613f0171c8f37ac42c3b474f6f8ad20127ec52007df292803b23c3dd6ebe8f6;
provider and logger identities above are unchanged. Bringup91-install-r2
requires the new public-library digest. Native acceptance is still pending.

Bringup91-header-verification.json independently reconstructs the aligned
header from pristine Mesa26.0.0 plus committed966adfdb9a, proving byte-for-byte
identity with the actual compiled header (SHA256
1805697ca0a50a00b5b27ac3ce6bb7ef02ae9e721ed79ed2140c3e535b6d443d).
Bringup91-aligned-v2.pkg.bz2 contains only the two libraries, logger and gated
install script, all with relative Bringup91 names for extraction into RAM.
It is8710858 bytes, SHA256
f708a1a0dd01ea2856e6ab2858265506ddece8fe55cdb67138088aab2a793ef0.
Host archive roundtrip is checked, but is not native deployment proof.

## Native installation passed; post-install boot still required

Cold91 reconnected NetShell at13:04:51Z. The v2 archive upload acknowledged
all8710858 bytes; both native archive SHA256 reads passed. Native Unpack TO
RAM returned0/error0/ioerr0 at000003abfd242442 (its existing unfreed-signal
warning is retained). The extracted install script independently passed both
hash reads before execution.

The explicit-output/error launcher ran Bringup91-install-r2. All8 gated native
hash operations passed: logger and candidate inputs, both normal-library
rollback copies, and all3 normal installed readbacks. Full old digests match
the prior installed baseline. Final native provider PASS at0000059de93e878a,
install-complete line at000005a06470d118, launcher result0/IoErr0/close_ok1 at
000005a0e702f408, and fresh NetShell prompt. This is deployment proof only.

Next graphics test must use a fresh physical boot. During source review,
confirmed that native ac_drm_bo_alloc intentionally rejects GFX12_DCC while
stock texture creation can request it. Earlier direct triangle used
PIPE_BIND_LINEAR. Inspect existing native AMD_DEBUG before choosing the
temporary nodcc control; compression remains a documented missing capability,
not something to silently accept or claim implemented. No new renderer test
has yet run on the installed candidate.

## Cold92: loader read stopped before renderer initialization

Physically powered off (12000ms hold), remained fully off for more than8s,
then powered on (800ms) at13:30:27Z. No virtual media mounted. NetShell
reconnected13:43:24Z. Closed the boot CLI, clicked Intuition Retry, and opened
a fresh local shell after the mode switch. AMD_DEBUG was absent; set/readback
`nodcc` in ENV only, with no SAVE/ENVARC mutation. These are a compression
control, not a software renderer selection or claim of compression support.

First stock `AROS:Extras/Demos/GL/gears -info -exit` launched through installed
AmdgpuRunLogged-v1 at UART00000314f15bf6e5. Separate output/error DEBUG handles
were reported. Continuous HDMI began13:45:14.409Z before launch and retained
20622 packets plus live wall-clock JSONL through13:55:21.278Z.

This attempt did NOT reach RadeonSI, framebuffer creation or glGenLists.
Public Mesa load stopped13:49:20.395Z at UART0000039d64f2605c:
Read handle0x1065156e0, offset17300960, length5347066, destination0x10afedba0;
the ACTION_READ packet was sent but no response appeared. Boot explicitly
confirmed `dma-mask=dword-v1`. No SFS requester was visible in1488; local
keyboard Echo/pointer check in1489 produced no visible response. Original
UART remained byte1597390616 through13:55; no root cause is inferred merely
from the final read. Neither alignment fix nor nodcc is accepted by this run.

Full boot/run preserved as Bringup91-cold92-loader-read-stop.uart.log:
9541360 bytes, range1587849256..1597390616, SHA256
9ea10a7e53b16c4328324e01d3e2b8cc4e705e6b86cdab535acc77f487c74b58.
Bringup91-cold92-run.json records exact installed identities and controls.
Cold93 recovery power-on completed13:56:33Z. No library or driver changed;
the next attempt must again be the first GL client on that physical boot.

## Cold93: old list-allocation crash cleared; later stop is not localized

Physical cold93 reconnected NetShell at14:09:31Z. AMD_DEBUG was absent, then
set/read back as nodcc in temporary ENV only. Screenshot1493 proves the
completed mode transition and volume icons;1494 shows a fresh local shell.
First stock gears launched at UART0000035413b06304 with both output streams
captured. No resident renderer or driver had been replaced since cold91.

The previous cold92 large read completed in this attempt: offset17300960,
length5347066, destination0x10afee0c0 returned the full count/IoErr0 at
000003dcb63924f2. This is non-reproduction, not a proven storage repair.
RadeonSI loaded (including its normal low-address relocation retry), and
reported RX9060XT/Navi44/gfx1200/ACO screen ready at00000678916651ba.
There was no earlier framebuffer resource_create NULL diagnostic. glGenLists
returned at0000068548016c23, clearing the previous util_sparse_array crash.
Hundreds of subsequent list-building GL calls returned.

No frame, GL_RENDERER stdout, FPS, normal return, or cleanup was observed.
UART stopped at14:30:45.049Z, byte1615012296, with GLTRACE255 ENTER glVertex3f.
The generated public GL wrapper admits ENTER when old counter<256 but LEAVE
only while incremented counter<256. Consequently this last unmatched ENTER
is the known instrumentation cap, NOT evidence locating the stop in vertex255.
Screenshots1499/1500 show a gray gears window and failed local Echo/pointer
liveness, with no SFS requester or logged CPU exception. No second context
was launched after the failure.

Full cold93 UART preserved as Bringup91-cold93-post-list-stop.uart.log:
17621680 bytes, range1597390616..1615012296, SHA256
f032cc96f18f138a3d155efdd9b8696afa4f43ba6495bf913954ce9637b2c860.
Two continuous HDMI captures overlap14:27:07..14:31:13, ending14:39:09Z;
wall-clock packet JSONL and manifests retained. Recovery cold94 power-on
completed14:40:20Z after12000ms hold and more than8s fully off.

## Bounded phase diagnostic, not renderer acceptance

Commit caabb7d684 adds NativeMesaGearsPhase.c and its native SDK build script.
It includes unchanged contrib gears.c (SHA256
dcc52bdace248ef0550607bb6a2d01122d3f4fc790fbd7fe1b9e9f1ecc6440a9),
preserving geometry and real GL calls. Bounded phase markers cover list
completion, renderer-string output, drawing, swapping, and shutdown. Every
admitted phase ENTER retains its LEAVE even at the budget boundary.

Bringup94 build completed with only the original gears callback/signedness
warnings; no undefined symbols or format/link errors. Executable907880 bytes,
SHA256 cac53c15c34849c2e780da88bd2dbdf9d33a0bd5ed43e1fafd102be4d7a850d1.
New target AROS:C/NativeMesaGearsPhase-v1 refuses an existing file, requires
two full native input hashes and two full normal-path readback hashes.
Cold94 is deployment only; a physical cold95 reboot is required before launch.
No stock demo, public Mesa, RadeonSI provider, AMDGPU, or GUI binary changes.
The phase run will omit -exit so first frames remain visible if reached.

Cold94 deployment completed: NetShell acknowledged all907880 bytes; the
692-byte install script passed both native hashes before execution. Both
complete candidate hashes passed; the normal-path readback passed both full
hashes at00000370ffabe451. Install-complete000003737a129b48, launcher-finished
0000037400ef30ef result0/IoErr0/close_ok1 and fresh prompt are retained.
Frozen Bringup94-installed-phase.uart.log covers1615012296..1623348115,
8335819 bytes,SHA256
34c94d20201b6e8956c27eb6aabd43443053fa74eef55f6ef2ab1e6a752ccf4a.
No GL client ran on cold94; the physical post-install boot follows.

## Cold95: postboot identity passed; loading failed before diagnostic main

Physical cold95 powered on approximately15:00UTC; native AMDGPU1920x1080
CreateObject00000284839d313b and local shell4 in1506 confirm preflight display.
NetShell connected15:13:37Z. Two complete POSTBOOT normal-executable readback
hashes passed at000002d70df3bcae. The subsequent final Echo attempted seek-8192
on DEBUG output and received unsupported ACTION_SEEK209 at000002d989637567;
Echo/script returned10, logger closed cleanly. This is retained as a separate
output-stream error: hash checks passed, but the whole script is NOT clean.
No shared DOS, Echo or GUI code was changed to mask it.

AMD_DEBUG was absent, then temporarily set/read back as nodcc. The first GL
client was normal AROS:C/NativeMesaGearsPhase-v1 -info through RunLogged,
begin000003ef3ca17d20, with independent output/error handles. Continuous video
began15:19:31.150Z, before launch; video cap extended to40min for future native
loads, retaining live per-packet wall times. It captured20212 packets through
15:29:24.355Z when stopped for recovery.

This attempt did not reach diagnostic main, GL, Gears window, or RadeonSI.
Public Mesa ELF read stopped15:23:22.392Z, byte1632845779:
offset14184320,length3057976,destination0x10ac1a340;
ACTION_READ000004582e788dd3 from Lib & Dev Loader Daemon had no reply.
No SFS requester or CPU trap was present. Screenshot1508 at15:27:47Z shows
failed local Echo/pointer liveness; UART stayed unchanged for more than5min.
This is another pre-render load failure, not the later cold93 list/draw stop,
and the last read alone does not identify which subsystem caused the stop.

Frozen Bringup94-cold95-loader-read-stop.uart.log covers
1623348115..1632845779,9497664 bytes,SHA256
fdf4b744d94617e60700983070825a9961fe3ec955026f715e3cb19315f5ca6a.
PhysicalOFF12000 completed15:29:43Z; more than8s fully off; cold96 power-on
completed15:30:50Z. No installed binary or persistent configuration changed.
Repeat first phase client after recovery; no second context was attempted.

Cold96 retry loaded both large sections completely: .rodata offset14184320,
length3057976,destination0x10ab35240 returned full count/IoErr0 at
000003bfdadf1333; .lrodata offset17300960,length5347066,destination0x10ae379c0
returned full count/IoErr0 at000003df5816fa54. This is non-reproduction, not
a storage repair. The phase program reached main000004754c7ffa15, glutInit
returned00000479c199fab3, and glutCreateWindow entered00000479c3880e00.
Native context creation and pixels are still pending.

Evidence correction: earlier notes mistakenly used17303392 as the cold92/93
read offset. Frozen UART and ELF section headers prove the requested offset
was17300960;17303392 was Seek's returned previous position. Corrected above
and in the run metadata; original logs and git history remain unchanged.

## Cold96: first glEndList does not return

Native RX9060XT/gfx1200/ACO screen ready0000067a7c1828cc; window1 and
glutCreateWindow returned0000068725bc5eff. glGenLists returned list1 at
000006873e7e9bc9 and glNewList returned00000687459bda11. Every first-gear
vertex submission returned: final glEnd LEAVE0000068ad99eb78a, vertices528.
Phase21 ENTER glEndList0000068adb8be31a is the last UART output, at
16:05:09.290281319Z byte1650325711. No paired LEAVE, first draw, swap or FPS.
This is below the256-event cap; an admitted call always retains its LEAVE.
Unlike cold93, the trace therefore narrows the non-return to glEndList.

Fresh1516 at16:07:51.373Z shows gray Gears and no local COLD96-ALIVE Echo
after KVM input. UART remained unchanged through16:13:36Z. No CPU trap or
SFS requester identified the internal cause. The complete cold96 UART is
Bringup94-cold96-endlist-stop.uart.log, range1632845779..1650325711,
17479932 bytes, SHA256
df90c0e34ec26f46d9c562eac68f565b6b49ee7509d1f9979cf0a8450ee4d38e.
Continuous video was closed before authorized physical recovery. Cold97
power-on completed16:15:54Z after OFF12000 and more than8seconds off;
no binaries changed before that recovery.

Next diagnostic: six bounded admissions per Mesa EndList/VBO compile site,
using paired RawDoFmt bug markers outside all vertex loops. A separate
fail-closed native source overlay preserves the shared upstream Mesa diff.
No renderer behavior change, no GUI/storage edits, and no stock-demo success
claimed. Any new Mesa library must be installed, fully hashed and physically
rebooted before running the normal-path phase app again.

## Cold98: recovery and rejected old-library backup gate

Cold97 stopped before NetShell while loading Shell; it did not execute changed
Mesa. Renewed console guards OVR-0056/0057 permit the next native cycle.
Cold98 physical OFF12000 completed19:14:58Z; ON800 completed19:16:03Z,
with more than8seconds fully off. NetShell connected19:29:03Z. Native AMDGPU
1920x1080 CreateObject000002e541663b49 and screenshot1526 at19:32:46Z show
the restored display/local shell. No installed binary changed before this boot.

Diagnostic Mesa source7b39600c7d produced32576992 bytes, SHA256
932d0fab4896bed2d6833ead2240f5cc823fd8e7389aac74ee7500fa5ab59112.
Native package4066328-byte acknowledgement, two full archive hashes, unpack,
and two full extracted candidate hashes all passed. The guarded install then
REJECTED the old-library backup readback before the new normal-path Copy.
Candidate passed0000046c5c52fbb7; old backup failed0000049d4d1d118d;
launcher returned20/IoErr0/close_ok1 at0000049de232a27e.

Expected old public Mesa32570656-byte hash is
5613f0171c8f37ac42c3b474f6f8ad20127ec52007df292803b23c3dd6ebe8f6,
also reverified on the retained host Bringup91-mesa3dgl26-0-r2.library.
The native backup RAM snapshot instead hashes
38fb6fdd31219af395aa40fbbe4da46665482e688f2d9eefe94b11410ee369b1.
Two subsequent independent normal-original-to-RAM Copy commands returned0
but produced respectively7f0af48f9f8947be751f83292cec54081971d89ef8dc46e6052db7136a32fb31
and38fb6fdd31219af395aa40fbbe4da46665482e688f2d9eefe94b11410ee369b1,
both the full32570656-byte size. This establishes inconsistent read/copy
results, NOT an identified SFS/NVMe/Copy/RAM cause. Normal Mesa is untouched.

Failures retained in Bringup98-backup-hash-rejected.uart.log,10304853 bytes,
SHA256 b5655375da99e9e434a285ddbc96b90d4ef24d722476d1dcc1493886dc7555c4;
and Bringup98-original-independent-mismatch.uart.log,1434668 bytes,
SHA256 ee05529d9458dd3cff7f2daaebcb2cc28ca8db2d8b90d8f394af17da59d8129b.
Next bounded deployment step stages the candidate under a new disk filename
and requires two independent disk-to-RAM full-hash readbacks. Original inode
must be preserved by rename, then promotion verified at the normal path,
followed by a physical reboot. No failed old hash is being accepted or hidden.
No changed Mesa execution, first swap, or stock-demo pixels claimed.

Cold98 new-name staging subsequently passed TWO independent disk-to-RAM
readbacks, each hashed twice:000007fa3464b1bd and0000082e7928d7a0 match
932d0fab.../32576992. Stage launcher finished0/IoErr0/close_ok1 at
0000082f2f1d9f23. The original normal file was retained by rename as
AROS:Libs/mesa3dgl26-0.library-cold98-original; verified stage was renamed
into normal AROS:Libs/mesa3dgl26-0.library. Two further independent normal
path readbacks each passed both full hashes at0000092db91ef1af and
000009621066268e; promotion finished0/IoErr0/close_ok1 at00000962c65db33a.
No library execution occurred before reboot. The original failed Copy backup
and all three mismatched old RAM snapshot digests remain separately recorded.

Stage UART2224831 bytes SHA256
e1ea68d1639f3b81195f47868a1e95930f0b0830e4dff93d5df236185b8e6191;
promotion UART1825768 bytes SHA256
e73ded432186abf379ee252a0b0a5a7f4ec203f6d0120bc43ccc8ee518e4977b.
This is verified installation, not a storage fix or rendering acceptance.
Physical OFF12000 completed20:06:46Z; cold99 ON800 completed20:07:48Z,
with more than8seconds fully off. Cold99 begins at UART1670522465 and
requires postboot normal-path identities before first phase-wrapped Gears.
Only public Mesa changed; RadeonSI, AMDGPU, boot/core and GUI are unchanged.

Cold99 reached NetShell20:20:50Z and native AMDGPU1920x1080 at
00000300e225cb56. The new normal public Mesa passed both complete postboot
readback hashes at0000035b23d54473. The unchanged RadeonSI provider then
FAILED its full40152424-byte readback gate at0000038115d2323e:
expected3f1056d4453b3cce047272817333b3705f1b687ee6cd7e3a5eecfc030e1b3b6a,
actuala2931adfd7f1c8ec99f82386c8767280e3059fb22ce84ff86b08d2ed3afbb276.
A separate direct-DOS hash of the same RAM snapshot reproduced that digest.
This is not a second independent disk read or a localized corruption cause.
The script stopped before the later AMDGPU/app identity checks, returning20
with IoErr0/close_ok1 at00000381ab182650. No GL client was launched.

Frozen Bringup98-cold99-postboot-provider-rejected.uart.log covers
1670522465..1679564852,9042387 bytes, SHA256
68882a7053e8705c33f72dfbfc54d4485e0096e10717070e7fa3abb5747e5ecd.
Current boot identifies both NVMe repairs: prp-align=pointer-width-v1 and
dma-mask=dword-v1 at0000004a3c8cd382; DH0 and AROSEFI publish7ffffffc.
Their presence does not explain the new digest failure. User requested the
historical NVMe/SFS commit lookup; no additional storage changes were made.

## Cold99: independent retry accepted, first diagnostic GL client launched

At21:12Z the existing boot remained responsive: fresh NetShell Echo and
HDMI1534 showed native1920x1080, a usable local shell and mounted Work.
The original rejected provider snapshot remains in RAM and frozen evidence.
The DOS hash helper intentionally accepts RAM paths only; a direct normal
path request returned usage without reading or modifying the provider.

Two fresh independent normal-provider-to-RAM copies, Bringup98-provider-r2
and -r3, then each passed both full SHA256 reads with expected3f1056d4...
and40152424 bytes. The r3 PASS marker is00000de94f2e9eab. AMDGPU560576B
matched2aedde96... twice at00000dfafec5aee3; the installed907880B phase
app matchedcac53c15... twice at00000e0d0428feff. Verified649-byte script
Bringup98-final-identities (SHAb8b0e57e0f5548708a7c162b5a5cd5f0b8cc9fa579750c1ffa8e774dc5abd966)
returned0/IoErr0/close_ok1 at00000e0dc109f263 and a fresh NetShell prompt.
Prepared provider restaging/promotion scripts were NOT executed. No
installed binary changed on cold99; no additional reboot was required.
These passing retries do not repair or localize the intermittent read/copy
failure and do not erase the initial rejected sample.

Frozen Bringup98-cold99-identities-accepted.uart.log covers
1691399480..1693797832,2398352 bytes, SHA256
4f8bbd99cce47cc596d3c4cb64a0fd9138b9f5f037923b67d0db292e10218c1f.
HDMI recording Bringup91-cold99-gears-phase-video began21:21:45.303Z.
ENV-only AMD_DEBUG=nodcc was set, then the first GL client of this boot,
AROS:C/NativeMesaGearsPhase-v1 -info, was launched21:23:13Z through the
installed RunLogged helper; begin marker00000e669b8cb8b9. UART continues
through the loader. No 3D pixels or completed rendering test yet.

Cold99 subsequently failed BEFORE diagnostic main. Last UART at
21:25:29.479Z, byte1695170730, is the public Mesa loader's ACTION_READ
00000ecf777ba33b: file offset14187008, length3058776, destination0x114922200.
No reply, GEARSPHASE entry, renderer context or Gears window followed.
Fresh KVM local Echo/pointer input produced no visible or UART change
through21:29Z (HDMI1536/1537). No SFS requester or CPU trap was visible.
This is a pre-render load failure, not a verdict on the EndList diagnostic.

Frozen Bringup98-cold99-pre-gl-read-stop.uart.log covers
1693797832..1695170730,1372898 bytes, SHA256
7aa3f579e775f977e23ea7c1f30e89bdc5b516cb5ee48de06df40c3523d35e1a.
The continuous cold99 HDMI video retained7414 packets through21:30:12.735Z.
Authorized OFF12000 completed21:30:25Z; unchanged-binary cold100 recovery
uses a new NetShell listener and preserves the sole continuous UART recorder.

## Cold100: first EndList localized to a display-list allocator mismatch

Unchanged binaries reached the native1920x1080 desktop after physical ON800
at21:31:26Z. NetShell connected21:44:26Z. The first normal installed
NativeMesaGearsPhase-v1 ran21:46:53Z. The cold99 loader read returned on
this boot; RadeonSI reported RX9060XT/gfx1200/ACO at0000061e2b2d5e34,
and glutCreateWindow returned window1 at0000062ad862f12f.

All528 first-gear vertices and all four bounded VBO compilation sequences
returned. First glEndList entered0000062ef13de6ec. Context/flush/save,
end opcode/hash lock and small-list storage returned. destroy-old-list
entered0000062f2e4811cf and did not return. Primary fault0000062f30395e12:
TLSF rejected a FREE outside its area, block0x10d3a7fe0, through StdC free,
FreePooled, tlsf_freemem and tlsf_freevec. Matching Bringup97 Mesa debug
symbols resolve _mesa_delete_list+0xda0 to native-list-phase/dlist.c1086.
Secondary fault0000062f66e9f016 is IP=CR2=0/error0x15, not the primary cause.

make_list() allocates gl_display_list with CALLOC_STRUCT. On AROS that
uses os_calloc_aros/AllocVec, while normal OPCODE_END_OF_LIST deletes the
object with plain C free. Its early null-Head path already uses FREE.
The owned native overlay now changes only this final object free to FREE
(os_free_aros/FreeVec). Head blocks and Label keep their libc allocator
pairings. Shared dirty Mesa patch, provider, driver, DOS and GUI untouched.
Retain this semantic correction if the temporary phase markers are removed.

Frozen Bringup98-cold100-endlist-destroy-trap.uart.log covers
1702704958..1712513325,9808367 bytes, SHA256
46da7720abca8554b214ac771d9849ccedf630aabc5d802de816ce4a8f21b6ac.
HDMI1548 shows a gray Gears window, not rendered gears. Exact source-level
diagnosis is not runtime acceptance: candidate rebuild, normal-path verified
installation and a physical reboot remain required before retesting.

FindDosEntry contributes4647 per-entry lines to that frozen capture due to
the DOS-wide compile-time DEBUG=1. It is not needed for this allocator
diagnosis. No DOS module or logging setting changed in this correction.
