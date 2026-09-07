# AROS Package Repository Protocol — v1

<!--
AI_PHASE: SOFTWARE_CENTER_PROTOCOL
AI_STATUS: IMPLEMENTED
AI_COMPLEXITY: MEDIUM
AI_NOTE: Shared wire contract between the AROS client (libpkg) and the
         publisher/server (arospkg-server). A copy lives in both repos;
         they MUST stay byte-identical. Bump `Protocol:` on breaking change.
-->

Copyright (C) 2026, The AROS Development Team. All rights reserved.

## 1. Repository layout (URL space)

All paths are relative to the repository base URL (e.g.
`http://updates.example.org/aros/`).  Everything is a plain file; the
server needs no dynamic behaviour beyond HTTP/1.1 GET.

```
<base>/
  Release                 repository descriptor (stanza)          signed
  Release.sig             detached Ed25519 signature of Release
  Packages                package index (stanza list)             signed
  Packages.sig
  Announcements           notification feed (stanza list)         signed
  Announcements.sig
  pool/<Filename>         package archives (.tar.gz) and screenshots (.png)
  keys/<KeyId>.pub        publisher public keys (informational; trust is
                          established out-of-band, see §5)
```

HTTP requirements for a conforming server: `Content-Length` on every 200;
`ETag` on `Release`/`Packages`/`Announcements` and honour `If-None-Match`
(→ `304`); honour `Range: bytes=N-` (→ `206`) for `pool/` so interrupted
downloads resume.  Servers MAY add TLS; clients MUST NOT require it in v1.

## 2. Stanza format

A stanza file is UTF-8 text made of **records** separated by one or more
blank lines.  A record is a sequence of `Key: value` lines.

* Keys: `[A-Za-z][A-Za-z0-9-]*`, case-insensitive on read, written in
  CamelCase.  Unknown keys MUST be ignored (forward compatibility).
* A value runs to end of line.  Continuation lines start with a single
  space; the continuation text (space stripped) is appended with `\n`.
  A continuation line containing only ` .` is an empty paragraph line.
* Lines beginning with `#` at column 0 are comments.
* Line ending is `\n`.  Writers MUST NOT emit `\r`.
* Values are plain text; no quoting, no escapes.
* Field order inside a record is not significant, but writers SHOULD emit
  the order documented below so files diff cleanly.

Example:

```
Package: SoftwareCenter
Version: 1.0.2
Summary: Package manager and updater
Description: Browse, install and update AROS software.
 .
 Second paragraph.
```

## 3. Records

### 3.1 `Release` (one record)

| Key         | Req | Meaning |
|-------------|-----|---------|
| `Protocol`  | yes | `1` |
| `Origin`    | yes | Human name of the repository ("AROS OPAL Updates") |
| `Suite`     | yes | Channel: `stable`, `testing`, `nightly` |
| `Arch`      | yes | Space-separated target list this repo serves: `pc-x86_64 pc-i386 linux-x86_64 all` |
| `Date`      | yes | RFC 3339 UTC, e.g. `2026-09-05T20:11:00Z` |
| `Serial`    | yes | Monotonic integer, incremented on every `index`/`announce` |
| `Packages`  | yes | `SHA256 <hex> <size>` of the current `Packages` file |
| `Announcements` | yes | `SHA256 <hex> <size>` of the current `Announcements` file |
| `KeyId`     | yes | Key id (hex, first 16 bytes of SHA-256 of the raw 32-byte pubkey) that signed this release |

Clients fetch `Release` + `Release.sig` first; `Packages`/`Announcements`
are then verified by hash against the signed `Release` (their own `.sig`
files exist for tools that fetch them standalone).

### 3.2 `Packages` — one record per package version

| Key            | Req | Meaning |
|----------------|-----|---------|
| `Package`      | yes | Name, `[A-Za-z0-9][A-Za-z0-9_.+-]*`, ≤ 30 chars (fits a filename everywhere) |
| `Version`      | yes | `MAJOR.MINOR[.PATCH][-REVISION]`; compared numerically per component, then revision |
| `Arch`         | yes | One target or `all` |
| `Summary`      | yes | One line, ≤ 72 chars |
| `Description`  | yes | Multi-line (continuations) |
| `Type`         | no  | `package` (default), `plugin`, `model`, `addon` — add-ons install *into* another package (see §4.3) |
| `Extends`      | no  | Host package name for add-ons (implies `Depends:` on it) |
| `Subdir`       | no  | Directory below the host package's install path that the add-on payload lands in, e.g. `models`, `Plugins` |
| `Prefix`       | no  | Assign the payload is rooted at (default `SYS:`); for add-ons whose host is image-shipped and unregistered, e.g. `Prefix: LlamaCpp:` |
| `Replaces`     | no  | Comma-separated packages whose files this one may overwrite |
| `Filename`     | yes* | Path under `pool/`, e.g. `pool/SoftwareCenter_1.0.2_pc-x86_64.tar.gz` (*or `URL`) |
| `URL`          | yes* | Absolute `http://` URL to fetch the archive from instead of `pool/` (large models on a CDN or Hugging Face); the signed `SHA256`/`Size` still gate acceptance, so the host is untrusted |
| `Size`         | yes | Bytes of the archive |
| `SHA256`       | yes | Hex digest of the archive |
| `InstalledSize`| no  | Bytes after extraction (for UI) |
| `Depends`      | no  | Comma-separated `Name [(>= Version)]` list; installed in dependency order, no solver beyond that in v1 |
| `Conflicts`    | no  | Comma-separated names that must not be installed simultaneously |
| `Provides`     | no  | Virtual names |
| `Section`      | no  | `system`, `libs`, `drivers`, `network`, `graphics`, `sound`, `development`, `games`, `utilities`, `emulation` |
| `Priority`     | no  | `required`, `important`, `standard`, `optional` (default) |
| `Homepage`     | no  | URL |
| `Maintainer`   | no  | `Name <email>` |
| `Licence`      | no  | SPDX id or free text |
| `Screenshot`   | no  | Space-separated `pool/…png` paths; each followed in the same value by nothing — hashes live in `ScreenshotSHA256` (same order) |
| `ScreenshotSHA256` | no | Space-separated hex digests, same order as `Screenshot` |
| `Changes`      | no  | Multi-line changelog for this version |
| `Startup`      | no  | `yes` if the archive ships `S/Package-Startup` (informational; the client inspects the archive) |
| `Reboot`       | no  | `yes` if a reboot is required after install (kickstart modules, handlers) |
| `Date`         | no  | RFC 3339 build/publish time |

### 3.3 `Announcements` — one record per notification

| Key        | Req | Meaning |
|------------|-----|---------|
| `Id`       | yes | Monotonic integer; client stores the highest seen |
| `Date`     | yes | RFC 3339 UTC |
| `Severity` | yes | `info`, `notice`, `critical` |
| `Title`    | yes | ≤ 60 chars |
| `Body`     | yes | Multi-line |
| `Package`  | no  | Related package name (the notifier can offer *Open* directly on it) |
| `URL`      | no  | More-info link |
| `Expires`  | no  | RFC 3339; clients hide expired records |

### 3.4 `PKGINFO` — inside every archive (one record)

Same keys as a `Packages` record **except** `Filename`, `Size`, `SHA256`,
`Screenshot*`, which the publisher computes.  The publisher copies every
other key verbatim into the index, so `PKGINFO` is the single source of
truth for metadata.  `Screenshot` files are shipped in the archive under
`screenshots/` and lifted into `pool/` by the publisher.

## 4. Archive format

`<Package>_<Version>_<Arch>.tar.gz` — gzip (RFC 1952) wrapping a POSIX
**ustar** tar (RFC / IEEE 1003.1-1988 headers; pax/GNU long-name extensions
are NOT permitted — names ≤ 100 chars, prefix field allowed).

```
PKGINFO                     stanza (§3.4)                       required
files/<dest>/...            payload, rooted at the install prefix
screenshots/*.png           optional, lifted into pool/ by the publisher
S/Package-Startup           optional Startup-Sequence hook (§4.2)
```

### 4.1 Payload destinations

`files/` mirrors the target volume.  The install prefix is `SYS:` unless the
`PKGINFO` `Prefix:` key names another assign (e.g. `Prefix: Extras:`).
Entries in `files/` are ordinary directories and regular files; symlinks,
devices and hard links are rejected.  File protection bits are taken from the
tar mode: `x` → `e`, `w` → `w`, `r` → `r`; the `s` (script) bit is set when
the file lives under `S/`.

Every file the client writes is recorded in the installed DB
(`SYS:Prefs/Packages/installed/<Package>.files`) so removal is exact.  A
file already present and **not** owned by another package is backed up as
`<name>.pkgsave` and restored on removal; a file owned by another package is
a `Conflicts` error unless `Replaces:` names that package.

### 4.2 Activation (the existing OS convention)

If the archive contains `S/Package-Startup`, the client installs the payload
under `SYS:Packages/<Package>/` instead of merging into `SYS:`, then writes
`ENVARC:SYS/Packages/<Package>` containing that path.  Startup-Sequence
already iterates `ENV:SYS/Packages` and, **with the package directory as
the current directory**, executes each package's `S/<Package>-Startup` if
present, else `S/Package-Startup` — AHI and AROSTCP ship this way today.
The client runs the same script the same way once, right after install, so
the package is usable without a reboot.  Scripts must therefore use
root-relative paths (`Assign Foo: ""`, `Path C ADD`, `Assign DEVS: Devs
ADD`) and never hardcode `SYS:Packages/` — the root is configurable
(`ENV:SoftwareCenter/PkgRoot`) and the absolute path is in
`${SYS/Packages/<Package>}`.  Removal deletes the env var first, then the
tree.

### 4.3 Add-ons: plugins and models for apps without an installer

An add-on (`Type: plugin|model|addon`) is an ordinary archive whose `files/`
tree is rooted at the **host package's install directory** joined with
`Subdir:`.  The host directory is resolved in this order:

1. the installed-DB record of `Extends:` (host installed by this tool);
2. `ENVARC:SYS/Packages/<Extends>` (host registered by the OS convention);
3. `Prefix:` from the add-on's record (host shipped in the image);
4. otherwise `PKGERR_DEPENDS`.

Example — a GGUF model for LlamaCpp: `Type: model`, `Extends: LlamaCpp`,
`Subdir: models`, `URL: https://huggingface.co/…/q4.gguf` — the client
downloads with resume, checks the signed digest, and drops the file under
`<LlamaCpp dir>/models/`.  The host app needs no installer of its own; the
Software Center lists add-ons grouped under their host.

### 4.4 Multiple sources

The client reads `SYS:Prefs/Packages/sources` — one stanza per repository
(`Name:`, `URL:`, `KeyId:`, `Enabled:`).  Indexes are merged; the record
with the highest `Version` wins per `(Package, Arch)`, ties broken by
source order.  A vendor (e.g. a commercial application) therefore hosts its
own repository, signed with its own key, and its plugins appear alongside
everything else.  Per-source HTTP authentication (paid repositories) is a
v2 item: token stored in `credentials.library`'s keyring, sent as
`Authorization: Bearer`.

## 5. Signing and trust

* Algorithm: **Ed25519** (RFC 8032), detached.  `X.sig` is exactly
  64 raw bytes — the signature over the *exact bytes* of `X`.
* `KeyId` = lowercase hex of the first 16 bytes of `SHA-256(pubkey32)`.
* The client holds trusted public keys as raw 32-byte files
  `SYS:Prefs/Packages/trusted/<KeyId>.pub`.  A key is trusted only if present
  there; `keys/` on the server is informational.
* Acceptance rules (client MUST enforce all):
  1. `Release.sig` verifies with a trusted key whose id equals `KeyId`.
  2. `Release.Serial` ≥ the cached serial (rollback protection); a lower
     serial is rejected and reported.
  3. `Packages` / `Announcements` bytes hash to the digests in `Release`.
  4. An archive is used only if `Size` and `SHA256` match its index record.
  5. A screenshot is displayed only if its `ScreenshotSHA256` matches.
* Anything failing verification is discarded, never cached, never executed.

## 6. Client polling

* `UpdateNotifier` fetches `Release` with `If-None-Match: <cached ETag>`
  every `CheckInterval` minutes (default 60; first check 2 min after start).
  `304` → nothing to do.  Otherwise it re-verifies the chain (§5), compares
  the index against the installed DB, and surfaces:
  * `N updates available` when any installed package has a higher version
    in the index for its arch (or `all`);
  * each `Announcements` record with `Id` greater than the last seen and not
    expired.
* Clients SHOULD add jitter of ±10 % to the interval and MUST back off
  exponentially (cap 24 h) on network failure.
* `User-Agent: arospkg/1 (<Arch>; AROS <version>)`.

## 7. Version comparison

Split `Version` on `-` into `upstream` and `revision`.  Split `upstream` on
`.`; compare component-wise as integers when both are numeric, else as
strings; a missing component is `0`.  Then compare `revision` the same way.
`1.0.10 > 1.0.9`, `1.2 == 1.2.0`, `2.0-2 > 2.0-1`.

## 8. Errors (client-side, stable strings for the CLI and log)

`PKGERR_NETWORK`, `PKGERR_HTTP <status>`, `PKGERR_NOKEY`, `PKGERR_BADSIG`,
`PKGERR_ROLLBACK`, `PKGERR_BADHASH`, `PKGERR_BADARCHIVE`, `PKGERR_CONFLICT`,
`PKGERR_DEPENDS`, `PKGERR_NOTFOUND`, `PKGERR_IO`, `PKGERR_ABORTED`.

## 9. Crash reports (optional, server-side intake)

A distribution MAY accept crash reports from `CrashReporter`
(`SYS:WBStartup/CrashReporter`).  Intake is a single dynamic endpoint;
a purely static host simply does not offer it and the client keeps reports
on disk.

### 9.1 Endpoint

```
POST <base>api/crash/v1/report
Content-Type: text/plain; charset=utf-8
Content-Length: <n>            (n <= 1 MiB)

<report text, §9.2>
```

Responses: `201 Created` (stored; body `Id: <id>` and `Status: stored`,
`Location: /api/crash/v1/report/<id>`), `200 OK` with `Status: duplicate`
when the identical body is already on file (a retry after a lost reply is
not a second report), `413` too large, `415` wrong content type, `422`
malformed (§9.2 required keys missing or unsupported `Report-Version`),
`404` when the server has no intake.  Any 2xx means the client marks the
report sent; anything else leaves it unsent for a later retry.

The client derives `<base>` from its first enabled package source unless
`ENV:CrashReporter/URL` names a full URL.  Nothing is uploaded without the
user clicking *Send* or enabling *AutoSend*.

### 9.2 Report text

UTF-8.  A header stanza (`Key: value`, one per line), one blank line, then
free-form bracketed sections (`[Registers]`, `[Backtrace]`, `[Modules]`,
`[Log]`) that a server stores verbatim and never parses — with one
exception: `[Description]`, the user's own words on what they were doing
when it happened (the client asks on the crash card), which a server MAY
extract for its maintainer views.  The section is appended after the
machine-generated ones and may be absent.

Required keys: `Report-Version` (`1`), `Task`, `Alert` (8 hex digits),
`Signature` (§9.3).  Defined optional keys: `Reporter`, `Date`,
`Task-Type` (`Task`|`Process`), `Program` (CLI command name), `Alert-Kind`
(`DeadEnd`|`Recoverable`), `Alert-Subsystem`, `Alert-Text`, `Exception`
(CPU vector, decimal), `Exception-Name`, `Fault-Address`, `Error-Code`,
`PC`, `Module`, `Function`, `Offset`, `Location`, `Summary`, `Arch`,
`AROS-Version`, `AROS-Release`, `AROS-Variant`, `ABI`, `Exec-Version`,
`CPUs`, `Memory-Free`, `Memory-Total`, `Memory-Largest`, `Product` (§9.5).
Unknown keys are preserved and ignored.

### 9.3 Signature

`Signature` is the grouping key: `<Module>!<Function>+0x<Offset>/<Alert>`
when the faulting PC symbolises, `<Module>+0x<Offset>/<Alert>` when only
the segment is known, and `?/<Alert>` otherwise.  Servers group by exact
string; it is stable across reboots of the same build because offsets are
module-relative.

### 9.4 Maintainer views (optional)

`GET <base>api/crash/v1/` (HTML listing), `.../groups` (stanza list, one
record per `Signature` with `Count`, `First`, `Last`, sample headers,
`Description: <id>: <text>` for reports that carry one, and
`Reports: <id>...`) and `.../report/<id>` (verbatim report).  Servers
SHOULD keep these off by default or behind authentication; reports are
not public data.

### 9.5 Program annotations

A program can hand the reporter its own state — the page a browser was
showing, the file an editor had open — through a `CrashAnnotationBlock`
(`crashreporter/annotations.h` in the SDK): a fixed table of key/value
strings in public memory that the program rewrites in place at any time and
registers with `AddPort()` under the name `CrashReporter.annotations`.
When one of the program's tasks crashes, the reporter copies the block and
the report carries:

* `Product: <name> <version>` in the header (e.g. `Chromium 124.0.6367.0`),
  from the block's product string, when the block has one;
* an `[Annotations]` section, `key: value` per line in table order, after
  `[Log]` and before `[Description]`.  Values are single-line (control
  characters become spaces); a value longer than one slot is split by the
  program into `key__1`, `key__2`, ... chunks (Chromium's convention),
  which a server MAY stitch back together for display.

Keys are the program's own vocabulary and a server MUST NOT require any of
them.  Servers MAY index `Product` and MAY surface well-known keys
(Chromium: `url-chunk`, `top-origin`, `ptype`, `pid`, `num-tabs`,
`num-windows`, `switch-N`, `gpu-*`) in their views; anything else is shown
verbatim.
