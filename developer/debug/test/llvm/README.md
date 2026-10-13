# ADT native LLVM 20 and self-hosting

Tracked as JOB-0127. Baseline: ADT branch `alpha14`, commit `cdafb29de`.
Historical LLVM 11/OPAL work is reference material, not LLVM 20 validation.

## Acceptance gates

1. Cross-build LLVM 20.1.0 native tools from ADT's shared patches.
2. Run `native-toolchain.sh` **inside AROS**. This checks separate C
   compilation, archive creation, driver/linker spawning, loading and running
   the result, response files with spaces, C++ static initialization and STL,
   emulated TLS isolation between threads, and nonzero compiler/linker failures.
3. Run the native shell, make, configure dependencies and AROS generators on
   AROS. Build a real AROS component through MetaMake, then the base system.
4. Build the configured AROS target entirely inside AROS, retaining the
   source revisions, configure command, build log and artifact hashes.
5. Boot the resulting image independently. A host cross-build or a successful
   component build alone does not satisfy the self-hosting gate.

## Native toolchain probe

Install the native Clang resource headers, SDK, runtime libraries, archiver,
and `collect-aros` with its linker backend. Put their executables on PATH.
Run from the AROS POSIX shell:

```sh
export CFLAGS='--sysroot=/SYS/Developer -mcmodel=large -mno-red-zone -nix'
export CXXFLAGS='--sysroot=/SYS/Developer -mcmodel=large -mno-red-zone -nix'
sh native-toolchain.sh /T/llvm-native-test
```

`CC`, `CXX`, `AR`, and `LDFLAGS` can select a particular installed toolchain.
The only final success marker is `NATIVE-TOOLCHAIN-PASS`; the two deliberate
diagnostics near the end must return failure. Preserve the complete log.

## Initial state (2026-10-13)

ADT shares `tools/crosstools/llvm/*-20.1.0.src-aros.diff` between cross and
native tools. It includes the September 30 AROS `call_once` fix. The native
tools target is `workbench-libs-llvm-tools`. Initial configuration uses a
fresh `linux-x86_64` hosted build with `--with-llvm-version=20.1.0` and the
existing ADT GCC 15.2 cross compiler to bootstrap native executables.

No native runtime or self-hosting pass has been established yet. The
LLVM 20 process-spawning implementation and current ADT posixc must be
tested together before deciding whether an older spawn workaround applies.
