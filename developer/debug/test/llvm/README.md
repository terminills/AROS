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

## Native CMake bootstrap (2026-10-14)

`native-cmake.py FRESH_DIRECTORY` passes in the isolated hosted runtime:
CMake 3.31.8 captures subprocess output and nonzero status, detects native
Clang 20 C/C++ through real ABI compile/link tests, generates Unix Makefiles,
builds with `--parallel 4`, and runs both resulting executables. The C++ test
uses `std::vector` and checks its result. Evidence is retained under
`aros-adt-selfhost-build/native-cmake-bootstrap/native-gate-evidence` and
`consolidated-gate-passed-*`. CMake itself is cross-built bootstrap support;
this does not establish a native CMake self-build or a complete AROS image.

The source patch and `build-cmake.py` are under `tools/native-bootstrap`.
The recipe requires explicit SDK, curl and uv1 inputs and retains its command
list. A fresh recipe build and the resulting binary's complete native gate
both pass (`recipe-check.log` and `recipe-native-gate-evidence`).
Its uv thunks use the existing aros-web generator and must match the
supplied uv1 module. No uv1 vectors or shared runtime libraries are changed.
The AROS platform module selects Unix process behavior and static libraries.
The unused legacy fork backend reports ENOSYS; it must not emulate fork with
an unsafe vfork substitution.

DOS PIPE: rejects the ChangeMode operation used by `fcntl(O_NONBLOCK)`.
CMake-created pipes therefore use uv1's existing DOS pipe registration,
WaitForChar readiness and read wrapper, just as uv_spawn's pipes do. This
adapter is local to the CMake executable and depends on the exact uv1 build.
Normal CMake path searches also need DOS requesters disabled so nonexistent
volumes return errors without blocking unattended builds. The guest needs
`bin:` and `usr:` assigned to its SDK, plus a running bsdsocket provider for
the linked curl support; the validation guest uses loopback only.


## Explicit SMP compiler placement (2026-10-14)

The follow-up private four-vCPU QEMU test also passes with explicit inherited
CPU affinity. The standalone fixture in `smp/` retains the exact tested
launcher and scripts. Build `affinity-launch.c` with the matching AROS SDK,
then install its executable and both Python scripts in `SYS:Parallel`.
Run `native-parallel-build.py` with `AROS_PARALLEL_OUTPUT=/RAM/NativeChecks`.
It uses the existing `NP_Affinity` CPU mask interface; no library vector is
added or moved. The child probe checks the inherited CPU before launching
each compiler/linker. This fixture uses fixed paths and generated argument
names without shell metacharacters; it is not a general command launcher.

All eight compilation jobs, four links and four executable checksum checks
passed, with four overlapping compilation jobs. Each of CPUs 0 through 3
was observed by three guest probes, and all four QEMU vCPU threads accumulated
CPU time. Evidence, exact commands and artifact hashes are retained in
`aros-adt-selfhost-build/qemu-parallel/affinity/attempt2/result.json`,
`serial.log`, `qemu-command.json` and `vcpu-samples.jsonl`.
This validates explicit placement, not automatic scheduler distribution or
speedup. The image still uses the existing SMP baseline kernel.

## Build guest networking (2026-10-14)

The full-build hosted guest now passes DNS lookup and HTTPS with certificate
verification through its exclusively reserved `aros0` TAP. The minimal runtime
needed `commodities.library` and `rexxsyslib.library` from the matching full
ADT runtime before AROSTCP could start. Its CMake installation also requires
the matching `fd.library` and `uv1.library`. These are bootstrap additions;
no existing shared library or vector was replaced for this integration.

The private configuration uses tap.device unit 0, 10.203.0.2/24, gateway
10.203.0.1 and the existing LAN DNS resolver. These are this host's settings,
not portable defaults. Startup assigns AROSTCP:, sets AROSTCP/Config to the
private configuration directory, starts AROSTCP and waits for its port.
Existing host forwarding/NAT was sufficient; no host firewall or route was
changed. Native curl verified HTTPS using the staged CA bundle. IPv6 remains
unqualified. Evidence is under `native-cmake-bootstrap/network-passed-*`.
The integrated native CMake, concurrent compiler and libuv process gates
pass with this network configuration, and the full native build has resumed.


A second SMP run allowed the parent build process on all four CPUs and used
unmodified `native-parallel-build.py`, with no per-compiler assignment. The
recursive `make -j4` workload again passed eight compilations, four links and
all four executable checks, with four overlapping compiler jobs. All four
vCPU threads accumulated substantial work during the workload. Evidence and
the exact all-CPU launcher are retained under
`aros-adt-selfhost-build/qemu-parallel/affinity/all-cpus/`.
The default boot process tree remains pinned to CPU 0; an explicitly widened
parent mask is inherited by this build tree. No scheduler or library changes
were needed for either test. These runs do not measure controlled speedup.

The network check additionally followed GitHub's LLVM release redirect using
the guest's configured `CURL_CA_BUNDLE`, downloaded the CMake 20.1.0 source
archive, and matched it byte-for-byte against the cached archive. Logs and
SHA-256 are retained as `native-cmake-bootstrap/network-github-*`.


Native LLVM's own CMake configuration subsequently reached host detection,
where its bundled config.guess lacked AROS support. The LLVM 20 source patch
now recognizes the AROS uname tuple and emits `<machine>-unknown-aros`.
This older script uses direct echo/exit cases, unlike ADT's newer GUESS-based
script. Native x86_64 detection and the unchanged Linux host result pass;
the patch replays against the original LLVM archive. Evidence is retained in
`regression/llvm-native-host-triple/`. The original LLVM/Clang/LLD CMake target now also passes native configuration
and Unix Makefile generation (`configure-passed-*`, CMakeCache.txt and
CMakeConfigureLog.yaml in that evidence directory). Native compilation and
the full system build remain in progress.


The Contrib Git port is `contrib/development/scm/git` (2.55.0). The existing
ADT bootstrap installation from `Developer/git` and `C/git` has been staged
in both private guests. With GIT_EXEC_PATH pointing at
`/SYS/Developer/git/libexec/git-core` and GIT_TEMPLATE_DIR at
`/SYS/Developer/git/share/git-core/templates`, init/add/commit/show/status/fsck
pass in fresh local test repositories. The executable SHA-256 is
`e6b9c42f981cd5ef89f39be2a790ec54c9f78e2d413e004ab3543156fca2e7e1`.
This is existing bootstrap support, not a native Git rebuild. Its installation
lacks git-remote-https, and discovery outside a repository fails at stat(`/`);
network Git operations are not qualified. Native curl HTTPS downloads pass
independently. Evidence is in `regression/native-git-bootstrap/`.


## Recursive Make job ceiling (2026-10-14)

The full LLVM compile exposed a bootstrap Make configuration problem:
Contrib Make 4.4.1 disabled its jobserver, so nested `-j4` builds each started
an independent set of workers. A three-level, twelve-worker reproducer
measured ten concurrent workers with that binary. Earlier compiler gates
proved parallel execution but did not cover this recursive ceiling.

Contrib now enables the jobserver and supplies an AROS-only transport using
a named Exec semaphore and public token pool. It shares the root Make's
budget across recursive children, including SMP, without depending on POSIX
pipe readiness or signal interruption. Other platforms retain their existing
implementation. This protocol is specific to GNU Make; unrelated clients
expecting a POSIX pipe jobserver are not qualified. An abruptly killed root
can leave its public pool allocated until restart; normal completion removes
it. No shared library vectors or runtime libraries changed.

Run `native-jobserver.py /SYS/Developer/bin/make` inside AROS. It checks
limits 1, 2 and 4 across three Make levels, an expected failed worker, and a
subsequent successful run. Optional second argument `baseline` measures the
old binary without asserting the ceiling. Hosted and four-core QEMU runs
pass; the native CMake C/C++ configure/build/run gate also passes. The
compiler gate now requires between two and four overlapping compilations.
QEMU completed eight compilations, four links and four executable checks,
with maximum compiler overlap four. This uses the existing SMP baseline
kernel, not a newly self-built OS image.

Evidence is retained under `aros-adt-selfhost-build/regression/native-jobserver/`
and `qemu-parallel/jobserver/`. The patch replays against the original Make
archive and matches the tested candidate source. The full native build has
resumed with that candidate and without `CMAKE_BUILD_PARALLEL_LEVEL=4`, so
CMake's nested Make inherits the outer job budget. Full LLVM compilation,
full system image generation and boot remain outstanding.


The resumed full LLVM build confirmed four live compiler processes, but
TaskBT snapshots exposed substantial allocator semaphore contention in this
single-CPU hosted runtime. A separate quiet hosted guest using the identical
compiler and flags compiled ItaniumDemangle.cpp successfully in 46.64 seconds;
the contended compilation was still unfinished after several minutes. This is a diagnostic comparison,
not a controlled SMP speedup benchmark. The full hosted build was therefore
resumed with `-j1`, retaining completed objects and quarantining incomplete
temporary files. The four-core QEMU parallel validation remains passing.
No allocator or shared runtime changes were made. Evidence includes
`fullbuild-processes.log`, `fullbuild-allocator-contention.log`,
`single-compile-details.json` and `fullbuild-j4-output.log` in the jobserver
evidence directory.


## Native LLVM executable relocation regression

The first native-built LLVMDemangle archive linked successfully, but a test
executable failed to load: the AROS ELF loader reported unknown relocation
29 (`R_X86_64_GOTPC64`). The host-tool recipe had left LLVM_ENABLE_PIC at
its default ON. The executable contained 466 GOTPC64 relocations, along
with GOT64/GOTOFF64 relocations. Linking alone did not establish usability.

The recipe now sets LLVM_ENABLE_PIC=OFF only when AROS_HOST_ARCH is aros,
matching the existing native LLVM library build policy. Other host settings
are unchanged. Recompiling all six demangler sources inside AROS with the
same options except removal of -fPIC, archiving, linking and running
`native-demangle.cpp` passes 1,000 symbol checks. The passing executable has
no GOT relocations. The failing and passing binaries, relocation listings,
compile arguments and logs are retained in
`aros-adt-selfhost-build/regression/native-demangle/`.

To repeat against a full native LLVM build, compile this test with native
clang++ (`-nix -mcmodel=large -mno-red-zone -std=c++17`), include the LLVM
source `include` directory, and link the build's `lib/libLLVMDemangle.a`.
Run the resulting executable and require `NATIVE-LLVM-DEMANGLE-PASS` with
exit status zero. The full build is regenerating its non-PIC configuration;
its generated-archive repeat and full compiler/system build remain pending.

The full LLVM CMake regeneration subsequently passed (934.3 seconds configure,
57.0 seconds generate) with LLVM_ENABLE_PIC=OFF. Its generated Makefiles rebuilt
LLVMDemangle; linking and executing the same test against that archive also
passed all 1,000 checks. The integrated executable has no GOT relocations.
The `integrated-*` evidence files retain the cache, flags, archive, executable,
relocation listing, hashes and log. Full native `make -j1 all` has resumed;
full LLVM compilation and the bootable system remain outstanding.


## Native LLVM TableGen configure cost

Native CMake repeatedly traversed the entire LLVM source include tree for
TableGen's global `.td` dependency list. Ten identical scans took 58.925
seconds. The LLVM 20 patch now caches that list in a CMake GLOBAL property
only on AROS, keyed by a hash of LLVM_MAIN_INCLUDE_DIR. Local per-target
inputs remain independently scanned. The global source inputs are assumed
unchanged during one configure; a new CMake process rescans them.

Ten native calls with the cache took 7.67 seconds and returned exactly the
same 45 entries and list hash. Host-side edge tests check distinct include
roots, empty results, fresh-config discovery of new inputs, and the unchanged
Linux path. The updated module patch replays against the original archive
and preserves the existing externally supplied TableGen exclusion fix.

Full native LLVM configuration fell from 934.3 to 48.1 seconds; generation
remained 57.0 seconds. The generated library build, link and 1,000-iteration
runtime test passed again, then full `make -j1 all` resumed. Evidence and
reproducers are in `aros-adt-selfhost-build/regression/native-tablegen-glob/`.
This is a configuration-time improvement; it is not an SMP compile speedup
claim or evidence that the full system build has completed.


## Native Clang frontend cleanup

The bootstrap driver passed `-disable-free`, which skips frontend destruction.
AROS shares an address space, so native compiler invocations must perform normal
cleanup. The Clang 20 patch now omits this default argument when the compiler
itself is built for AROS; other hosts retain their existing behavior.

Ten minimal C compiles retained roughly 1.84 MB per invocation before this change
and 1.09 MB afterward (Avail-only control: about 12 KB). A controlled native
ItaniumDemangle.cpp compile retained 52,051,264 bytes before and 1,498,464 bytes
afterward. Both large and small object files were byte-identical. The large
compile took 207.46 seconds before and 170.87 seconds afterward in this sample;
these timings are not a general performance guarantee. Some retention remains.

Native C/C++ CMake configure, build and execution passed, as did an expected
invalid-source failure followed by successful compilation. A private four-vCPU
QEMU run passed eight compilations, four links and four executions, with peak
compiler overlap three within the jobserver limit of four; all four CPUs did
work. No shared runtime library or library vector changed. This does not replace
full Chromium, RPCS3, Node or V8 qualification. Reproducers, binaries, hashes and
logs are retained in `aros-adt-selfhost-build/regression/native-compiler-lifetime/`
and `aros-adt-selfhost-build/qemu-parallel/clang-cleanup-1/`.


## Native LLVM tool dead stripping

The native release build completed LLVMSupport and LLVMTableGen but failed
linking llvm-min-tblgen: GNU ld reported that gc-sections requires an entry or
undefined symbol. LLVM's release defaults supplied --gc-sections to AROS's
relocatable executable link. The native host-tool recipe now sets
LLVM_NO_DEAD_STRIP=ON; non-AROS host options and shared runtime ABIs are unchanged.
This preserves startup and registration sections without inventing GC roots.

Relinking snapshots of the existing native objects and archives with only
--gc-sections removed succeeded in both the build guest and an isolated
validation guest. Both executables passed --version, --help and -gen-attrs
against LLVM 20 Attributes.td. Their generated headers matched the Linux
reference byte for byte (SHA-256
6aa199ba78aa86ec2cd09e71af225a4f4ea01935640dc1713f37005568d8b857).
Evidence is retained in aros-adt-selfhost-build/regression/native-llvm-dead-strip/

Native CMake regeneration passed and removed the link option. Recovery used
/SYS instead of the previous equivalent /System include paths, causing
avoidable recompilation; compiler options otherwise matched. Future recovery
should preserve the configured path spelling. Full native LLVM and OS builds
remain in progress; this gate proves one usable native TableGen executable.

The generated Makefile target subsequently linked and ran llvm-min-tblgen
successfully. Full native make all passed the original failure and generated
LLVM's attributes and intrinsic headers using that executable. Its Attributes.inc
again matched the Linux reference. The native SYS/System path alias was
verified with samefile; recovery reused 308 flags.make timestamps only when
contents differed solely by that verified alias, avoiding a second unnecessary
recompile. The full LLVM and system builds remain unfinished.


## Native LLVM IR library runtime gate

`native-ir.cpp` links against the freshly native-built LLVM archives and runs
inside isolated hosted AROS. It constructs and verifies 100 valid modules,
requires rejection of 100 modules with an invalid return type, removes each
invalid function, and checks verification and IR printing again. This exercises
IR construction, diagnostics and object lifetimes; it does not execute generated
IR or qualify a JIT. The native link and runtime gate passed with exit zero.

Compile with native clang++ using `-nix --sysroot=/SYS/Developer -mcmodel=large
-mno-red-zone -std=c++17 -fno-exceptions -fno-rtti`, with the matching LLVM source
and generated include directories. Link the object with a linker group containing
LLVMCore, LLVMBinaryFormat, LLVMRemarks, LLVMBitstreamReader, LLVMSupport,
LLVMTargetParser and LLVMDemangle, followed by `-lpthread -lm -lz`.
Require `NATIVE-LLVM-IR-PASS modules=100 rejected_invalid=100` and exit zero.

Keep the archive set and C++ standard library consistent: the existing GCC
cross-built archives use libstdc++, while this native set uses libc++. They are
not interchangeable baselines. The validation guest needed real local snapshots
of the include trees because its external host symlink exposed duplicate native
inode identities and caused Clang to discard a source include directory.
The full build guest uses its own real directories. Neither issue prompted a
shared runtime or ABI change. Logs, object, executable and archive hashes are
retained in `aros-adt-selfhost-build/regression/native-ir/`.


The same native-linked IR executable subsequently passed in a private four-vCPU
QEMU run using the existing SMP baseline OS. Four concurrent workers performed
five executions each: 2,000 valid-module checks and 2,000 invalid-module
rejections, with all 20 processes exiting zero. The launch-to-return intervals
overlap four ways. This exercises separate LLVM processes on SMP; it does not
establish thread safety within one shared LLVM context, JIT correctness, or a
self-built OS boot. The VM shut down cleanly. Commands, the exact executable,
serial output, per-worker intervals and vCPU samples are retained under
`aros-adt-selfhost-build/qemu-parallel/native-ir-1/`.


## Parallel native LLVM source build on QEMU

A private four-vCPU QEMU run compiled all six LLVM Demangle sources and
`native-demangle.cpp` using native make -j4 and the bootstrap Clang. It reused
the native full build's release compiler flags and defines, with staged source
and generated include directories. Seven compilations passed, with peak
launch-to-return overlap four. Native llvm-ar created the archive, native
Clang linked the probe, and its 1,000 runtime iterations passed. All four
vCPUs accumulated work; the private VM exited cleanly.

This extends the earlier small parallel compiler probes to real LLVM sources.
It establishes this bounded parallel build and runtime result, not a speedup
ratio, a complete native compiler build, or a self-built OS boot. The existing
SMP baseline OS was used. Source and object hashes, archive and executable
hashes, original flags, complete commands, scripts and serial evidence are in
`aros-adt-selfhost-build/qemu-parallel/native-demangle-build-1/`.


The six demangler object files produced by the four-vCPU make -j4 run were
subsequently compared with the hosted native full build's corresponding
objects. All six SHA-256 hashes match exactly, covering DLang, Itanium,
Microsoft, Microsoft nodes, Rust and the common Demangle entry points.
`qemu-parallel/native-demangle-build-1/hosted-object-comparison.json` records
both hashes for every object. This is byte equality for this source set,
in addition to the successful archive/link/runtime gate; it is not a claim
that the full compiler or every parallel workload has been qualified.

## Native bitcode round-trip gate

`native-bitcode.cpp` writes and reads 100 LLVM bitcode modules, checking each
module's constant value, source filename and verifier result. It also damages
each bitcode signature and requires a recoverable parse error. Its success
marker is `NATIVE-LLVM-BITCODE-PASS roundtrips=100 rejected_invalid=100`.
This checks serialization and parsing, not execution of generated code.

The probe compiles, links and runs successfully with the native compiler,
matching LLVM 20 headers, and 23 native-built LLVM archives using libc++.
The archive hashes and successful hosted run are retained in
`aros-adt-selfhost-build/regression/native-bitcode/`. The executable SHA-256 is
`a53733263e554acb9a74dc1e8cf4bc41c2e9b958e69d48ae1af119dad3b0b92d`.

The identical executable also passes on four-vCPU QEMU after the debug-module
publication fix below: four workers perform five executions each, completing
2,000 round trips and rejecting 2,000 invalid modules. Process intervals overlap
four deep and all four vCPU threads accumulate CPU time; this is not individual
process-to-CPU attribution. Evidence is in `qemu-parallel/native-bitcode-3/`.
The image uses the existing SMP baseline with the candidate debug library;
it is not a completed self-built operating system.

## ELF loader temporary-table lifetime

Repeated Clang invocations exposed two generic ELF loader cleanup omissions.
`SHT_SYMTAB_SHNDX` tables were loaded for large executables but never freed.
An unsupported relocation also bypassed release of its temporary relocation
table. The loader now frees the relocation table before handling failure and
includes extended symbol-index tables in final cleanup. Library vectors and
public structures are unchanged.

`native-loadseg-memory.c` measures `LoadSeg`/`UnLoadSeg` without executing the
loaded program, separating loader allocations from C++ startup and teardown.
It accepts a native DOS pathname, `valid` or `invalid`, a repetition count and
a maximum total retained-byte limit. A negative limit records the baseline
without asserting memory recovery. For example:

```
native-loadseg-memory SYS:Developer/bin/clang valid 5 65536
native-loadseg-memory SYS:NativeChecks/loader-memory/rejected-clang invalid 5 65536
```

`make-loadseg-rejection.py SOURCE DESTINATION` creates the second fixture from
an ELF64 little-endian compiler image by replacing one loadable section's
relocation type with an unsupported value. It preserves the input and refuses
to overwrite an existing destination. Only load the fixture; do not execute it.

On the matched old hosted DOS library, each successful load/unload retained
966,656 bytes and each rejected load retained 30,326,784 bytes. With both fixes,
all five repetitions of each case retained zero bytes. Warmed-up Clang version
invocations decreased from about 1.08 MB to 114 KB retained; independent C++
runtime retention remains. These are hosted measurements, not SMP results.

The candidate also passed native C/C++ compilation, linking, executable and TLS
checks, expected compile/link errors, CMake subprocess capture and parallel
build/run checks, Node worker/message-port checks, separate-process libuv work
and process-lifecycle tests, and a Chromium JavaScript/Canvas smoke page.
The outer Python harness used file-backed output after pipe-EOF waits were
reproduced with both old and candidate DOS libraries. The initial empty libuv
logs and Chromium launch with an incorrect program directory were retained as
failed attempts, then rerun in the appropriate native process/shell contexts.
This does not establish full Chromium, RPCS3 or V8 qualification.

Before/after library binaries, original and rejected images, reproducible
fixture hashes, measurements, task backtraces, gate logs and the browser
screenshot are retained in `aros-adt-selfhost-build/regression/native-elf-shndx/`.

## Debug-module publication during concurrent loading

The four-core bitcode gate initially faulted in
`debug.library:UnregisterModule+0x90`, reading address 0x10 through a null
segment entry. Registration added the module to `db_Modules` before allocating
and filling its segment array. An unload on another CPU could therefore see
an incomplete module despite holding `db_ModSem`. ELF symbol construction also
continued after publication.

Registration now builds the segment and symbol tables privately, then adds the
complete module under `db_ModSem`. ELF, Hunk and Parthenope registration use
the same publication helper. Library vectors and public layouts are unchanged.
The runtime replay covers ELF; the other input formats have not been exercised
on their native platforms.

The failed image and fault screenshot remain in `qemu-parallel/native-bitcode-1/`.
An intermediate attempt replaced only the loose library, while GRUB still
loaded the old copy from `boot/aros-base.pkg.xz`; that attempt is retained in
`native-bitcode-2/` and is not a candidate test. The corrected package changes
only debug.library among its 26 members. Its bytes were extracted from the ISO
and hash-verified before the successful `native-bitcode-3/` boot.
Source snapshots, private SMP build provenance and package member hashes are
in `regression/debug-publication/`.

With the hosted candidate, native C/C++ compile/link/run, CMake configure and
make -j4, Node worker messaging, libuv work and process lifecycle probes, and
Chromium JavaScript/canvas smoke checks pass. This is smoke coverage, not full
Chromium, RPCS3 or V8 qualification.

The extended four-core replay in `qemu-parallel/debug-publication-regression/`
passes seven concurrent native compilations and the archive/link/runtime gate;
all seven object hashes match the preceding parallel run. Its 100 bitcode child
executions return zero and complete 10,000 round trips and 10,000 invalid-input
rejections. However, their enclosing Python process reports status 1 after its
success output, stopping the combined harness before the IR stage. This combined
run is therefore recorded as failed, with exit-status diagnosis kept separately.


## Python threaded subprocess return-code preservation

The extended debug-library replay exposed a separate bootstrap Python issue:
all LLVM probes succeeded and `Py_BytesMain()` returned zero, but the waiting
parent sometimes received status 1. Tracing `__stdc_get_errorptr()` before and
after the interpreter showed that concurrent `vfork()` calls had changed the
shared CRT base's saved return-code pointer. The interpreter uses absolute CRT
link libraries; its workers therefore share this state.

The AROS Python patch now retains the GIL across the vfork launch window instead
of releasing it as upstream does on Linux. The existing AROS restriction against
pre-exec Python callbacks remains. Launches are serialized until exec restores
the parent's CRT state; the launched programs still execute concurrently. No
shared CRT library, vector table or public structure is changed.

A private per-task-relative-link alternative was rejected because subprocess
creation failed with EBADF. It was not deployed. The retained-GIL candidate
passes 240 LLVM probe executions on four-core QEMU, with four overlapping
process intervals in every stage and stable return-code pointers in both traced
stages. All wrapper return codes are zero. The uninstrumented candidate also
passes three hosted runs of 100 bitcode executions using file-backed output.
Concurrent hosted pipe capture still stalls in a DOS read; a fresh original
interpreter reproduces the same worker-stack pattern, so that existing problem
is not claimed fixed here.

Evidence and rejected experiments are retained under
`regression/debug-publication/python-exit-trace/` and
`qemu-parallel/python-serialized-runtime/`. The focused
`native-python-thread-exit.py WORKDIR` regression checks nested threaded child
launches with both zero and nonzero exits. Its full installed-candidate replay,
including make -j4 of LLVM demangler sources, is tracked separately in
`qemu-parallel/python-serialized-regression/`.

That installed-candidate replay passes: 96 nested child exit-code checks, seven
native make -j4 compilations, archive/link and 1,000 demangler iterations,
100 bitcode executions and 20 IR executions. All seven compiled object hashes
match the previous parallel build. Compilation and both LLVM runtime stages
reach four overlapping process intervals, and all four vCPU threads accumulate
CPU time. Native C/C++ and CMake configure/make -j4 gates also pass with the
candidate interpreter on hosted AROS. The complete patch applies to the
original Python archive and reproduces the tested subprocess source exactly.


## Native transformation utility gate

`native-transform.cpp` parses a function with two arithmetic instructions,
clones its module, simplifies the clone to a single return of constant 42,
and verifies both the clone's validity and the original's unchanged instruction
count. A second simplification must report no change. It repeats this 100 times.
This tests native transformation utilities, not JIT execution or code generation.

Native Clang compiles and links the probe against the newly native-built
LLVMTransformUtils archive and the same 23 native libc++ LLVM archives used by
the bitcode gate. The hosted run passes. The identical executable also passes
20 executions across four QEMU workers, completing 2,000 clones and
simplifications, with peak process overlap four and activity on every vCPU.
The executable SHA-256 is `5957cd978abe2c6967981755d9a63d0e3d44fe3d4b73cc35479fb3c47fc198e0`.

Source, commands, archive hashes and hosted/QEMU output are retained in
`regression/native-transform/` and `qemu-parallel/native-transform-1/`.
The QEMU image uses the existing SMP baseline with the debug publication and
Python launch fixes; it is not the completed self-built OS.


## Native exit handlers and xargs

`native-exit-handler.c`, linked with `-nix`, checks six paths: return 0,
exit(7), _Exit(7), exit(7) from an atexit handler, _Exit(7) from an atexit
handler, and return 7. Each path prints MAIN; all except direct _Exit also
print HANDLER exactly once. Expected statuses are 0, 7, 7, 7, 7, 7.

The previous nixmain wrapper left its exit jump buffer installed after returning.
An atexit handler calling _Exit reproduced a double free in nixmain's cleanup.
The wrapper now restores the outer startup frame before returning. A noinline
helper owns setjmp while its caller owns the saved previous buffer, avoiding
indeterminate automatic storage after longjmp. No library vector or public
structure changes are involved.

The candidate passes a native-Clang-built six-path test, native C/C++ and CMake
make -j4 tests, Node worker messaging, libuv work/process tests, and Chromium
canvas/JavaScript smoke checks. These are focused regressions, not full RPCS3,
Chromium, or standalone V8 qualification. Before/after crash logs, library
hashes, commands and outputs are in `regression/native-nixmain-exit/`.

Separately, Contrib commit `9ddacc82` fixes xargs' vfork bookkeeping: the child
must not increment the parent's process count with a placeholder PID or erase
its accumulated failure status. The original two-item batch waited forever
for a nonexistent remaining child. The candidate passes actual shell pipeline,
failure-then-success, -P4, process-slot variable, missing-command baseline,
child exit 255, and empty-input checks. The preexisting missing-command EBADF
mapping still gives status 126; this patch does not claim to correct it.
The complete findutils patch applies to the original archive and reproduces
the tested source. Evidence is in `regression/native-iso-tools/xargs-wait/`.

The four-core QEMU replay in `qemu-parallel/nixmain-exit/` also passes:
96 six-path exit executions, 96 nested Python child-status controls, seven
make -j4 native compilations, archive/link and 1,000 demangler iterations,
100 bitcode and 20 IR executions. All seven object hashes match the prior
runtime. Compilation, bitcode and IR stages each reach four overlapping
process intervals, with CPU time on all four vCPUs. The ISO's posixc library
was extracted and hash-checked against the tested candidate. Regenerating the
posixc dispatch tables against revision `2d6c77fa37` confirms all 288 existing
entries retain their slots, with none added or removed. The main hosted build
was restarted at the GVNSink compilation boundary and resumed with the
candidate resident. This is validation on the existing SMP baseline, not a
completed self-built OS image.


## Native image-tool runtime regressions

`native-filesystem-locale.c` checks native and Unix statvfs paths, missing/null
arguments, and repeated locale queries. statvfs previously sent `/SYS/...` and
`.` directly to DOS Lock, returning ENOENT for existing paths. It now uses the
same path translator as stat/statfs. nl_langinfo(CODESET) now identifies the C
locale as ASCII, and setlocale queries preserve the current locale instead of
resetting C.UTF-8 to C. The native-built probe passes with the combined fixes.
Generated dispatch comparisons retain all 288 posixc and 431 stdc entries,
with none added, removed or moved. Evidence is in `regression/native-statvfs/`.

Native C/C++, CMake make -j4, Node worker messaging, libuv work/process tests,
and Chromium canvas/JavaScript smoke checks pass with the candidate libraries.
The same libraries also pass filesystem/locale and exit probes plus seven
native make -j4 compilations after booting the AROS-written ISO described below.
Object hashes match the previous runtime. This does not constitute full RPCS3
or standalone V8 qualification.


## Hosted file examination above 4 GB

The Unix-hosted handler now accepts the DOS64 object, handle and next-entry
examination packets directly. The previous path went through 32-bit ExAllData
and truncated a 4,801,626,112-byte ISO to 506,658,816 bytes, although fstat,
seek-to-end and reads beyond 4 GB already worked. The new path fills a separate
FileInfoBlock64 from host metadata; legacy 32-bit packet handling is unchanged.
Other hosted backends do not enable these new handlers.

Native tests create/read files of 1,234 and 4,294,975,489 bytes, compare path,
handle and directory-entry sizes, and check canaries around both FIB layouts.
The legacy large-file size remains its previous low 32 bits (8,193). Direct
ExamineFH64 also succeeds after renaming an open file. POSIX fstat after rename
still fails ENOENT through its pathname-discovery path: a matched small-file
comparison reproduces that on both old and candidate runtimes, so it is not
claimed fixed or introduced here. Fixtures, native probe source/binary,
before/after output and consumer smoke evidence are in
`regression/native-largefile/`. Native size extension currently writes zeros;
the large fixture is not claimed to remain sparse on the host filesystem.

## Native ISO creation and boot

Libiconv 1.19 and xorriso 1.5.8 build natively from the existing Contrib recipes
and patches. Libiconv stays in a private SDK so its headers do not replace the
default developer headers. Its UTF-8/Latin-1 round trip passes. One configure
probe exits from a worker and hits the existing foreign-stack exit guard;
a separate native probe joins its workers and confirms reader preference in
10/10 runs before supplying `gl_cv_pthread_rwlock_rdlock_prefer_writer=no`.
That worker-exit limitation is not claimed fixed.

Contrib commit `ec2ecad3` lets xorriso accept its existing virtual `/dev/zero`
system-area name without requiring a real device. The complete patch applies
to the original archive and matches the native-tested source. Native mkisofs
invocation with the AROS recipe's filesystem options creates an ISO and restores
both payloads byte-for-byte. Native xorriso then repacks the existing SMP test
image, preserving its boot information and adding the candidate CRT libraries.
The resulting 4,801,626,112-byte ISO has SHA-256
`7b0859f70e073ccc3b85a0756f377349806242325d2816e8d690c6d244057601`,
confirmed independently on AROS and the host.

That exact native-written ISO boots on four-core QEMU and passes locale/path,
six exit cases, native make -j4 compilation of seven objects, archive/link and
1,000 demangler iterations. All object hashes match the previous run.
Build/round-trip evidence is in `regression/native-iso-tools/`; boot evidence
is in `qemu-parallel/native-created-iso/`. The payload is the existing SMP
baseline plus fixes, not the completed full native self-build.
