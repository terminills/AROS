/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Streaming gzip + POSIX ustar reader (PROTOCOL.md §4).  Compressed
          bytes come from a caller callback, inflate via zlib (z.static on
          AROS, system zlib on the host), so an archive of any size is
          walked with a fixed ~40 KB of buffers.

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: MEDIUM
    AI_DEPENDENCIES: zlib (inflateInit2 with gzip wrapper)
*/

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "pkg_core.h"

#define INBUF   16384
#define BLOCK   512

struct pkg_tar
{
    pkg_tar_readfn read;
    void          *ctx;
    z_stream       z;
    unsigned char  in[INBUF];
    int            in_eof;
    int            z_end;
    uint64_t       remaining;      /* payload bytes left in current entry  */
    uint64_t       padding;        /* bytes to the next 512 boundary       */
    const char    *err;
    int            open;
};

static int fill(struct pkg_tar *t)
{
    long n;
    if (t->in_eof) return 0;
    n = t->read(t->ctx, t->in, INBUF);
    if (n < 0) { t->err = "read error"; return PKG_TARERR_IO; }
    if (n == 0) { t->in_eof = 1; return 0; }
    t->z.next_in = t->in;
    t->z.avail_in = (uInt)n;
    return 1;
}

/* Inflate exactly n bytes into buf (or fewer only at end of stream). */
static long inflate_exact(struct pkg_tar *t, void *buf, size_t n)
{
    size_t got = 0;
    t->z.next_out = buf;
    t->z.avail_out = (uInt)n;
    while (t->z.avail_out)
    {
        int rc;
        if (t->z_end) break;
        if (t->z.avail_in == 0)
        {
            int f = fill(t);
            if (f < 0) return f;
            if (f == 0)
            {
                t->err = "truncated gzip stream";
                return PKG_TARERR_GZIP;
            }
        }
        rc = inflate(&t->z, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) { t->z_end = 1; break; }
        if (rc != Z_OK && rc != Z_BUF_ERROR)
        {
            t->err = t->z.msg ? t->z.msg : "inflate error";
            return PKG_TARERR_GZIP;
        }
    }
    got = n - t->z.avail_out;
    return (long)got;
}

struct pkg_tar *pkg_tar_open(pkg_tar_readfn read, void *ctx)
{
    struct pkg_tar *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->read = read;
    t->ctx = ctx;
    /* 15 + 16: zlib auto-detects the gzip wrapper */
    if (inflateInit2(&t->z, 15 + 16) != Z_OK)
    {
        free(t);
        return NULL;
    }
    t->open = 1;
    return t;
}

void pkg_tar_close(struct pkg_tar *t)
{
    if (!t) return;
    if (t->open) inflateEnd(&t->z);
    free(t);
}

const char *pkg_tar_error(struct pkg_tar *t)
{
    return t && t->err ? t->err : "";
}

static uint64_t octal(const char *p, size_t n)
{
    uint64_t v = 0;
    size_t i;
    /* GNU base-256 (high bit set) is not permitted by PROTOCOL.md §4 */
    for (i = 0; i < n; i++)
    {
        char c = p[i];
        if (c == 0 || c == ' ') { if (v) break; else continue; }
        if (c < '0' || c > '7') return (uint64_t)-1;
        v = (v << 3) | (uint64_t)(c - '0');
    }
    return v;
}

static int drain(struct pkg_tar *t, uint64_t n)
{
    unsigned char sink[BLOCK * 4];
    while (n)
    {
        size_t want = n > sizeof sink ? sizeof sink : (size_t)n;
        long got = inflate_exact(t, sink, want);
        if (got < 0) return (int)got;
        if ((size_t)got != want) { t->err = "truncated archive"; return PKG_TARERR_FORMAT; }
        n -= want;
    }
    return 0;
}

int pkg_tar_skip(struct pkg_tar *t)
{
    int rc = drain(t, t->remaining + t->padding);
    t->remaining = 0;
    t->padding = 0;
    return rc;
}

int pkg_tar_next(struct pkg_tar *t, struct pkg_tar_entry *e)
{
    unsigned char h[BLOCK];
    long got;
    int rc, i;
    unsigned sum = 0, stored;
    uint64_t v;

    if ((rc = pkg_tar_skip(t)) < 0) return rc;

    for (;;)
    {
        got = inflate_exact(t, h, BLOCK);
        if (got < 0) return (int)got;
        if (got == 0) return 0;                 /* clean EOF, no zero blocks */
        if (got != BLOCK) { t->err = "short header"; return PKG_TARERR_FORMAT; }

        /* all-zero block = end marker */
        for (i = 0; i < BLOCK && h[i] == 0; i++) ;
        if (i == BLOCK) return 0;
        break;
    }

    /* checksum: bytes 148..155 treated as spaces */
    for (i = 0; i < BLOCK; i++)
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    v = octal((const char *)h + 148, 8);
    stored = (unsigned)v;
    if (v == (uint64_t)-1 || stored != sum) { t->err = "bad header checksum"; return PKG_TARERR_FORMAT; }

    if (memcmp(h + 257, "ustar", 5) != 0)      /* "ustar\0" or "ustar " */
    {
        t->err = "not a ustar archive";
        return PKG_TARERR_FORMAT;
    }

    memset(e, 0, sizeof *e);
    e->type = h[156] == 0 ? PKG_TAR_REG : (char)h[156];
    if (e->type == '7') e->type = PKG_TAR_REG;  /* contiguous == regular   */
    if (e->type != PKG_TAR_REG && e->type != PKG_TAR_DIR)
    {
        t->err = "unsupported entry type (link/device/pax/gnu)";
        /* still position the stream past its data for callers that skip */
        v = octal((const char *)h + 124, 12);
        t->remaining = (v == (uint64_t)-1) ? 0 : v;
        t->padding = (BLOCK - (t->remaining % BLOCK)) % BLOCK;
        return PKG_TARERR_UNSUPPORTED;
    }

    v = octal((const char *)h + 100, 8);
    e->mode = (v == (uint64_t)-1) ? 0644 : (uint32_t)v;
    v = octal((const char *)h + 124, 12);
    if (v == (uint64_t)-1) { t->err = "bad size field"; return PKG_TARERR_FORMAT; }
    e->size = v;

    /* name = prefix + '/' + name */
    {
        size_t pl = 0, nl = 0, o = 0;
        while (pl < 155 && h[345 + pl]) pl++;
        while (nl < 100 && h[nl]) nl++;
        if (pl)
        {
            memcpy(e->name, h + 345, pl); o = pl;
            e->name[o++] = '/';
        }
        memcpy(e->name + o, h, nl); o += nl;
        e->name[o] = 0;
        /* normalise: strip "./", trailing '/' */
        if (e->name[0] == '.' && e->name[1] == '/')
            memmove(e->name, e->name + 2, strlen(e->name + 2) + 1);
        o = strlen(e->name);
        while (o && e->name[o - 1] == '/') e->name[--o] = 0;
        /* reject path escapes */
        if (e->name[0] == '/' || strstr(e->name, "..") || strchr(e->name, ':'))
        {
            t->err = "unsafe path in archive";
            return PKG_TARERR_FORMAT;
        }
    }

    t->remaining = (e->type == PKG_TAR_DIR) ? 0 : e->size;
    t->padding = (BLOCK - (t->remaining % BLOCK)) % BLOCK;
    return 1;
}

long pkg_tar_read(struct pkg_tar *t, void *buf, size_t n)
{
    long got;
    if (t->remaining == 0) return 0;
    if (n > t->remaining) n = (size_t)t->remaining;
    got = inflate_exact(t, buf, n);
    if (got < 0) return got;
    if ((size_t)got != n) { t->err = "truncated archive"; return PKG_TARERR_FORMAT; }
    t->remaining -= n;
    return (long)n;
}
