# i8042 helper-task startup ownership

Status: confirmed

## Rationale

Both constructors ignored NewCreateTask failure and waited on reserved
SIGF_SINGLE without clearing it or checking a completion state. A pending
unrelated signal can return the constructor while the helper still accesses
the object. In the mouse constructor the still-zero IRQ token then causes
the object to be freed. Task creation failure instead hangs the caller.

Use a private allocated completion signal shared only with this helper.
Check both signal and task allocation. Preserve pointer-sized task arguments.
Both helpers receive the completion mask as their third task argument.

Keyboard allocation failures formerly accessed CtrlTask before it had been
initialized. Set it from FindTask first, balance timer.device and allocations
on every failure, check IRQ allocation, and finish object accesses before
signaling the constructor. Refuse startup if no LED signal is available
instead of borrowing an unallocated signal. LED notification and handling
now both use the mask, not the bit number.

## Validation

tests/run-host.py extracts the actual start helper and controller function
bodies into fault-injecting host mocks. ASan/UBSan tests cover stale
SIGF_SINGLE, signal/task allocation failure, full-width task arguments,
seven controller failure stages (including immediate object free at the
completion signal), successful registration and LED wakeup.

The previous controller body, with only a third unused argument added for
the harness ABI, reports null Task member access on the first allocation
failure. All seven target translation units also cross-compile with the
ADT GCC 15.2 x86-64 SMP SDK. These are source and compile checks, not a
native scheduler or hardware test.

## Remaining boundaries

Normal keyboard Dispose still lacks cooperative controller-task shutdown.
The controller's synchronous timer waits under Disable, LED command/IRQ
response competition, mouse timer retention and SMP serialization of the
shared 0x60/0x64 ports need separate runtime-led work. This patch does not
claim to fix all controller lifetime/concurrency concerns, nor to reproduce
the photographed laptop exception.
