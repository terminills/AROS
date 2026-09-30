# Render topology query — JOB-0084

Topology and firmware queries passed cold50 native acceptance. Visible hardware-rendered 3D remains
the user's next milestone; this is a dependency, not a completed renderer.

Cold48 loaded installed b364e1b2ec after independent byte comparison and physical
power cycle. Binary v1 size1444 checksum6e43/6e43; GC1.3 offset1000 size164,
checksum04eb/04eb. The first implementation refused field validation before
any selector writes. Managed graphics service stayed ready after its retained
32-pair gate. The first validation additionally assumed nonzero TCC/SC and
literal32/64 wave_size fields, unlike Linux's raw-field consumption. Remove
those descriptive-field constraints, retain all checksum/extent and essential
SE/SA/WGP/RB bitmap bounds, and log the actual fields for discrimination. This
is a pending correction, not proof of the exact offending value or a pass.

Cold49 boot of the independently installed/byte-verified94e60b0dee correction
discriminates the failure: GC1.3 reports SC/SE=1 and SA/SE=2, so the first
implementation's extra SC divisibility check was wrong. Wave32 and TCC16
were valid. The corrected cache reaches READY at0000016a590e21f2 with SE2,
SA/SE2, maxCU/SA8, activeCU32, RB maskff, GB_ADDR_CONFIG08200545 and exact
GRBM_GFX_INDEX restoratione0000000/e0000000. Retained32/32 ME/PFP pairs passed,
max50polls, WPTR330; SDMA HOSTBUF0/512bad. Frozen cold49-render-info-init.uart.log
in JOB-0084-native-completion-20260906 records this boot-time result. Native
query client and retained regressions are still pending; no shader-rendering
or completed full-driver claim.

Appended vector 41 returns an immutable versioned render-topology snapshot.
The native init ladder captures it after device initialization and before
display/public submission service publication. No runtime query reads MMIO,
retargets a register bank, maps memory, initializes hardware or allocates a BO.
The optional service is currently limited to device 7590 with GC 12.0.0.

The discovery blob supplies shader engines, arrays, WGP/CU maxima, backend
counts, and cache/geometry configuration. Both binary and GC table checksums,
sizes, offsets, versions and bitmap bounds are checked. GC info 1.0 through
1.3 are supported; unknown/malformed data fails closed. Optional cache flags
distinguish table extensions from unavailable zero values. No spec-sheet
guesses, firmware versions, clocks or invented heap sizes are advertised.

Actual GC12 SA/RB/WGP disable registers determine active hardware masks. The
driver uses discovered GC segments and checked register aperture bounds.
It only selects SE/SA/instance broadcast in GRBM_GFX_INDEX; it never changes
harvest masks, GPU VM state or display registers. The exact prior selector
is restored on every path following the first write, then read back before
the cache becomes queryable. Enabled CUs use the stable Linux 4x4 bitmap
packing. References: local Linux AMD discovery.h/amdgpu_discovery.c,
gfx_v12_0.c get_sa/rb/wgp_active_bitmap and gc_12_0_0 register definitions.

AmdgpuRenderInfoProbe opens only the already-resident driver and checks vector
extent, actual device, topology/bitmap consistency, 256 identical snapshots,
null/unknown-device refusal, short-buffer non-mutation and preserved output
tail. Required acceptance: committed focused build, frozen hashes, installed
bytes compared with an independent RAM reference, physical reboot, boot
selector-restoration marker, native client result, retained managed CS/Mesa
adapter and 2D tests. No private/RAM driver acceptance. No pointer, GUI,
filesystem or foreign dirty-file edits.

Cold49 resident topology client PASS: installed driver94e60b0dee, opened base
0x1069a6e30 identical to resident. Client Work/RAM bytes55128 compared identical,
actual native0 at000004c8b0436bf6. Topology/CU population, selector restoration,
256 cached snapshots and invalid/short/oversized buffers passed. END0 at
000005c19dd447bb, actual native0/error0/ioerr0 at000005c1e1e049ff. Frozen evidence:
cold49-render-info-client-pass.uart.log, SHA256
5757b8ee5d918dd8065b0988c6dfd221e17d8edd65215461d84fe9dd85c57c8c.
Retained CS/Mesa/2D acceptance is still pending for this boot.

Next driver revision0.2 appends vector42 for immutable CP firmware provenance.
It captures the exact PFP/ME/MEC files used by the serial PSP load path, validates
gfx header2.0 and every instruction/data extent before forming pointers, and
preserves the existing instruction-then-stack order. Transport-completion bits
and raw PSP response words are separate: STAGED is not authentication success
or compute-engine readiness. The header CRC is recorded, not independently
verified. Runtime queries only copy records; no firmware-file reread, register
selection, engine initialization or BO work. Invalid headers are not submitted.
The extended probe requires library revision0.2 as well as the vector extent:
negative-size alignment padding alone can make an older table look large enough.
Three firmware records,256 snapshots each, bounds and invalid-device checks
must pass after committed build/install/byte comparison/physical reboot. This
extension is not yet built, installed or native-accepted.

Firmware extension a9abf4b0b0 and Mesa adapter25ee6f87fa built successfully.
Firmware driver538328 bytes independently matched DEVS/RAM; native0 at
00000919dd4845d6. Cold50 physically power-cycled04:00:47/04:01:21UTC and is
booting. The frozen0.2 probe, not subsequent source edits, will test it.

Next source revision0.3 appends vector43 for ASIC/VBIOS query provenance.
NBIF6.3.1 chip revision is sampled twice from STRAP0 DWORD1c/BASE_IDX2, bounded
by the mapped BAR; this is not NBIO7.11's STRAP1 register and not PCI revision.
GC12.0.0 external revision follows soc24's internal+40hex rule. No register
writes occur. The existing host VBIOS copy supplies bounded ATOM2.2/master2.1
GFX timer reference, UMC4.0 memory type/channel width, and optional firmware3.x
boot clocks. Raw10kHz units and independent validity flags are explicit.
Boot clocks are not advertised as maximum clocks; zero values are retained.
Unknown/malformed optional tables produce missing flags rather than invented
values. Runtime queries only copy the immutable cache. This next source change
is built but untested; it must not be attributed to cold50's installed0.2.

Cold50 firmware client accepted: Work/RAM59016 identical and native0 at
000005f5b5dc480c; resident driver0.2 at0x1069b4bc0. PFP3010/ME2930/MEC3250
feature29 and stages3/3,3/3,7/7 agree with the actual boot records. Snapshot,
buffer-bound and invalid-device checks passed; END0 at000006370028bd50,
actual native0/error0/ioerr0 at0000063727122cee. Frozen
cold50-firmware-client-pass.uart.log SHA256
59f3c9716047e8be7eab5a2d7c380f43b50200676142415312f66b8e3aa24ca7.
Mesa25ee6f87fa also passed its firmware queries and four real GPU writes,
END0 cleanupPASS at000006a731f8f58c, native0 at000006a758946915. Retained
CS/VA/GTT/copy/drawing checks are now being rerun, not yet accepted cold50.

ASIC candidate7aeba3fc7a: frozen driver541976 bytes SHA256
45b6e841af64e6dd94fe553b8c4443c2f8b679c98604c143fcf32c8660665b0c;
probe62240 bytes SHA25668709fffccdf04b4308378d33b2e74909aff284e1222ec0181c9370d93a7fad6.
The combined top-level targets raced the probe against stale generated headers
(exit2, driver branch compiled/linked); sequential probe retry compiled/linked
exit0. Both logs retained. Not installed, physically rebooted or native-accepted.

Cold51 installed7aeba3fc7a after independent541976-byte comparison (native0 at
000008f1f6f0dbb0), physical longOFF completed by04:50:10UTC, shortON at04:50:37.
Boot discriminates another parser assumption: ATOM header2.3 is present, not
2.2, so the reader intentionally withholds master/UMC/timer data (flags1).
NBIF strapd3c=310c7590 gives internal1/external41hex; topology/SDMA/managedCS
remain ready. This is a failed ASIC query candidate, not a GPU initialization
failure. Raw cold51 boot and failed candidate are retained in the evidence set.

Correction accepts the2.3 stable ROM-header prefix as well as2.2. AMD's
atomfirmware.h explicitly fixes masterdatatable_offset's position, and Linux
atom.c loads it via ATOM_ROM_DATA_PTR=0x20 without a minor-version filter.
ROM/table extents, signature, exact supported master version and individual
data-table versions remain checked. No speculative read of future formats.
Boot marker becomes asic-info-v2; ABI/vector table unchanged. Correction needs
committed build, installed-byte comparison, physical reboot and native tests.
# Cold52 ATOM2.3 correction: native query acceptance

Driverd3bb74515a,542008B SHA256
7b68e1dd11eaac5c18889a481288ddce77523402706f50e99dd081af5d8c2f24,
installed and independently byte-compared with actual native0 at
000006041ca8527f before the physical OFF/ON cycle. Cold52 captures flagsf,
ATOM203/master201/UMC400/GFX301, GDDR6type70, bus128, timer10000x10kHz.
The resident-only frozen62240B ASIC client compared native0 at000004276162578c
and then passed firmware/topology/ASIC snapshots and bounds. END0 at
000004ba62e51a6b; actual native0/error0/ioerr0 at000004ba9f13f511.
Raw cold52-asic-client-pass.uart.log SHA256
1d19ff7cf75ed3a91c029d11d30f8a8cc93e3cee5a0d93f72cd071bb403126b1.
Cold51's failed ATOM2.3 run remains preserved, not relabelled successful.
Mesa client and retained2D regression acceptance are separate; no visible3D.
