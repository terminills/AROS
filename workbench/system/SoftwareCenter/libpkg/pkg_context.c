/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: libpkg context lifecycle and misc API (pkg_open/close, strerror,
          version).

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: LOW
*/

#include <proto/exec.h>
#include <proto/dos.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pkg_private.h"

#define LIBPKG_VERSION "1.0"

struct PkgContext *pkg_open(struct Hook *hook)
{
    struct PkgContext *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    ctx->Hook = hook;
    NewList(&ctx->Sources);

    /* state/install roots: env overrides for live CDs and tests */
    if (GetVar(PKG_ENV_BASEDIR, ctx->BaseDir, sizeof ctx->BaseDir, GVF_GLOBAL_ONLY) <= 0)
        strlcpy(ctx->BaseDir, PKG_DEFAULT_BASEDIR, sizeof ctx->BaseDir);
    if (GetVar(PKG_ENV_PKGROOT, ctx->PkgRoot, sizeof ctx->PkgRoot, GVF_GLOBAL_ONLY) <= 0)
        strlcpy(ctx->PkgRoot, PKG_DEFAULT_PKGROOT, sizeof ctx->PkgRoot);
    {
        ULONG l;
        l = strlen(ctx->BaseDir); if (l && ctx->BaseDir[l - 1] == '/') ctx->BaseDir[l - 1] = 0;
        l = strlen(ctx->PkgRoot); if (l && ctx->PkgRoot[l - 1] == '/') ctx->PkgRoot[l - 1] = 0;
    }

    pkg_mkdir_p(ctx->BaseDir);
    pkg_prefs_load(ctx);
    pkg_sources_load(ctx);
    pkg_db_load(ctx);
    pkg_cache_load(ctx);
    return ctx;
}

void pkg_close(struct PkgContext *ctx)
{
    struct Node *n;
    if (!ctx) return;
    while ((n = RemHead(&ctx->Sources)))
        free(n);
    pkg_stanza_free(ctx->Index);
    pkg_stanza_free(ctx->Installed);
    pkg_stanza_free(ctx->Announcements);
    free(ctx);
}

CONST_STRPTR pkg_strerror(LONG err)
{
    switch (err)
    {
    case PKGERR_OK:           return "OK";
    case PKGERR_NETWORK:      return "PKGERR_NETWORK";
    case PKGERR_HTTP:         return "PKGERR_HTTP";
    case PKGERR_NOKEY:        return "PKGERR_NOKEY";
    case PKGERR_BADSIG:       return "PKGERR_BADSIG";
    case PKGERR_ROLLBACK:     return "PKGERR_ROLLBACK";
    case PKGERR_BADHASH:      return "PKGERR_BADHASH";
    case PKGERR_BADARCHIVE:   return "PKGERR_BADARCHIVE";
    case PKGERR_CONFLICT:     return "PKGERR_CONFLICT";
    case PKGERR_DEPENDS:      return "PKGERR_DEPENDS";
    case PKGERR_NOTFOUND:     return "PKGERR_NOTFOUND";
    case PKGERR_IO:           return "PKGERR_IO";
    case PKGERR_ABORTED:      return "PKGERR_ABORTED";
    case PKGERR_NOMEM:        return "PKGERR_NOMEM";
    case PKGERR_NOTINSTALLED: return "PKGERR_NOTINSTALLED";
    case PKGERR_BADPREFS:     return "PKGERR_BADPREFS";
    case PKGERR_BADURL:       return "PKGERR_BADURL";
    case PKGERR_FORMAT:       return "PKGERR_FORMAT";
    default:                  return "PKGERR_UNKNOWN";
    }
}

LONG pkg_last_http_status(struct PkgContext *ctx)
{
    return ctx ? ctx->HttpStatus : 0;
}

CONST_STRPTR pkg_last_error_text(struct PkgContext *ctx)
{
    return ctx ? (CONST_STRPTR)ctx->ErrText : (CONST_STRPTR)"";
}

LONG pkg_version_compare(CONST_STRPTR a, CONST_STRPTR b)
{
    return pkg_vercmp(a, b);
}

CONST_STRPTR pkg_version_string(void)
{
    return "libpkg " LIBPKG_VERSION " (protocol 1)";
}
