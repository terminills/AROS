# ADT native LLVM 20 and self-hosting

Tracked as JOB-0127. Baseline: ADT branch `alpha14`, commit `cdafb29de`.
Historical LLVM 11/OPAL work is reference material, not LLVM 20 validation.

## Acceptance gates

1. Cross-build LLVM 20.1.0 native tools from ADT's shared patches.
2. Run `native-toolchain.py` **inside AROS** with the bootstrap Python. This checks separate C
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
The checked subprocess gate is independent of the shell:

```text
SYS:Developer/bin/python3 /SYS/Developer/native-toolchain.py
```

`native-toolchain.sh` exercises the same operations through the POSIX shell,
which must also be validated before using it for configure:

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

The checked Python-driven native LLVM gate passes C/archive/link/run,
response files with spaces, C++ static initialization/STL, emulated TLS,
and nonzero compile/link failure status (`NATIVE-TOOLCHAIN-PASS`). Native
Clang SHA256: `c067d4b89f6a5cfea0d01278d49a30eb423b45c3a79b1bf4817f272c895d3edb`.
LLVM source buffers must use the normal read path on AROS: the malloc-based
mapping fallback does not provide mmap's zero-filled tail page, causing
out-of-bounds source parsing and diagnostic crashes. The shell-driven native
toolchain gate also passes after fixing ABC shell status propagation, option
table alignment, and inherited local environment variables. Its broader
configure compatibility is still under test. Use `-m 2048` (separate arguments)
for the hosted bootstrap; `-m2048` is ignored and leaves only 256 MiB.

The private contrib clone includes `ef8adf7d` (following `a65885d6` for ABC shell).
Flex skips a redundant no-child wait because its AROS filters already reap
children synchronously. Coreutils now recognizes read-only posixc streams
instead of treating every stream as output during cleanup. Native direct
input, pipeline, and redirected-file cut probes pass. Both saved patches
reproduce the tested source files from pristine archives.
Autoconf 2.73 also needs its exit-trap shell argument protected from M4
expansion. The bundled patch and corrected generated configure retain
failure status; a generated intentional-error configure returns 1 inside AROS.
The subsequent ABC shell fix `2b761d0d` handles escaped double quotes inside
double-quoted backtick substitutions in POSIX mode. Native regression cases
cover the Autoconf printf/sed forms and modern command substitution.

Native configure also exposed buffered output left behind by posixc `fclose`
on inherited handles. Flush output before closing the descriptor, including
when close deliberately leaves the underlying DOS handle open. Without this,
the shell writes a heredoc terminator before sed's buffered body; generated
`config.status` input is invalid. The ordered-output shell regression now
passes, as do the native Python/compiler gates and bounded consumer probes
with candidate posixc SHA256
`c23e96a31a14ea5649cc2f6289a386cfd7a7aca57e8062c59a711c2a2112f40c`.
Waiting for the exact shell child did not fix the reproducer; that experiment
was removed and its diff retained in the build's regression directory.

Hosted `emul-handler` must resolve a rename destination against the destination
lock, not the source lock. `native-rename.py` checks all eight Python/mv
relative/absolute combinations and failure when the destination parent is
missing. Node/libuv and Chromium smoke gates also pass with this handler.
Its SHA256 is `04c667dff79f36759832174b56892b04efe1fc79d1ebba59501f144c0efbab7b`.
Python's generic nested-directory `shutil.rmtree` rejected a normal directory
as a symbolic link during fixture cleanup; that separate metadata issue remains
open. The rename test explicitly removes its known fixtures; its original
cleanup failure is retained as `native-rename-cleanup-failure.log`.

The Python 3.12.12 bootstrap passes file/pickle operations, threading,
ten captured subprocesses, child exit status, unsafe preexec rejection,
and Mako/YAML imports and rendering (`NATIVE-PYTHON-PASS`). Networking,
syslog, termios, and multiprocessing remain disabled. The email utility
imports socket only when generating a hostname-based message ID; the
bootstrap does not pretend to supply networking. AROS subprocesses use
the standard descriptor-close fallback because there is no procfs.
Install the generated `_sysconfigdata__aros_.py` alongside the standard
library. The bootstrap recipe now does this and the native test checks it.

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

Full application gates remain pending; the bounded results below do not
establish complete application compatibility.
The C/C++ header-order matrix passes with identical probe machine code.
RPCS3's platform/backend seam compiles, but its VM probe needs exception
interfaces absent from this ADT baseline; baseline headers reproduce it.
An isolated copy of the complete ADT runtime passes Node 22.23.1 array execution
and the existing worker-thread/MessageChannel probe with both original and
candidate posixc. Candidate libuv work-queue and process-lifecycle tests pass.
Repeated Node launches still fault with both libraries; the user reports
Claude is fixing uv1.library, so that result needs retesting against the fix.
The earlier mixed-runtime internal-symbol fault is retained as failed setup
evidence. Chromium startup, JavaScript reduction over 100,000 elements, and
Canvas rendering pass with original and candidate posixc in fresh private
profiles. This requires a 16 GiB hosted guest for this Chromium artifact's
address-space allocations. Media, GPU and broader browser regressions remain
open. Existing application runtimes remain untouched.

Regenerating posixc dispatch tables from baseline `cdafb29de` and the candidate
confirms all 286 existing entries (including 282 stack calls) retain their
vector numbers. `clock_getres` occupies reserved slot 574; `clock_gettime`
remains 575. New callers require the updated library. Any future movement of
existing vectors requires rebuilding linked consumers before runtime testing.
The generated comparison and JSON report are in the isolated build's
`regression/posixc-vector-audit` directory.
Repeat it with `tools/native-bootstrap/check-posixc-vectors.py SOURCE GENMODULE
BASELINE OUTPUT`; any changed or removed dispatch entry produces failure.
