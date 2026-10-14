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

Native top-level configure now completes for `pc-x86_64` with LLVM 20.1.0.
MetaMake compiles and links inside AROS after its configuration header requests
POSIX.1-2008/XSI declarations. The matching pregenerated MetaMake configure
script is still a bootstrap input: native Perl/Autoconf regeneration is not
validated. Load the hosted clock with `C:SetClock LOAD` and assign
`bin: SYS:Developer/bin` before starting the native build runner.

Private contrib commit `6761ca0f` fixes GNU Make 4.4.1 startup: AROS passes
only argc/argv to main, so the port must obtain the environment from `environ`
instead of reading an unspecified third argument. `native-make.py` checks
inherited variables, newly exported recipe variables, recursive make and
nonzero recipe status. All pass in the current candidate runtime; the tools
build is underway, and a complete self-hosted system/boot remains outstanding.

The next libc candidate fixes two independently reproduced bootstrap failures:
`exec` must apply a supplied environment even when its vector address matches
`environ`, since programs can edit the vector directly; and large-file callers
of `fstatat` need the wide structure implementation. `fstatat64` is appended at
vector 798 (posixc 0.43), with all 286 baseline dispatch entries unchanged.
Rebuild large-file callers against the corrected header to select the new
entry; their runtime must provide it. Existing calls have not moved.

Native `native-env.py`, `native-stat.py`, Python, shell, make and LLVM C/C++/TLS
checks pass with posixc SHA256
`a751eca74f4c2895ec1fc337a44105577b8bfb0b40c073e406b0b61c05b67651`.
The same library passes first-launch Node worker, libuv work/process, and
Chromium JavaScript/Canvas smoke checks in the private full ADT runtime.
These are bounded regressions, not full browser/RPCS3 application qualification.

The native `make -j1 tools` target now passes. Native link recipes retain the
configured SDK/compiler flags; FlexCat includes its AROS support objects and
uses pointer-width tag arguments. `native-flexcat.py` successfully generates
an Input preferences header and German IFF catalog with the freshly built tool.
The native AFS utility library includes its security implementation. Its host
configuration probe uses the configured compiler, detects byte order without
assuming 32-bit host long, fails on compiler errors, and avoids regenerating
Make dependencies merely because its output directory changed.

The passing tools run used a 16 GiB private hosted guest. An earlier 2 GiB
parallel run produced a truncated temporary object; it remains retained as
failed evidence, and its cause is not established. Native libiberty, codesets,
Unicode data and pregenerated MetaMake configure remain bootstrap inputs.
All 78 source submodules were staged at their pinned commits. This milestone
still does not establish a complete native system build or boot.

Native Clang now defaults its AROS SDK root to the parent of the executable's
bin directory. Explicit sysroots take precedence; cross-hosted compilers keep
their configured behavior. `native-sdk-default.py` compiles, links and runs C
and C++ without a sysroot argument and checks explicit override paths. It and
the existing native C/archive/response/C++/TLS/error-status suite pass with
Clang SHA256 `2632e3f58866d126cd6908fedd7b45e73b4c9a2a3c68fe5cc4d255e1a5001c12`.
The updated Clang patch replays cleanly on pristine 20.1.0 source. The initial
system feature scan with the old driver could not find stdio.h; that scan is
retained and must be rerun with this compiler before accepting its results.

MetaMake now honors TMPDIR (falling back to /T on AROS) when writing the
makefile-generation list. Its build configures against the real source tree,
not the stale copy used to bootstrap Autoconf. After rebuilding it natively,
the full target passes its source scan, creates the list under /T, and starts
regenerating 1,217 makefiles. The previous /tmp failure and the no-op rebuild
against copied sources are retained as failed evidence.

The full system run now generates all 1,217 makefiles, completes dependency
scanning, builds genmodule natively and starts installing SDK headers. Running
the vector audit with that native genmodule and an exported baseline config
confirms all 286 existing entries remain unchanged.

`native-fetch.py SHELL FETCH_SCRIPT` passes cached gzip extraction (including a
256 KiB payload), patch application, repeat calls, lock cleanup and invalid
arguments on Linux sh and native AROS sh. The fetch script uses portable shell
constructs. Native tar no longer pulls in remote-tape socket startup for its
local-only AROS port; its vfork call executes in the caller frame rather than
returning through a wrapper before exec. The original socket requester and
dup2 failure are retained as failed evidence. The tar patch replays cleanly
on pristine 1.35 source. Tar and hostname remain cross-built bootstrap inputs.
The complete system build and boot are still outstanding.

The timestamped, multi-option fetch case now also passes, and the real ACPICA
20260408 source patch applies natively. GNU patch guards a missing tm_zone
when parsing timestamps. The hosted Unix filesystem handler initializes the
entire host struct tm and requests automatic daylight-saving detection before
calling host mktime; leaving tm_isdst uninitialized lost an hour on winter
file dates. `native-utime.py` verifies repeated winter, summer and leap-day
round trips. Native stat, rename, make and default-SDK checks plus first-launch
Node workers, libuv work/process, and Chromium JavaScript/Canvas checks pass
with the corrected handler. No shared C library or vector changes were needed
for these fixes; full RPCS3 runtime validation remains outstanding.

The bootstrap SDK now also carries the standalone xzdec decoder. When xz is
absent, fetch uses tar with that explicit decoder for .tar.xz archives. An
isolated private ADT guest passed 256 KiB extraction/content comparison and
rejected a truncated XZ archive. `native-xzdec.py` carries the same fixture.
The full xz command is not ported by this change; xzdec remains a cross-built
bootstrap input, and compression is not claimed.

The curl 8.21.0 bootstrap now passes native file transfer, HTTP to the host,
and DNS plus certificate-verified HTTPS in the isolated hosted ADT runtime.
The Contrib patch uses TCP wakeup sockets rather than POSIX pipes, uses
IoctlSocket for nonblocking mode, and avoids socket flags and SO_NOSIGPIPE
that the SDK advertises but this AROSTCP stack does not implement. Retained
failure traces distinguish the wakeup pipe EBADF, unsupported SO_NOSIGPIPE,
and unsupported socket creation flags. A pristine source patch replay passes.
The executable is a cross-built bootstrap input, not proof of a native curl
build. Its private link uses the existing ADT libzstd-static.a.

Keep stdc, stdcio and posixc from a compatible SDK together when constructing
a regression runtime. Curl exposed a mismatch in the older consumer image:
its stdc exit list was at offset 0x370, while the newer posixc expected 0x378.
Pairing the libraries removes that startup fault without changing vectors.
The earlier bounded consumer smoke passes do not qualify untested paths
through the mixed runtime. Full system build/boot and RPCS3 runtime validation
remain outstanding.

Offline PortsSources staging must preserve nested archive paths, including
utf8proc/v2.11.3.tar.gz. A top-level-only copy misses these dependencies.

The paired CRT runtime passes Node worker messaging, libuv work/process, and
Chromium JavaScript/Canvas smoke checks in the established 16 GiB guest. A
4 GiB run failed Chromium partition address allocation before the browser
smoke; that failed run is retained and is not counted as a pass.

Mesa extraction exposed hosted symlink path translation defects: a DOS
parent target such as /bin/ci was passed to Unix symlink() as an absolute
host path. The Unix handler now converts relative targets in both directions;
DOS read-link resolution normalizes the target after joining the link's
parent directory. Targets are not case-resolved against the handler's host
working directory. native-symlink-archive.py checks parent and ancestor links,
forward references, directory traversal and file contents. Its initial native
tar failure, create-only intermediate failure and successful full path are
retained. Native timestamp/stat/rename/make and paired CRT Node/libuv/Chromium
smokes pass. These changes do not modify library vectors.

At the Mesa failure checkpoint, native genmodule has an x86-64 AROS ELF
header and no undefined symbols; running it again generates the expected
vector maps with all 286 existing entries unchanged. Native FlexCat also
passes its real catalog/header generation test again. No target-system
executable had been produced at this checkpoint; those executable tests
cover the native build tools, not a newly built operating-system image.

The next Mesa checkpoint reached GNU patch and faulted in stdc strlen after
printing literal `%jd`/`%td` conversions. The shared narrow/wide formatter
had not consumed these arguments, so a later `%s` received the numeric offset.
It now reads intmax_t/uintmax_t and ptrdiff_t/its unsigned counterpart, supports
`%jn`/`%tn`, and computes negative magnitudes without overflowing at signed
minimum. The wide-integer scratch buffer also accommodates 64-bit decimal
values on 32-bit builds. `native-format.py` checks x86-64 limits, flags, mixed
arguments, wide output, and real GNU patch offset/rejection messages. Native
checks and a host ASan/UBSan formatter harness pass; 32-bit runtime execution
has not been validated. Clang still warns that its built-in intmax_t type
and the SDK typedef differ (long versus long long); the tested x86-64 ABI
widths agree, but this diagnostic mismatch remains a separate issue.

With the candidate stdc and the paired existing stdcio/posixc, Node worker
messaging, libuv work, a fresh process-lifecycle run (exit 0), and Chromium's
100000-element JavaScript/Canvas smoke pass. The first combined process test
left an empty log and is retained separately, not counted as a pass. No
public headers or library dispatch layouts change. RPCS3 runtime and the
full application regression suites remain unqualified.

With that formatter, a fresh native `mesa3d-fetch` completes extraction and
all patches with exit 0. Host inspection confirms all ten originally failing
relative symlinks resolve. Evidence is retained as
`NativeJobs/mesa-format-fixed-passed-{output.log,result.json}` and
`regression/mesa-native-symlink-inspection.json` in the isolated build tree.
A freestanding Linux i386 harness also executes the shared integer formatter
successfully, including intmax limits, pointer differences, octal/hex output
and count pointers. This adds ILP32 core coverage, not AROS 32-bit runtime
qualification. Native `make all` resumes after this dependency checkpoint;
completion of Mesa fetch does not establish a successful system build.

The next native dependency failure was zstd's same-directory forward link
`unzstd -> zstd`, encountered before its target in the archive. lstat and
lstat64 now query the handler without DOS Lock automatically following the
final soft link. Parent-directory fallback preserves each entry's inode
identity; POSIX trailing slashes still require directories, including `/`
and `///`. fchmodat rejects unsupported no-follow link changes instead of
changing the target. Contrib tar also needs its gnulib fchmodat fallback to
retain no-follow semantics when readlinkat is unavailable.

`native-lstat.py` checks forward/dangling links, intermediate directory links,
trailing slashes, roots, dirfd operations, no-follow chmod and GNU mkdir -p.
It and the expanded archive fixture pass alongside native timestamp, stat,
rename, make and SDK checks. Fresh native zstd fetch passes. A root-path
regression found by the full build's makedirs target was corrected and that
target now passes. No library vectors move. Evidence is retained in
`NativeJobs/lstat-root-filesystem-gates-passed-*`,
`NativeJobs/post-failure-tools-zstd-passed-*` and
`regression/lstat-root-artifacts.json` in the private build tree.

With the final runtime, Node workers, libuv work/process and Chromium's
JavaScript/Canvas smoke pass. Intermediate Chromium black-window captures
remain retained: an unchanged intermediate candidate also passed a repeat,
so those captures alone do not establish their cause. RPCS3 and full V8
regression qualification remain pending.

`native-aros-command.py` separately compiles stock workbench/c/Which.c with
native Clang and the installed SDK, links it, then runs the resulting AROS
command to resolve SYS:C/Version. This passes with an ELF64 AROS ABI 1 output.
Evidence is `NativeJobs/which-and-makedirs-passed-*` and the actual console
capture `logs/aros-native-which-linked.png`. This proves a real OS command
can be built and run on AROS; it is not completion of the full system build.

The full build subsequently passed zstd and libpng fetch and reached the CIA
include generator, whose `/usr/bin/perl` interpreter was missing. Contrib's
Perl 5.7.2 x86_64 port has now been compiled and linked with native LLVM 20,
including static Fcntl and IO extensions. Its old configuration needs
`i_time=define` for the modern SDK. A locale warning can initialize PerlIO
before PerlIO_init: registering exit cleanup independently of the I/O table
fixes lost buffered stdout. Native script/module/output checks pass.

Perl's Cwd module also exposed GNU pwd dropping the AROS volume component.
Coreutils' gnulib fallback walks Unix parents and loses that component;
preferring native AROS getcwd preserves it. The rebuilt bootstrap pwd passes
default, -L and -P checks against native getcwd, and Perl File::Find can return
to its starting directory. This pwd is cross-built bootstrap support; the Perl
interpreter itself was built natively. Neither fix changes shared libraries.

With a private `usr:` assign to `SYS:Developer`, the unmodified sfdc script
generates clib, macros and proto CIA headers that byte-match host Perl output.
The original kernel-cia-includes make target passes. Evidence resides in
`NativeJobs/perl-sfdc-passed-*`, `NativeJobs/cia-includes-perl-passed-*`,
`regression/perl-sfdc-artifacts.json` and `regression/perl-sfdc-host`.
The consolidated `tools/native-bootstrap/build-perl.py` recipe captures the
bootstrap commands and is syntax-checked; a fresh end-to-end run of that
consolidated recipe is still pending. It intentionally builds only the
extensions required by this bootstrap. Full Perl tests, locale support and
its reported missing PATH in `%ENV` remain unqualified. The complete native
AROS build resumes beyond CIA; no full system image has yet been validated.

## Native concurrent build gate (2026-10-14)

`native-parallel-build.py` exercises recursive GNU make `-j4`, eight C/C++
compilations and four links, then executes all four programs and checks their
independent checksums. Per-command process IDs, timings and logs are retained.
The private four-vCPU QEMU SMP run passed with four overlapping compiler jobs.
Evidence: `aros-adt-selfhost-build/qemu-parallel/serial.log` and
`concurrent-build-result.json`. CPU time was concentrated on vCPU 0, so this
proves concurrent builds, not multicore scheduling or speedup. The boot image
is an existing SMP baseline with the tested SDK/CRT staged; it is not a
self-built full system. No shared library or application code changed.
