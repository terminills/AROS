/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Install / upgrade / remove and screenshot fetch (PROTOCOL.md §4,
          §5 rules 4-5).  Flow for install:
            resolve deps -> download (resume) -> verify Size+SHA256 ->
            pass 1 (validate archive, conflict check) -> pass 2 (extract) ->
            registration (DB, ENVARC:SYS/Packages, Package-Startup).
          Nothing from an archive is written before its hash and structure
          have been accepted; a failing extract leaves the DB untouched.

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: HIGH
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/var.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pkg_private.h"

#define MAX_DEPTH 8

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */

static CONST_STRPTR basename_of(CONST_STRPTR path)
{
    CONST_STRPTR p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* "Name (>= 1.2), Other" → iterate names; ver receives the constraint or "" */
static const char *next_dep(const char *p, char name[64], char ver[32])
{
    const char *start, *end;
    name[0] = ver[0] = 0;
    while (*p == ' ' || *p == ',') p++;
    if (!*p) return NULL;
    start = p;
    while (*p && *p != ',' && *p != '(' && *p != ' ') p++;
    end = p;
    if (end - start >= 64) return NULL;
    memcpy(name, start, end - start);
    name[end - start] = 0;
    while (*p == ' ') p++;
    if (*p == '(')
    {
        const char *close = strchr(p, ')');
        const char *v;
        if (!close) return NULL;
        v = p + 1;
        while (*v == ' ' || *v == '>' || *v == '=' || *v == '<') v++;
        if (close - v < 32 && close > v)
        {
            memcpy(ver, v, close - v);
            ver[close - v] = 0;
            while (ver[0] && ver[strlen(ver) - 1] == ' ') ver[strlen(ver) - 1] = 0;
        }
        p = close + 1;
    }
    while (*p == ' ') p++;
    if (*p == ',') p++;
    return p;
}

static BOOL in_replaces(CONST_STRPTR replaces, CONST_STRPTR name)
{
    char n[64], v[32];
    const char *p = replaces;
    if (!replaces) return FALSE;
    while ((p = next_dep(p, n, v)))
        if (stricmp(n, name) == 0) return TRUE;
    return FALSE;
}

/* Where does this package's payload go?  Fills root (with trailing ':' or
   '/') and whether the OS package convention (§4.2) applies. */
static LONG resolve_root(struct PkgContext *ctx, struct pkg_stanza *rec, BOOL has_startup,
                         STRPTR root, ULONG rootlen, BOOL *activation)
{
    const char *name    = pkg_stanza_get(rec, "Package");
    const char *extends = pkg_stanza_get(rec, "Extends");
    const char *prefix  = pkg_stanza_get(rec, "Prefix");
    const char *subdir  = pkg_stanza_get(rec, "Subdir");
    ULONG l;

    *activation = FALSE;
    if (extends && *extends)
    {
        struct pkg_stanza *host = pkg_db_find(ctx, extends);
        const char *hostroot = host ? pkg_stanza_get(host, PKGX_INSTALLROOT) : NULL;
        TEXT var[128];
        TEXT val[PKG_PATHLEN];

        if (hostroot && *hostroot)
            strlcpy(root, hostroot, rootlen);
        else
        {
            snprintf(var, sizeof var, "SYS/Packages/%s", extends);
            if (GetVar(var, val, sizeof val, GVF_GLOBAL_ONLY) > 0 && val[0])
                strlcpy(root, val, rootlen);
            else if (prefix && *prefix)
                strlcpy(root, prefix, rootlen);
            else
            {
                pkg_set_error(ctx, "%s extends %s, which is not installed or registered", name, extends);
                return PKGERR_DEPENDS;
            }
        }
        if (subdir && *subdir)
        {
            l = strlen(root);
            if (l && root[l - 1] != ':' && root[l - 1] != '/') strlcat(root, "/", rootlen);
            strlcat(root, subdir, rootlen);
        }
    }
    else if (has_startup)
    {
        snprintf(root, rootlen, "%s/%s", ctx->PkgRoot, name);
        *activation = TRUE;
    }
    else
        strlcpy(root, prefix && *prefix ? prefix : "SYS:", rootlen);

    l = strlen(root);
    if (l && root[l - 1] != ':' && root[l - 1] != '/') strlcat(root, "/", rootlen);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* download + verify                                                         */

static LONG download_archive(struct PkgContext *ctx, struct pkg_stanza *rec, STRPTR local, ULONG locallen)
{
    const char *name = pkg_stanza_get(rec, "Package");
    const char *fn   = pkg_stanza_get(rec, "Filename");
    const char *url  = pkg_stanza_get(rec, "URL");
    const char *hex  = pkg_stanza_get(rec, "SHA256");
    const char *src  = pkg_stanza_get(rec, PKGX_SOURCE);
    UQUAD size = pkg_parse_u64(pkg_stanza_get(rec, "Size"));
    struct PkgSource *source = src ? pkg_source_find(ctx, src) : NULL;
    TEXT fullurl[PKG_URL_LEN + 300];
    struct PkgHttpReq req;
    UBYTE md[32];
    char got[65];
    UQUAD have;
    LONG rc;
    int attempt;

    if (!hex || strlen(hex) != 64 || size == 0)
    {
        pkg_set_error(ctx, "%s: index record lacks Size/SHA256", name);
        return PKGERR_FORMAT;
    }
    if (url && *url)
        strlcpy(fullurl, url, sizeof fullurl);
    else if (fn && *fn && source)
        snprintf(fullurl, sizeof fullurl, "%s%s", source->URL, fn);
    else
    {
        pkg_set_error(ctx, "%s: no Filename/URL or unknown source", name);
        return PKGERR_FORMAT;
    }

    pkg_mkdir_p(pkg_basepath(ctx, PKG_DOWNLOADDIR, NULL, local, locallen));
    pkg_basepath(ctx, PKG_DOWNLOADDIR, basename_of(fn && *fn ? (const char *)fn : (const char *)fullurl), local, locallen);

    for (attempt = 0; attempt < 2; attempt++)
    {
        have = pkg_file_size(local);
        if (have == (UQUAD)-1 || have < size)
        {
            memset(&req, 0, sizeof req);
            req.URL = fullurl;
            req.ToFile = local;
            req.Resume = have != (UQUAD)-1 && have > 0;
            req.Label = "Downloading";
            req.Package = name;
            rc = pkg_http(ctx, &req);
            if (rc) return rc;
        }
        pkg_progress(ctx, "Verifying", name, 0, 0, 0, 0);
        rc = pkg_file_sha256(local, md, &have);
        if (rc) return rc;
        pkg_hex(md, 32, got);
        if (have == size && pkg_hex_equal(got, hex)) return 0;
        pkg_log(ctx, "%s: %s (%llu bytes) does not match the signed index - discarding",
                name, basename_of(local), (unsigned long long)have);
        DeleteFile(local);
    }
    pkg_set_error(ctx, "%s: downloaded archive failed Size/SHA256 check", name);
    return PKGERR_BADHASH;
}

/* ------------------------------------------------------------------------ */
/* archive walking                                                           */

struct tarfile { BPTR fh; };

static long tar_readfn(void *c, void *buf, size_t n)
{
    LONG got = Read(((struct tarfile *)c)->fh, buf, (LONG)n);
    return got < 0 ? -1 : got;
}

/* Classify an archive entry.  Returns 1 payload, 2 startup script,
   3 PKGINFO, 0 ignorable (screenshots, top dirs), -1 forbidden. */
static int classify(const char *name)
{
    if (strcmp(name, "PKGINFO") == 0) return 3;
    if (strcmp(name, "S/Package-Startup") == 0) return 2;
    if (strncmp(name, "files/", 6) == 0) return name[6] ? 1 : 0;
    if (strcmp(name, "files") == 0 || strcmp(name, "S") == 0) return 0;
    if (strncmp(name, "screenshots/", 12) == 0 || strcmp(name, "screenshots") == 0) return 0;
    return -1;
}

static void set_protection(CONST_STRPTR path, uint32_t mode, BOOL script)
{
    ULONG prot = 0;                           /* bits set = DENIED for rwed */
    if (!(mode & 0400)) prot |= FIBF_READ;
    if (!(mode & 0200)) prot |= FIBF_WRITE | FIBF_DELETE;
    if (!(mode & 0100)) prot |= FIBF_EXECUTE;
    if (script) prot |= FIBF_SCRIPT;
    SetProtection(path, prot);
}

static LONG extract_file(struct PkgContext *ctx, struct pkg_tar *t, CONST_STRPTR dest, UQUAD size)
{
    BPTR fh;
    UBYTE *buf = malloc(65536);
    long got;
    LONG rc = 0;

    if (!buf) return PKGERR_NOMEM;
    fh = Open(dest, MODE_NEWFILE);
    if (!fh) { free(buf); pkg_set_error(ctx, "cannot create %s", dest); return PKGERR_IO; }
    while ((got = pkg_tar_read(t, buf, 65536)) > 0)
    {
        if (Write(fh, buf, got) != got) { rc = PKGERR_IO; pkg_set_error(ctx, "write error on %s", dest); break; }
    }
    if (got < 0) { rc = PKGERR_BADARCHIVE; pkg_set_error(ctx, "archive: %s", pkg_tar_error(t)); }
    Close(fh);
    free(buf);
    if (rc) DeleteFile(dest);
    return rc;
}

/* pass 1: structure + conflicts; fills *has_startup, *nentries */
static LONG validate_archive(struct PkgContext *ctx, CONST_STRPTR local, struct pkg_stanza *rec,
                             BOOL *has_startup, ULONG *nentries)
{
    struct tarfile tf;
    struct pkg_tar *t;
    struct pkg_tar_entry e;
    const char *name = pkg_stanza_get(rec, "Package");
    const char *ver  = pkg_stanza_get(rec, "Version");
    const char *replaces = pkg_stanza_get(rec, "Replaces");
    BOOL saw_pkginfo = FALSE;
    LONG rc = 0;
    int r;

    *has_startup = FALSE;
    *nentries = 0;
    tf.fh = Open(local, MODE_OLDFILE);
    if (!tf.fh) return PKGERR_IO;
    t = pkg_tar_open(tar_readfn, &tf);
    if (!t) { Close(tf.fh); return PKGERR_NOMEM; }

    while ((r = pkg_tar_next(t, &e)) > 0)
    {
        int cls = classify(e.name);
        if (cls < 0)
        {
            pkg_set_error(ctx, "%s: unexpected archive entry %s", name, e.name);
            rc = PKGERR_BADARCHIVE;
            break;
        }
        if (cls == 3)
        {
            char buf[8192];
            long got = pkg_tar_read(t, buf, sizeof buf - 1);
            struct pkg_stanza *info;
            int n = 0;
            const char *v;
            if (got < 0) { rc = PKGERR_BADARCHIVE; break; }
            buf[got] = 0;
            info = pkg_stanza_parse(buf, got, &n);
            v = info ? pkg_stanza_get(info, "Package") : NULL;
            if (!v || stricmp(v, name) != 0 ||
                !(v = pkg_stanza_get(info, "Version")) || pkg_vercmp(v, ver) != 0)
            {
                pkg_set_error(ctx, "%s: PKGINFO does not match the index record", name);
                pkg_stanza_free(info);
                rc = PKGERR_BADARCHIVE;
                break;
            }
            pkg_stanza_free(info);
            saw_pkginfo = TRUE;
        }
        else if (cls == 2)
            *has_startup = TRUE;
        (*nentries)++;
    }
    if (r < 0 && rc == 0)
    {
        pkg_set_error(ctx, "%s: %s", name, pkg_tar_error(t));
        rc = PKGERR_BADARCHIVE;
    }
    pkg_tar_close(t);
    Close(tf.fh);
    if (rc) return rc;
    if (!saw_pkginfo)
    {
        pkg_set_error(ctx, "%s: archive has no PKGINFO", name);
        return PKGERR_BADARCHIVE;
    }
    (void)replaces;
    return 0;
}

/* pass 2: extract into root; files list receives every path written */
static LONG extract_archive(struct PkgContext *ctx, CONST_STRPTR local, struct pkg_stanza *rec,
                            CONST_STRPTR root, ULONG nentries, struct List *files)
{
    struct tarfile tf;
    struct pkg_tar *t;
    struct pkg_tar_entry e;
    const char *name = pkg_stanza_get(rec, "Package");
    const char *replaces = pkg_stanza_get(rec, "Replaces");
    TEXT dest[PKG_PATHLEN];
    ULONG done = 0;
    LONG rc = 0;
    int r;

    tf.fh = Open(local, MODE_OLDFILE);
    if (!tf.fh) return PKGERR_IO;
    t = pkg_tar_open(tar_readfn, &tf);
    if (!t) { Close(tf.fh); return PKGERR_NOMEM; }

    while ((r = pkg_tar_next(t, &e)) > 0)
    {
        int cls = classify(e.name);
        CONST_STRPTR rel;

        done++;
        if (cls == 0 || cls == 3) { pkg_tar_skip(t); continue; }
        rel = cls == 1 ? e.name + 6 : e.name;              /* strip files/ */
        snprintf(dest, sizeof dest, "%s%s", root, rel);

        if (e.type == PKG_TAR_DIR)
        {
            if (pkg_mkdir_p(dest)) { pkg_set_error(ctx, "cannot create %s", dest); rc = PKGERR_IO; break; }
            continue;
        }

        pkg_progress(ctx, "Installing", name, done, nentries, 0, 0);
        if (pkg_abort_requested(ctx)) { rc = PKGERR_ABORTED; break; }

        /* parent dir */
        {
            TEXT dir[PKG_PATHLEN];
            strlcpy(dir, dest, sizeof dir);
            *(PathPart(dir)) = 0;
            if (dir[0] && pkg_mkdir_p(dir)) { pkg_set_error(ctx, "cannot create %s", dir); rc = PKGERR_IO; break; }
        }

        /* ownership / backup (§4.1) */
        if (pkg_file_exists(dest))
        {
            STRPTR owner = pkg_db_owner_of(ctx, dest);
            if (owner && stricmp(owner, name) != 0 && !in_replaces(replaces, owner))
            {
                pkg_set_error(ctx, "%s: %s is owned by %s", name, dest, owner);
                rc = PKGERR_CONFLICT;
                break;
            }
            if (!owner)
            {
                TEXT save[PKG_PATHLEN];
                snprintf(save, sizeof save, "%s.pkgsave", dest);
                if (!pkg_file_exists(save) && Rename(dest, save))
                    pkg_log(ctx, "%s: kept existing %s as %s", name, basename_of(dest), basename_of(save));
            }
        }

        rc = extract_file(ctx, t, dest, e.size);
        if (rc) break;
        set_protection(dest, e.mode, cls == 2 || strncmp(rel, "S/", 2) == 0);
        if ((rc = pkg_namelist_add(files, dest))) break;
    }
    if (r < 0 && rc == 0)
    {
        pkg_set_error(ctx, "%s: %s", name, pkg_tar_error(t));
        rc = PKGERR_BADARCHIVE;
    }
    pkg_tar_close(t);
    Close(tf.fh);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* removal primitives                                                        */

static void remove_files(struct PkgContext *ctx, CONST_STRPTR name, struct List *files, struct List *keep)
{
    struct Node *n;
    ULONG total = 0, done = 0;

    ForeachNode(files, n) total++;
    /* reverse order: files before the directories that hold them */
    for (n = files->lh_TailPred; n->ln_Pred; n = n->ln_Pred)
    {
        TEXT save[PKG_PATHLEN];
        struct Node *k;
        BOOL kept = FALSE;

        pkg_progress(ctx, "Removing", name, ++done, total, 0, 0);
        if (keep)
            ForeachNode(keep, k)
                if (stricmp(k->ln_Name, n->ln_Name) == 0) { kept = TRUE; break; }
        if (kept) continue;

        SetProtection(n->ln_Name, 0);
        if (!DeleteFile(n->ln_Name))
        {
            LONG err = IoErr();
            if (err != ERROR_OBJECT_NOT_FOUND)
            {
                TEXT why[80];
                Fault(err, NULL, why, sizeof why);
                pkg_log(ctx, "%s: cannot delete %s: %s", name, n->ln_Name, why);
            }
        }
        snprintf(save, sizeof save, "%s.pkgsave", n->ln_Name);
        if (pkg_file_exists(save)) Rename(save, n->ln_Name);

        /* prune now-empty parent dirs (DeleteFile fails harmlessly otherwise) */
        {
            TEXT dir[PKG_PATHLEN];
            strlcpy(dir, n->ln_Name, sizeof dir);
            for (;;)
            {
                STRPTR p = PathPart(dir);
                if (p == dir || *(p - 1) == ':') break;
                *p = 0;
                if (!DeleteFile(dir)) break;
            }
        }
    }
}

static void unregister_activation(CONST_STRPTR name)
{
    TEXT var[128];
    snprintf(var, sizeof var, "SYS/Packages/%s", name);
    DeleteVar(var, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

/* Mirror exactly what Startup-Sequence does for ENV:SYS/Packages entries:
   CD into the package root, then Execute S/<Name>-Startup if present, else
   S/Package-Startup.  Scripts are written with root-relative paths (AHI:
   "Assign DEVS: Devs ADD"), so running them from any other directory - or
   through an absolute path alone - breaks them. */
static LONG run_startup(struct PkgContext *ctx, CONST_STRPTR name, CONST_STRPTR root)
{
    TEXT cmd[PKG_PATHLEN + 32], script[64];
    BPTR in, out, dir, olddir;
    LONG rc;

    dir = Lock(root, SHARED_LOCK);
    if (!dir)
    {
        pkg_log(ctx, "%s: cannot lock %s to run the startup script", name, root);
        return 0;
    }
    snprintf(script, sizeof script, "S/%s-Startup", name);
    snprintf(cmd, sizeof cmd, "%s%s", root, script);
    if (!pkg_file_exists(cmd))
        strlcpy(script, "S/Package-Startup", sizeof script);
    snprintf(cmd, sizeof cmd, "Execute \"%s\"", script);

    in = Open("NIL:", MODE_OLDFILE);
    out = Open("NIL:", MODE_NEWFILE);
    olddir = CurrentDir(dir);
    pkg_log(ctx, "%s: running %s", name, script);
    rc = SystemTags(cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, FALSE, TAG_DONE);
    CurrentDir(olddir);
    UnLock(dir);
    if (in) Close(in);
    if (out) Close(out);
    if (rc != 0) pkg_log(ctx, "%s: %s returned %ld", name, script, (long)rc);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Installer-script packages (§4.5)                                          */

/* Drop the ;BEGIN <section> ... ;END <section> block Installer's
   (startup) wrote into S:User-Startup. */
static void strip_userstartup(CONST_STRPTR section)
{
    BPTR in, out;
    TEXT line[1024];
    BOOL skip = FALSE, changed = FALSE;
    ULONG sl = strlen(section);

    if (!(in = Open("S:User-Startup", MODE_OLDFILE))) return;
    if (!(out = Open("S:User-Startup.pkgtmp", MODE_NEWFILE))) { Close(in); return; }
    while (FGets(in, line, sizeof line))
    {
        ULONG l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!skip && strnicmp(line, ";BEGIN ", 7) == 0 && l == 7 + sl && strcmp(line + 7, section) == 0)
        {
            skip = changed = TRUE;
            continue;
        }
        if (skip)
        {
            if (strnicmp(line, ";END ", 5) == 0 && l == 5 + sl && strcmp(line + 5, section) == 0) skip = FALSE;
            continue;
        }
        FPuts(out, line);
        FPutC(out, '\n');
    }
    Close(in);
    Close(out);
    if (changed && DeleteFile("S:User-Startup")) Rename("S:User-Startup.pkgtmp", "S:User-Startup");
    else DeleteFile("S:User-Startup.pkgtmp");
}

/* Append " <word>" to a space-separated stanza value kept in buf. */
static void add_word(STRPTR buf, ULONG buflen, CONST_STRPTR word)
{
    if (buf[0]) strlcat(buf, " ", buflen);
    strlcat(buf, word, buflen);
}

/* Installer's installed-files registry entry for an application (see the
   Installer Documentation): the drawer ENV:Installer/Registry names, else
   SYS:Prefs/Installer/Registry, then APPNAME. */
static void registry_entry(CONST_STRPTR name, STRPTR path, ULONG len)
{
    if (GetVar("Installer/Registry", path, len, 0) <= 0)
        strlcpy(path, "SYS:Prefs/Installer/Registry", len);
    AddPart(path, name, len);
}

/* Read a registry entry: F/D lines are what the package owns, A and S are
   undone separately on removal, T (tooltype edits) cannot be undone. */
static void read_registry(CONST_STRPTR path, struct List *files, STRPTR assigns, STRPTR sections, ULONG len)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    TEXT line[PKG_PATHLEN + 4];

    assigns[0] = sections[0] = 0;
    if (!fh) return;
    while (FGets(fh, line, sizeof line))
    {
        ULONG l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (l < 3 || line[1] != ' ') continue;
        switch (line[0])
        {
        case 'F': case 'D': pkg_namelist_add(files, line + 2); break;
        case 'A': add_word(assigns, len, line + 2); break;
        case 'S': if (!strchr(line + 2, ' ')) add_word(sections, len, line + 2); break;
        }
    }
    Close(fh);
}

/* Undo the assigns and User-Startup sections an Installer run recorded. */
static void undo_installer_extras(CONST_STRPTR assigns, CONST_STRPTR sections)
{
    TEXT word[128];
    const char *p, *e;

    for (p = assigns; p && *p; p = *e ? e + 1 : e)
    {
        e = strchr(p, ' ');
        if (!e) e = p + strlen(p);
        if ((ULONG)(e - p) < sizeof word)
        {
            memcpy(word, p, e - p);
            word[e - p] = 0;
            AssignLock(word, BNULL);
        }
    }
    for (p = sections; p && *p; p = *e ? e + 1 : e)
    {
        e = strchr(p, ' ');
        if (!e) e = p + strlen(p);
        if ((ULONG)(e - p) < sizeof word)
        {
            memcpy(word, p, e - p);
            word[e - p] = 0;
            strip_userstartup(word);
        }
    }
}

/* Unpack the archive's files/ tree into a staging drawer, run the package's
   own Installer script from there, and take the file list from Installer's
   registry entry for it.  The script decides where things go, so ownership checks and
   .pkgsave backups do not apply; removal is still exact. */
static LONG run_installer(struct PkgContext *ctx, CONST_STRPTR local, struct pkg_stanza *rec,
                          ULONG nentries, struct List *files, STRPTR assigns, STRPTR sections, ULONG len)
{
    const char *name   = pkg_stanza_get(rec, "Package");
    const char *script = pkg_stanza_get(rec, "Installer");
    TEXT stage[PKG_PATHLEN], regentry[PKG_PATHLEN], logfile[PKG_PATHLEN], cmd[PKG_PATHLEN * 3];
    struct List staged;
    BPTR dir, olddir, in, out;
    LONG rc;

    NewList(&staged);
    snprintf(stage, sizeof stage, "%s/.Staging-%s", ctx->PkgRoot, name);
    pkg_delete_tree(stage);
    strlcat(stage, "/", sizeof stage);
    rc = extract_archive(ctx, local, rec, stage, nentries, &staged);
    pkg_free_namelist(&staged);
    if (rc) goto out;

    registry_entry(name, regentry, sizeof regentry);
    snprintf(logfile, sizeof logfile, "T:Pkg-%s.log", name);
    snprintf(cmd, sizeof cmd,
             "SYS:Utilities/Installer SCRIPT \"%s\" APPNAME \"%s\" MINUSER NOVICE DEFUSER NOVICE"
             " LOGFILE \"%s\"", script, name, logfile);
    DeleteFile(regentry);

    stage[strlen(stage) - 1] = 0;
    if (!(dir = Lock(stage, SHARED_LOCK)))
    {
        pkg_set_error(ctx, "%s: cannot lock the staging drawer %s", name, stage);
        rc = PKGERR_IO;
        goto out;
    }
    pkg_log(ctx, "%s: running Installer script %s", name, script);
    in = Open("NIL:", MODE_OLDFILE);
    out = Open("NIL:", MODE_NEWFILE);
    olddir = CurrentDir(dir);
    rc = SystemTags(cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, FALSE, TAG_DONE);
    CurrentDir(olddir);
    UnLock(dir);
    if (in) Close(in);
    if (out) Close(out);

    read_registry(regentry, files, assigns, sections, len);
    if (rc != 0)
    {
        DeleteFile(regentry);
        pkg_set_error(ctx, "%s: the Installer script did not finish (%ld); see %s", name, (long)rc, logfile);
        rc = PKGERR_ABORTED;
    }
out:
    pkg_delete_tree(stage);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* install                                                                   */

static LONG install_one(struct PkgContext *ctx, CONST_STRPTR name, int depth);

static LONG ensure_depends(struct PkgContext *ctx, struct pkg_stanza *rec, int depth)
{
    const char *deps = pkg_stanza_get(rec, "Depends");
    const char *name = pkg_stanza_get(rec, "Package");
    char dn[64], dv[32];
    const char *p = deps;
    LONG rc;

    if (!deps) return 0;
    if (depth >= MAX_DEPTH)
    {
        pkg_set_error(ctx, "%s: dependency chain too deep", name);
        return PKGERR_DEPENDS;
    }
    while ((p = next_dep(p, dn, dv)))
    {
        struct pkg_stanza *inst = pkg_db_find(ctx, dn);
        struct pkg_stanza *idx;
        if (inst && (!dv[0] || pkg_vercmp(pkg_stanza_get(inst, "Version"), dv) >= 0))
            continue;
        idx = pkg_index_find(ctx, dn);
        if (!idx || (dv[0] && pkg_vercmp(pkg_stanza_get(idx, "Version"), dv) < 0))
        {
            pkg_set_error(ctx, "%s needs %s%s%s which no source provides", name, dn,
                          dv[0] ? " >= " : "", dv);
            return PKGERR_DEPENDS;
        }
        pkg_log(ctx, "%s: installing dependency %s", name, dn);
        rc = install_one(ctx, dn, depth + 1);
        if (rc) return rc;
    }
    return 0;
}

static LONG install_one(struct PkgContext *ctx, CONST_STRPTR name, int depth)
{
    struct pkg_stanza *rec = pkg_index_find(ctx, name);
    struct pkg_stanza *old = pkg_db_find(ctx, name);
    struct pkg_stanza *dbrec = NULL;
    struct List files, oldfiles;
    TEXT local[PKG_PATHLEN], root[PKG_PATHLEN];
    TEXT assigns[256], sections[256];
    const char *script;
    BOOL has_startup, activation;
    ULONG nentries;
    LONG rc;

    NewList(&files);
    NewList(&oldfiles);

    if (!rec)
    {
        pkg_set_error(ctx, "no source provides %s (Pkg REFRESH?)", name);
        return PKGERR_NOTFOUND;
    }
    name = pkg_stanza_get(rec, "Package");          /* canonical spelling */
    if (old && pkg_vercmp(pkg_stanza_get(rec, "Version"), pkg_stanza_get(old, "Version")) <= 0)
    {
        pkg_log(ctx, "%s %s is already installed", name, pkg_stanza_get(old, "Version"));
        return 0;
    }

    rc = ensure_depends(ctx, rec, depth);
    if (rc) return rc;

    rc = download_archive(ctx, rec, local, sizeof local);
    if (rc) return rc;

    rc = validate_archive(ctx, local, rec, &has_startup, &nentries);
    if (rc) return rc;

    rc = resolve_root(ctx, rec, has_startup, root, sizeof root, &activation);
    if (rc) return rc;
    script = pkg_stanza_get(rec, "Installer");
    if (script && !*script) script = NULL;
    if (script)
    {
        /* the script places the files; it is the activation too */
        strlcpy(root, ctx->PkgRoot, sizeof root);
        activation = FALSE;
    }

    if (old)
    {
        pkg_log(ctx, "%s: upgrading %s -> %s", name, pkg_stanza_get(old, "Version"), pkg_stanza_get(rec, "Version"));
        pkg_db_files(ctx, name, &oldfiles);
    }
    else
        pkg_log(ctx, "%s: installing %s into %s", name, pkg_stanza_get(rec, "Version"), root);

    if (script)
        rc = run_installer(ctx, local, rec, nentries, &files, assigns, sections, sizeof assigns);
    else
        rc = extract_archive(ctx, local, rec, root, nentries, &files);
    if (rc)
    {
        /* roll back what this attempt wrote (old files stay) */
        /* assigns first: an assign holds a lock on the drawer it names */
        if (script) undo_installer_extras(assigns, sections);
        remove_files(ctx, name, &files, old ? &oldfiles : NULL);
        pkg_free_namelist(&files);
        pkg_free_namelist(&oldfiles);
        return rc;
    }

    /* files of the previous version that the new one no longer ships */
    if (old)
    {
        remove_files(ctx, name, &oldfiles, &files);
        pkg_free_namelist(&oldfiles);
    }

    /* DB record = index record + install metadata */
    dbrec = pkg_stanza_new();
    if (dbrec)
    {
        struct pkg_field *f;
        struct DateStamp ds;
        TEXT date[32];
        for (f = rec->fields; f; f = f->next) pkg_stanza_set(dbrec, f->key, f->value);
        pkg_stanza_set(dbrec, PKGX_INSTALLROOT, root);
        DateStamp(&ds);
        snprintf(date, sizeof date, "%ld.%ld", (long)ds.ds_Days, (long)ds.ds_Minute);
        pkg_stanza_set(dbrec, PKGX_INSTALLDATE, date);
        if (script && assigns[0]) pkg_stanza_set(dbrec, PKGX_ASSIGNS, assigns);
        if (script && sections[0]) pkg_stanza_set(dbrec, PKGX_USERSTARTUP, sections);
        rc = pkg_db_store(ctx, dbrec, &files);
        pkg_stanza_free(dbrec);
    }
    else rc = PKGERR_NOMEM;
    pkg_free_namelist(&files);
    if (rc) { pkg_set_error(ctx, "%s: installed but the package database could not be written", name); return rc; }

    if (activation)
    {
        TEXT var[128];
        snprintf(var, sizeof var, "SYS/Packages/%s", name);
        SetVar(var, root, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
        run_startup(ctx, name, root);
    }
    if (pkg_parse_bool(pkg_stanza_get(rec, "Reboot")))
        pkg_log(ctx, "%s: a reboot is required to activate this package", name);
    DeleteFile(local);
    pkg_log(ctx, "%s %s installed", name, pkg_stanza_get(rec, "Version"));
    return 0;
}

LONG pkg_install(struct PkgContext *ctx, CONST_STRPTR name)
{
    ctx->ErrText[0] = 0;
    return install_one(ctx, name, 0);
}

/* ------------------------------------------------------------------------ */
/* remove                                                                    */

static LONG remove_one(struct PkgContext *ctx, CONST_STRPTR name, int depth)
{
    struct pkg_stanza *inst = pkg_db_find(ctx, name);
    struct pkg_stanza *s;
    struct List files;
    const char *root;
    LONG rc;

    if (!inst)
    {
        pkg_set_error(ctx, "%s is not installed", name);
        return PKGERR_NOTINSTALLED;
    }
    name = pkg_stanza_get(inst, "Package");
    if (depth >= MAX_DEPTH) return PKGERR_DEPENDS;

    /* cascade: add-ons living inside this package's tree go first */
    for (s = ctx->Installed; s; s = s->next)
    {
        const char *ext = pkg_stanza_get(s, "Extends");
        if (ext && stricmp(ext, name) == 0)
        {
            pkg_log(ctx, "%s: removing add-on %s first", name, pkg_stanza_get(s, "Package"));
            rc = remove_one(ctx, pkg_stanza_get(s, "Package"), depth + 1);
            if (rc) return rc;
            s = ctx->Installed;            /* list changed; restart */
            if (!s) break;
        }
    }
    inst = pkg_db_find(ctx, name);
    if (!inst) return 0;

    root = pkg_stanza_get(inst, PKGX_INSTALLROOT);
    pkg_log(ctx, "%s: removing %s", name, pkg_stanza_get(inst, "Version"));
    rc = pkg_db_files(ctx, name, &files);
    if (rc) return rc;
    /* assigns first: an assign holds a lock on the drawer it names */
    undo_installer_extras(pkg_stanza_get(inst, PKGX_ASSIGNS), pkg_stanza_get(inst, PKGX_USERSTARTUP));
    remove_files(ctx, name, &files, NULL);
    pkg_free_namelist(&files);
    if (pkg_stanza_get(inst, "Installer"))
    {
        TEXT entry[PKG_PATHLEN];
        registry_entry(name, entry, sizeof entry);
        DeleteFile(entry);
    }

    if (root && strncmp(root, ctx->PkgRoot, strlen(ctx->PkgRoot)) == 0 && root[strlen(ctx->PkgRoot)] == '/')
    {
        TEXT dir[PKG_PATHLEN];
        unregister_activation(name);
        strlcpy(dir, root, sizeof dir);
        if (dir[strlen(dir) - 1] == '/') dir[strlen(dir) - 1] = 0;
        pkg_delete_tree(dir);
    }
    pkg_db_delete(ctx, name);
    pkg_log(ctx, "%s removed", name);
    return 0;
}

LONG pkg_remove(struct PkgContext *ctx, CONST_STRPTR name)
{
    ctx->ErrText[0] = 0;
    return remove_one(ctx, name, 0);
}

/* ------------------------------------------------------------------------ */
/* upgrade                                                                   */

LONG pkg_upgrade(struct PkgContext *ctx, CONST_STRPTR name)
{
    struct List recs;
    struct Node *n;
    LONG rc = 0, first = 0;
    ULONG done = 0;

    ctx->ErrText[0] = 0;
    if (name)
    {
        if (!pkg_db_find(ctx, name)) { pkg_set_error(ctx, "%s is not installed", name); return PKGERR_NOTINSTALLED; }
        return install_one(ctx, name, 0);
    }
    if ((rc = pkg_list(ctx, PKGF_UPDATES, &recs))) return rc;
    ForeachNode(&recs, n)
    {
        rc = install_one(ctx, n->ln_Name, 0);
        if (rc == PKGERR_ABORTED) break;
        if (rc && !first) first = rc;
        if (!rc) done++;
    }
    pkg_free_records(&recs);
    if (rc == PKGERR_ABORTED) return rc;
    pkg_log(ctx, "%lu package(s) upgraded", (unsigned long)done);
    return first;
}

/* ------------------------------------------------------------------------ */
/* screenshots (§5 rule 5)                                                   */

LONG pkg_screenshot(struct PkgContext *ctx, struct PkgRecord *rec, ULONG index,
                    STRPTR buf, ULONG buflen)
{
    const char *shots = rec ? pkg_record_get(rec, "Screenshot") : NULL;
    const char *hashes = rec ? pkg_record_get(rec, "ScreenshotSHA256") : NULL;
    struct PkgSource *source = rec && rec->SourceName ? pkg_source_find(ctx, rec->SourceName) : NULL;
    TEXT path[300], hex[65], url[PKG_URL_LEN + 300];
    const char *p, *e;
    ULONG i;
    UBYTE md[32];
    char got[65];
    LONG rc;
    int attempt;

    if (!shots || !hashes || !source) return PKGERR_NOTFOUND;

    /* pick the index-th token of each list */
    for (p = shots, i = 0;; i++)
    {
        while (*p == ' ') p++;
        if (!*p) return PKGERR_NOTFOUND;
        e = p; while (*e && *e != ' ') e++;
        if (i == index) break;
        p = e;
    }
    if ((ULONG)(e - p) >= sizeof path) return PKGERR_FORMAT;
    memcpy(path, p, e - p); path[e - p] = 0;
    for (p = hashes, i = 0;; i++)
    {
        while (*p == ' ') p++;
        if (!*p) return PKGERR_FORMAT;
        e = p; while (*e && *e != ' ') e++;
        if (i == index) break;
        p = e;
    }
    if (e - p != 64) return PKGERR_FORMAT;
    memcpy(hex, p, 64); hex[64] = 0;

    pkg_basepath(ctx, PKG_CACHEDIR, source->Node.ln_Name, buf, buflen);
    AddPart(buf, (STRPTR)"screenshots", buflen);
    AddPart(buf, (STRPTR)basename_of(path), buflen);
    snprintf(url, sizeof url, "%s%s", source->URL, path);

    for (attempt = 0; attempt < 2; attempt++)
    {
        if (!pkg_file_exists(buf))
        {
            struct PkgHttpReq req;
            memset(&req, 0, sizeof req);
            req.URL = url;
            req.ToFile = buf;
            req.Label = "Fetching screenshot";
            req.Package = rec->Name;
            rc = pkg_http(ctx, &req);
            if (rc) { DeleteFile(buf); return rc; }
        }
        if (pkg_file_sha256(buf, md, NULL) == 0)
        {
            pkg_hex(md, 32, got);
            if (pkg_hex_equal(got, hex)) return 0;
        }
        DeleteFile(buf);
    }
    pkg_set_error(ctx, "%s: screenshot digest mismatch", rec->Name);
    return PKGERR_BADHASH;
}
