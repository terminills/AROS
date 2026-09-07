# AROS Software Center — Design

<!--
AI_PHASE: SOFTWARE_CENTER_DESIGN
AI_STATUS: IMPLEMENTED
AI_COMPLEXITY: HIGH
AI_NOTE: Job JOB-0090 (amiwest-2026). Client lives in this directory; the
         repository server lives in the sibling repo `arospkg-server`.
         PROTOCOL.md is the shared wire contract — change it on BOTH sides.
AI_DEPENDENCIES: z.static (zlib inflate), credentials_crypto.c (SHA-256),
         bsdsocket.library v4, muimaster (Zune), NList MCC.
-->

Copyright (C) 2026, The AROS Development Team. All rights reserved.

## 0. Purpose

A package manager and system updater for AROS with a notification system —
the piece that turns "AROS installs natively on NVMe" into "AROS keeps
itself up to date".  Demo target: AmiWest 2026.

Both sides are built here-and-now; **deployment is deliberately deferred**
(terminills, 2026-09-05).  Everything the client fetches is a signed static
file, so the server can later be anything from the Python reference server
to a plain static web host.

## 1. Components

```
 AROS client (this directory)                     Host side (arospkg-server)
 ─────────────────────────────                    ─────────────────────────
 SYS:System/SoftwareCenter      Zune GUI          arospkg   publisher CLI
 SYS:WBStartup/UpdateNotifier   background        arospkgd  reference HTTP server
 C:Pkg                          CLI
 libpkg.a                       core (static)      repo/    on-disk repository
```

### 1.1 `libpkg` — core (static linklib)

One API used by all three programs.  Static for the demo (no genmodule/ABI
ceremony under deadline); the API is shaped so it can be promoted to
`package.library` afterwards without changing callers.

| Module            | Responsibility                                                            |
|-------------------|---------------------------------------------------------------------------|
| `pkg_stanza.c`    | Parse/emit the stanza format (PROTOCOL.md §2)                              |
| `pkg_http.c`      | HTTP/1.1 GET over bsdsocket: DNS, Host, Content-Length, Range, ETag, to-memory and to-file, cancellable via signal mask (`WaitSelect`) |
| `pkg_verify.c`    | SHA-256 (reused from `credentials_crypto.c`) + Ed25519 verify (vendored TweetNaCl) |
| `pkg_tar.c`       | gzip (zlib inflate, `z.static`) + ustar reader                              |
| `pkg_repo.c`      | Refresh: fetch `Packages`/`Packages.sig`, verify against trusted key(s), cache under `SYS:Prefs/Packages/` |
| `pkg_db.c`        | Installed database: one stanza file + file list per package under `SYS:Prefs/Packages/installed/` |
| `pkg_install.c`   | Download → verify → stage in `T:` → commit files → register `ENVARC:SYS/Packages/<Name>` → run `Startup:` |
| `pkg_remove.c`    | Reverse of install from the recorded file list                            |
| `pkg_announce.c`  | Fetch/verify `Announcements`, track last-seen sequence number             |
| `pkg_prefs.c`     | `SYS:Prefs/Packages/prefs` (repo URL, check interval, notify on/off)          |

Both roots are overridable for live CDs (read-only `SYS:`) and tests:
`ENV:SoftwareCenter/BaseDir` replaces `SYS:Prefs/Packages`,
`ENV:SoftwareCenter/PkgRoot` replaces `SYS:Packages`.  Read once in
`pkg_open()`.

Progress and log lines flow through a caller-supplied `struct Hook`
(`PKGH_PROGRESS`, `PKGH_LOG`, `PKGH_ABORTQUERY`) so the GUI, CLI and notifier
each render them their own way.  Nothing in libpkg touches Intuition.

### 1.2 `SoftwareCenter` — Zune GUI (`SYS:System/SoftwareCenter`)

Modelled on the in-tree patterns that already work:

* `Register` tabs — **Updates / Installed / Available**
  (SysExplorer `computer_page_cl.c`).
* `NListview` with `MUIA_NList_Format` + `MUIA_NList_DisplayHook`
  (`system/find/findgroup_class.c`) — columns: Name · Installed · Available
  · Size · Summary.
* Details pane: `Floattext` (description, dependencies, changelog) plus a
  **screenshot thumbnail** (`Screenshot:` PNGs from the repo pool, cached
  under `SYS:Prefs/Packages/cache/`).  Clicking the thumbnail opens a **large
  view window** scaled to fit the current screen (arrows cycle when a
  package ships several).  Decode via `datatypes.library` →
  `MUI Bitmap`/`Bodychunk`; Dtpic is *not* relied upon (it had a
  blank-render bug on hosted that is still open).
* Actions: **Install · Remove · Update All · Refresh**; `Gauge` with
  `MUIA_Gauge_InfoText` driven from a worker (InstallAROS `ia_main.c`).
* All network/IO on a **worker Process**; results delivered with
  `MUIM_Application_PushMethod` (`muimaster/classes/application.c:1914`).
* Prefs window: repository URL, check interval, "notify me", trusted key.
* Resolution independent: layout groups only, no pixel geometry, font from
  the screen (see rpcs3-zune-port `SPEC.md:94`).

### 1.3 `UpdateNotifier` — background notifier (`SYS:WBStartup/UpdateNotifier`)

Lifecycle copied from `tools/commodities/Exchange.c`: Zune application with
`MUIA_Application_BrokerHook` + `MUIA_Application_DiskObject`, no window at
start, CloseRequest → iconify, never quits on its own.

* Timer loop (`timer.device`, interval from prefs; default 60 min; first
  check ~2 min after boot so the network package has come up).
* On `updates > 0` **or** a new announcement → open a small **borderless
  top-right Zune window** (`MUIA_Window_Borderless`, positioned like
  BoingIconBar `iconbar.c:1166`) — title, one-line body, **[Open] [Later]**.
  *Open* launches `SYS:System/SoftwareCenter` via `OpenWorkbenchObject`.
* Announcements carry a `Severity:` (info/notice/critical); critical ones
  are not auto-dismissed.
* Started by Wanderer's `ExecuteStartup` from `SYS:WBStartup` via
  `%build_icons` (`UpdateNotifier.info.src` with `DONOTWAIT`).

There is no notification-area concept anywhere in the tree today; this is
the first one.  Kept inside the tool for the deadline, factored so it can
become `notification.library` later.

### 1.4 `Pkg` — CLI (`C:Pkg`)

`Pkg REFRESH | LIST [INSTALLED|UPDATES] | SEARCH pat | INFO name |
INSTALL name | REMOVE name | UPGRADE | ANNOUNCEMENTS` via `ReadArgs`.
Exists first: it is the test harness for libpkg before any GUI, and it
makes the demo scriptable.

### 1.5 Server side (`arospkg-server`, Python 3, stdlib + optional `cryptography`)

* `arospkg keygen` — Ed25519 keypair (`cryptography` if present; pure-Python
  reference implementation otherwise — signing an index is not hot-path).
* `arospkg publish <archive.tar.gz>` — validates `PKGINFO`, computes SHA-256
  and size, drops it into `pool/`.
* `arospkg index` — regenerates `Packages` + `Packages.sig` from `pool/`.
* `arospkg announce --title --body [--severity]` — appends to
  `Announcements`, re-signs.
* `arospkgd --root repo/ --port 8090` — threaded static server with ETag /
  `If-None-Match`, `Range`, correct `Content-Length`; only ever serves files
  under the repo root.

Because everything is a signed static file the server has **no state the
client trusts** — trust is the publisher's key, so any static host works.

## 2. Decisions (and why)

| Decision | Why |
|----------|-----|
| **Gzipped ustar tarball** as package format | No zip/lha/xad *library* in the default build; `z.library`/`z.static` are in-tree and built for pc-x86_64; Python `tarfile` makes it free on the server. |
| **Link zlib statically** (`uselibs="z.static"`) | The updater must never be broken by a missing library it is meant to update. |
| **Stanza format** (`Key: value` blocks) everywhere | No JSON in core AROS; stanzas are trivial in C, human-readable, diffable, and ReadArgs-friendly. |
| **Ed25519 signature over the index + SHA-256 per package**, plain HTTP | TLS is contrib-only (OpenSSL/curl not in default build). Signatures give integrity + authenticity; TLS would add only privacy — a post-AmiWest add-on via the OpenSSL port. |
| **Reuse `ENVARC:SYS/Packages/<Name>` + `S/Package-Startup`** | Already honoured by Startup-Sequence (AHI, AROSTCP ship this way); InstallAROS has a literal `TODO: enumerate available packages`. No new convention. |
| **Polling, not push** for notifications | ETag polling is nearly free, works through any NAT/KVM, needs no server state; interval is a pref. |
| **Static `libpkg.a` now, `package.library` later** | Zero genmodule/ABI risk under deadline; API is library-shaped from day one. |
| Reuse SHA-256 from `credentials_crypto.c`; vendor TweetNaCl for Ed25519 | Only vendor what does not exist in tree; nothing is reimplemented. |

## 3. Trust model

* The client ships with the publisher's Ed25519 public key(s) in
  `SYS:Prefs/Packages/trusted/` (installable/rotatable as files).
* `Packages` and `Announcements` are accepted only if their detached
  signature verifies against a trusted key **and** their `Date:` is not older
  than the cached copy (rollback protection).
* A package archive is accepted only if its SHA-256 and size match the
  signed index entry.  Nothing unsigned is ever executed or copied.
* `Startup:` scripts run with the same rights as Startup-Sequence's package
  hook does today — no new privilege.

## 4. Milestones

| # | Milestone | Acceptance |
|---|-----------|------------|
| M1 | PROTOCOL.md + `arospkg` publisher + `arospkgd`; a real repo with 2–3 packages | `curl` fetches a verifying `Packages`/`.sig` pair |
| M2 | `libpkg` + `C:Pkg` REFRESH/LIST/INFO | `Pkg REFRESH` on hosted AROS verifies the index and lists packages |
| M3 | `Pkg INSTALL/REMOVE/UPGRADE` | install → files present + `ENVARC:SYS/Packages/<Name>`; remove → clean |
| M4 | `SoftwareCenter` GUI | tabs, list, details, install/remove/update-all with live gauge |
| M5 | `UpdateNotifier` (`notifier/UpdateNotifier.c`, built 2026-09-06; runtime verification pending) | boots from WBStartup; popup on new updates / announcement; Open launches the GUI. Tooltypes/args: `CX_POPUP`, `CX_PRIORITY`, `FIRSTCHECK=<s>` (first poll delay, default 120) |
| M6 | Demo package set + on-DUT run (native NVMe install) | end-to-end on real hardware, screenshots registered as evidence |

### 4.1 Runtime notes (QEMU, pc-x86_64-smp, 2026-09-06)

Found while bringing M4 up in QEMU on the v22 DUT ISO
(`qemu-test/logs/scgui-qemu-20260906/`):

* **NList 64-bit sentinel.** `MUIV_NList_ConstructHook_String` /
  `DestructHook_String` were bare `-1` in `NList_mcc.h`; through a varargs
  taglist they arrive as `0x00000000FFFFFFFF`, NList's `== (IPTR)-1` test
  fails and it *calls* the value as a hook (PC 0xAFFFF).  Fixed in the
  header (`((IPTR)-1)`, same shape as `MUIV_NList_Font`).
* **No non-ASCII in runtime strings.** This tree's muimaster text engine
  routes every chunk through `TextUTF8()`; any byte >= 0x80 makes
  `graphics.library` open `NotoColorEmoji.font` -> freetype2 -> codesets,
  and the caller blocks forever in `ObtainSemaphore` (TRIG-0914, owned by
  codex).  Until that is fixed the client uses ` | ` and `->` instead of
  ` · ` and `→`.
* **Description reflow.** The index keeps the publisher's hard wraps; the
  GUI joins continuation lines Debian-style (blank = paragraph, leading
  space = verbatim) before handing the text to `Floattext`.
* **CD hot-swap is not a test path.** Replacing the payload ISO with
  `change cd1` in QEMU wedges the guest's CD handler; relaunch instead.
* **Picture datatypes must be initialised.** The diag ISO's
  Startup-Sequence skips `C:AddDataTypes`, so *no* picture datatype exists:
  MultiView hex-dumps a PNG and the screenshot pane stays blank.
  `S/scdemo` now runs `C:AddDataTypes REFRESH QUIET` first, and
  `PictureView` says "Cannot display image" instead of drawing nothing
  when a file is set but does not decode.
* **The installed DB is shared state.** `pkg_check()` / `pkg_refresh()`
  re-read `installed/` on every call: the notifier's long-lived context
  otherwise kept the snapshot from `pkg_open()` and reported "up to date"
  after the GUI (another process) had changed what is installed.

Post-AmiWest: `package.library`, TLS via OpenSSL port, delta updates,
dependency solver beyond `Depends:` ordering, `notification.library`,
system-image (kickstart/SYS:) updates with rollback, feed from
`aros-distro-index` (ADT per-target built/included data).

## 5. Layout

```
workbench/system/SoftwareCenter/
  DESIGN.md  PROTOCOL.md
  libpkg/        core + vendored tweetnacl (single .c/.h)
  Pkg/           CLI
  gui/           SoftwareCenter (Zune)
  notifier/      UpdateNotifier (Zune commodity) + icons/
  tests/         host-side unit tests for stanza/tar/verify (built with the
                 host cc — run before every commit)
```

Server: `/home/terminills/Documents/GitHub/arospkg-server/` (own git repo).
