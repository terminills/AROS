# Compute dispatch + reliability — status & investigation (2026-08-27)

JOB-0084 (radv on the RX 9060 XT, GFX12/RDNA4). This records the state of the
compute-dispatch path and the reliability investigation, so the next session
starts from findings, not a re-derivation. Companion docs: `GFX_BRINGUP.md`
(IMU/PSP/RLC bring-up), `MES_WEDGE_HYPOTHESES.md` (MES scheduler), `SEAM-ac_drm.md`
(radv seam).

## Update 2026-09-05: ten native shader passes; eight bounded queue-reuse passes

Boots4 and5 repeated the shader and two-submit reuse successes while testing
graphics ME diagnostics and omitting its late reset, respectively. Graphics ME
still failed its fence test. See ME_DISPATCH_STATUS.md for those exact commits,
serial boundaries/hashes and the separate MES graphics-mapping candidate.
The detailed first three boot records below are retained unchanged.
Boot6 also passed both compute tests before its new MES graphics mapping; the
mapping acknowledged successfully, but a control-readback difference stopped
the graphics fence submission. It is not a new ME execution result.
Boot7 accepted that exact readback variation without rewriting the controls,
then submitted the graphics packets: ME0/PFP600df00d. Its shader and bounded
reuse tests passed again. Mapping alone is not a sufficient graphics fix.
Boot8 adds valid CSIB/SRM initialization before CP configuration. Shader and
bounded reuse pass again, and graphics ME now passes its memory-effect test for
the first time (ME/PFP600df00d, CP_STAT0). See ME_DISPATCH_STATUS.md for exact
source/evidence and limits: one graphics cold pass is not sustained/SMP/fullRADV.
Boot9 repeats the exact CSIB candidate: both compute tests and both graphics
fences pass again, with clean queue retirement/idle and Probe0. Graphics now has
two independent cold passes; bounded graphics reuse is a separate candidate.
Boot10 runs that graphics reuse candidatec0b1143491: the shader and A1/B2 compute
tests pass again, followed by32/32 unique ME/PFP pairs with full retirement/idle.
This does not add sustained/SMP or mixed compute/graphics concurrency evidence.

The old "per-boot execute lottery" interpretation below is superseded as the
leading explanation. Ordinary AllocMem did not guarantee 256-byte alignment,
and amdgpu_gart_map deliberately preserves the CPU allocation's page offset.
Yet ring/EOP bases and COMPUTE_PGM_LO encode addresses with `va >> 8`: a
misaligned buffer points the GPU *before* the data written by the CPU.

The 83 completed Aug27 shader-dispatch records separate perfectly on this
property: all eight 256-aligned mappings ran the shader, while all 75 unaligned
mappings failed. Incomplete logs without a shader-result line are excluded.
The new native miss at VA0x0217c510 also encodes a base sixteen bytes too early.
See `alignment-correlation.tsv` in the registered evidence directory below.
This establishes a concrete encoding/allocation bug. Ten patched native cold
boots now pass; a larger run count and sustained submission are still needed
before claiming reliability. Fence-packet and fence-polling defects remain
separate historical findings.

Fix `65478114df` page-aligns all six compute/MES queue buffers, retains their
raw allocations for correct cleanup, rejects unaligned GART mappings, and
aligns the IH bus ring with fail-closed alignment/contiguity checks. Its host
regression harness exercises the actual production helpers over all 4096 page
offsets and allocation/mapping/overflow failure paths: PASS. Native cross-build
passed. `76ce4fd830` adds `AmdgpuProbe LIBRARY=path` for a RAM-staged driver test
without replacing the installed baseline. No whole-tree clean-release claim:
pre-existing dirty driver inputs are retained in the evidence patch.

### First patched native run: one hardware initialization, first-attempt pass

On the physical NVMe-booted DUT, the RAM-staged driver mapped the shader buffer
at page-aligned VA `0x0217d000`. The shader returned `out=0x600dc0de`,
`eopfence=0x600df00d`, `wrfence=0x600dbeef`, `rptr_reg=0x30`, and
`won@att=1/24`. The KIQ WRITE_DATA fence also landed (`0x0c0ffee1`), although
its scratch register remained unchanged. MES reported SET_HW_RESOURCES
status 1; that report alone does not validate the complete scheduler.

The reusable-queue self-test and separate subsequent MEC compute test still
failed. These are distinct remaining paths, not fixed by the one-shot pass.
No claim of reliable multi-boot, sustained, or full-driver operation is made.

The staged filename was mistakenly `amdgpu-aligned.hidd`: its resident
`amdgpu.hidd` initialized successfully, but lddemon's subsequent open by the
requested basename failed, yielding probe return 20. Reopening the canonical
resident name returned 0/error0/ioerr0 without another hardware initialization.
This is **one** hardware pass, not two independent passes. A new probe guard
requires the basename `amdgpu.hidd`; use `RAM:amdgpu.hidd` and a fresh boot to
test a different build. An explicit path still reuses an existing resident.

Evidence: `alignment-native1-complete.uart.log` and its marker index, captured
2026-09-05T04:54:40Z in the directory below. Continuous UART starts this test at
byte 290502132. NetShell acknowledged both complete RAM uploads; optional
binary readback stalled at 17192 bytes, so no complete DUT/host hash match is
claimed. Networking was active for this transfer, unlike the earlier baseline.
The installed NVMe driver is unchanged, Work was cleanly exported, and serial
logging remained active. KVM streaming still returned no frame; physical HDMI
signal loss has not been distinguished from a KVM streaming failure.

### Independent alignment-only boot 2: PASS with networking off

The same original pair was copied locally into the separate diagnostic folder
`DH0:Developer/Debug/JOB-0084-alignment-20260905/` before a real cold power cycle.
Both copy commands returned 0, and native List reported driver 433096 bytes /
probe 39720 bytes. The installed `DEVS:Drivers/amdgpu.hidd` was not replaced.
With no virtual CD, AROSTCP or NetShell, Work auto-imported and was cleanly
exported before the explicit-path probe. The canonical basename opened and the
probe returned 0/error0/ioerr0 on this fresh hardware initialization.

Again: `out=0x600dc0de`, `eopfence=0x600df00d`, `wrfence=0x600dbeef`,
`rptr_reg=0x30`, `won@att=1/24`. The later MEC fence and reusable CQ still failed.
Evidence: `alignment-native2.uart.log` and marker index, captured
2026-09-05T05:19:59Z; cold-boot UART boundary 291304375, probe boundary 295396185.
The intended OpenSSL hash check could not run because that executable is not
installed on the DUT; there is still no whole-binary DUT/host hash comparison.

Separate follow-up candidate `9e55754136` drains an active HQD before later
compute-buffer/context replacement and refuses to overwrite on timeout. Its
production-helper host tests and native cross-build pass; its first hardware
result is recorded below. It was **not** used in alignment-only boot 2. The
first-load shader path is unchanged; multi-client/SMP serialization is outside
this candidate. Source reference: local Linux `f377d0025eb0`,
`gfx_v12_0_kiq_init_register`, which requests a drain before replacing an active
queue's pointers.

### Independent boot 3: queue-drain candidate passes the bounded reuse test

Fresh native boot, no virtual CD or networking, Work cleanly exported before
the probe. The separate diagnostic folder
`DH0:Developer/Debug/JOB-0084-cqdrain-20260905/` contains candidate `9e55754136`
and guarded probe `ee5cc84984`. Full NetShell upload acknowledgements, successful
local copies, and native sizes (434248/40632 bytes) establish staging evidence;
there is still no complete DUT/host binary hash comparison. The installed
`DEVS:Drivers/amdgpu.hidd` remains unchanged.

Observed on a genuine first initialization of this candidate:

- Shader `out=0x600dc0de`, EOP `0x600df00d`, write fence `0x600dbeef`,
  RPTR `0x30`, `won@att=1/24`: third independent cold-boot shader/fence pass.
- The previous HQD drained after 4 polls. The standalone MEC test then
  returned fence `0x00c0ffee`, HQD RPTR `0x05`.
- Reusable submissions drained the previous HQD after 3 polls each; submit 1
  returned `0xa1a1a1a1` and submit 2 returned `0xb2b2b2b2`. Both were zero in
  the two alignment-only boots. This is one native two-submission pass, not
  sustained-load or multi-client/SMP validation.
- SDMA fence `0x5d3af00d` retired; blit comparison had 0/256 mismatched dwords.
- The diagnostic graphics test still had ME fence 0 while the PFP fence was
  `0x600df00d`. Full graphics execution remains unfinished. MES acknowledgement
  banners and these compute self-tests do not establish end-to-end RADV.
- Probe returned 0/error0/ioerr0 and the shell resumed; this return alone is
  not the GPU success oracle.

Evidence: `cq-native3.uart.log` and `cq-native3-markers.log`, raw slice captured
2026-09-05T05:51:08.977916818Z, cold-boot boundary 297358817 and probe boundary
301454119 in continuous UART. The raw slice is 299777 bytes, SHA256
`4cebbc7cbed92f3fa955b8952721dbc72f85db0c94132b27198183eb465c1f92`.
Candidate driver SHA256 is
`f0422018458193689440bd5b50281c0b56f5f7681d1e9c25db628db4665bf2ab`.
Continuous serial remains active. KVM capture still produced no frame before
this probe, so this result contains no visual-display validation.

The existing 24-retry loop is not evidence for an independent hardware lottery:
every retry reused the same misaligned allocation. Do not add broader reset
sweeps to compensate for this address-encoding defect.

### Earlier native late-load baseline (before the alignment fix)

The historical results below remain the starting point, not a fresh proof of
reliability. JOB-0087 now has AROS booting from the Samsung NVMe and a Work
volume passing native 46/46 plus cold-boot 16/16 checks. A first JOB-0084
late-load baseline was run from its local shell after boot completed, with no
CD, AROSTCP, or NetShell active. Work was cleanly exported before GPU probing.

`C:AmdgpuProbe` opened the installed `amdgpu.hidd` and ran the existing hardware
initialization, including its D3hot/D0 transition and IP ladder. This was not a
read-only probe or a newly built driver. Native List reported a 434368-byte
driver, matching the host candidate's size, but DUT/host hash equality has not
yet been verified.

Observed:

- RLC AUTOLOAD COMPLETE, GRBM_STATUS=0x382c, IMU_RESET_CTRL=0x7f.
- Compute shader output, EOP fence, and write fence all zero; rptr_reg=0;
  `won@att=0/24` with the existing full-MEC rebringup loop.
- KIQ rptr=0x17, but scratch remained 0xcafed00d and WRITE_DATA fence stayed
  zero. Prefetch still does not establish execution.
- IP ladder completed; HDMI EDID was valid and 15 modes were enumerated.
- Probe returned 0/error0/ioerr209. Library-open success is not GPU readiness.
- Two subsequent KVM capture attempts returned no JPEG frame. A local
  `Echo GPU_LATE_CONSOLE_ALIVE` returned 0/error0/ioerr0 and the shell resumed
  reading commands: the OS was not wedged. A later direct MJPEG request also
  received no headers within five seconds, so physical HDMI-signal loss has
  not been independently distinguished from a KVM streaming failure.

Verdict: this one late first-load is a compute miss. It neither proves that
late initialization can never help nor isolates a debug-induced timing race.
The existing loop restarts MEC firmware/pipes and reinstalls the queue; it
must not be mistaken for a new, whole-GFX/IP-ladder recovery implementation.
No source fix or new cause is claimed by this baseline.

Evidence: native build directory
`qemu-test/artifacts/JOB-0084-late-native-20260905/`, registered in ACD with raw
`late-first-load.uart.log`, marker index, and STATUS.md. Continuous UART is
`qemu-test/logs/runs/JOB-0087/dut-v16-physical-20260903/dut-serial.log`;
preparation starts at byte284790698, probe at284839524, recovery at285163677.
The probe completed without a system reset; an approved cold power cycle was
then initiated to restore the known NVMe boot and video output. No partition,
filesystem-format, or firmware-flash changes were made in this GPU run.

## Historical investigation through 2026-08-27 (preserved, not current guidance)

The original findings and failed hypotheses below are retained for provenance.
In particular, the per-boot race conclusion and proposed reset strategies are
superseded by the allocation-alignment evidence above. The one-shot dispatch
result does not establish that the reusable submission path works.

## 1. Milestones reached

### 1a. First GPU compute SHADER executed on RDNA4 — DONE
`gfx_v12_compute_dispatch_test()` (amdgpu_gfx.c) ran a real GFX12 compute shader
on the CUs: the output buffer read back `0x600DC0DE`, the sentinel our kernel
writes via `global_store_b32`. First GPU compute in AROS history.
- Kernel: `test/shaders/sentinel_gfx1200.s`, assembled with LLVM-20
  `llvm-mc -mcpu=gfx1200`. `PGM_RSRC1=0x600C0000 / RSRC2=0x84` from the amdhsa
  kernel descriptor. `COMPUTE_*` reg offsets authoritative `gc_12_0_0` (BASE_IDX 0,
  SET_SH_REG offset = `gc_seg[0] + reg - 0x2C00`). `DISPATCH_DIRECT(1,1,1)`.
- Path: the proven one-shot install (`cq_install`) — NOT the reusable submit
  (`gfx_v12_compute_submit`, which has NEVER executed, 0/all-history).
- Evidence: `qemu-test/logs/dispatch-20260827-113259/try-4.log`
  (registered to JOB-0084).

### 1b. radv cs_submit wired to the working path — DONE
The radv winsys already called `amdgpu_compute_submit`, but that routed to the
dead reusable primitive. Re-pointed onto the proven one-shot install.
- `gfx_v12_compute_submit_oneshot()` — inline PM4 via `cq_install`; backs
  `amdgpu_compute_submit` (the winsys entry). Commit `c016676574`.
- `gfx_v12_compute_submit_ib()` + LVOs `amdgpu_compute_submit_ib` /
  `amdgpu_compute_ib_fence_cpu` — INDIRECT_BUFFER bridge for the future upstream
  radv via `ac_drm_cs_submit_raw2`. Commit `698f15520b`.

### 1c. Completion fence — DONE (validated on hardware)
Validated `eopfence=0x600df00d` on a good boot (`dispatch-20260827-195404/try-12`).
The compute path is now fully validated end-to-end: dispatch + cs_submit + fence.
- `RELEASE_MEM` **hangs the MEC** (rptr freezes at the packet boundary regardless
  of DW1 flags; a WRITE_DATA right after DISPATCH_DIRECT lands — so the stall is
  RELEASE_MEM-specific). Replaced with `EVENT_WRITE(CS_PARTIAL_FLUSH)` +
  `WRITE_DATA` (commit `b475f0e35a`). `CS_PARTIAL_FLUSH` does NOT stall — the CP
  consumes the full ring (`rptr_reg=0x30=48`).
- The eopfence then read 0 not from encoding but from **poll timing**: we polled
  the shader `out` (lands first) and sampled the fence before `CS_PARTIAL_FLUSH`
  drained + the trailing WRITE_DATA ran. Fix = poll the eopfence itself until
  `0x600DF00D` (commit `47e2de7442`).
- Encoding (verified against `soc15d.h`): `EVENT_WRITE=0x46` (event `0x07`,
  index `4`); `WRITE_DATA DW1=0x00100500` = `WR_CONFIRM | DST_SEL=5` (mem 64-bit
  GVA). GART memory is COHERENT — the shader's store is CPU-visible without a
  cache flush, so the fence only signals completion.

## 2. The reliability wall — the ~1/7 MEC execute race

The compute path is code-complete, but the MEC executes an installed queue on
only ~1/7 of boots. On a miss boot the MEC PREFETCHES rings (rptr advances) but
does not EXECUTE their packets — confirmed across all three queues:
- KIQ: `kiq_HQD_RPTR` advances 16–23 dw but `SCRATCH_REG0` never changes.
- Compute queue0: `rptr_reg=0` (not even prefetched).
- MES scheduler: consumes SET_HW_RESOURCES (`sched HQD_RPTR=0x80`) then spins in
  a nested linked-list lookup at PC `0x64d9` (`halted@1PC, no trap`).

### 2a. Everything observable is IDENTICAL good vs miss
Rigorously eliminated, in order:

| Hypothesis | Result |
|---|---|
| Missing/unshipped SMP kernel fixes | ELIMINATED — all present in HEAD *and* upstream; race persists with them |
| A static config register we fail to set | ELIMINATED — MEC bring-up byte-identical (`CP_MEC_RS64_CNTL=0x3c000000`, INSTR_PNTR `0800->4078`) |
| Uninitialized driver buffers | ELIMINATED — every buffer is `MEMF_CLEAR` (mdbuf, sched-ctx, all) |
| Malformed SET_HW/packet content | ELIMINATED — byte-correct, and it *sometimes* works |
| RLC autoload incomplete | ELIMINATED — `BOOTLOAD=0x8000003f` COMPLETE, `RESET_CTRL=0x7f` (GFX fully un-reset), every boot |
| Per-install timing window (pipe-reset-retry) | ELIMINATED — see 2b |
| Bring-up event *timing* differs | ELIMINATED — SMU→autoload→RLC→MEC deltas all ratio 1.00 good-vs-miss |
| GFX power/clock-gate (codex GRBM_STATUS2 lead, TRIG-0515) | ELIMINATED — `GRBM_STATUS2=0` (no settle over 1ms), `RLC_SAFE_MODE=0`, `CGCG_CGLS=0`, `CP_STAT=0`, `RESET_CTRL=0x7f` all identical good-vs-miss (commit `4ad02147d6`, run `dispatch-20260827-225047`) |
| IH interrupt block not up | ELIMINATED — `amdgpu_ih_hw_init` runs, "IH RING UP 256KB" |

### 2b. Pipe-reset-retry EXPERIMENT — FAILED, but decisive
`gfx_v12_compute_dispatch_test` was wrapped in a 24× loop: reset+re-activate the
MEC pipes (the proven mec-bringup sequence) then re-install each attempt (commit
`aa405e6610`). Result over 7 boots: try-1..6 = **0/24** (all resets miss), try-7 =
**won@att=1** (a natural ~1/7 good boot, first attempt). ZERO boots won on
attempts 2–24. `0/24` at independent 1/6 would be 1.3% → the rolls are NOT
independent.

**Conclusion: the MEC execute is a per-BOOT state, not a per-install timing
window.** Whether the MEC will service queue0 is fixed for the whole boot, set
during GFX/MEC bring-up, and unchanged by resetting+reinstalling the queue 24×.
Evidence: `qemu-test/logs/dispatch-20260827-172639/`.

### 2c. Net
The execute variance is finer than anything observable at register OR
inter-event-timing resolution. Observe-and-fix (register/timing diffing) is
EXHAUSTED — do not re-run it.

## 3. Remaining strategies (all different in kind, not observe-and-fix)

1. **Full-GFX-re-bringup-until-execute (untested, highest-value).** The
   pipe-reset only re-rolled the pipe; the per-boot state is set by the FULL
   bring-up. Re-run the entire GFX/MEC init (SMU→RLC→MEC) and probe a test
   packet; repeat until the MEC executes, then proceed. At ~1/7 per full-init
   roll, ~15 re-inits ≈ 90%+ per boot. A workaround (makes radv usable), not a
   cure — but usable is what the payoff apps need. This is the one lever the
   per-boot finding has not ruled out.
2. **Match Linux's EXACT compute dequeue path.** We use raw MMIO `cq_install`;
   Linux adds compute queues via the MES/KIQ `MAP_QUEUES` scheduler. Prime
   suspect for a step we omit — but the KIQ shows the same per-boot execute
   variance, so it may not help while the boot is in the "won't execute" state.
3. **Deep MEC firmware RE** (`mec.bin`/`mes1.bin`, RS64=RISC-V per
   `rs64-is-riscv-disasm`): understand the dequeue-to-execute state machine and
   what boot-time condition gates it.

## 4. Commits (this session, branch merge-upstream-20260808)
- `93cb6f4f9b` decision record + `SEAM-ac_drm.md`
- first-shader dispatch → reworked onto one-shot → fence diagnostics/fix chain
- `698f15520b` IB bridge · `c016676574` cs_submit re-point · `b475f0e35a`
  CS_PARTIAL_FLUSH fence · `aa405e6610` pipe-reset-retry
- Decision record: `.acd/decisions/ACD-amdgpu-mesa-vendoring-layout.md`
- Memory: `radv-vulkan-bringup-ladder`, `rs64-is-riscv-disasm`,
  `radeonsi-2d-harvest-into-amdgpu`.

## 5. Next-session start points
- Reliability: try (1) full-GFX-re-bringup-until-execute.
- Fence: close the post-`CS_PARTIAL_FLUSH` `WRITE_DATA` (poll the eopfence after
  the flush drains; or use RELEASE_MEM's data write via a form the MEC accepts).
- Both are DUT experiments gated on the ~1/7 lottery to *measure* — which is why
  reliability (1) is the highest-leverage: it collapses every future test from an
  hour to minutes.
