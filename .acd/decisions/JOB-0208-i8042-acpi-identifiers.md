# Recognize standard PS/2 ACPI identifiers

Status: confirmed

## Rationale

The original probe only requested PNP0303 and PNP0F03. Other standard
keyboard and PS/2 pointing-device IDs, including PNP030B, PNP0F12 and
PNP0F13, were classified as absent. Moving the probe before registration
makes that exclusion effective, so support the standard PNP keyboard and
PS/2 mouse identifier families before using the flags to gate registration.
AcpiGetDevices already checks _HID and _CID in ACPICA's nsxfeval.c.
No I2C-HID or USB identity is treated as a PS/2 device.

Reference for recognized identifiers (factual identifiers, not imported
implementation):
https://github.com/torvalds/linux/blob/master/drivers/input/serio/i8042-acpipnpio.h
The fetched reference is retained with the JOB-0208 private evidence.

## Validation

Forty mocked enumeration/registration scenarios cover two keyboard IDs and
four mouse IDs with no devices, each device alone, both, and absent ACPI.
The original single-ID probe fails the expanded suite. The fixed probe and
all other host suites pass ASan/UBSan; complete target units compile.
The mock does not execute ACPICA namespace traversal or validate firmware.

## Laptop boundary

JOB-0187 identifies ASUS Vivobook 16 M1607KA. ASUS lists a Precision Touchpad
and ELAN/Primax options; this does not establish the transport on this unit.
The selected ADT tree has no matching PNP0C50/I2C-HID touchpad implementation
in the inspected arch, rom/hidds and rom/devs paths. If firmware exposes an
I2C HID device, PS/2 packet fixes will not implement it, nor is missing
PS/2-to-USB handoff established. Need the unit's _HID/_CID and _CRS (I2C
serial bus versus IRQ12 PS/2) or a working OS's device enumeration.

Vendor reference:
https://www.asus.com/in/supportonly/m1607ka/helpdesk_download/
