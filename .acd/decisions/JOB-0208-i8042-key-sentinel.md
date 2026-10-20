# Reject unsigned no-key events

Status: confirmed

## Rationale

pHidd_Kbd_Event.code is UWORD, while NOKEY is signed -1. Assigning the
sentinel to code yields 65535, which integer-promotes to positive int during
comparison. Consequently all three checks against -1 failed and unmapped
scan codes were broadcast as 0xffff. Compare against (UWORD)NOKEY instead.
This is integer signedness, not 64-bit pointer truncation, and applies to
32-bit int targets as well.

The keyboard.device callback bounds-checks its matrix index, so this invalid
event alone does not establish the photographed OOP dispatch crash.

## Validation

The host test executes the actual translator with the real key tables and
raw-key constants: 768 byte cases across ordinary/E0/E1 states plus known
make/break/repeat/Pause/ACK/error and LED-signal cases. ASan/UBSan pass.
Restoring the old comparisons fails on the first unmapped byte (0x7f),
which reaches the test consumer as an invalid event.

The same suite also now executes the actual ACPI probe and hardware-register
functions in their source-registered priority order with mock OS calls.
No-device failure installs no hardware; keyboard-only, mouse-only, both and
absent-ACPI scenarios pass. Restoring priority 20 fails the no-device test
because both hardware drivers have already been registered.

All seven target translation units compile with the ADT x86-64 SMP SDK.
No QEMU or physical laptop execution is claimed.
