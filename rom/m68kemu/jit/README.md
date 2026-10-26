# Experimental MC68K JIT: guarded MOVEQ lowering

This directory contains a Linux x86-64 differential harness and an experimental
native AROS backend. The independent library integration is **off by default**;
`SetEnv M68KEMU_JIT 1` opts subsequent executions in. Moira itself is unchanged.

`MoveqCache` lazily generates native MOVEQ register/CCR payloads, one cached
payload per opcode, with a separate cache per owner. Unsupported opcodes,
disabled caches, unsupported hosts, allocation failures and protection failures
return null for interpreter fallback. Linux mappings are created RW and changed
to RX, with no RWX interval. The explicit `M68KJIT_NATIVE_EXECMEM` AROS build
uses 64-byte executable heap allocations instead: this backend does **not**
provide W^X and never changes permissions on shared heap pages. The native ABI
and layout were checked by the standalone payload test. Keep the cache alive
until its generated calls finish. Each cache is task-local, not concurrently shared.

The cache key is the instruction word. It never embeds guest addresses or keys
translations by PC. Use the CPU's prefetched IRD, not a fresh read from guest RAM:
self-modifying code must preserve the interpreter's prefetch behavior. A future
multi-instruction block cache will need its own explicit invalidation contract.

Run `run-host.sh /absolute/private/build-directory` with GCC supporting C++20.
The harness compares every MOVEQ encoding and all 32 initial CCR combinations
against Moira, checks every unsupported word, isolation, same-PC mutation, and
fallback for NOP, A-line, RTE, F-line, tracing, interrupt, model and disabled paths.
The host harness tests both 68040 and 68LC040. Its shared step also compares the PC, prefetch queue, function code,
read buffer, cycle count and internal flags. The shared step is only valid for
nonthrowing ordinary-RAM callbacks. The AROS integration additionally guards the
next prefetch against bounds, the sentinel, custom-chip reads, odd PCs, CPU
model, instruction hooks and nonzero state flags. All rejected cases execute
Moira. The outer instruction loop retains VBL polling, watchdog and PC history.

Timing reports separate runtime code generation, warmup, payload-only cost and a
matched synthetic MOVEQ/BRA loop including fallback. Payload cost and whole CPU
step cost are different operations and must not be presented as a speedup ratio.
Even the matched synthetic loop is not a representative application benchmark.

## Provenance and integration gates

- Relay: JOB-0240, parent JOB-0238, authoritative handoff TRIG-1602.
- Initial source: `bfe4fb2a57ff869fa1f44a95200e2d55500b7d2b`, Claude's
  `uvc-length-fix-20261024`; last committed MC68K change `1a5cb1567a38fac90f756638b13657104a550543`.
- Moira: `1efd69467ca13b27b2fb40febd5cb31dbecdea5f`, unmodified independent clone.
- AROS and Moira Git metadata/objects are independent; no linked worktree,
  alternate object store, shared build or push to Claude's repository.
- Claude's v115 working tree included four uncommitted compatibility files at
  handoff. Those were not imported. A committed checkpoint is required before
  claiming a rebuilt v115 baseline. Copied v115 binaries are reference artifacts.
- Target compiler/SDK baseline is the published 20261023 ISO and contrib SDK.
  The first cross-object probe failed because contrib `type_traits` includes an
  absent `bits/version.h`. No local SDK replacement is permitted to silently
  satisfy this prerequisite. No AROS executable or runtime proof exists yet.

Before enabling JIT in transparent execution, reproduce Claude's full current
corpus with a committed baseline and an exclusively held corpus QEMU slot:
SysInfo SPEED; Aminet `corpus5`; ReAction `rra68`; ClassAct `rdemoc`; subsequently
xSysInfo and every new regression Claude adds. Preserve screenshots and serial
logs, and compare results, not compilation status. Only Wordle's known trap #0
stop is expected in the handed-off GUI batch.

Preserve A-line library thunks, Supervisor trampoline at 0x108, RTE at 0x100,
fault stub at 0x188, VBL polling/injection at the same instruction boundaries,
watchdog counts, planar write mirroring, CIA/custom accesses, task isolation,
exception state and self-modifying code. Keep the outer execution loop unchanged
for the first integration. Any native allocator/cache synchronization prerequisite
must be independently validated using the published SDK before integration.

Shared compatibility fixes must be separate commits delivered to Claude by relay
with a reproducer. Coordinate interface changes before editing the integration
adapter. Publish only to a separately agreed destination. Follow CONTRIBUTING.md
and the current AROS PR policy: no draft or dependency-on-unmerged-PR submissions.

## Preparing a private corpus harness

`prepare-corpus.py --reference-qt /path/to/reference/qemu-test --maps
/path/to/maps-v115.txt --out /path/to/new/private-directory` copies the handed-off
inputs and records their hashes. It adapts the harness to private monitor, disk,
NVRAM, log and screenshot paths, keeps failed images, binds VNC to loopback and
rejects a missing completion marker. It refuses an existing output directory and
never boots a VM. Check/claim the relay runtime scope before a run. The original
reference harness, repository, build and disk remain untouched.

The copied `corpus5` script has historical `Run >SER:` launcher failures for its
CLI entries; those are also present in Claude's v103 reference. Report them as
baseline limitations, not successful CLI tests. `rdemoc` prints `cp done` before
its delayed crash check and has no SHOT markers: keep that VM, capture its page
screens explicitly, and wait for `==CRASHEND` before interpreting the run.

## Endian accesses and native build

`endian_access.h` provides ordinary-RAM 16/32-bit big-endian accessors. The
portable path uses memcpy plus byte-swap on little-endian targets. CPUID gates
MOVBE once per execution; it is never queried per access. The adapter only
uses MOVBE after special-read callbacks and bounds checks. It does not replace
CIA/custom callbacks or planar writes. Compile `endian_test.cpp` with C++20,
`-O2 -mno-movbe`, and run both normally and with `--fallback`. Timings include
matched indirect calls and are microbenchmarks, not application speedups.

`build-native.py --help` describes the independent full-library build. It
requires a new private build directory, published SDK/contrib paths, a cross
compiler, and an explicit genmodule executable. `--cxx-overlay` and
`--source-dos-headers` are diagnostic opt-ins and are recorded in provenance.
The tested 20261023 SDK lacks bits/version.h and the committed relocation and
process-marker definitions. A build using those options is integration evidence,
not pristine SDK acceptance. Every command, compile failure and link map is kept.

`native_probe.c` tests KrnAllocPages by default, or AllocMem executable heap with
argument `execmem`. The latter passed on the tested nightly; page allocation
returned NULL. `native_payload_test.cpp` checks emitted MOVEQ register/flag
results and CPU-gated endian reads/writes directly under AROS. Neither standalone
test establishes full application compatibility. See STATUS.md for evidence.
