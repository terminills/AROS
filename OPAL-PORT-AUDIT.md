# OPAL → ADT port audit: USB, NVMe, dos/dos64, exec/kernel SMP

**Date:** 2026-09-18
**Trees:** `aros-adt-src` @ `chromium-clean` (ADT, shipping) vs `AROS` @
`merge-upstream-20260808` (the OPAL playground), with
`aros-adt-src` @ `origin/master` (upstream AROS, `aa58365f`, 2026-09-10) as a
third reference for deciding direction on conflicting hunks.
**Driving symptom:** ADT SMP cannot install to a USB pen drive on bare metal.

## Verdict in one table

| Subsystem | Which tree is ahead | Action |
|---|---|---|
| `rom/usb/pcixhci`, hub classes | **ADT**, with 2 real OPAL gaps | Cherry-pick 2 (+1 optional) |
| `rom/usb/classes/massstorage` | Mixed | Cherry-pick 3 committed; **do not** take the uncommitted work |
| `rom/devs/nvme` | **OPAL**, and ADT has defects OPAL does not | Codex's lane — 6 items, 4 of them corruption-class |
| `rom/dos`, `rom/dos64` | **ADT**, decisively | Cherry-pick 3 low-risk; 1 needs a decision |
| `rom/exec`, `rom/kernel`, `arch/*/kernel` (SMP) | **ADT**, by ~1200 lines | **Do not merge.** 3 non-SMP items only |

The headline: for everything except NVMe, **the merge direction most people
would assume is backwards.** ADT carries corruption- and hang-class fixes that
OPAL lacks, including several that Chromium/Electron depend on to load at all.

## Method, and three traps that invalidate the obvious shortcuts

**1. The two trees have UNRELATED git histories.** They live in one repo (OPAL
is fetched into ADT as `fork/merge-upstream-20260808`) but `git merge-base`
returns nothing. Therefore:

> `git cat-file -e <sha>` succeeding means only that the object was fetched.
> `git merge-base --is-ancestor <sha> HEAD` failing is **NOT** evidence that the
> fix is missing.

This audit's first pass got this wrong and concluded "13 missing USB fixes".
Content comparison then showed 8 of 11 were already present. Compare with
`git diff fork/merge-upstream-20260808 chromium-clean -- <path>` (minus = OPAL,
plus = ADT) or `diff -u` on the working trees.

**2. Version strings are NOT evidence.** `pcixhci.conf`, `nvme.conf` etc. are
bumped inconsistently by both agents working these trees — sometimes per change,
sometimes not at all. A higher `version` does not mean newer code. Every
"X is ahead" claim below is backed by named symbols and quoted lines, never by a
version number.

**3. ADT's history is grafted** (396 commits, root = `a342421c "bump version"`),
so ADT-side commit archaeology is also unreliable. Content only.

---

## 1. USB — 2 real gaps (relevant to the pen-drive symptom)

### PORT: `512d1c39e1` — re-arm the event interrupter on every serviced interrupt

`rom/usb/pcixhci/xhci_hcd.c`, `xhciIntCode()`. ADT still has the pre-fix guard
at **`xhci_hcd.c:3734`**:

```c
        /* Update the hardware dequeue pointer to the next TRB. */
        if (idx != startidx) { /* Don't write same value as per xHCI 2.0 spec */
```

Writing ERDP with `XHCIF_IR_ERDP_EHB` is the only way to clear Event Handler
Busy, and the xHC raises no further interrupt while EHB is latched. A
zero-consume pass — posted TRB not yet visible to the CPU when the cycle bit was
sampled — latches EHB permanently: every later Transfer Event posts to the ring
and never interrupts. OPAL replaced the guard with an unconditional block.

Fix: make the block unconditional (`{`). **This is the best match for "a
pen-drive write that never finishes".** Note the prior recorded correction: this
was NOT the DUT CD-boot blocker, so fix it as the latent bug it is and do not
re-attribute old CD hangs to it.

### PORT: `08fe11deb5` — Address Device CC=4 retry + live-task Signal guards

Two independent halves, both absent from ADT:

- **Address Device retry with slot recycle.** OPAL retries up to 5× on
  `TRB_CC_USB_TRANSACTION_ERROR`, and because a bare re-issue on the same slot
  stays CC=4, it does `xhciCmdSlotDisable` → clear DCBAA → backoff (10→160 ms) →
  `xhciCmdSlotEnable` on a **fresh slot**, re-prepping the input context each
  round. ADT gives up after one attempt. OPAL's commit says this is what had left
  devices un-enumerated on **AMD 17h/19h root ports** — the DUT is B550/3950X.
- **Live-task guards on `Signal()`** in `xhciCompleteInt()` and the port-change
  path, falling back to `checkRHchanges = TRUE`. ADT signals unconditionally,
  which faults on a NULL/stale task and evaluates `1L << -1` on teardown.

### OPTIONAL: `fe699d9981` — `USB=nomsi` + ISR debug gating

Adds `HDF_NOMSI` (`pcixhci.h`), parsing in `pcixhci_init.c`, honoured in
`xhciInit`, to force legacy INTx around the **AMD Ryzen MSI storm**; and gates
two unconditional traces that otherwise firehose COM1 in debug builds.
`grep -rn "HDF_NOMSI\|nomsi" rom/usb/pcixhci/` in ADT returns nothing, and ADT
enters the MSI/MSI-X block unguarded. Worth having on an AMD DUT.

### DO NOT PORT: `27b9271b61` — double-free in `xhciDestroyEndpoint`

OPAL removes a weak-reference fallback. ADT solved the same bug by a better
design that OPAL has no trace of (`grep -rn "ectx_RefCount\|xhc_DevLock"` over
OPAL's pcixhci = **zero hits**): an endpoint-context refcount `ectx_RefCount`
(`xhci_hcd.h:255`) and a teardown semaphore `xhc_DevLock` (`xhci_hcd.h:126`),
with `xhciDestroyEndpoint` returning early unless the caller holds the last
reference. Deleting those two lines in ADT would **leak** the context in the
legitimate "last ref, slot already cleared" case.

### ADT is ahead in `rom/usb/pcixhci` (content-verified, not version-verified)

`ectx_RefCount` refcounting; `xhc_DevLock`; stale-device-on-route discard in
`xhciObtainDeviceCtx`; `xhciEndpointIsIsoch()` + iso tail completion; clearing
`epRing->ringio[]` back-pointers on reap; `xhciLynxPointQuirk()`; a 3-byte
root-hub port-change bitmap for >16-port hubs. **None exist in OPAL.**

Already present in ADT (do not re-port): `8a9feda3fe`, `56a5eee831`,
`0e0a2e06d4`, `73591ac979`, `f402f88528`, `24f687ab73`, `dab23c21f5` (ADT is a
superset — it guards two further `CLEAR_FEATURE(PORT_ENABLE)` sites),
`16dc9e95d3` (file byte-identical), `edd4b1fe56`.

Inventory gap, unrelated to install: ADT lacks `rom/usb/classes/video/` (the UVC
class) entirely.

---

## 2. massstorage — 3 to port, and one patch to leave alone

### PORT (committed, all absent from ADT)

| sha | Change | Why |
|---|---|---|
| `4a4aa07935` | `envec->de_Reserved = 2` — ADT still reads `0xffffffff` | RDB/DosEnvec correctness |
| `d2cf91f88a` | `de_HighCyl` from real capacity — ADT still `envec->de_HighCyl = 1` | geometry for auto-mounted media |
| `3ab068f622` | `ncm_Geometry.dg_TotalSectors = 0` on failed capacity | hardening |

### CANDIDATE, test on its own: the `pb_Environment` IPTR-view bug

From OPAL's **uncommitted** work, this is the one item with reach beyond optical
media:

```c
+    struct DosEnvec *envec = (struct DosEnvec *) rdsk->rdsk_PART.pb_Environment;
-    if((fse = FindFileSystem(ncm, rdsk->rdsk_PART.pb_Environment[DE_DOSTYPE])))
+    if((fse = FindFileSystem(ncm, envec->de_DosType)))
```

`pb_Environment[]` is `ULONG[]` (on-disk RDB) but `AutoMountCD`/`AutoMountFAT`
*write* it through a `struct DosEnvec *` whose fields are `IPTR`. On 64-bit the
reader looks at byte 64 while the writer used byte 128 — so `FindFileSystem`
misses the resident handler for **any** auto-mounted (no-RDB) volume, which a FAT
pen drive is. Same for `de_BootPri` at the `AddBootNode` call. OPAL's own comment
flags the real fix as deferred (build the env in a real `DosEnvec` buffer rather
than overlaying the 80-byte `ULONG` array).

### DO NOT PORT: the rest of OPAL's uncommitted massstorage work

It is mid-experiment: five braided changes plus always-on diagnostics that are
not `D()`-gated — `#include <aros/debug.h> /* [MS-DIAG] ... (revert with the
probes) */`, eight `bug("[MS-DIAG] ...")` calls, retained
`KPrintF("[massstorage] CDDBG ...")`, a bare `{ }` block added only to scope
`rderr`, and broken indentation on one error arm. The logic (truthful
`ncm_HasMounted`, skip FAT no-table probe on `PDT_CDROM`/`PDT_WORM`, CD DosType
fallback, always name a CD `UCD0`) is coherent and worth revisiting **as a clean
patch**, but not in this state.

Also: `rom/usb/pcixhci/xhci_async.c`'s only uncommitted delta is a lone
`#include <aros/debug.h>` whose `bug()` call has already been deleted — dead
scaffolding, ignore.

Already in ADT verbatim: `1302d0d61f` (libraries opened in `libInit`).
ADT is ahead: a unit-attention retry loop in `nGetCapacity` (3 × 10 ms after a
failed READ CAPACITY) that OPAL replaced with a `CDDBG` print.

---

## 3. NVMe — Codex's lane. ADT has defects OPAL does not

Reported as `TRIG-1115`, answering `TRIG-0758`. Ranked:

| Rank | ADT defect / gap | File | Effect |
|---|---|---|---|
| 1 | `page_mask` is `ULONG`, so `~page_mask` is 32-bit and zero-extends | `nvme_prp.c:157` | Discards the top 32 bits of the allocation pointer → faults for any PRP list above 4 GiB. Physically reproduced (`.acd/decisions/ACD-nvme-prp-pointer-width.md`). **One-token fix:** `& ~(IPTR)page_mask` |
| 2 | Submit-failure path does not re-add the request | `nvme_io.c:137-145` | `BeginIO` `Remove()`s an already-unlinked node → `au_IOs` corruption; on SMP, arbitrary memory damage |
| 3 | `ceh_Reply` published before DMA teardown / `io_Error`; `ReplyMsg()` before slot clear | `nvme_queue_io.c:36` | Requester can reuse or free the IORequest while the callback is still filling it |
| 4 | `(struct nvme_lba_range_type *)(IPTR)buffer + 4096` — cast binds tighter than `+` | `nvme_busclass.c:512` | +4096 **elements** = 262 144 B past an 8 192 B buffer, used as a **DMA target** |
| 5 | Empty LBA-range descriptor accepted (`nlb == 0` → `lbaEnd == 0`) | `nvme_busclass.c:531` | NVMe disk entirely invisible; nondeterministic in combination with #4 |
| 6 | No partition-table scan at all | `nvme_busclass.c` | One whole-disk `DOS\1` node; a GPT/MBR target has nothing mountable. OPAL's `68cc613480` adds `nvme_MountAllPartitions` + `bootwait.c`, a second `struct Resident` at `RTF_COLDSTART` priority **−49**, one band above `dosboot.resource`'s −50 |

Plus: 4Kn capacity overstated 8× (`au_SecCnt = nsze << (au_SecShift - 9)` where
`au_SecShift` **is** `lbaf[].ds`), and `CMD_UPDATE` is a `// FIXME: Implement
cache flush` stub so a filesystem flush is acknowledged without one ever being
issued — install durability is not real.

**Documented dead end, do not re-walk:** `91a12c2159` (centralize task-context
completion polling) → reverted by `9207e84b58` ~90 minutes later. **Neither
commit has a body**, so the reason is not recorded in git. The comment the change
deleted states the requirement it broke: one shared INTx pin means a completion
posted while the line is already low generates no new edge, so the submitter's
inline drain is what delivers it. ADT already matches the correct post-revert
state.

**Do not port** OPAL's `[NVME:DIAG]`/`[NVME:WRITE]` layer (`nvme_watchtask`,
`q_diag_*`, ~20 `NVME_WRITE_STAGE_*`, ~16 extra struct members) — observability,
not a fix, and it changes struct layout.

---

## 4. dos / dos64 — ADT is ahead; 3 small ports, 1 decision

### PORT (low risk)

- `f41b2e78` **(a) and (c) only** — NULL guards in
  `newcliproc.c::SetWindowPtrFromConsole` (ADT dereferences
  `((struct FileHandle *)BADDR(fh))->fh_Type` unconditionally at
  `newcliproc.c:28`) and `if (fh && fh->fh_Type)` around the
  `ACTION_CHANGE_SIGNAL` dopacket in `systemtaglist.c`.
- `bac09f26` — `Lock("")` resolves the current directory uniformly
  (`lock.c::InternalLock`). Unblocks posix `stat(".")` on handlers that reject
  `""` (CDVDFS). Matters for the native clang/git lane, not for Chromium.
- `ea583bd1` + `5c6b2b40` — relax `ASSERT_VALID_PTR` to `ASSERT((IPTR)x >= 1024)`
  for caller-owned buffers. **No effect on shipping ROMs** (`ADEBUG` defaults to
  0) but any `ADEBUG=1` bring-up on ADT will `Alert(AG_BadParm)` on mapped
  payload buffers.

### DECISION NEEDED, not a port: `f41b2e78` **(b)**

OPAL and ADT hold *opposite* readings of the same field:

```c
/* OPAL */ me->pr_WindowPtr = (APTR)id.id_VolumeNode;   /* NULL must overwrite an inherited requester */
/* ADT  */ if (id.id_VolumeNode) me->pr_WindowPtr = (APTR)id.id_VolumeNode;  /* a console opened AUTO has no window yet */
```

Neither is provably right from the code. Pick one deliberately.

### DO NOT PORT

- `10a10f62`'s `cliinit.c` half — OPAL identifies a CD boot by `memcmp`ing the
  FSSM device BSTR; ADT **and upstream** key off `de_DosType == 'CDVD'|'CDFS'`.
  Upstream's form is later and avoids reading a BSTR out of the FSSM, which
  matters given the `AUXF_DEVICE` FSSM BPTR-corruption trap. Porting OPAL's
  version is a regression.
- `b547f979` — ADT/upstream widened `dos64_UnsupportedPkt` to accept any nonzero
  error (pfs3 answers `ERROR_NOT_A_DOS_DISK`); strictly broader than OPAL's.
- `85059510` as written — it *rejects* any lock whose `fl_Access` is not exactly
  `SHARED_LOCK`/`EXCLUSIVE_LOCK`. ADT already warns on the same predicate via
  `ASSERT_VALID_FILELOCK`. Port as a warning, or gate it.
- `01cd70f6` (m68k BCPL), OPAL's `[DOS-MISSING-VOLUME]` / `6e4f0661` /
  `[ELF-XINDEX]` tracing, and `9d771762`/`77ca4732` (NexOS dynamic GOT/TLS —
  self-reverted by OPAL's own `0aa2cef2`).

### ADT is ahead here — a blind merge would lose these

1. `4040e367` — **a rejected open must not claim the handle**
   (`fs_driver.c:71` `if (status && !handle->fh_Type)`, plus `open.c:218`
   clearing `fh_Type`/`fh_Arg1` first). Upstream still has the unguarded form.
   Without it, a multi-assign walk reads the second member's file through the
   first member's handler — every `FONTS:TrueType/*.ttf` read 0 bytes on the Live
   CD. **Data corruption.**
2. `16032454` — ELF extended numbering + hoisting the `.got` search out of
   `relocate()`. OPAL scans for `.got` once per relocation section bounded by the
   16-bit `eh->shnum`, which reads 0 for extended-numbering objects — so for
   `cef-blink.library` that is ≈1.9e11 `strcmp()`. **Chromium/CEF cannot load
   without ADT's version.**
3. `ac462fba` — dos64 falls back to `ACTION_SEEK` when a handler answers
   `GET_FILE_POSITION64` but refuses `CHANGE_FILE_POSITION64`. This **is** the
   recorded "dos64 seek mask" fix, and it is **ADT's, not OPAL's**. Without it
   `lseek`/`pwrite` past EOF fail on `SYS:` and Chromium's disk cache reports
   "Could not write stream 0 data".
4. `dos64_packet.c` — ADT (and upstream) set `pkt64.dp_Arg1` and `dp_Arg5`; OPAL
   sets neither, so a genuine 64-bit handler reads the object from garbage.
5. `81077d55` — `DRIVERS:` and `LIBS:` get separate locks. OPAL shares one, so a
   later non-ADD `Assign LIBS:` unlocks every member and leaves `DRIVERS:`
   pointing at freed memory. **Use-after-free.**
6. `12a04e13` — `ReadArgs` reads lines longer than 256 characters (heap `argline`
   that doubles). OPAL cuts a quoted item in two → `ERROR_LINE_TOO_LONG` on long
   Chromium command lines.
7. `finddosentry.c`/`remdosentry.c` corruption guards; m68kemu HUNK routing;
   `dos_platform.h` + `%get_archincludes` build plumbing; 19 catalogs vs 10.

**Merge hazard:** ADT's ELF loader uses `ELF_ST_BIND`, defined only in **ADT's**
`compiler/include/dos/elf.h:218`. OPAL has `ELF_S_BIND` and no `ELF_ST_BIND`
(both are `((val) >> 4)`). Porting ADT's loader into OPAL without that header
will not compile.

Refuted as in-scope: the "vfork chdir orphan lock" fix lives in
`compiler/crt/posixc/__vfork.c`, and `TD_FORMAT` appears nowhere in `rom/dos` or
`rom/dos64` in either tree.

---

## 5. exec / kernel SMP — do not merge

**ADT is ahead by ~1200 lines and by four public-struct ABI additions.** Every
spinlock, scheduler, IPI, trap-entry and `core_interrupts.s` file in
`arch/all-pc/kernel` is **byte-identical** between the trees: there is no
OPAL-only SMP fix at all.

ADT-only, absent from OPAL entirely:

- `spinlock_t` in four public structs — `Task.tc_SpinLock` (**38 uses in ADT, 0
  in OPAL**), `MemHeader.mh_SpinLock` (32 vs 0), `Library.lib_SpinLock`,
  `SoftIntList.sh_SpinLock` — plus `KernelBase.kb_IntrSpinLock`. Merging OPAL
  over this changes public struct layouts: an **ABI break**, not a risk.
- `krnRunExceptionHandlers()` now snapshots the chain under the lock and calls
  handlers outside it — a **live pc-x86_64 deadlock fix**: the x86-64 syscall
  vector dispatches through there and a scheduling syscall never returns to the
  frame, leaking one read-lock hold per task switch until the next
  `KrnAddIRQHandler` spun forever.
- Locking through `stdAlloc`/`stdDealloc`, `nommu_*`, semaphores, ports,
  `signal.c` (+168), `tlsf.c` (+74, incl. two `AllocAbs` corruption fixes),
  `cpu_Switch()` releasing the task's own lock after its context is saved.

**Three recorded "unshipped OPAL SMP fixes" are refuted by content:**
cli-before-`LeaveInterrupt` (`kernel_cpu.c:194-198`) and the 64 GiB/128 GiB MMU
window (`core_SetupMMU()`) are **identical in OPAL, ADT and `origin/master`**;
and `grep -niE 'redzone|red_zone'` over `rom/exec`, `rom/kernel` and
`arch/*/kernel` returns **zero hits in both trees**. The 32-core scheduler stall
and the SMP idle #GP have **no differing code** — open in both. "Disable/Forbid
serialise nothing across cores" is real, and **ADT is the tree that fixed it.**

OPAL's only genuine deltas here are not SMP:

| Item | Risk to port | Note |
|---|---|---|
| Split-PTE pool 32→64 + rate-limited exhaustion warning (`arch/x86_64-pc/kernel/mmu.c`) | **LOW** | ADT hardcodes `* 512 * 32` at the allocation site while the guard uses the macro — **raise both or you get heap overrun instead of graceful degradation**. +192 KB boot memory. ADT's exhaustion `bug()` is ungated: thousands of serial lines = a stall lookalike |
| IA32_PAT + `MAP_WriteCombine` | **MEDIUM-HIGH** | Writes MSR 0x277 on **every AP** inside `core_CPUSetup()`; three coupled parts, partial port silently produces wrong memory types. No ADT consumer today — defer until something needs a fast framebuffer aperture |
| Memdisk `AllocAbs` reserve | **do not port as-is** | Self-labelled TEMP with an ungated `bug()`; its `KRN_MEMDisk*` tags do not exist in ADT, so it is dead code there |
| `debug=memory,serial` comma parse + x86_64 memory-RawIO backend | **LOW** | ADT's `serial_Init(&opts[13])` is a fixed offset that mis-parses a list. Cheapest win on this page |

Do not port: `icid_t` widening (superseded by ADT's `kernel_irqtypes.h`),
`KrnMapGlobal` `length` → `uintptr_t` (upstream **and** ADT say `uint32_t` — an
OPAL fork, and an LVO signature change), hosted AVX gating (native behaviour
equivalent), and the ~4100-line `opal_accounting.h` layer (the one extractable
piece is `ExecOpalTrapFaultAddress()`'s CR2 capture, ~20 lines).

---

## 6. Verification gate

Each port lands as its own bisectable commit with an ACD footer, and **the
known-good case is re-run, not just the failing one.** This is not boilerplate:
on 2026-09-17 a fix for "C:Node then Electron" was pinned into the tree, the
two-app Electron demo regressed, and the symptom was chased for hours as if it
were pre-existing. "The bug moved" is the signature of a regression.

Before any port, record the baseline; after each port, re-run all of:

1. **USB install (the target):** boot the ADT SMP ISO on the DUT, plug a FAT pen
   drive into a **root port**, run `InstallAROS` with the Developer component
   ticked, and capture the complete COM1 log to a stable end. Note whether the
   device is behind a hub — the KVM presents its media through one, and
   device-behind-hub boot is separately known-incomplete.
2. **Two-app Electron (the demo path):** fresh boot, `VS Code + GitHub Desktop`
   in one `ElectronShell`. Pass = two `loading main entry`, two
   `opened Intuition window widget`, extension host started, and a
   case-insensitive serial grep for `software failure|program failed|assertion|
   relwrapper|aborted` returning **zero**.
3. **Chromium loads at all** — guards the ELF-loader and dos64 items above.
4. **A Guru is a crash.** Do not click `More...`; the symbolised trace is written
   to `ENVARC:CrashReports/<...>.report` (`RAM:CrashReports` on a Live CD). Read
   that file.
5. `pc-x86_64` **and** `pc-i386` for anything shared — noting no pc-i386 tree
   exists on this box, so ROM-scope changes are single-target verified only.

## 7. Cross-references

- Relay: `JOB-0099` history; `TRIG-1115` (NVMe, answers `TRIG-0758`);
  `TRIG-1113`, `TRIG-1114` (scope/ownership answers to codex).
- `.acd/decisions/ACD-nvme-prp-pointer-width.md` — the physical reproduction of
  NVMe item 1.
- OPAL's `developer/debug/test/SFS-READ-SHAPE.md` — referenced by `d70ce458a6`
  and explicitly compares against "ADT e50efc8c".
