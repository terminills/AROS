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
