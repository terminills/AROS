/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: PkgRecord / PkgAnnouncement views over the merged index and the
          installed DB: pkg_list(), pkg_find(), pkg_list_addons(),
          pkg_announcements() and friends.

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

/* ------------------------------------------------------------------------ */
/* records                                                                   */

static STRPTR dupfield(struct pkg_stanza *s, CONST_STRPTR key)
{
    const char *v = s ? pkg_stanza_get(s, key) : NULL;
    return v ? strdup(v) : NULL;
}

static UBYTE parse_type(const char *v)
{
    if (!v) return PKGTYPE_PACKAGE;
    if (stricmp(v, "plugin") == 0) return PKGTYPE_PLUGIN;
    if (stricmp(v, "model") == 0) return PKGTYPE_MODEL;
    if (stricmp(v, "addon") == 0) return PKGTYPE_ADDON;
    return PKGTYPE_PACKAGE;
}

/* s = index record (may be NULL for orphans), installed = DB record or NULL */
struct PkgRecord *pkg_record_from_stanza(struct PkgContext *ctx, struct pkg_stanza *s,
                                         struct pkg_stanza *installed)
{
    struct PkgRecord *r = calloc(1, sizeof *r);
    struct pkg_stanza *src = s ? s : installed;
    struct pkg_stanza *copy;
    struct pkg_field *f;

    if (!r || !src) { free(r); return NULL; }

    copy = pkg_stanza_new();
    if (!copy) { free(r); return NULL; }
    for (f = src->fields; f; f = f->next) pkg_stanza_set(copy, f->key, f->value);
    r->Stanza = copy;

    r->Name         = dupfield(src, "Package");
    r->Version      = dupfield(src, "Version");
    r->Arch         = dupfield(src, "Arch");
    r->Summary      = dupfield(src, "Summary");
    r->Description  = dupfield(src, "Description");
    r->Section      = dupfield(src, "Section");
    r->Depends      = dupfield(src, "Depends");
    r->Extends      = dupfield(src, "Extends");
    r->Subdir       = dupfield(src, "Subdir");
    r->Homepage     = dupfield(src, "Homepage");
    r->Maintainer   = dupfield(src, "Maintainer");
    r->Licence      = dupfield(src, "Licence");
    r->Changes      = dupfield(src, "Changes");
    r->Screenshots  = dupfield(src, "Screenshot");
    r->SourceName   = dupfield(src, PKGX_SOURCE);
    r->Size         = pkg_parse_u64(pkg_stanza_get(src, "Size"));
    r->InstalledSize= pkg_parse_u64(pkg_stanza_get(src, "InstalledSize"));
    r->Type         = parse_type(pkg_stanza_get(src, "Type"));
    r->Reboot       = pkg_parse_bool(pkg_stanza_get(src, "Reboot"));
    r->Node.ln_Name = r->Name;

    if (installed)
    {
        const char *iv = pkg_stanza_get(installed, "Version");
        r->InstalledVersion = iv ? strdup(iv) : strdup("?");
        if (!s) r->State = PKGSTATE_ORPHAN;
        else if (pkg_vercmp(r->Version, r->InstalledVersion) > 0) r->State = PKGSTATE_UPGRADABLE;
        else r->State = PKGSTATE_INSTALLED;
        if (!r->SourceName) r->SourceName = dupfield(installed, PKGX_SOURCE);
    }
    else
        r->State = PKGSTATE_AVAILABLE;
    return r;
}

void pkg_free_record(struct PkgRecord *r)
{
    if (!r) return;
    free(r->Name); free(r->Version); free(r->InstalledVersion); free(r->Arch);
    free(r->Summary); free(r->Description); free(r->Section); free(r->Depends);
    free(r->Extends); free(r->Subdir); free(r->Homepage); free(r->Maintainer);
    free(r->Licence); free(r->Changes); free(r->Screenshots); free(r->SourceName);
    pkg_stanza_free(r->Stanza);
    free(r);
}

void pkg_free_records(struct List *list)
{
    struct Node *n;
    while ((n = RemHead(list)))
        pkg_free_record((struct PkgRecord *)n);
}

CONST_STRPTR pkg_record_get(struct PkgRecord *rec, CONST_STRPTR key)
{
    return rec && rec->Stanza ? pkg_stanza_get(rec->Stanza, key) : NULL;
}

static BOOL filter_ok(struct PkgRecord *r, ULONG filter)
{
    switch (filter)
    {
    case PKGF_INSTALLED: return r->State != PKGSTATE_AVAILABLE;
    case PKGF_UPDATES:   return r->State == PKGSTATE_UPGRADABLE;
    case PKGF_AVAILABLE: return r->State == PKGSTATE_AVAILABLE;
    default:             return TRUE;
    }
}

/* insertion by name so front-ends get a sorted list for free */
static void insert_sorted(struct List *list, struct PkgRecord *r)
{
    struct Node *n;
    ForeachNode(list, n)
    {
        if (stricmp(n->ln_Name, r->Name) > 0)
        {
            Insert(list, &r->Node, n->ln_Pred);
            return;
        }
    }
    AddTail(list, &r->Node);
}

LONG pkg_list(struct PkgContext *ctx, ULONG filter, struct List *out)
{
    struct pkg_stanza *s;

    NewList(out);
    for (s = ctx->Index; s; s = s->next)
    {
        const char *name = pkg_stanza_get(s, "Package");
        struct PkgRecord *r;
        if (!name) continue;
        r = pkg_record_from_stanza(ctx, s, pkg_db_find(ctx, name));
        if (!r) { pkg_free_records(out); return PKGERR_NOMEM; }
        if (filter_ok(r, filter)) insert_sorted(out, r);
        else pkg_free_record(r);
    }
    /* orphans: installed but gone from every index */
    if (filter == PKGF_ALL || filter == PKGF_INSTALLED)
    {
        for (s = ctx->Installed; s; s = s->next)
        {
            const char *name = pkg_stanza_get(s, "Package");
            struct PkgRecord *r;
            if (!name || pkg_index_find(ctx, name)) continue;
            r = pkg_record_from_stanza(ctx, NULL, s);
            if (!r) { pkg_free_records(out); return PKGERR_NOMEM; }
            insert_sorted(out, r);
        }
    }
    return 0;
}

struct PkgRecord *pkg_find(struct PkgContext *ctx, CONST_STRPTR name)
{
    struct pkg_stanza *s = pkg_index_find(ctx, name);
    struct pkg_stanza *i = pkg_db_find(ctx, name);
    if (!s && !i) return NULL;
    return pkg_record_from_stanza(ctx, s, i);
}

LONG pkg_list_addons(struct PkgContext *ctx, CONST_STRPTR host, struct List *out)
{
    struct pkg_stanza *s;

    NewList(out);
    for (s = ctx->Index; s; s = s->next)
    {
        const char *ext = pkg_stanza_get(s, "Extends");
        const char *name = pkg_stanza_get(s, "Package");
        struct PkgRecord *r;
        if (!ext || !name || stricmp(ext, host) != 0) continue;
        r = pkg_record_from_stanza(ctx, s, pkg_db_find(ctx, name));
        if (!r) { pkg_free_records(out); return PKGERR_NOMEM; }
        insert_sorted(out, r);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* announcements                                                             */

/* RFC 3339 dates compare as strings when both are UTC "YYYY-MM-DDTHH:MM:SSZ" */
static BOOL expired(const char *expires)
{
    struct DateStamp ds;
    TEXT now[32];
    LONG y, m, d;

    if (!expires || strlen(expires) < 10) return FALSE;
    DateStamp(&ds);
    /* days since 1978-01-01 → y/m/d without depending on locale formats */
    {
        LONG days = ds.ds_Days;
        LONG year = 1978;
        static const UBYTE mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        for (;;)
        {
            LONG ylen = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;
            if (days < ylen) break;
            days -= ylen;
            year++;
        }
        for (m = 0; m < 12; m++)
        {
            LONG ml = mdays[m] + ((m == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) ? 1 : 0);
            if (days < ml) break;
            days -= ml;
        }
        y = year; d = days + 1; m = m + 1;
    }
    snprintf(now, sizeof now, "%04ld-%02ld-%02ldT%02ld:%02ld:00Z", (long)y, (long)m, (long)d,
             (long)(ds.ds_Minute / 60), (long)(ds.ds_Minute % 60));
    return strcmp(expires, now) < 0;
}

static void insert_by_id_desc(struct List *list, struct PkgAnnouncement *a)
{
    struct Node *n;
    ForeachNode(list, n)
    {
        if (((struct PkgAnnouncement *)n)->Id < a->Id)
        {
            Insert(list, &a->Node, n->ln_Pred);
            return;
        }
    }
    AddTail(list, &a->Node);
}

LONG pkg_announcements(struct PkgContext *ctx, BOOL unseenonly, struct List *out)
{
    struct pkg_stanza *s;

    NewList(out);
    for (s = ctx->Announcements; s; s = s->next)
    {
        struct PkgAnnouncement *a;
        ULONG id = (ULONG)pkg_parse_u64(pkg_stanza_get(s, "Id"));
        BOOL seen = id <= ctx->Prefs.LastAnnouncementId;
        if (unseenonly && seen) continue;
        if (expired(pkg_stanza_get(s, "Expires"))) continue;
        a = calloc(1, sizeof *a);
        if (!a) { pkg_free_announcements(out); return PKGERR_NOMEM; }
        a->Id       = id;
        a->Date     = dupfield(s, "Date");
        a->Severity = dupfield(s, "Severity");
        a->Title    = dupfield(s, "Title");
        a->Body     = dupfield(s, "Body");
        a->Package  = dupfield(s, "Package");
        a->URL      = dupfield(s, "URL");
        a->SourceName = dupfield(s, PKGX_SOURCE);
        a->Seen     = seen;
        a->Node.ln_Name = a->Title ? a->Title : (STRPTR)"";
        insert_by_id_desc(out, a);
    }
    return 0;
}

void pkg_free_announcements(struct List *list)
{
    struct PkgAnnouncement *a;
    while ((a = (struct PkgAnnouncement *)RemHead(list)))
    {
        free(a->Date); free(a->Severity); free(a->Title); free(a->Body);
        free(a->Package); free(a->URL); free(a->SourceName);
        free(a);
    }
}

LONG pkg_announcement_seen(struct PkgContext *ctx, ULONG id)
{
    if (id <= ctx->Prefs.LastAnnouncementId) return 0;
    ctx->Prefs.LastAnnouncementId = id;
    return pkg_prefs_save(ctx);
}
