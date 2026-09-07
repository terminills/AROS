/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: C:Pkg - command-line front-end to libpkg.

          Pkg REFRESH [FORCE]
          Pkg CHECK
          Pkg LIST [INSTALLED|UPDATES|AVAILABLE]
          Pkg SEARCH <text>
          Pkg INFO <package>
          Pkg INSTALL <package>...
          Pkg REMOVE <package>...
          Pkg UPGRADE [<package>]
          Pkg ANNOUNCEMENTS [ALL]
          Pkg SOURCE ADD <name> <url> [<keyid>] | REMOVE <name> | LIST
          Pkg TRUST <pubfile> | TRUST LIST
          Pkg PREFS [ARCH=<arch>] [INTERVAL=<min>] [NOTIFY=yes|no]

    AI_PHASE: SOFTWARE_CENTER_CLI
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: LOW
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pkg.h"

const char version[] = "$VER: Pkg 1.0 (05.09.2026)";

#define TEMPLATE "COMMAND/A,ARGS/M,FORCE/S,ALL/S,QUIET/S"
enum { A_COMMAND, A_ARGS, A_FORCE, A_ALL, A_QUIET, A_COUNT };

static BOOL quiet;
static BOOL verbose_progress = TRUE;
static TEXT last_label[64];

static void put(CONST_STRPTR fmt, ...)
{
    va_list ap;
    TEXT buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    FPuts(Output(), buf);
    Flush(Output());
}

AROS_UFH3(static IPTR, hookfn,
          AROS_UFHA(struct Hook *, hook, A0),
          AROS_UFHA(APTR, ctx, A2),
          AROS_UFHA(struct PkgHookMsg *, msg, A1))
{
    AROS_USERFUNC_INIT

    switch (msg->Type)
    {
    case PKGH_LOG:
        if (!quiet) put("%s\n", msg->Text);
        break;
    case PKGH_PROGRESS:
        if (quiet || !verbose_progress) break;
        if (msg->BytesTotal)
            put("\r%s %s: %llu/%llu KB (%lu%%)   ", msg->Text, PKG_STR(msg->Package, ""),
                (unsigned long long)(msg->BytesDone / 1024), (unsigned long long)(msg->BytesTotal / 1024),
                (unsigned long)(msg->BytesDone * 100 / msg->BytesTotal));
        else if (msg->Total)
            put("\r%s %s: %lu/%lu   ", msg->Text, PKG_STR(msg->Package, ""),
                (unsigned long)msg->Done, (unsigned long)msg->Total);
        else if (strcmp(last_label, msg->Text) != 0)
            put("%s %s...\n", msg->Text, PKG_STR(msg->Package, ""));
        strlcpy(last_label, msg->Text, sizeof last_label);
        break;
    case PKGH_ABORTQUERY:
        return 0;
    }
    return 0;

    AROS_USERFUNC_EXIT
}

static void fail(struct PkgContext *ctx, LONG rc)
{
    CONST_STRPTR detail = pkg_last_error_text(ctx);
    put("\n");
    if (rc == PKGERR_HTTP)
        put("Pkg: %s %ld\n", pkg_strerror(rc), (long)pkg_last_http_status(ctx));
    else
        put("Pkg: %s%s%s\n", pkg_strerror(rc), detail && *detail ? " - " : "", PKG_STR(detail, ""));
}

static CONST_STRPTR state_name(UBYTE st)
{
    switch (st)
    {
    case PKGSTATE_INSTALLED:  return "installed";
    case PKGSTATE_UPGRADABLE: return "UPDATE";
    case PKGSTATE_ORPHAN:     return "orphan";
    default:                  return "";
    }
}

static void human(UQUAD bytes, STRPTR out, ULONG len)
{
    if (bytes >= 1024ULL * 1024 * 1024) snprintf(out, len, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024) snprintf(out, len, "%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024) snprintf(out, len, "%llu KB", (unsigned long long)(bytes / 1024));
    else snprintf(out, len, "%llu B", (unsigned long long)bytes);
}

static LONG cmd_list(struct PkgContext *ctx, ULONG filter, CONST_STRPTR search)
{
    struct List recs;
    struct PkgRecord *r;
    ULONG n = 0;
    LONG rc = pkg_list(ctx, filter, &recs);
    if (rc) return rc;
    ForeachNode(&recs, r)
    {
        TEXT sz[24];
        if (search && !strstr(r->Name, search) &&
            !(r->Summary && strstr(r->Summary, search)) &&
            !(r->Description && strstr(r->Description, search)))
            continue;
        human(r->Size, sz, sizeof sz);
        put("%-22s %-12s %-10s %-9s %-8s %s\n", r->Name, r->Version,
            PKG_STR(r->State == PKGSTATE_UPGRADABLE ? r->InstalledVersion : NULL, ""),
            state_name(r->State), sz, PKG_STR(r->Summary, ""));
        n++;
    }
    pkg_free_records(&recs);
    if (n == 0) put(filter == PKGF_UPDATES ? "All packages are up to date.\n" : "No packages (Pkg REFRESH?).\n");
    return 0;
}

static LONG cmd_info(struct PkgContext *ctx, CONST_STRPTR name)
{
    struct PkgRecord *r = pkg_find(ctx, name);
    struct List addons;
    static const char *keys[] = { "Package", "Version", "Arch", "Type", "Extends", "Subdir", "Section",
                                  "Summary", "Maintainer", "Homepage", "Licence", "Depends", "Replaces",
                                  "Size", "InstalledSize", "Filename", "URL", "Screenshot", "Reboot",
                                  "X-Source", "X-InstallRoot", NULL };
    int i;

    if (!r) { put("Pkg: %s: unknown package\n", name); return PKGERR_NOTFOUND; }
    for (i = 0; keys[i]; i++)
    {
        CONST_STRPTR v = pkg_record_get(r, keys[i]);
        if (v) put("%-14s %s\n", keys[i], v);
    }
    put("%-14s %s%s%s\n", "State",
        r->State == PKGSTATE_AVAILABLE ? "available" : (const char *)state_name(r->State),
        r->InstalledVersion ? " " : "", PKG_STR(r->InstalledVersion, ""));
    if (r->Description) put("\n%s\n", r->Description);
    if (r->Changes) put("\nChanges:\n%s\n", r->Changes);
    if (pkg_list_addons(ctx, r->Name, &addons) == 0)
    {
        struct PkgRecord *a;
        BOOL first = TRUE;
        ForeachNode(&addons, a)
        {
            if (first) { put("\nAdd-ons:\n"); first = FALSE; }
            put("  %-22s %-12s %s %s\n", a->Name, a->Version, state_name(a->State), PKG_STR(a->Summary, ""));
        }
        pkg_free_records(&addons);
    }
    pkg_free_record(r);
    return 0;
}

static LONG cmd_announcements(struct PkgContext *ctx, BOOL all)
{
    struct List anns;
    struct PkgAnnouncement *a;
    ULONG n = 0, maxid = 0;
    LONG rc = pkg_announcements(ctx, !all, &anns);
    if (rc) return rc;
    ForeachNode(&anns, a)
    {
        put("[%lu] %s  %s  %s%s\n  %s\n", (unsigned long)a->Id, PKG_STR(a->Date, ""), PKG_STR(a->Severity, ""),
            PKG_STR(a->Title, ""), a->Seen ? "" : "  (new)", PKG_STR(a->Body, ""));
        if (a->Package) put("  Package: %s\n", a->Package);
        if (a->URL) put("  %s\n", a->URL);
        put("\n");
        if (a->Id > maxid) maxid = a->Id;
        n++;
    }
    pkg_free_announcements(&anns);
    if (n == 0) put("No %sannouncements.\n", all ? "" : "new ");
    else if (!all) pkg_announcement_seen(ctx, maxid);
    return 0;
}

static LONG cmd_source(struct PkgContext *ctx, STRPTR *args)
{
    if (!args || !args[0] || stricmp(args[0], "LIST") == 0)
    {
        struct PkgSource *s;
        ForeachNode(pkg_sources(ctx), s)
            put("%-16s %s %s%s serial %lu\n", s->Node.ln_Name, s->URL, PKG_STR(s->KeyId[0] ? s->KeyId : NULL, "(any trusted key)"),
                s->Enabled ? "" : " [disabled]", (unsigned long)s->Serial);
        if (IsListEmpty(pkg_sources(ctx))) put("No sources.  Pkg SOURCE ADD <name> <http://url/ | file://path/> [<keyid>]\n");
        return 0;
    }
    if (stricmp(args[0], "ADD") == 0 && args[1] && args[2])
        return pkg_source_add(ctx, args[1], args[2], args[3]);
    if (stricmp(args[0], "REMOVE") == 0 && args[1])
        return pkg_source_remove(ctx, args[1]);
    put("Pkg SOURCE ADD <name> <url> [<keyid>] | REMOVE <name> | LIST\n");
    return PKGERR_BADPREFS;
}

static LONG cmd_trust(struct PkgContext *ctx, STRPTR *args)
{
    if (!args || !args[0] || stricmp(args[0], "LIST") == 0)
    {
        struct List keys;
        struct Node *n;
        LONG rc = pkg_trust_list(ctx, &keys);
        if (rc) return rc;
        ForeachNode(&keys, n) put("%s\n", n->ln_Name);
        if (IsListEmpty(&keys)) put("No trusted keys.  Pkg TRUST <file.pub>\n");
        pkg_free_namelist(&keys);
        return 0;
    }
    return pkg_trust_add(ctx, args[0]);
}

static LONG cmd_prefs(struct PkgContext *ctx, STRPTR *args)
{
    struct PkgPrefs *p = pkg_prefs(ctx);
    BOOL changed = FALSE;
    int i;
    for (i = 0; args && args[i]; i++)
    {
        if (strnicmp(args[i], "ARCH=", 5) == 0) { strlcpy(p->Arch, args[i] + 5, sizeof p->Arch); changed = TRUE; }
        else if (strnicmp(args[i], "INTERVAL=", 9) == 0) { p->CheckInterval = atoi(args[i] + 9); changed = TRUE; }
        else if (strnicmp(args[i], "NOTIFY=", 7) == 0) { p->Notify = stricmp(args[i] + 7, "yes") == 0; changed = TRUE; }
        else { put("Pkg PREFS [ARCH=<arch>] [INTERVAL=<minutes>] [NOTIFY=yes|no]\n"); return PKGERR_BADPREFS; }
    }
    if (changed) return pkg_prefs_save(ctx);
    put("Arch:               %s\nCheckInterval:      %lu min\nNotify:             %s\nLastSerial:         %lu\nLastAnnouncementId: %lu\n%s\n",
        p->Arch, (unsigned long)p->CheckInterval, p->Notify ? "yes" : "no",
        (unsigned long)p->LastSerial, (unsigned long)p->LastAnnouncementId, pkg_version_string());
    return 0;
}

int main(void)
{
    struct RDArgs *rda;
    IPTR args[A_COUNT] = { 0 };
    struct Hook hook;
    struct PkgContext *ctx;
    CONST_STRPTR cmd;
    STRPTR *rest;
    LONG rc = 0;
    int i;

    rda = ReadArgs(TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "Pkg");
        put("Usage: Pkg REFRESH|CHECK|LIST|SEARCH|INFO|INSTALL|REMOVE|UPGRADE|ANNOUNCEMENTS|SOURCE|TRUST|PREFS [args] [FORCE] [ALL] [QUIET]\n");
        return RETURN_ERROR;
    }
    cmd = (CONST_STRPTR)args[A_COMMAND];
    rest = (STRPTR *)args[A_ARGS];
    quiet = args[A_QUIET] != 0;

    memset(&hook, 0, sizeof hook);
    hook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(hookfn);

    ctx = pkg_open(&hook);
    if (!ctx) { put("Pkg: out of memory\n"); FreeArgs(rda); return RETURN_FAIL; }

    if (stricmp(cmd, "REFRESH") == 0)
        rc = pkg_refresh(ctx, args[A_FORCE] != 0);
    else if (stricmp(cmd, "CHECK") == 0)
    {
        ULONG upd = 0, ann = 0;
        rc = pkg_check(ctx, &upd, &ann);
        if (!rc) put("%lu update(s) available, %lu new announcement(s)\n", (unsigned long)upd, (unsigned long)ann);
    }
    else if (stricmp(cmd, "LIST") == 0)
    {
        ULONG f = PKGF_ALL;
        if (rest && rest[0])
        {
            if (stricmp(rest[0], "INSTALLED") == 0) f = PKGF_INSTALLED;
            else if (stricmp(rest[0], "UPDATES") == 0) f = PKGF_UPDATES;
            else if (stricmp(rest[0], "AVAILABLE") == 0) f = PKGF_AVAILABLE;
        }
        rc = cmd_list(ctx, f, NULL);
    }
    else if (stricmp(cmd, "SEARCH") == 0 && rest && rest[0])
        rc = cmd_list(ctx, PKGF_ALL, rest[0]);
    else if (stricmp(cmd, "INFO") == 0 && rest && rest[0])
        rc = cmd_info(ctx, rest[0]);
    else if (stricmp(cmd, "INSTALL") == 0 && rest && rest[0])
    {
        for (i = 0; rest[i] && !rc; i++) rc = pkg_install(ctx, rest[i]);
    }
    else if (stricmp(cmd, "REMOVE") == 0 && rest && rest[0])
    {
        for (i = 0; rest[i] && !rc; i++) rc = pkg_remove(ctx, rest[i]);
    }
    else if (stricmp(cmd, "UPGRADE") == 0)
        rc = pkg_upgrade(ctx, rest && rest[0] ? rest[0] : NULL);
    else if (stricmp(cmd, "ANNOUNCEMENTS") == 0)
        rc = cmd_announcements(ctx, args[A_ALL] != 0);
    else if (stricmp(cmd, "SOURCE") == 0)
        rc = cmd_source(ctx, rest);
    else if (stricmp(cmd, "TRUST") == 0)
        rc = cmd_trust(ctx, rest);
    else if (stricmp(cmd, "PREFS") == 0)
        rc = cmd_prefs(ctx, rest);
    else
    {
        put("Pkg: unknown command %s\n", cmd);
        rc = PKGERR_BADPREFS;
    }

    if (rc) fail(ctx, rc);
    pkg_close(ctx);
    FreeArgs(rda);
    return rc ? (rc == PKGERR_ABORTED ? RETURN_WARN : RETURN_ERROR) : RETURN_OK;
}
