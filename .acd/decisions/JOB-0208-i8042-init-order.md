# i8042 probe ordering

Status: confirmed

## Rationale

INITLIB callbacks execute in ascending priority. Previously attributes opened
at 0, i8042_Init installed hardware objects and IRQs at 10, then the ACPI
probe at 20 could return FALSE. compiler/autoinit/functions.c then calls
EXPUNGELIB and class/library cleanup. There is no i8042 expunge callback
that unregisters the installed hardware objects, IRQs or keyboard task.
The disable flags were also set too late to govern driver registration.

Run ACPI at 5, before any hardware registration. Correct the inverted
utility-library close condition so an early init failure does not close NULL
and a successful open is balanced during unwind.

This establishes a source lifetime defect. It does not establish that the
reported laptop used this exact binary, returned no ACPI devices, or followed
this failure path. The IRQ1 photographs and JOB-0187 are historical evidence;
no physical hardware was exercised for JOB-0208.

The 64-bit truncation hypothesis is unconfirmed: common base/self fields,
cached OOP methods/classes, IRQ tokens and task arguments are pointer-sized.
