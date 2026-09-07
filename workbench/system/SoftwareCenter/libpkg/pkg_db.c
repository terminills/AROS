/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Installed-package database: SYS:Prefs/Packages/installed/<Name>
          (the index record + X-Installed* keys) and <Name>.files (one
          absolute path per line, in write order) — PROTOCOL.md §4.1.

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: LOW
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/exall.h>
#include <exec/memory.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pkg_private.h"

static void db_path(struct PkgContext *ctx, CONST_STRPTR name, CONST_STRPTR suffix,
                    STRPTR out, ULONG outlen)
{
    TEXT file[128];
    snprintf(file, sizeof file, "%s%s", name, PKG_STR(suffix, ""));
    pkg_basepath(ctx, PKG_DBDIR, file, out, outlen);
}

LONG pkg_db_load(struct PkgContext *ctx)
{
    BPTR lock;
    struct ExAllControl *eac;
    struct ExAllData *ead;
    UBYTE *buf;
    BOOL more;
    TEXT path[PKG_PATHLEN];

    pkg_stanza_free(ctx->Installed);
    ctx->Installed = NULL;

    lock = Lock(pkg_basepath(ctx, PKG_DBDIR, NULL, path, sizeof path), SHARED_LOCK);
    if (!lock) return 0;
    buf = AllocMem(8192, MEMF_ANY);
    eac = AllocDosObject(DOS_EXALLCONTROL, NULL);
    if (!buf || !eac)
    {
        if (buf) FreeMem(buf, 8192);
        if (eac) FreeDosObject(DOS_EXALLCONTROL, eac);
        UnLock(lock);
        return PKGERR_NOMEM;
    }
    eac->eac_LastKey = 0;
    do
    {
        more = ExAll(lock, (struct ExAllData *)buf, 8192, ED_TYPE, eac);
        if (!more && IoErr() != ERROR_NO_MORE_ENTRIES) break;
        for (ead = (struct ExAllData *)buf; eac->eac_Entries && ead; ead = ead->ed_Next)
        {
            struct pkg_stanza *s = NULL;
            ULONG nl = strlen(ead->ed_Name);
            if (ead->ed_Type >= 0) continue;
            if (nl > 6 && strcmp(ead->ed_Name + nl - 6, ".files") == 0) continue;
            if (nl > 4 && strcmp(ead->ed_Name + nl - 4, ".tmp") == 0) continue;
            pkg_basepath(ctx, PKG_DBDIR, ead->ed_Name, path, sizeof path);
            if (pkg_read_stanzas(path, &s) == 0 && s)
            {
                /* one record per file; drop any extras */
                if (s->next) { pkg_stanza_free(s->next); s->next = NULL; }
                if (!pkg_stanza_get(s, "Package")) pkg_stanza_set(s, "Package", ead->ed_Name);
                s->next = ctx->Installed;
                ctx->Installed = s;
            }
        }
    } while (more);
    FreeDosObject(DOS_EXALLCONTROL, eac);
    FreeMem(buf, 8192);
    UnLock(lock);
    return 0;
}

struct pkg_stanza *pkg_db_find(struct PkgContext *ctx, CONST_STRPTR name)
{
    struct pkg_stanza *s;
    for (s = ctx->Installed; s; s = s->next)
    {
        const char *n = pkg_stanza_get(s, "Package");
        if (n && stricmp(n, name) == 0) return s;
    }
    return NULL;
}

LONG pkg_db_store(struct PkgContext *ctx, struct pkg_stanza *rec, struct List *files)
{
    TEXT path[PKG_PATHLEN];
    const char *name = pkg_stanza_get(rec, "Package");
    struct pkg_stanza *copy, *old;
    struct pkg_field *f;
    struct Node *n;
    ULONG len = 0;
    STRPTR text, p;
    LONG rc;

    if (!name) return PKGERR_FORMAT;
    copy = pkg_stanza_new();
    if (!copy) return PKGERR_NOMEM;
    for (f = rec->fields; f; f = f->next) pkg_stanza_set(copy, f->key, f->value);

    db_path(ctx, name, NULL, path, sizeof path);
    rc = pkg_write_stanzas(path, copy);
    if (rc) { pkg_stanza_free(copy); return rc; }

    ForeachNode(files, n) len += strlen(n->ln_Name) + 1;
    text = malloc(len + 1);
    if (!text) { pkg_stanza_free(copy); return PKGERR_NOMEM; }
    p = text;
    ForeachNode(files, n)
    {
        ULONG l = strlen(n->ln_Name);
        memcpy(p, n->ln_Name, l);
        p += l;
        *p++ = '\n';
    }
    *p = 0;
    db_path(ctx, name, ".files", path, sizeof path);
    rc = pkg_file_write(path, text, len);
    free(text);
    if (rc) { pkg_stanza_free(copy); return rc; }

    /* replace in memory */
    old = pkg_db_find(ctx, name);
    if (old) ctx->Installed = pkg_stanza_unlink(ctx->Installed, old);
    copy->next = ctx->Installed;
    ctx->Installed = copy;
    return 0;
}

LONG pkg_db_delete(struct PkgContext *ctx, CONST_STRPTR name)
{
    TEXT path[PKG_PATHLEN];
    struct pkg_stanza *old = pkg_db_find(ctx, name);

    db_path(ctx, name, ".files", path, sizeof path);
    DeleteFile(path);
    db_path(ctx, name, NULL, path, sizeof path);
    DeleteFile(path);
    if (old) ctx->Installed = pkg_stanza_unlink(ctx->Installed, old);
    return 0;
}

LONG pkg_db_files(struct PkgContext *ctx, CONST_STRPTR name, struct List *out)
{
    TEXT path[PKG_PATHLEN];
    UBYTE *buf;
    ULONG len;
    STRPTR line, nl;
    LONG rc;

    NewList(out);
    db_path(ctx, name, ".files", path, sizeof path);
    if (!pkg_file_exists(path)) return 0;
    rc = pkg_file_read(path, &buf, &len, 0);
    if (rc) return rc;
    for (line = (STRPTR)buf; line && *line; line = nl)
    {
        nl = strchr(line, '\n');
        if (nl) *nl++ = 0;
        if (*line && (rc = pkg_namelist_add(out, line))) break;
    }
    free(buf);
    if (rc) pkg_free_namelist(out);
    return rc;
}

/* Which installed package owns an absolute path?  Linear over all .files
   lists — fine for the package counts an AROS system will see. */
STRPTR pkg_db_owner_of(struct PkgContext *ctx, CONST_STRPTR path)
{
    struct pkg_stanza *s;

    for (s = ctx->Installed; s; s = s->next)
    {
        const char *name = pkg_stanza_get(s, "Package");
        struct List files;
        struct Node *n;
        BOOL hit = FALSE;
        if (!name || pkg_db_files(ctx, name, &files)) continue;
        ForeachNode(&files, n)
            if (stricmp(n->ln_Name, path) == 0) { hit = TRUE; break; }
        pkg_free_namelist(&files);
        if (hit) return (STRPTR)name;       /* lives as long as ctx->Installed */
    }
    return NULL;
}
