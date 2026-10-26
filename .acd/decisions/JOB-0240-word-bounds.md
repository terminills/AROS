# JOB-0240: reject wrapping native CPU word accesses

Status: confirmed

## Rationale

The four CPU word callbacks compare `addr + 1` against containment size, but
addr is uint32. At 0xffffffff the addition wraps to zero and permits a host
access outside the guest allocation. Widen before addition. For every other
uint32 address the predicate is unchanged. Device and sentinel checks retain
their existing order. This is an interpreter compatibility fix independent of
the JIT, and must be forwarded to the compatibility owner as a separate patch.

## Validation

Source history and relay correlation found no existing fix. Native regression
uses a tiny 68k HUNK performing word read and write through A0=0xffffffff, then
returning with D0=0. Both interpreter and JIT runs must return normally. The
JIT's ordinary-RAM fast path now also benefits from a nonwrapping shared guard.
Runtime results are recorded separately when captured.
