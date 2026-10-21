# Bound the PS/2 mouse packet assembler

Status: confirmed

## Rationale

The mouse helper installs IRQ12 before reset/detection sets mouse_packetsize.
An initial zero size can never equal the incrementing collected-byte count.
A first byte with bit 3 set lets the IRQ handler keep writing past the
five-byte array. At index 5 it overwrites the adjacent collected-byte counter,
so the next write can escape the instance altogether. A queued ACK (0xfa)
is sufficient in the host reproducer. Invalid packet sizes and stale counters
have the same unchecked-index problem.

Discard early bytes until size is 3 or 4 and restart stale partial packets
before indexing. Decode the wheel only for four-byte packets, avoiding a
stale fourth byte generating wheel events for a three-byte mouse.

## Validation

The actual IRQ function with the source-equivalent packet/counter layout is
executed under ASan/UBSan. Early size-zero input reproduces an out-of-object
store in the original. The fixed function passes bursts at sizes 0,1,2,5,255,
a stale index of 255, standard motion, wheel, keyboard-byte and parity filtering.
All seven ADT x86-64 SMP translation units compile.

## Hardware boundary

This is a demonstrated memory-safety defect, not proof that IRQ12 executed
in that startup window on the laptop. Disable/DoIO scheduling, controller
response ownership and exact shipped binaries remain unverified. An overflow
could explain later unrelated OOP corruption, but that link is a hypothesis.
The fix does not provide USB or I2C touchpad support or establish the built-in
trackpad's transport.
