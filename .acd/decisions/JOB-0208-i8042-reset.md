# Bounded keyboard reset retries

Status: confirmed

## Rationale

Both RESET and DISABLE command loops accepted an unlimited stream of RESEND
responses. The caller has Disable active, so a responsive but rejecting
controller can prevent startup from ever finishing. Limit each command to
three transmissions. Keep status as int to retain the helper's negative
error sentinel instead of narrowing it to UBYTE.

## Validation

The host harness executes the actual reset function with controller mocks:
normal reset, endless RESEND at either command, successful one-retry responses,
and negative timeout results at each of six response stages. ASan/UBSan pass.
Seven complete i8042 units cross-compile for ADT x86-64 SMP.

This bounds retries; it does not redesign timer waits under Disable or handle
all embedded-controller timing quirks. Hardware confirmation remains pending.
