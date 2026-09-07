/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: libpkg — package manager core used by SoftwareCenter (Zune GUI),
          UpdateNotifier and the Pkg CLI.  Static linklib today; the API is
          shaped so it can become package.library without touching callers.

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: HIGH
    AI_NOTE: Nothing in here touches Intuition.  Progress/log/abort flow
             through a caller-supplied Hook so each front-end renders them
             its own way.  See PROTOCOL.md for the wire contract.
*/

#ifndef LIBPKG_PKG_H
#define LIBPKG_PKG_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <utility/hooks.h>

/* ------------------------------------------------------------------------ */
/* Errors (stable strings via pkg_strerror(); PROTOCOL.md §8)               */

#define PKGERR_OK           0
#define PKGERR_NETWORK      1
#define PKGERR_HTTP         2   /* pkg_last_http_status() has the code   */
#define PKGERR_NOKEY        3
#define PKGERR_BADSIG       4
#define PKGERR_ROLLBACK     5
#define PKGERR_BADHASH      6
#define PKGERR_BADARCHIVE   7
#define PKGERR_CONFLICT     8
#define PKGERR_DEPENDS      9
#define PKGERR_NOTFOUND     10
#define PKGERR_IO           11
#define PKGERR_ABORTED      12
#define PKGERR_NOMEM        13
#define PKGERR_NOTINSTALLED 14
#define PKGERR_BADPREFS     15
#define PKGERR_BADURL       16
#define PKGERR_FORMAT       17  /* malformed response/index/prefs      */

/* "p or dflt" for printf-style output without TEXT-vs-char pointer
   mismatches (AROS TEXT is unsigned char; string literals are char). */
#define PKG_STR(p, dflt)    ((p) ? (const char *)(p) : (const char *)(dflt))

/* ------------------------------------------------------------------------ */
/* Hook messages                                                             */

#define PKGH_LOG          1   /* text line for a log view / console         */
#define PKGH_PROGRESS     2   /* phase label + counters                      */
#define PKGH_ABORTQUERY   3   /* return non-zero from the hook to abort      */

struct PkgHookMsg
{
    ULONG        Type;      /* PKGH_#?                                       */
    CONST_STRPTR Text;      /* LOG: the line; PROGRESS: phase label          */
    CONST_STRPTR Package;   /* package being worked on, or NULL              */
    ULONG        Done;      /* PROGRESS: items done                          */
    ULONG        Total;     /* PROGRESS: items total (0 = unknown)           */
    UQUAD        BytesDone; /* PROGRESS: bytes transferred in this phase     */
    UQUAD        BytesTotal;/* PROGRESS: bytes expected (0 = unknown)        */
};

/* ------------------------------------------------------------------------ */
/* Preferences (SYS:Prefs/Packages/prefs, one stanza)                          */

#define PKG_URL_LEN   256
#define PKG_ARCH_LEN  32

struct PkgPrefs
{
    TEXT  Arch[PKG_ARCH_LEN];      /* target this machine accepts, e.g. pc-x86_64 */
    ULONG CheckInterval;           /* minutes; 0 = never                    */
    BOOL  Notify;                  /* show the notifier popup               */
    BOOL  NotifyAnnouncements;
    ULONG LastSerial;              /* highest Release.Serial accepted       */
    ULONG LastAnnouncementId;      /* highest Announcements.Id shown        */
};

/* A repository source (SYS:Prefs/Packages/sources, one stanza each) */
struct PkgSource
{
    struct Node Node;              /* ln_Name = Name                        */
    TEXT   URL[PKG_URL_LEN];       /* base URL, always ends with '/'        */
    TEXT   KeyId[33];              /* hex, may be empty = any trusted key   */
    BOOL   Enabled;
    /* runtime */
    TEXT   ETag[80];               /* last Release ETag                     */
    ULONG  Serial;                 /* last accepted Release.Serial          */
};

/* ------------------------------------------------------------------------ */
/* Package records                                                           */

#define PKGSTATE_AVAILABLE   0     /* in an index, not installed            */
#define PKGSTATE_INSTALLED   1     /* installed, up to date                 */
#define PKGSTATE_UPGRADABLE  2     /* installed, newer version in an index  */
#define PKGSTATE_ORPHAN      3     /* installed, no longer in any index     */

#define PKGTYPE_PACKAGE 0
#define PKGTYPE_PLUGIN  1
#define PKGTYPE_MODEL   2
#define PKGTYPE_ADDON   3

struct PkgRecord
{
    struct Node   Node;            /* ln_Name = Name                        */
    STRPTR        Name;
    STRPTR        Version;         /* newest available (or installed if orphan) */
    STRPTR        InstalledVersion;/* NULL if not installed                 */
    STRPTR        Arch;
    STRPTR        Summary;
    STRPTR        Description;
    STRPTR        Section;
    STRPTR        Depends;
    STRPTR        Extends;         /* host package for add-ons              */
    STRPTR        Subdir;
    STRPTR        Homepage;
    STRPTR        Maintainer;
    STRPTR        Licence;
    STRPTR        Changes;
    STRPTR        Screenshots;     /* space-separated pool paths            */
    STRPTR        SourceName;      /* which source this record came from    */
    UQUAD         Size;
    UQUAD         InstalledSize;
    UBYTE         State;           /* PKGSTATE_#?                           */
    UBYTE         Type;            /* PKGTYPE_#?                            */
    BOOL          Reboot;
    APTR          Stanza;          /* full record for pkg_record_get()      */
};

struct PkgAnnouncement
{
    struct Node   Node;            /* ln_Name = Title                       */
    ULONG         Id;
    STRPTR        Date;
    STRPTR        Severity;        /* "info" | "notice" | "critical"        */
    STRPTR        Title;
    STRPTR        Body;
    STRPTR        Package;         /* may be NULL                           */
    STRPTR        URL;             /* may be NULL                           */
    STRPTR        SourceName;
    BOOL          Seen;
};

/* pkg_list() filters */
#define PKGF_ALL        0
#define PKGF_INSTALLED  1
#define PKGF_UPDATES    2
#define PKGF_AVAILABLE  3          /* not installed                         */

/* ------------------------------------------------------------------------ */
/* Context                                                                   */

struct PkgContext;                 /* opaque                                */

/* Open a context.  hook may be NULL.  Loads prefs, sources and the cached
   (previously verified) indexes — no network. */
struct PkgContext *pkg_open(struct Hook *hook);
void               pkg_close(struct PkgContext *ctx);

struct PkgPrefs   *pkg_prefs(struct PkgContext *ctx);     /* live copy      */
LONG               pkg_prefs_save(struct PkgContext *ctx);
struct List       *pkg_sources(struct PkgContext *ctx);   /* of PkgSource   */
LONG               pkg_sources_save(struct PkgContext *ctx);
LONG               pkg_source_add(struct PkgContext *ctx, CONST_STRPTR name,
                                  CONST_STRPTR url, CONST_STRPTR keyid);
LONG               pkg_source_remove(struct PkgContext *ctx, CONST_STRPTR name);

/* Trusted keys: SYS:Prefs/Packages/trusted/<KeyId>.pub (raw 32 bytes) */
LONG pkg_trust_add(struct PkgContext *ctx, CONST_STRPTR pubfile);
LONG pkg_trust_list(struct PkgContext *ctx, struct List *out); /* Node ln_Name = KeyId */
void pkg_free_namelist(struct List *list);                     /* frees a name list  */

/* ------------------------------------------------------------------------ */
/* Repository                                                                */

/* Fetch Release/Packages/Announcements from every enabled source, verify the
   chain (PROTOCOL.md §5), cache under SYS:Prefs/Packages/cache/<Name>/.
   With force == FALSE a source whose Release ETag is unchanged is skipped.
   Returns PKGERR_OK if at least one source refreshed or is current. */
LONG pkg_refresh(struct PkgContext *ctx, BOOL force);

/* Cheap poll for the notifier: conditional GET on Release only.  Fills the
   counts without downloading indexes unless something changed. */
LONG pkg_check(struct PkgContext *ctx, ULONG *updates, ULONG *newannouncements);

/* Re-read the installed database from disk.  It is shared with every other
   process (C:Pkg, the GUI, the notifier); a long-lived context calls this
   before listing so it does not report the state it was opened with.
   pkg_refresh() and pkg_check() do it implicitly. */
LONG pkg_reload(struct PkgContext *ctx);

/* Build a list of PkgRecord (caller frees with pkg_free_records). */
LONG pkg_list(struct PkgContext *ctx, ULONG filter, struct List *out);
void pkg_free_records(struct List *list);

/* Find one record by name (in the merged index or installed DB). */
struct PkgRecord *pkg_find(struct PkgContext *ctx, CONST_STRPTR name);
void              pkg_free_record(struct PkgRecord *rec);

/* Any raw key of a record ("Homepage", "Changes", ...), NULL if absent. */
CONST_STRPTR pkg_record_get(struct PkgRecord *rec, CONST_STRPTR key);

/* Add-ons of a host package (records with Extends == host). */
LONG pkg_list_addons(struct PkgContext *ctx, CONST_STRPTR host, struct List *out);

/* ------------------------------------------------------------------------ */
/* Actions (all long-running: call from a worker task, they emit hook msgs) */

LONG pkg_install(struct PkgContext *ctx, CONST_STRPTR name);
LONG pkg_remove(struct PkgContext *ctx, CONST_STRPTR name);
LONG pkg_upgrade(struct PkgContext *ctx, CONST_STRPTR name);   /* NULL = all */

/* Download a screenshot (verified) into the cache; returns the local path in
   buf.  Used by the GUI's details pane and large view. */
LONG pkg_screenshot(struct PkgContext *ctx, struct PkgRecord *rec, ULONG index,
                    STRPTR buf, ULONG buflen);

/* ------------------------------------------------------------------------ */
/* Announcements                                                             */

LONG pkg_announcements(struct PkgContext *ctx, BOOL unseenonly, struct List *out);
void pkg_free_announcements(struct List *list);
LONG pkg_announcement_seen(struct PkgContext *ctx, ULONG id);  /* persists */

/* ------------------------------------------------------------------------ */
/* Misc                                                                      */

CONST_STRPTR pkg_strerror(LONG err);
LONG         pkg_last_http_status(struct PkgContext *ctx);
CONST_STRPTR pkg_last_error_text(struct PkgContext *ctx); /* detail line   */

/* PROTOCOL.md §7: <0, 0, >0 */
LONG pkg_version_compare(CONST_STRPTR a, CONST_STRPTR b);

/* Library version string, e.g. "libpkg 1.0 (protocol 1)" */
CONST_STRPTR pkg_version_string(void);

/* HTTP POST with the same transport as the repository client (bsdsocket,
   or a file:// drop when the URL names a mirror).  Used by CrashReporter
   (PROTOCOL.md §9).  *reply is the NUL-terminated response body or NULL;
   free it with pkg_free_reply().  *status = HTTP status (0 = transport). */
LONG pkg_http_post(struct PkgContext *ctx, CONST_STRPTR url, CONST_STRPTR ctype,
                   CONST_APTR body, ULONG len, STRPTR *reply, LONG *status);
void pkg_free_reply(STRPTR reply);

#endif /* LIBPKG_PKG_H */
