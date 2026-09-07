# CrashReporter — design

Copyright (C) 2026, The AROS Development Team. All rights reserved.

A distribution-side crash reporter.  When a program hits a CPU trap or calls
`Alert()`, CrashReporter captures the machine state, writes a plain-text
report, publishes a line to `log.resource`, and (with the user's consent)
POSTs the report to the distribution's package server — the same server the
Software Center already talks to.  Nothing in ROM changes.

    AI_PHASE: CRASH_REPORTER
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: MEDIUM

## 1. Scope and non-goals

* **Intercept, don't replace.**  The stock "Program failed" requester still
  appears exactly as before; CrashReporter observes the path that leads to
  it.  Exec, kernel and the alert requester are untouched.
* **Distro tool, not exec feature.**  It is a WBStartup commodity
  (`SYS:WBStartup/CrashReporter`).  Remove the file and the system behaves
  as stock AROS.
* **Soft crashes only.**  A task trap or `Alert()` with the system still
  scheduling.  Guru-level (supervisor) failures cannot be reported from
  userland and are out of scope.
* **No kernel debugger, no core dumps.**  Registers, backtrace, module list
  and the last `log.resource` entries are enough for a maintainer to
  triage; a symbolised PC and a stable signature are the point.

## 2. Hooks (crashhook.c)

Two observation points, both public API:

1. **Trap observers** — the classic `KrnAddExceptionHandler()` on every
   fault vector (#DE #DB #BP #OF #BR #UD #NM #TS #NP #SS #GP #PF #MF #AC
   #MC #XM), one closure per vector with the vector number in `data2`
   (the classic handler is not told which trap fired).  The observer runs
   in trap context *before* exec's own handler, copies the
   `ExceptionContext` (GPRs, rip, rsp, rflags, cs/ss; FPU pointer cleared)
   into a ring slot marked `CRS_TRAP`, reads CR2 for a #PF (ring 0), and
   returns 0 so the kernel continues down the normal chain.  Nothing is
   called from here — no exec, no libc; the copy helpers are hand-rolled.
   The classic API does not expose the CPU error code, so `Error-Code:` is
   an optional line that a future kernel API may fill in; the daemon must
   work against every kernel.resource that shipped (a newer LVO is never
   called from a distro-side tool — it page-faults on the older kernel).

2. **`Alert()` thunk** — `SetFunction(SysBase, LVOAlert, cr_alert_thunk)`.
   The thunk (x86_64 asm) preserves the argument registers, passes the
   return address and the caller's frame pointer to `cr_alert_record()`,
   then tail-jumps to the original `Alert()`.  The original therefore runs
   with its original arguments, and the requester it opens is the stock
   one.  `cr_alert_record()` runs in the failing task in user mode: it finds
   the task's pending `CRS_TRAP` slot (or claims a fresh one for a pure
   `Alert()`), fills in alert number, task name/type, CLI command name, a
   backtrace (`KrnBacktraceFromFrame()` from the trap's rbp or the caller's
   frame) and marks the slot `CRS_ALERT`, then `Signal()`s the daemon.
   Supervisor-mode alerts are ignored (we could not Signal safely).

The ring (`struct CrashRing`, `CR_RING` = 8 slots) lives in the daemon's
memory; slots are claimed with an atomic counter so concurrent crashes on
SMP never share a slot.  The daemon drains slots in claim order via
`crashhook_next()`, which copies the record out and frees the slot.

**Uninstall safety.**  `crashhook_remove()` refuses (and the daemon keeps
running) if `LVOAlert` no longer points at our thunk — some other patch
was layered on top and restoring ours would leave it jumping into freed
memory.  The status window says so.

## 3. Report format (report.c)

A report is UTF-8 text: a header stanza of `Key: value` lines, a blank line,
then bracketed sections.  It is what gets stored on disk and what is POSTed
verbatim.  The normative description is PROTOCOL.md §9 in SoftwareCenter.

    Report-Version: 1
    Reporter: CrashReporter/1.0
    Date: 2026-09-06 14:02:11
    Task: CrashTest
    Task-Type: Process
    Program: CrashTest
    Alert: 80000003
    Alert-Kind: DeadEnd
    Alert-Subsystem: CPU
    Alert-Text: Bus error / page fault
    Exception: 14
    Exception-Name: #PF page fault
    Fault-Address: 0000000000000000
    Error-Code: 0000000000000004        (optional; absent with the classic API)
    PC: 000000004a1c0b2e
    Module: CrashTest
    Function: main
    Offset: 0x9e
    Location: CrashTest!main+0x9e
    Signature: CrashTest!main+0x9e/80000003
    Summary: #PF page fault at CrashTest!main+0x9e (fault address 0)
    Arch: pc-x86_64
    AROS-Version: ...
    ...
    Memory-Free: ...

    [Registers]
    rax=... rbx=... (all GPRs, rip, rsp, rflags, cs, ss, cr2, error)

    [Backtrace]
    #0  000000004a1c0b2e CrashTest!main+0x9e
    #1  ...

    [Modules]
    exec.library 51.65
    ...  (LibList + DeviceList, name version.revision)

    [Log]
    <last 50 log.resource entries: date type component:sub text>

    [Description]
    <what the user typed on the crash card; absent when they typed nothing>

Symbolisation uses `debug.library/DecodeLocation()`; a PC outside any known
segment is printed raw and the signature falls back to the alert number.

### Storage

`ENV:CrashReporter/Reports` (default `ENVARC:CrashReports`, fallback
`RAM:CrashReports`), one file per crash:
`<days>-<minute>-<tick>-<task>.report`.  After a successful upload the file
is renamed `*.report.sent` so it never goes twice, but stays for the user.
A `Crit` entry (`CrashReporter/report`) is added to `log.resource` so
LogView shows the crash next to the driver messages that preceded it.

## 4. Daemon (CrashReporter.c)

Zune commodity, one `MUIC_Process` worker for network I/O.

* **Popup** on each crash (unless AutoSend): task, one-line summary, a
  "What were you doing when this happened?" field, and
  `[View] [Send] [Later]`.  Nothing is sent without a click.  Whatever was
  typed is appended to the stored report as `[Description]` when the card
  is dismissed by any button, so it also reaches a report sent later from
  the status window.  It is the one part of a bug report the machine
  cannot capture, and the part maintainers most often lack.
* **Viewer**: the full report in a fixed-font Floattext.
* **Status window** (commodity show / Exchange): counts, target URL,
  AutoSend checkbox (persisted in `ENVARC:CrashReporter/AutoSend`),
  `[Send all unsent]`, `[Open reports drawer]`.
* **Target URL**: `ENV:CrashReporter/URL`, else the first enabled package
  source from libpkg + `api/crash/v1/report`.  A distro that ships a
  Software Center source gets crash reporting with zero extra config.
* **Upload**: `pkg_http_post()` with `text/plain; charset=utf-8`; 201/202
  count as sent.  Failures stay on disk and are retried by `Send all`.

### 3.1 Program annotations (crashreporter/annotations.h)

A program that wants its own state in the report keeps a
`CrashAnnotationBlock` in `MEMF_PUBLIC` memory and `AddPort()`s the
embedded `MsgPort` under `CRA_PORTNAME` (`CrashReporter.annotations`,
`PA_IGNORE`, never messaged).  The block is a header (magic `CRA1`,
version, sizes, `cab_Owner` = registering task, a product string) and a
fixed table of `cae_Key[40]`/`cae_Value[128]` slots that the program writes
lock-free from any of its tasks — a torn value beats a lock a crashing
task might hold.

`cr_alert_record()` walks `SysBase->PortList` for a block whose
`cab_Owner` is the failing task or one of its ancestors (`ETask
et_Parent`, so a pthread of the process matches), validates magic/version/
sizes, and under `Forbid()` renders the live entries as `key: value` lines
into `rec_Annot` (`CR_ANNOTLEN` 36 KiB, enough for a full 200-slot block)
with bounded copies that treat the block as volatile.  `report.c` then
emits `Product:` in the header and an `[Annotations]` section.  PROTOCOL.md
§9.5 is the wire-level description.

Chromium implements its crash-key interface over this block
(`base/aros/crash_annotations`, `components/crash/core/common/
crash_key_aros.cc`), so a Chromium report carries `url-chunk`,
`top-origin`, `ptype`, the switches and the GPU keys without any Chromium
knowledge in the reporter.

## 5. Testing

`C:CrashTest MODE/A,DELAY/N` fails on purpose: `NULL WRITE ILLEGAL BREAK
DIV ALERT DEADEND DEEP`.  `ALERT` is the recoverable case (the program
survives and prints "survived"); `DEEP` faults five frames down to check
the backtrace.  The QEMU evidence run lives in
`qemu-test/logs/crashreporter-qemu-<date>/`.

The annotations path is exercised with Chromium on the hosted tree:
`chrome://crash` typed in the omnibox (single-process) traps the
in-process renderer thread; the report must carry `Product: Chromium ...`
and an `[Annotations]` section with `url-chunk`.  Note that the popup is
only opened when `AutoSend` is off — with AutoSend the daemon queues and
posts silently.

## 6. Files

| file | role |
|---|---|
| `crashrec.h` | ring + record shared by hook and daemon |
| `include/crashreporter/annotations.h` | the program-side annotation block contract (installed to the SDK) |
| `crashhook.c` | trap observers, `Alert()` thunk, ring |
| `report.[ch]` | report text, storage, `[Description]` annotation, `log.resource` publish |
| `CrashReporter.c` | Zune commodity, worker, upload |
| `tests/CrashTest.c` | fault generator |
| `icons/` | WBStartup icon |
