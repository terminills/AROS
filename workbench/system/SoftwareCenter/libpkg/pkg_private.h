/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: libpkg internals shared between the AROS-side modules.

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
*/

#ifndef LIBPKG_PKG_PRIVATE_H
#define LIBPKG_PKG_PRIVATE_H

#include <exec/types.h>
#include <exec/lists.h>
#include <dos/dos.h>
#include <utility/hooks.h>

#include "pkg.h"
#include "core/pkg_core.h"

/* On-disk layout (all under SYS:Prefs so it survives and is not copied to
   ENV: at every boot like ENVARC: would be).  Both roots can be redirected
   with ENV:SoftwareCenter/BaseDir and ENV:SoftwareCenter/PkgRoot — a live CD
   has a read-only SYS:, and tests want RAM:.  Resolved once in pkg_open(). */
#define PKG_DEFAULT_BASEDIR  "SYS:Prefs/Packages"
#define PKG_DEFAULT_PKGROOT  "SYS:Packages"           /* activation-style installs */
#define PKG_ENV_BASEDIR      "SoftwareCenter/BaseDir"
#define PKG_ENV_PKGROOT      "SoftwareCenter/PkgRoot"

#define PKG_PREFSFILE     "prefs"
#define PKG_SOURCESFILE   "sources"
#define PKG_TRUSTDIR      "trusted"
#define PKG_CACHEDIR      "cache"        /* /<Source>/...     */
#define PKG_DBDIR         "installed"    /* <Name>, <Name>.files */
#define PKG_DOWNLOADDIR   "downloads"

#define PKG_PROTOCOL      1
#define PKG_USERAGENT     "arospkg/1"
#define PKG_MAXMEMFILE    (8UL * 1024 * 1024)         /* Release/Packages cap */
#define PKG_PATHLEN       512

/* synthetic keys libpkg adds to cached/merged records */
#define PKGX_SOURCE        "X-Source"
#define PKGX_INSTALLROOT   "X-InstallRoot"
#define PKGX_INSTALLDATE   "X-InstallDate"
#define PKGX_INSTALLEDVER  "X-InstalledVersion"

struct PkgContext
{
    struct Hook       *Hook;
    struct PkgPrefs    Prefs;
    struct List        Sources;          /* PkgSource                         */
    struct pkg_stanza *Index;            /* merged, verified, best-per-name   */
    struct pkg_stanza *Installed;        /* installed DB records              */
    struct pkg_stanza *Announcements;    /* merged                            */
    LONG               HttpStatus;
    TEXT               ErrText[256];
    BOOL               IndexDirty;
    TEXT               BaseDir[256];        /* state root (no trailing '/')  */
    TEXT               PkgRoot[256];        /* SYS:Packages or override      */
};

/* "<BaseDir>/<sub>" [+ "/<name>"] into out; returns out */
STRPTR pkg_basepath(struct PkgContext *ctx, CONST_STRPTR sub, CONST_STRPTR name,
                    STRPTR out, ULONG outlen);

/* --- hooks --------------------------------------------------------------- */
void pkg_log(struct PkgContext *ctx, CONST_STRPTR fmt, ...);
void pkg_progress(struct PkgContext *ctx, CONST_STRPTR label, CONST_STRPTR package,
                  ULONG done, ULONG total, UQUAD bdone, UQUAD btotal);
BOOL pkg_abort_requested(struct PkgContext *ctx);
void pkg_set_error(struct PkgContext *ctx, CONST_STRPTR fmt, ...);

/* --- files (pkg_file.c) --------------------------------------------------- */
LONG   pkg_file_read(CONST_STRPTR path, UBYTE **buf, ULONG *len, ULONG maxlen); /* NUL-terminated extra byte */
LONG   pkg_file_write(CONST_STRPTR path, CONST_APTR buf, ULONG len);       /* atomic via .tmp + Rename */
BOOL   pkg_file_exists(CONST_STRPTR path);
LONG   pkg_mkdir_p(CONST_STRPTR path);
LONG   pkg_file_sha256(CONST_STRPTR path, UBYTE md[32], UQUAD *size);
UQUAD  pkg_file_size(CONST_STRPTR path);                 /* (UQUAD)-1 if absent */
void   pkg_join(STRPTR out, ULONG outlen, CONST_STRPTR dir, CONST_STRPTR file);
LONG   pkg_delete_tree(CONST_STRPTR path);
LONG   pkg_read_stanzas(CONST_STRPTR path, struct pkg_stanza **out);       /* NULL list if absent */
LONG   pkg_write_stanzas(CONST_STRPTR path, struct pkg_stanza *list);

/* --- http (pkg_http.c) ---------------------------------------------------- */
struct PkgHttpReq
{
    CONST_STRPTR URL;
    CONST_STRPTR IfNoneMatch;    /* ETag or NULL                             */
    CONST_STRPTR ToFile;         /* NULL = to memory                         */
    BOOL         Resume;         /* append to ToFile with Range              */
    CONST_STRPTR Label;          /* progress phase label                     */
    CONST_STRPTR Package;
    CONST_APTR   PostBody;       /* non-NULL = POST (crash reports)          */
    ULONG        PostLen;
    CONST_STRPTR ContentType;    /* POST only; default text/plain            */
    /* out */
    LONG         Status;         /* HTTP status, 0 on transport failure      */
    TEXT         ETag[80];
    UQUAD        ContentLength;  /* as advertised (0 = unknown)              */
    UBYTE       *Mem;            /* to-memory body (+1 NUL), caller frees     */
    ULONG        MemLen;
    UQUAD        Written;        /* to-file bytes written this call          */
};
LONG pkg_http(struct PkgContext *ctx, struct PkgHttpReq *req);
void pkg_http_free(struct PkgHttpReq *req);
BOOL pkg_url_is_file(CONST_STRPTR url);                  /* file://<AROS path>/ mirror */

/* --- repo (pkg_repo.c) ---------------------------------------------------- */
LONG pkg_prefs_load(struct PkgContext *ctx);
LONG pkg_sources_load(struct PkgContext *ctx);
LONG pkg_cache_load(struct PkgContext *ctx);            /* rebuild Index/Announcements */
LONG pkg_trust_lookup(struct PkgContext *ctx, CONST_STRPTR keyid, UBYTE pub[32]);
struct pkg_stanza *pkg_index_find(struct PkgContext *ctx, CONST_STRPTR name);
struct PkgSource  *pkg_source_find(struct PkgContext *ctx, CONST_STRPTR name);
BOOL  pkg_arch_ok(struct PkgContext *ctx, CONST_STRPTR arch);

/* --- db (pkg_db.c) -------------------------------------------------------- */
LONG pkg_db_load(struct PkgContext *ctx);
struct pkg_stanza *pkg_db_find(struct PkgContext *ctx, CONST_STRPTR name);
LONG pkg_db_store(struct PkgContext *ctx, struct pkg_stanza *rec, struct List *files);
LONG pkg_db_delete(struct PkgContext *ctx, CONST_STRPTR name);
LONG pkg_db_files(struct PkgContext *ctx, CONST_STRPTR name, struct List *out);   /* Node ln_Name = path */
STRPTR pkg_db_owner_of(struct PkgContext *ctx, CONST_STRPTR path);              /* package name or NULL */
LONG pkg_namelist_add(struct List *list, CONST_STRPTR name);

/* --- records (pkg_records.c) ---------------------------------------------- */
struct PkgRecord *pkg_record_from_stanza(struct PkgContext *ctx, struct pkg_stanza *s,
                                         struct pkg_stanza *installed);

#endif /* LIBPKG_PKG_PRIVATE_H */
