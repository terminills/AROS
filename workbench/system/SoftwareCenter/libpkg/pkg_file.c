/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: dos.library file helpers and hook plumbing for libpkg.

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: LOW
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/exall.h>

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "pkg_private.h"

/* ------------------------------------------------------------------------ */
/* hooks                                                                     */

static void call_hook(struct PkgContext *ctx, struct PkgHookMsg *msg)
{
    if (ctx && ctx->Hook)
        CallHookPkt(ctx->Hook, ctx, msg);
}

void pkg_log(struct PkgContext *ctx, CONST_STRPTR fmt, ...)
{
    struct PkgHookMsg msg;
    TEXT buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    memset(&msg, 0, sizeof msg);
    msg.Type = PKGH_LOG;
    msg.Text = buf;
    call_hook(ctx, &msg);
}

void pkg_progress(struct PkgContext *ctx, CONST_STRPTR label, CONST_STRPTR package,
                  ULONG done, ULONG total, UQUAD bdone, UQUAD btotal)
{
    struct PkgHookMsg msg;
    memset(&msg, 0, sizeof msg);
    msg.Type = PKGH_PROGRESS;
    msg.Text = label;
    msg.Package = package;
    msg.Done = done;
    msg.Total = total;
    msg.BytesDone = bdone;
    msg.BytesTotal = btotal;
    call_hook(ctx, &msg);
}

BOOL pkg_abort_requested(struct PkgContext *ctx)
{
    struct PkgHookMsg msg;
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        return TRUE;
    if (!ctx || !ctx->Hook)
        return FALSE;
    memset(&msg, 0, sizeof msg);
    msg.Type = PKGH_ABORTQUERY;
    return CallHookPkt(ctx->Hook, ctx, &msg) != 0;
}

void pkg_set_error(struct PkgContext *ctx, CONST_STRPTR fmt, ...)
{
    va_list ap;
    if (!ctx) return;
    va_start(ap, fmt);
    vsnprintf(ctx->ErrText, sizeof ctx->ErrText, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* paths                                                                     */

void pkg_join(STRPTR out, ULONG outlen, CONST_STRPTR dir, CONST_STRPTR file)
{
    strlcpy(out, dir, outlen);
    AddPart(out, (STRPTR)file, outlen);
}

STRPTR pkg_basepath(struct PkgContext *ctx, CONST_STRPTR sub, CONST_STRPTR name,
                    STRPTR out, ULONG outlen)
{
    pkg_join(out, outlen, ctx->BaseDir, sub);
    if (name) AddPart(out, (STRPTR)name, outlen);
    return out;
}

BOOL pkg_file_exists(CONST_STRPTR path)
{
    BPTR lock = Lock(path, SHARED_LOCK);
    if (!lock) return FALSE;
    UnLock(lock);
    return TRUE;
}

UQUAD pkg_file_size(CONST_STRPTR path)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    struct FileInfoBlock *fib;
    UQUAD size = (UQUAD)-1;

    if (!fh) return size;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib)
    {
        if (ExamineFH(fh, fib))
            size = (UQUAD)(ULONG)fib->fib_Size;
        FreeDosObject(DOS_FIB, fib);
    }
    Close(fh);
    return size;
}

/* Create every missing component of a directory path ("SYS:a/b/c"). */
LONG pkg_mkdir_p(CONST_STRPTR path)
{
    TEXT buf[PKG_PATHLEN];
    STRPTR p;
    BPTR lock;

    strlcpy(buf, path, sizeof buf);
    lock = Lock(buf, SHARED_LOCK);
    if (lock) { UnLock(lock); return 0; }

    /* walk forward creating each component after the volume/assign part */
    p = strchr(buf, ':');
    p = p ? p + 1 : buf;
    for (;;)
    {
        STRPTR slash = strchr(p, '/');
        if (slash) *slash = 0;
        if (*p)
        {
            lock = Lock(buf, SHARED_LOCK);
            if (lock) UnLock(lock);
            else
            {
                lock = CreateDir(buf);
                if (!lock) return PKGERR_IO;
                UnLock(lock);
            }
        }
        if (!slash) break;
        *slash = '/';
        p = slash + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* whole-file read/write                                                     */

LONG pkg_file_read(CONST_STRPTR path, UBYTE **buf, ULONG *len, ULONG maxlen)
{
    BPTR fh;
    struct FileInfoBlock *fib;
    ULONG size = 0;
    UBYTE *mem;

    *buf = NULL;
    *len = 0;
    fh = Open(path, MODE_OLDFILE);
    if (!fh) return PKGERR_IO;

    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib) { Close(fh); return PKGERR_NOMEM; }
    if (ExamineFH(fh, fib)) size = (ULONG)fib->fib_Size;
    FreeDosObject(DOS_FIB, fib);

    if (maxlen && size > maxlen) { Close(fh); return PKGERR_FORMAT; }

    mem = malloc(size + 1);
    if (!mem) { Close(fh); return PKGERR_NOMEM; }
    if (size && Read(fh, mem, size) != (LONG)size)
    {
        free(mem);
        Close(fh);
        return PKGERR_IO;
    }
    mem[size] = 0;
    Close(fh);
    *buf = mem;
    *len = size;
    return 0;
}

/* Atomic: write <path>.tmp, then rename over the destination. */
LONG pkg_file_write(CONST_STRPTR path, CONST_APTR buf, ULONG len)
{
    TEXT tmp[PKG_PATHLEN];
    TEXT dir[PKG_PATHLEN];
    BPTR fh;
    LONG rc = 0;

    strlcpy(dir, path, sizeof dir);
    *(PathPart(dir)) = 0;
    if (dir[0] && pkg_mkdir_p(dir) != 0) return PKGERR_IO;

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fh = Open(tmp, MODE_NEWFILE);
    if (!fh) return PKGERR_IO;
    if (len && Write(fh, (APTR)buf, len) != (LONG)len) rc = PKGERR_IO;
    Close(fh);
    if (rc == 0)
    {
        DeleteFile(path);
        if (!Rename(tmp, path)) rc = PKGERR_IO;
    }
    if (rc) DeleteFile(tmp);
    return rc;
}

LONG pkg_file_sha256(CONST_STRPTR path, UBYTE md[32], UQUAD *size)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    pkg_sha256_ctx c;
    UBYTE *buf;
    LONG got;
    UQUAD total = 0;

    if (!fh) return PKGERR_IO;
    buf = malloc(65536);
    if (!buf) { Close(fh); return PKGERR_NOMEM; }
    pkg_sha256_init(&c);
    while ((got = Read(fh, buf, 65536)) > 0)
    {
        pkg_sha256_update(&c, buf, got);
        total += got;
    }
    free(buf);
    Close(fh);
    if (got < 0) return PKGERR_IO;
    pkg_sha256_final(&c, md);
    if (size) *size = total;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* stanza files                                                              */

LONG pkg_read_stanzas(CONST_STRPTR path, struct pkg_stanza **out)
{
    UBYTE *buf;
    ULONG len;
    int n = 0;
    LONG rc;

    *out = NULL;
    if (!pkg_file_exists(path)) return 0;
    rc = pkg_file_read(path, &buf, &len, PKG_MAXMEMFILE);
    if (rc) return rc;
    *out = pkg_stanza_parse((const char *)buf, len, &n);
    free(buf);
    return n < 0 ? PKGERR_NOMEM : 0;
}

LONG pkg_write_stanzas(CONST_STRPTR path, struct pkg_stanza *list)
{
    size_t len = 0;
    char *text = pkg_stanza_write(list, &len);
    LONG rc;
    if (!text) return PKGERR_NOMEM;
    rc = pkg_file_write(path, text, (ULONG)len);
    free(text);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* recursive delete (used for T: staging and cache purge)                    */

LONG pkg_delete_tree(CONST_STRPTR path)
{
    BPTR lock;
    struct ExAllControl *eac;
    struct ExAllData *ead;
    UBYTE *buf;
    BOOL more;
    TEXT sub[PKG_PATHLEN];

    lock = Lock(path, SHARED_LOCK);
    if (!lock) return 0;                    /* nothing to delete */

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
            pkg_join(sub, sizeof sub, path, ead->ed_Name);
            if (ead->ed_Type > 0) pkg_delete_tree(sub);
            else DeleteFile(sub);
        }
    } while (more);
    FreeDosObject(DOS_EXALLCONTROL, eac);
    FreeMem(buf, 4096);
    UnLock(lock);
    DeleteFile(path);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* name lists                                                                */

LONG pkg_namelist_add(struct List *list, CONST_STRPTR name)
{
    struct Node *n = malloc(sizeof *n + strlen(name) + 1);
    if (!n) return PKGERR_NOMEM;
    memset(n, 0, sizeof *n);
    n->ln_Name = (STRPTR)(n + 1);
    strcpy(n->ln_Name, name);
    AddTail(list, n);
    return 0;
}

void pkg_free_namelist(struct List *list)
{
    struct Node *n;
    while ((n = RemHead(list)))
        free(n);
}
