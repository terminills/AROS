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

The isolated hosted ADT runtime has now run Clang 20.1.0, compiled a C
program, linked it through native collect-aros/binutils, and executed it
(`NATIVE-LINK-RUN-PASS`). Its executable directory is resolved through
GetProgramDir, including when invoked using an AROS volume path. This is
a component bootstrap result, not a self-hosting pass.

The Python 3.12.12 bootstrap passes file/pickle operations, threading,
ten captured subprocesses, child exit status, unsafe preexec rejection,
and Mako/YAML imports and rendering (`NATIVE-PYTHON-PASS`). Networking,
syslog, termios, and multiprocessing remain disabled. The email utility
imports socket only when generating a hostname-based message ID; the
bootstrap does not pretend to supply networking. AROS subprocesses use
the standard descriptor-close fallback because there is no procfs.

Use the BFD backend for native collect-aros: the generic backend depends
on ADT's unfinished popen/pclose path. The bootstrap recipe is
`tools/native-bootstrap/build-collect-aros.sh`; first build
`development-binutils` to supply the native BFD/libiberty archives.

## Consumer regression gates

Before integration into a shared ADT SDK or runtime, record the exact source
and toolchain revisions for Chromium, RPCS3, Node, and V8 and compare each
against its existing passing baseline. Keep candidate libraries in an
isolated runtime until these gates pass:

- Compile the changed headers in C and C++, including pthread and socket
  headers in both orders; preserve public types and library vector offsets.
- Rebuild affected consumer objects and link targets with the candidate SDK.
- Run Chromium startup and its existing browser smoke workload, RPCS3's
  existing startup/JIT smoke workload, Node's existing process/thread tests,
  and V8's existing execution/JIT/thread tests in the candidate runtime.
- Retain logs and distinguish baseline failures from new failures. A compile
  probe alone does not establish application runtime compatibility.

These gates are pending; no consumer regression pass is claimed yet.
The C/C++ header-order matrix passes with identical probe machine code.
RPCS3's platform/backend seam compiles, but its VM probe needs exception
interfaces absent from this ADT baseline; baseline headers reproduce it.
Copied Node/V8 runtime artifacts fault in the libuv internal-symbol base
wrapper in both candidate and baseline posixc A/B runs. That comparison
does not establish a passing Node/V8 baseline. Chromium runtime validation
has not yet run. Existing application runtimes remain untouched.
