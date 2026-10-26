# JOB-0240: reduce opt-in dispatch overhead

Status: hypothesis

## Rationale

A single paired native xSysInfo run measured 94664 Dhrystones with the
interpreter and 84250 with MOVEQ enabled. This is a regression observation,
not a statistically established performance result. Reject unsupported
opcodes before RAM range checks and cache dispatch. Inline explicit MOVBE
assembly into generic callers instead of forcing a target-specific call.
Keep CPUID gating, make assembly volatile, and retain portable fallbacks.

## Validation

Run host differential and endian tests, inspect emitted assembly, then test
native execution with MOVBE enabled and disabled. Compare repeated native
benchmark samples before making a performance claim. Retain prior results.

Host differential and endian checks pass. Native CPU=host and host,-movbe both
pass payload, endian, word-boundary and integrated loop checks. Disassembly
confirms inlined, guarded MOVBE. A six-run direct RunHunk xSysInfo pair on the
later 2fbda536 baseline observes medians 96480 interpreter and 97758 JIT (+1.3%).
Shared-host variability, three samples per mode and changed compatibility/launch
inputs prevent attributing the earlier slowdown's removal solely to this change.
Performance benefit remains a hypothesis; correctness evidence is retained.
