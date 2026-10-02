/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Preferences, sources, trusted keys and the repository refresh chain
          (PROTOCOL.md §1, §4.4, §5, §6).  Verified index files are cached
          under SYS:Prefs/Packages/cache/<Source>/ so pkg_open() never needs
          the network; nothing that fails verification touches the cache.

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: MEDIUM
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/exall.h>
#include <exec/memory.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pkg_private.h"

/* ------------------------------------------------------------------------ */
/* preferences                                                               */

static void prefs_defaults(struct PkgPrefs *p)
{
    memset(p, 0, sizeof *p);
#if defined(__x86_64__)
    strcpy(p->Arch, "pc-x86_64");
#elif defined(__i386__)
    strcpy(p->Arch, "pc-i386");
#elif defined(__arm__)
    strcpy(p->Arch, "raspi-arm");
#elif defined(__aarch64__)
    strcpy(p->Arch, "raspi-arm64");
#elif defined(__mc68000__)
    strcpy(p->Arch, "amiga-m68k");
#else
    strcpy(p->Arch, "unknown");
#endif
#if defined(__AROS_HOSTED__) || defined(HOST_OS_linux)
    if (strncmp(p->Arch, "pc-", 3) == 0)
        snprintf(p->Arch, sizeof p->Arch, "linux-%s", p->Arch + 3);
#endif
    p->CheckInterval = 60;
    p->Notify = TRUE;
    p->NotifyAnnouncements = TRUE;
}

LONG pkg_prefs_load(struct PkgContext *ctx)
{
    struct pkg_stanza *s = NULL;
    const char *v;
    TEXT path[PKG_PATHLEN];
    LONG rc;

    prefs_defaults(&ctx->Prefs);
    rc = pkg_read_stanzas(pkg_basepath(ctx, PKG_PREFSFILE, NULL, path, sizeof path), &s);
    if (rc || !s) return rc;
    if ((v = pkg_stanza_get(s, "Arch")) && *v) strlcpy(ctx->Prefs.Arch, v, sizeof ctx->Prefs.Arch);
    if ((v = pkg_stanza_get(s, "CheckInterval"))) ctx->Prefs.CheckInterval = (ULONG)pkg_parse_u64(v);
    if ((v = pkg_stanza_get(s, "Notify"))) ctx->Prefs.Notify = pkg_parse_bool(v);
    if ((v = pkg_stanza_get(s, "NotifyAnnouncements"))) ctx->Prefs.NotifyAnnouncements = pkg_parse_bool(v);
    if ((v = pkg_stanza_get(s, "LastSerial"))) ctx->Prefs.LastSerial = (ULONG)pkg_parse_u64(v);
    if ((v = pkg_stanza_get(s, "LastAnnouncementId"))) ctx->Prefs.LastAnnouncementId = (ULONG)pkg_parse_u64(v);
    pkg_stanza_free(s);
    return 0;
}

LONG pkg_prefs_save(struct PkgContext *ctx)
{
    struct pkg_stanza *s = pkg_stanza_new();
    TEXT num[24];
    TEXT path[PKG_PATHLEN];
    LONG rc;

    if (!s) return PKGERR_NOMEM;
    pkg_stanza_set(s, "Arch", ctx->Prefs.Arch);
    snprintf(num, sizeof num, "%lu", (unsigned long)ctx->Prefs.CheckInterval);
    pkg_stanza_set(s, "CheckInterval", num);
    pkg_stanza_set(s, "Notify", ctx->Prefs.Notify ? "yes" : "no");
    pkg_stanza_set(s, "NotifyAnnouncements", ctx->Prefs.NotifyAnnouncements ? "yes" : "no");
    snprintf(num, sizeof num, "%lu", (unsigned long)ctx->Prefs.LastSerial);
    pkg_stanza_set(s, "LastSerial", num);
    snprintf(num, sizeof num, "%lu", (unsigned long)ctx->Prefs.LastAnnouncementId);
    pkg_stanza_set(s, "LastAnnouncementId", num);
    rc = pkg_write_stanzas(pkg_basepath(ctx, PKG_PREFSFILE, NULL, path, sizeof path), s);
    pkg_stanza_free(s);
    return rc;
}

struct PkgPrefs *pkg_prefs(struct PkgContext *ctx)
{
    return &ctx->Prefs;
}

/* ------------------------------------------------------------------------ */
/* sources                                                                   */

static void source_cache_path(struct PkgContext *ctx, struct PkgSource *src, CONST_STRPTR file,
                              STRPTR out, ULONG outlen)
{
    pkg_basepath(ctx, PKG_CACHEDIR, src->Node.ln_Name, out, outlen);
    if (file) AddPart(out, (STRPTR)file, outlen);
}

static void source_state_load(struct PkgContext *ctx, struct PkgSource *src)
{
    struct pkg_stanza *s = NULL;
    TEXT path[PKG_PATHLEN];
    const char *v;

    source_cache_path(ctx, src, "State", path, sizeof path);
    if (pkg_read_stanzas(path, &s) || !s) return;
    if ((v = pkg_stanza_get(s, "ETag"))) strlcpy(src->ETag, v, sizeof src->ETag);
    if ((v = pkg_stanza_get(s, "Serial"))) src->Serial = (ULONG)pkg_parse_u64(v);
    pkg_stanza_free(s);
}

static LONG source_state_save(struct PkgContext *ctx, struct PkgSource *src)
{
    struct pkg_stanza *s = pkg_stanza_new();
    TEXT path[PKG_PATHLEN], num[24];
    LONG rc;

    if (!s) return PKGERR_NOMEM;
    pkg_stanza_set(s, "ETag", src->ETag);
    snprintf(num, sizeof num, "%lu", (unsigned long)src->Serial);
    pkg_stanza_set(s, "Serial", num);
    source_cache_path(ctx, src, "State", path, sizeof path);
    rc = pkg_write_stanzas(path, s);
    pkg_stanza_free(s);
    return rc;
}

static struct PkgSource *source_new(CONST_STRPTR name, CONST_STRPTR url, CONST_STRPTR keyid, BOOL enabled)
{
    struct PkgSource *src;
    ULONG ul;

    if (!name || !*name || !url) return NULL;
    if (strncmp(url, "http://", 7) != 0 && !pkg_url_is_file(url)) return NULL;
    if (strpbrk(name, "/:")) return NULL;             /* used as a directory name */
    src = calloc(1, sizeof *src + strlen(name) + 1);
    if (!src) return NULL;
    src->Node.ln_Name = (STRPTR)(src + 1);
    strcpy(src->Node.ln_Name, name);
    strlcpy(src->URL, url, sizeof src->URL);
    ul = strlen(src->URL);
    if (ul && src->URL[ul - 1] != '/' && ul + 1 < sizeof src->URL)
    {
        src->URL[ul] = '/';
        src->URL[ul + 1] = 0;
    }
    if (keyid) strlcpy(src->KeyId, keyid, sizeof src->KeyId);
    src->Enabled = enabled;
    return src;
}

LONG pkg_sources_load(struct PkgContext *ctx)
{
    struct pkg_stanza *list = NULL, *s;
    TEXT path[PKG_PATHLEN];
    LONG rc;

    rc = pkg_read_stanzas(pkg_basepath(ctx, PKG_SOURCESFILE, NULL, path, sizeof path), &list);
    if (rc) return rc;
    for (s = list; s; s = s->next)
    {
        const char *en = pkg_stanza_get(s, "Enabled");
        struct PkgSource *src = source_new(pkg_stanza_get(s, "Name"), pkg_stanza_get(s, "URL"),
                                           pkg_stanza_get(s, "KeyId"), en ? pkg_parse_bool(en) : TRUE);
        if (!src)
        {
            pkg_log(ctx, "sources: ignoring malformed record (%s)",
                    pkg_stanza_get(s, "Name") ? pkg_stanza_get(s, "Name") : "?");
            continue;
        }
        source_state_load(ctx, src);
        AddTail(&ctx->Sources, &src->Node);
    }
    pkg_stanza_free(list);
    return 0;
}

LONG pkg_sources_save(struct PkgContext *ctx)
{
    struct pkg_stanza *head = NULL, *tail = NULL;
    struct PkgSource *src;
    TEXT path[PKG_PATHLEN];
    LONG rc;

    ForeachNode(&ctx->Sources, src)
    {
        struct pkg_stanza *s = pkg_stanza_new();
        if (!s) { pkg_stanza_free(head); return PKGERR_NOMEM; }
        pkg_stanza_set(s, "Name", src->Node.ln_Name);
        pkg_stanza_set(s, "URL", src->URL);
        if (src->KeyId[0]) pkg_stanza_set(s, "KeyId", src->KeyId);
        pkg_stanza_set(s, "Enabled", src->Enabled ? "yes" : "no");
        if (tail) tail->next = s; else head = s;
        tail = s;
    }
    rc = pkg_write_stanzas(pkg_basepath(ctx, PKG_SOURCESFILE, NULL, path, sizeof path), head);
    pkg_stanza_free(head);
    if (rc) pkg_set_error(ctx, "cannot write %s", path);
    return rc;
}

struct List *pkg_sources(struct PkgContext *ctx)
{
    return &ctx->Sources;
}

struct PkgSource *pkg_source_find(struct PkgContext *ctx, CONST_STRPTR name)
{
    struct PkgSource *src;
    ForeachNode(&ctx->Sources, src)
        if (stricmp(src->Node.ln_Name, name) == 0) return src;
    return NULL;
}

LONG pkg_source_add(struct PkgContext *ctx, CONST_STRPTR name, CONST_STRPTR url, CONST_STRPTR keyid)
{
    struct PkgSource *src;
    if (pkg_source_find(ctx, name)) { pkg_set_error(ctx, "source %s already exists", name); return PKGERR_CONFLICT; }
    src = source_new(name, url, keyid, TRUE);
    if (!src) { pkg_set_error(ctx, "bad source name or URL (http:// or file://)"); return PKGERR_BADURL; }
    AddTail(&ctx->Sources, &src->Node);
    return pkg_sources_save(ctx);
}

LONG pkg_source_remove(struct PkgContext *ctx, CONST_STRPTR name)
{
    struct PkgSource *src = pkg_source_find(ctx, name);
    TEXT path[PKG_PATHLEN];
    if (!src) return PKGERR_NOTFOUND;
    Remove(&src->Node);
    source_cache_path(ctx, src, NULL, path, sizeof path);
    pkg_delete_tree(path);
    free(src);
    ctx->IndexDirty = TRUE;
    return pkg_sources_save(ctx);
}

/* ------------------------------------------------------------------------ */
/* trusted keys                                                              */

LONG pkg_trust_lookup(struct PkgContext *ctx, CONST_STRPTR keyid, UBYTE pub[32])
{
    TEXT path[PKG_PATHLEN];
    UBYTE *buf;
    ULONG len;
    TEXT check[33];
    ULONG i;

    if (!keyid || strlen(keyid) != 32) return PKGERR_NOKEY;
    for (i = 0; i < 32; i++)
        if (!strchr("0123456789abcdefABCDEF", keyid[i])) return PKGERR_NOKEY;
    { TEXT f[40]; snprintf(f, sizeof f, "%s.pub", keyid); pkg_basepath(ctx, PKG_TRUSTDIR, f, path, sizeof path); }
    if (pkg_file_read(path, &buf, &len, 64)) return PKGERR_NOKEY;
    if (len != 32) { free(buf); return PKGERR_NOKEY; }
    memcpy(pub, buf, 32);
    free(buf);
    /* the file name must match the key it holds */
    pkg_keyid(pub, check);
    if (!pkg_hex_equal(check, keyid)) return PKGERR_NOKEY;
    return 0;
}

LONG pkg_trust_add(struct PkgContext *ctx, CONST_STRPTR pubfile)
{
    UBYTE *buf;
    ULONG len;
    TEXT keyid[33], path[PKG_PATHLEN];
    LONG rc;

    rc = pkg_file_read(pubfile, &buf, &len, 1024);
    if (rc) { pkg_set_error(ctx, "cannot read %s", pubfile); return rc; }
    if (len == 64 || len == 65)
    {
        /* accept hex too */
        UBYTE raw[32];
        if (pkg_unhex((const char *)buf, raw, 32) == 0 || (len == 65 && (buf[64] = 0, pkg_unhex((const char *)buf, raw, 32) == 0)))
        {
            memcpy(buf, raw, 32);
            len = 32;
        }
    }
    if (len != 32)
    {
        free(buf);
        pkg_set_error(ctx, "%s is not a raw 32-byte Ed25519 public key", pubfile);
        return PKGERR_FORMAT;
    }
    pkg_keyid(buf, keyid);
    { TEXT f[40]; snprintf(f, sizeof f, "%s.pub", keyid); pkg_basepath(ctx, PKG_TRUSTDIR, f, path, sizeof path); }
    rc = pkg_file_write(path, buf, 32);
    free(buf);
    if (rc) pkg_set_error(ctx, "cannot write %s", path);
    else pkg_log(ctx, "trusted key %s added", keyid);
    return rc;
}

LONG pkg_trust_list(struct PkgContext *ctx, struct List *out)
{
    TEXT path[PKG_PATHLEN];
    BPTR lock;
    struct ExAllControl *eac;
    struct ExAllData *ead;
    UBYTE *buf;
    BOOL more;

    NewList(out);
    lock = Lock(pkg_basepath(ctx, PKG_TRUSTDIR, NULL, path, sizeof path), SHARED_LOCK);
    if (!lock) return 0;
    buf = AllocMem(4096, MEMF_ANY);
    eac = AllocDosObject(DOS_EXALLCONTROL, NULL);
    if (!buf || !eac)
    {
        if (buf) FreeMem(buf, 4096);
        if (eac) FreeDosObject(DOS_EXALLCONTROL, eac);
        UnLock(lock);
        return PKGERR_NOMEM;
    }
    eac->eac_LastKey = 0;
    do
    {
        more = ExAll(lock, (struct ExAllData *)buf, 4096, ED_TYPE, eac);
        if (!more && IoErr() != ERROR_NO_MORE_ENTRIES) break;
        for (ead = (struct ExAllData *)buf; eac->eac_Entries && ead; ead = ead->ed_Next)
        {
            ULONG nl = strlen(ead->ed_Name);
            if (ead->ed_Type < 0 && nl == 36 && strcmp(ead->ed_Name + 32, ".pub") == 0)
            {
                TEXT id[33];
                memcpy(id, ead->ed_Name, 32);
                id[32] = 0;
                pkg_namelist_add(out, id);
            }
        }
    } while (more);
    FreeDosObject(DOS_EXALLCONTROL, eac);
    FreeMem(buf, 4096);
    UnLock(lock);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* arch                                                                      */

BOOL pkg_arch_ok(struct PkgContext *ctx, CONST_STRPTR arch)
{
    if (!arch || !*arch) return FALSE;
    if (stricmp(arch, "all") == 0) return TRUE;
    return stricmp(arch, ctx->Prefs.Arch) == 0;
}

/* ------------------------------------------------------------------------ */
/* cache → merged index                                                      */

struct pkg_stanza *pkg_index_find(struct PkgContext *ctx, CONST_STRPTR name)
{
    struct pkg_stanza *s;
    for (s = ctx->Index; s; s = s->next)
    {
        const char *n = pkg_stanza_get(s, "Package");
        if (n && stricmp(n, name) == 0) return s;
    }
    return NULL;
}

/* Merge one source's Packages into ctx->Index: best version per name for
   an acceptable arch, earlier sources win ties (§4.4). */
static void merge_index(struct PkgContext *ctx, struct PkgSource *src, struct pkg_stanza *list)
{
    struct pkg_stanza *s, *next;
    for (s = list; s; s = next)
    {
        const char *name = pkg_stanza_get(s, "Package");
        const char *ver  = pkg_stanza_get(s, "Version");
        struct pkg_stanza *have;

        next = s->next;
        if (!name || !ver || !pkg_arch_ok(ctx, pkg_stanza_get(s, "Arch"))) continue;
        have = pkg_index_find(ctx, name);
        if (have)
        {
            int c = pkg_vercmp(ver, pkg_stanza_get(have, "Version"));
            /* prefer exact arch over "all" at equal version */
            if (c < 0) continue;
            if (c == 0 && !(stricmp(pkg_stanza_get(have, "Arch"), "all") == 0 &&
                            stricmp(pkg_stanza_get(s, "Arch"), "all") != 0))
                continue;
            ctx->Index = pkg_stanza_unlink(ctx->Index, have);
        }
        /* move the record over (detach from its list) */
        {
            struct pkg_stanza *copy = pkg_stanza_new();
            struct pkg_field *f;
            if (!copy) continue;
            for (f = s->fields; f; f = f->next) pkg_stanza_set(copy, f->key, f->value);
            pkg_stanza_set(copy, PKGX_SOURCE, src->Node.ln_Name);
            copy->next = ctx->Index;
            ctx->Index = copy;
        }
    }
}

static void merge_announcements(struct PkgContext *ctx, struct PkgSource *src, struct pkg_stanza *list)
{
    struct pkg_stanza *s;
    for (s = list; s; s = s->next)
    {
        struct pkg_stanza *copy;
        struct pkg_field *f;
        if (!pkg_stanza_get(s, "Id") || !pkg_stanza_get(s, "Title")) continue;
        copy = pkg_stanza_new();
        if (!copy) continue;
        for (f = s->fields; f; f = f->next) pkg_stanza_set(copy, f->key, f->value);
        pkg_stanza_set(copy, PKGX_SOURCE, src->Node.ln_Name);
        copy->next = ctx->Announcements;
        ctx->Announcements = copy;
    }
}

LONG pkg_cache_load(struct PkgContext *ctx)
{
    struct PkgSource *src;
    TEXT path[PKG_PATHLEN];

    pkg_stanza_free(ctx->Index);
    pkg_stanza_free(ctx->Announcements);
    ctx->Index = NULL;
    ctx->Announcements = NULL;

    ForeachNode(&ctx->Sources, src)
    {
        struct pkg_stanza *list = NULL;
        if (!src->Enabled) continue;
        source_cache_path(ctx, src, "Packages", path, sizeof path);
        if (pkg_read_stanzas(path, &list) == 0 && list)
        {
            merge_index(ctx, src, list);
            pkg_stanza_free(list);
        }
        list = NULL;
        source_cache_path(ctx, src, "Announcements", path, sizeof path);
        if (pkg_read_stanzas(path, &list) == 0 && list)
        {
            merge_announcements(ctx, src, list);
            pkg_stanza_free(list);
        }
    }
    ctx->IndexDirty = FALSE;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* refresh chain (§5)                                                        */

static LONG fetch_mem(struct PkgContext *ctx, struct PkgSource *src, CONST_STRPTR file,
                      CONST_STRPTR etag, struct PkgHttpReq *req)
{
    TEXT url[PKG_URL_LEN + 64];
    memset(req, 0, sizeof *req);
    snprintf(url, sizeof url, "%s%s", src->URL, file);
    req->URL = url;
    req->IfNoneMatch = etag;
    req->Label = "Fetching";
    req->Package = file;
    return pkg_http(ctx, req);
}

/* Verify the signature of a fetched blob with a trusted key. */
static LONG verify_sig(struct PkgContext *ctx, struct PkgSource *src, CONST_STRPTR keyid,
                       const UBYTE *msg, ULONG len, const UBYTE *sig, ULONG siglen)
{
    UBYTE pub[32];
    if (siglen != 64) { pkg_set_error(ctx, "%s: Release.sig is not 64 bytes", src->Node.ln_Name); return PKGERR_BADSIG; }
    if (src->KeyId[0] && !pkg_hex_equal(src->KeyId, keyid))
    {
        pkg_set_error(ctx, "%s: signed by %s, source pinned to %s", src->Node.ln_Name, keyid, src->KeyId);
        return PKGERR_NOKEY;
    }
    if (pkg_trust_lookup(ctx, keyid, pub))
    {
        pkg_set_error(ctx, "%s: key %s is not trusted (Pkg TRUST <file>)", src->Node.ln_Name, PKG_STR(keyid, "?"));
        return PKGERR_NOKEY;
    }
    if (!pkg_ed25519_verify(msg, len, sig, pub))
    {
        pkg_set_error(ctx, "%s: Release signature INVALID", src->Node.ln_Name);
        return PKGERR_BADSIG;
    }
    return 0;
}

static LONG check_digest(struct PkgContext *ctx, struct PkgSource *src, CONST_STRPTR what,
                         const char *line, const UBYTE *data, ULONG len)
{
    char hex[65], got[65];
    uint64_t size;
    UBYTE md[32];

    if (!line || pkg_parse_digest_line(line, hex, &size))
    {
        pkg_set_error(ctx, "%s: Release has no valid %s digest", src->Node.ln_Name, what);
        return PKGERR_FORMAT;
    }
    if (size != len) { pkg_set_error(ctx, "%s: %s size mismatch", src->Node.ln_Name, what); return PKGERR_BADHASH; }
    pkg_sha256(data, len, md);
    pkg_hex(md, 32, got);
    if (!pkg_hex_equal(hex, got)) { pkg_set_error(ctx, "%s: %s digest mismatch", src->Node.ln_Name, what); return PKGERR_BADHASH; }
    return 0;
}

/* Refresh one source.  *changed is set when new index files were cached. */
static LONG refresh_source(struct PkgContext *ctx, struct PkgSource *src, BOOL force,
                           BOOL headonly, BOOL *changed)
{
    struct PkgHttpReq rel, sig, pk, an;
    struct pkg_stanza *release = NULL;
    const char *v, *keyid;
    ULONG serial;
    LONG rc;
    TEXT path[PKG_PATHLEN];
    int n = 0;

    *changed = FALSE;
    memset(&sig, 0, sizeof sig); memset(&pk, 0, sizeof pk); memset(&an, 0, sizeof an);

    rc = fetch_mem(ctx, src, "Release", force ? NULL : src->ETag, &rel);
    if (rc) return rc;
    if (rel.Status == 304)
    {
        pkg_log(ctx, "%s: up to date", src->Node.ln_Name);
        pkg_http_free(&rel);
        return 0;
    }
    if (headonly)
    {
        /* pkg_check(): only tell whether something changed */
        *changed = TRUE;
        pkg_http_free(&rel);
        return 0;
    }

    release = pkg_stanza_parse((const char *)rel.Mem, rel.MemLen, &n);
    if (!release) { pkg_set_error(ctx, "%s: Release is empty", src->Node.ln_Name); rc = PKGERR_FORMAT; goto out; }
    v = pkg_stanza_get(release, "Protocol");
    if (!v || pkg_parse_u64(v) != PKG_PROTOCOL)
    {
        pkg_set_error(ctx, "%s: unsupported Protocol %s", src->Node.ln_Name, v ? v : "?");
        rc = PKGERR_FORMAT;
        goto out;
    }
    keyid = pkg_stanza_get(release, "KeyId");

    rc = fetch_mem(ctx, src, "Release.sig", NULL, &sig);
    if (rc) goto out;
    rc = verify_sig(ctx, src, keyid, rel.Mem, rel.MemLen, sig.Mem, sig.MemLen);
    if (rc) goto out;

    v = pkg_stanza_get(release, "Serial");
    serial = v ? (ULONG)pkg_parse_u64(v) : 0;
    if (serial < src->Serial)
    {
        pkg_set_error(ctx, "%s: Release serial %lu is older than cached %lu (rollback?)",
                      src->Node.ln_Name, (unsigned long)serial, (unsigned long)src->Serial);
        rc = PKGERR_ROLLBACK;
        goto out;
    }

    rc = fetch_mem(ctx, src, "Packages", NULL, &pk);
    if (rc) goto out;
    rc = check_digest(ctx, src, "Packages", pkg_stanza_get(release, "Packages"), pk.Mem, pk.MemLen);
    if (rc) goto out;

    rc = fetch_mem(ctx, src, "Announcements", NULL, &an);
    if (rc == PKGERR_NOTFOUND) { rc = 0; an.Mem = NULL; an.MemLen = 0; }
    if (rc) goto out;
    if (an.Mem)
    {
        rc = check_digest(ctx, src, "Announcements", pkg_stanza_get(release, "Announcements"), an.Mem, an.MemLen);
        if (rc) goto out;
    }

    /* everything verified: commit to cache */
    source_cache_path(ctx, src, "Release", path, sizeof path);
    if ((rc = pkg_file_write(path, rel.Mem, rel.MemLen))) goto out;
    source_cache_path(ctx, src, "Packages", path, sizeof path);
    if ((rc = pkg_file_write(path, pk.Mem, pk.MemLen))) goto out;
    source_cache_path(ctx, src, "Announcements", path, sizeof path);
    if ((rc = pkg_file_write(path, an.Mem ? an.Mem : (UBYTE *)"", an.MemLen))) goto out;

    strlcpy(src->ETag, rel.ETag, sizeof src->ETag);
    src->Serial = serial;
    source_state_save(ctx, src);
    if (serial > ctx->Prefs.LastSerial) ctx->Prefs.LastSerial = serial;
    *changed = TRUE;
    pkg_log(ctx, "%s: %s (%s) serial %lu, %s", src->Node.ln_Name,
            pkg_stanza_get(release, "Origin") ? pkg_stanza_get(release, "Origin") : "?",
            pkg_stanza_get(release, "Suite") ? pkg_stanza_get(release, "Suite") : "?",
            (unsigned long)serial, pkg_stanza_get(release, "Date") ? pkg_stanza_get(release, "Date") : "");

out:
    pkg_stanza_free(release);
    pkg_http_free(&rel);
    pkg_http_free(&sig);
    pkg_http_free(&pk);
    pkg_http_free(&an);
    return rc;
}

LONG pkg_refresh(struct PkgContext *ctx, BOOL force)
{
    struct PkgSource *src;
    LONG rc, first_err = 0;
    ULONG ok = 0, total = 0;
    BOOL any_changed = FALSE;

    ForeachNode(&ctx->Sources, src)
    {
        BOOL changed;
        if (!src->Enabled) continue;
        total++;
        rc = refresh_source(ctx, src, force, FALSE, &changed);
        if (rc == PKGERR_ABORTED) return rc;
        if (rc)
        {
            pkg_log(ctx, "%s: %s - %s", src->Node.ln_Name, pkg_strerror(rc), ctx->ErrText);
            if (!first_err) first_err = rc;
            continue;
        }
        ok++;
        if (changed) any_changed = TRUE;
    }
    if (total == 0)
    {
        pkg_set_error(ctx, "no enabled sources (Pkg SOURCE ADD <name> <url>)");
        return PKGERR_BADPREFS;
    }
    if (any_changed || ctx->IndexDirty)
    {
        pkg_cache_load(ctx);
        pkg_prefs_save(ctx);
    }
    pkg_reload(ctx);
    return ok ? 0 : first_err;
}

/* The installed DB is shared with other processes (C:Pkg, the GUI, the
   notifier): a long-lived context must re-read it, or its update count
   reflects whatever was installed when the context was opened. */
LONG pkg_reload(struct PkgContext *ctx)
{
    return pkg_db_load(ctx);
}

LONG pkg_check(struct PkgContext *ctx, ULONG *updates, ULONG *newannouncements)
{
    struct PkgSource *src;
    BOOL need_full = FALSE;
    LONG rc = 0, first_err = 0;
    ULONG total = 0;

    if (updates) *updates = 0;
    if (newannouncements) *newannouncements = 0;

    ForeachNode(&ctx->Sources, src)
    {
        BOOL changed;
        if (!src->Enabled) continue;
        total++;
        rc = refresh_source(ctx, src, FALSE, TRUE, &changed);
        if (rc == PKGERR_ABORTED) return rc;
        if (rc) { if (!first_err) first_err = rc; continue; }
        if (changed) need_full = TRUE;
    }
    if (total == 0) return PKGERR_BADPREFS;
    if (need_full)
    {
        rc = pkg_refresh(ctx, FALSE);
        if (rc) return rc;
    }
    else if (first_err && !ctx->Index)
        return first_err;
    else
        pkg_reload(ctx);               /* pkg_refresh() reloads it otherwise */

    /* count from the (cached or fresh) index */
    {
        struct List recs;
        struct Node *n;
        ULONG cnt = 0;
        if (pkg_list(ctx, PKGF_UPDATES, &recs) == 0)
        {
            ForeachNode(&recs, n) cnt++;
            pkg_free_records(&recs);
        }
        if (updates) *updates = cnt;
    }
    {
        struct List anns;
        struct Node *n;
        ULONG cnt = 0;
        if (pkg_announcements(ctx, TRUE, &anns) == 0)
        {
            ForeachNode(&anns, n) cnt++;
            pkg_free_announcements(&anns);
        }
        if (newannouncements) *newannouncements = cnt;
    }
    return 0;
}
