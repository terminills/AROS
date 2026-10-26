# Experimental MC68K JIT: first lowering

This directory is an independently testable **Linux x86-64 host prototype**.
It is not enabled in m68kemu.library. `m68kemu_moira.cpp` and Moira are unchanged.

`MoveqCache` lazily generates native MOVEQ register/CCR payloads, one immutable
RX mapping per opcode, with a separate cache per owner. Unsupported opcodes,
disabled caches, unsupported hosts, allocation failures and protection failures
return null for interpreter fallback. Mappings are created RW and changed to RX;
there is no RWX interval. The SysV calling convention and native layout offsets
are used only under the Linux/x86-64 guard. The caller must keep the cache alive
until its generated calls finish. Each cache is task-local, not concurrently shared.

The cache key is the instruction word. It never embeds guest addresses or keys
translations by PC. Use the CPU's prefetched IRD, not a fresh read from guest RAM:
self-modifying code must preserve the interpreter's prefetch behavior. A future
multi-instruction block cache will need its own explicit invalidation contract.

Run `run-host.sh /absolute/private/build-directory` with GCC supporting C++20.
The harness compares every MOVEQ encoding and all 32 initial CCR combinations
against Moira, checks every unsupported word, isolation, same-PC mutation, and
fallback for NOP, A-line, RTE, F-line, tracing, interrupt, model and disabled paths.
Its bounded host adapter also compares the PC, prefetch queue, function code,
read buffer, cycle count and internal flags. That adapter is only valid for its
nonthrowing ordinary-RAM callbacks. It is **not** an AROS memory/fault adapter.

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
