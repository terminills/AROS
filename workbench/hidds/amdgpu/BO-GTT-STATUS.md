# AMDGPU driver-owned GTT storage

## storage-gtt-v1 — cold45 native storage and retained drawing/copy PASS

Source d5f1f5065b, driver506240B SHA256
6032c42fb4c61e1c1cd0fbf9addf6688f470209edd26158c476bf7720c1e2e2f.
Client66968B SHA256
0e82e9e78c8dc77441ecee6a2ef8df8084f54111a1f74ba30e2312808ce972aa.
Both focused targets built separately after a retained parallel makefile
regeneration failure. Work rollback505216 and installed candidate506240 each
passed native byte comparisons; candidate native0 at00000a7edd3cabbb.
Physical OFF17:24:18/ON17:25:04UTC, empty virtual media. Cold44 fresh marker
0000016b9200fe20; compute600dc0de/EOP600df00d/WRITE600dbeef,32uniqueME/PFP
pairs and all three retained SDMA transfers passed. This is not Mesa rendering.

Cold44 desktop did NOT pass: after the normal interactive startup prompt,
endcli returned0 and Wanderer was retained, but UART stopped17:35:51UTC during
interleaved Shell4 ELF/filesystem and Wanderer lock activity. IPrefs screen
reset requester did not respond to bounded KVM retries. Recorder remained
alive, NetShell never connected, and the GTT client never ran. Cause unknown;
do not attribute this to GTT/GPU or SFS without further evidence.
Same installed binary was physically retried as cold45 without source or
disk changes. Raw bringup and failed desktop slices plus fresh screenshots are
registered in JOB-0084-native-completion-20260906/bo-gtt-v1-provenance.md.

Cold45 OFF17:40:01/ON17:40:54UTC, same506240-byte driver, fresh marker
0000016953f58369/base1069a4c30. Compute,32uniqueME/PFP and three SDMA transfer
checks repeated PASS. Visible SFS requesters were canceled during networking
and desktop loads. NetShell connected before completed startup Shell1 was
closed; native AMDGPU1920x1080 selected at0000037420f09c12. This operational
recovery does not establish the cold44 failure's cause or boot stability.

GTT client Work-vs-fresh-RAM IDENTICAL66968/native0 at0000039ac70b6610.
All GTT+VRAM caps/alignment/zero/pattern/borrow/lifetime/stale-generation/
shared-budget/mixed64slot-pressure/cleanup/recovery checks passed. END0
cleanupPASS000004094987409b, actual native0/error0/ioerr0 at000004096ea4027e.
Frozen cold45-gtt-storage-pass.uart.log347127B SHA256
2ae508dd6102e114190a4114560724476e488759419647ec4c9bc7dda5efef84.

After storage stress, unchanged copy client40848B compared native0, then two
512x256SDMA completions and four131072-pixel bad0 checks passed; fresh
490..492-cold45 images show the expected source/destination/solid final
rectangles. Actual native0 at000004b7dbfc1e61. Drawing18PASS/0FAIL including
4taskx12disjoint DMA and correct fresh672 image, actual native0/error0/ioerr0
at00000523e2657c27. Existing ARGB readback test masks are limited; these are
retained regression checks, not strict full-pixel or full2D/cursor acceptance.
No GPU VA/submission exposure, Mesa rendering, reusable-VA/retirement or
general SMP/soak claim follows from this native CPU-storage milestone.

JOB-0084, 2026-09-07. Uses the cold43-native-proven guarded GART mapper.
Extends the existing BO storage ABI with optional DOMAIN_GTT=2; no vector,
struct-layout or ABI-version changes. Query advertises GTT only when GART is
ready. Existing VRAM allocations and the shared16MiB budget are retained.

GTT backing uses paired AllocMem/FreeMem with an explicitly aligned payload,
rounded/zeroed complete pages and checked padding/address arithmetic. The
entire raw allocation is budgeted. Its GART mapping and raw pointer are private
driver metadata. Device/index/generation handles, multiple borrowed CPU maps,
deferred owner release and stale-handle rejection work across both domains.
Final borrow release unmaps the exact GART mapping before freeing raw RAM.
No GPU use is exposed, so there are no outstanding GPU references in this tier.

Not public GPU VA, Mesa submission, scanout, import/export or client-death
cleanup. Future VA/submit services must add GPU lifetime/retirement references
before exposing these buffers. Internal GART VA is only page-aligned; it is
not the final requested-address/alignment VA contract. VA remains monotonic;
repeated allocations can exhaust the aperture despite free RAM/BO budget.
No VA reuse until native invalidation/retirement proof, and no readiness claim
based on GTT CPU storage alone.

Acceptance: new resident-only AmdgpuGttProbe, focused driver/client build,
frozen hashes, Work rollback and installed byte comparison, physical reboot,
fresh marker, native GTT/VRAM lifetime/alignment/zero/reuse/pressure tests and
retained compute/SDMA/display checks. No RAM-loaded private driver harness.
