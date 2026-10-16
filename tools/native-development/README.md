# Native development kit

`package.py --developer DONOR --notices NOTICES --output NEW_DIRECTORY` packages
an AROS ABIv1 x86_64 native Developer tree, SDK, selected tools, examples and
SHA-256 manifest. It does not build a compiler. Include the donor toolchain's
licences and source/build provenance in NOTICES when preparing a distributable
kit. Runtime validation belongs to the exact generated kit, not merely its donor.

On writable SYS:, run `Execute Install-Development` from the package directory.
The installer refuses an existing SYS:Development, creates Development:, and
registers SYS:Development in ENVARC:SYS/Packages/Development. The standard
Startup-Sequence runs Development/S/Package-Startup on subsequent boots; the
installer also executes it immediately. Existing package registrations are
preserved, and S:User-Startup is not edited. To disable automatic setup, delete
ENV:SYS/Packages/Development and ENVARC:SYS/Packages/Development.
It does not replace system libraries.

For a live CD, put Development at SYS:Development and execute
`SYS:Development/S/Development-Demo` during startup. It stages only the example
under RAM:Development, then uses a multi-assign with read-only SYS:Development as
the tools/SDK fallback. Re-running it preserves the edited RAM example.
RAM examples disappear on reboot; copy your work to persistent storage to keep it.

VS Code contract: `Development:Examples/HelloAROS`, commands `make all`,
`make run`, `make clean`, or `make -j4 all`. The sample uses explicit compiler,
SDK and linker paths, emits file:line:column diagnostics, and preserves nonzero
build/run status. The explicit `--ld-path` is necessary because this bootstrap
collect-aros was built with /Developer/bin/ld as its default.

This is a bootstrap toolchain package, not evidence of a self-built compiler or
a completed native operating-system build. The fresh-boot example checks cover
C compilation, linking, execution, cleaning and parallel make. C++ and the full
SDK have separate acceptance requirements.
