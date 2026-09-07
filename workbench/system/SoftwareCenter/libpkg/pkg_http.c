/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Minimal HTTP/1.1 GET (and POST, for crash reports: PROTOCOL.md
          §9) client over bsdsocket.library for libpkg
          (PROTOCOL.md §1.2, §6).  Supports Host, Content-Length, chunked
          transfer, ETag/If-None-Match (304), Range resume (206), to-memory
          and to-file bodies, progress reporting and abort (CTRL-C or the
          PKGH_ABORTQUERY hook) between socket reads.

          bsdsocket.library bases are per-task, so the library is opened
          and closed around every request: libpkg contexts can then be
          shared between a GUI task and a worker without any affinity.

          file:// URLs (PROTOCOL.md §1) name a repository mirror on local
          or removable media (CD, USB stick, network share): the same
          layout is read straight from the file system, so a signed repo
          can be carried to a machine with no network at all.  The path
          after file:// is an AROS path ("file://PKGTEST:repo/").

    AI_PHASE: SOFTWARE_CENTER_LIBPKG
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: MEDIUM
    AI_DEPENDENCIES: bsdsocket.library (AROSTCP)
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "pkg_private.h"

#define HTTP_TIMEOUT_S   30
#define HTTP_BUF         16384

struct http_conn
{
    struct Library *SocketBase;
    int             s;
    UBYTE          *buf;            /* read-ahead buffer                    */
    ULONG           head, tail;     /* buf[head..tail) unread               */
    BOOL            eof;
};

/* ------------------------------------------------------------------------ */

static LONG parse_url(CONST_STRPTR url, STRPTR host, ULONG hostlen, UWORD *port,
                      STRPTR path, ULONG pathlen)
{
    CONST_STRPTR p, slash, colon;
    ULONG n;

    if (strncmp(url, "http://", 7) != 0) return PKGERR_BADURL;
    p = url + 7;
    slash = strchr(p, '/');
    n = slash ? (ULONG)(slash - p) : strlen(p);
    if (n == 0 || n >= hostlen) return PKGERR_BADURL;
    memcpy(host, p, n);
    host[n] = 0;
    *port = 80;
    colon = strchr(host, ':');
    if (colon)
    {
        *port = (UWORD)atoi(colon + 1);
        *(STRPTR)colon = 0;
        if (*port == 0) return PKGERR_BADURL;
    }
    strlcpy(path, slash ? slash : (CONST_STRPTR)"/", pathlen);
    return 0;
}

/* Wait until the socket is readable, honouring CTRL-C and the abort hook.
   Returns 0 ready, PKGERR_ABORTED, or PKGERR_NETWORK on timeout/error. */
static LONG wait_readable(struct PkgContext *ctx, struct http_conn *c)
{
    struct Library *SocketBase = c->SocketBase;
    fd_set rd;
    struct timeval tv;
    ULONG sigmask;
    int n, secs;

    /* one-second slices so the abort hook is polled while idle */
    for (secs = 0; secs < HTTP_TIMEOUT_S; secs++)
    {
        FD_ZERO(&rd);
        FD_SET(c->s, &rd);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        sigmask = SIGBREAKF_CTRL_C;
        n = WaitSelect(c->s + 1, &rd, NULL, NULL, &tv, &sigmask);
        if (sigmask & SIGBREAKF_CTRL_C) return PKGERR_ABORTED;
        if (n < 0) return PKGERR_NETWORK;
        if (n > 0) return 0;
        if (pkg_abort_requested(ctx)) return PKGERR_ABORTED;
    }
    pkg_set_error(ctx, "timeout waiting for data");
    return PKGERR_NETWORK;
}

/* Refill the read-ahead buffer.  0 ok (may set eof), <0 error. */
static LONG fill(struct PkgContext *ctx, struct http_conn *c)
{
    struct Library *SocketBase = c->SocketBase;
    LONG rc, n;

    if (c->eof) return 0;
    if (c->head == c->tail) c->head = c->tail = 0;
    if (c->tail == HTTP_BUF) return PKGERR_FORMAT;        /* header line too long */
    rc = wait_readable(ctx, c);
    if (rc) return rc;
    n = recv(c->s, c->buf + c->tail, HTTP_BUF - c->tail, 0);
    if (n < 0) return PKGERR_NETWORK;
    if (n == 0) c->eof = TRUE;
    c->tail += n;
    return 0;
}

/* Read one CRLF-terminated line (CR/LF stripped) into out. */
static LONG read_line(struct PkgContext *ctx, struct http_conn *c, STRPTR out, ULONG outlen)
{
    for (;;)
    {
        ULONG i;
        for (i = c->head; i < c->tail; i++)
        {
            if (c->buf[i] == '\n')
            {
                ULONG n = i - c->head;
                if (n && c->buf[i - 1] == '\r') n--;
                if (n >= outlen) return PKGERR_FORMAT;
                memcpy(out, c->buf + c->head, n);
                out[n] = 0;
                c->head = i + 1;
                return 0;
            }
        }
        if (c->eof) return PKGERR_NETWORK;
        {
            LONG rc = fill(ctx, c);
            if (rc) return rc;
        }
    }
}

/* Read up to n body bytes; returns count (0 = EOF) or <0. */
static LONG read_body(struct PkgContext *ctx, struct http_conn *c, UBYTE *out, ULONG n)
{
    ULONG avail;
    if (c->head == c->tail)
    {
        LONG rc = fill(ctx, c);
        if (rc) return rc;
        if (c->head == c->tail) return 0;
    }
    avail = c->tail - c->head;
    if (avail > n) avail = n;
    memcpy(out, c->buf + c->head, avail);
    c->head += avail;
    return (LONG)avail;
}

/* ------------------------------------------------------------------------ */
/* body sink: memory or file                                                 */

struct sink
{
    UBYTE *mem;
    ULONG  memlen, memcap;
    BPTR   fh;
    UQUAD  written;
};

static LONG sink_put(struct sink *k, const UBYTE *p, ULONG n)
{
    if (k->fh)
    {
        if (Write(k->fh, (APTR)p, n) != (LONG)n) return PKGERR_IO;
    }
    else
    {
        if (k->memlen + n + 1 > k->memcap)
        {
            ULONG cap = k->memcap ? k->memcap * 2 : 65536;
            UBYTE *nm;
            while (cap < k->memlen + n + 1) cap *= 2;
            if (cap > PKG_MAXMEMFILE + 1) return PKGERR_FORMAT;
            nm = realloc(k->mem, cap);
            if (!nm) return PKGERR_NOMEM;
            k->mem = nm;
            k->memcap = cap;
        }
        memcpy(k->mem + k->memlen, p, n);
        k->memlen += n;
        k->mem[k->memlen] = 0;
    }
    k->written += n;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* file:// transport: same request/result contract, no socket              */

static LONG file_get(struct PkgContext *ctx, struct PkgHttpReq *req, struct sink *k, UBYTE *body)
{
    TEXT path[PKG_PATHLEN];
    BPTR fh;
    UQUAD size, got = 0;
    LONG rc = 0, n;

    strlcpy(path, req->URL + 7, sizeof path);
    size = pkg_file_size(path);
    if (size == (UQUAD)-1)
    {
        req->Status = 404;
        ctx->HttpStatus = 404;
        pkg_set_error(ctx, "not found: %s", path);
        return PKGERR_NOTFOUND;
    }
    /* size+date as a weak validator so unchanged mirrors report 304 */
    {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        BPTR lock = Lock(path, SHARED_LOCK);
        if (fib && lock && Examine(lock, fib))
            snprintf(req->ETag, sizeof req->ETag, "\"%llu-%lu-%lu\"", (unsigned long long)size,
                     (unsigned long)fib->fib_Date.ds_Days, (unsigned long)fib->fib_Date.ds_Minute);
        if (lock) UnLock(lock);
        if (fib) FreeDosObject(DOS_FIB, fib);
    }
    if (req->IfNoneMatch && req->IfNoneMatch[0] && req->ETag[0] && strcmp(req->IfNoneMatch, req->ETag) == 0)
    {
        req->Status = 304;
        ctx->HttpStatus = 304;
        return 0;
    }
    fh = Open(path, MODE_OLDFILE);
    if (!fh) { pkg_set_error(ctx, "cannot open %s", path); return PKGERR_IO; }
    req->Status = 200;
    ctx->HttpStatus = 200;
    req->ContentLength = size;
    if (req->Label)
        pkg_progress(ctx, req->Label, req->Package, 0, 0, 0, size);
    while ((n = Read(fh, body, HTTP_BUF)) > 0)
    {
        rc = sink_put(k, body, n);
        if (rc) break;
        got += n;
        if (req->Label)
            pkg_progress(ctx, req->Label, req->Package, 0, 0, got, size);
        if (pkg_abort_requested(ctx)) { rc = PKGERR_ABORTED; break; }
    }
    if (n < 0 && rc == 0) { pkg_set_error(ctx, "read error on %s", path); rc = PKGERR_IO; }
    Close(fh);
    return rc;
}

/* ------------------------------------------------------------------------ */

static void header_value(STRPTR line, CONST_STRPTR name, STRPTR out, ULONG outlen)
{
    ULONG nl = strlen(name);
    if (strncasecmp(line, name, nl) == 0 && line[nl] == ':')
    {
        STRPTR v = line + nl + 1;
        while (*v == ' ' || *v == '\t') v++;
        strlcpy(out, v, outlen);
    }
}

LONG pkg_http(struct PkgContext *ctx, struct PkgHttpReq *req)
{
    struct Library *SocketBase;
    struct http_conn c;
    struct sink k;
    struct sockaddr_in sa;
    struct hostent *he;
    TEXT host[128], path[512], line[1024], tmp[128];
    TEXT reqbuf[1200];
    UWORD port;
    LONG rc;
    UQUAD resume_from = 0, total_hint = 0, got = 0;
    BOOL chunked = FALSE, have_length = FALSE;
    UQUAD content_length = 0;
    UBYTE *body = NULL;

    req->Status = 0;
    req->ETag[0] = 0;
    req->ContentLength = 0;
    req->Mem = NULL;
    req->MemLen = 0;
    req->Written = 0;
    ctx->HttpStatus = 0;

    memset(&c, 0, sizeof c);
    memset(&k, 0, sizeof k);
    c.s = -1;

    if (pkg_url_is_file(req->URL))
    {
        /* local mirror: no socket, no resume (the "download" is a copy) */
        SocketBase = NULL;
        req->Resume = FALSE;
    }
    else
    {
        rc = parse_url(req->URL, host, sizeof host, &port, path, sizeof path);
        if (rc) { pkg_set_error(ctx, "bad URL: %s", req->URL); return rc; }

        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (!SocketBase)
        {
            pkg_set_error(ctx, "bsdsocket.library not available (is the network stack running?)");
            return PKGERR_NETWORK;
        }
        c.SocketBase = SocketBase;
    }

    c.buf = malloc(HTTP_BUF);
    body = malloc(HTTP_BUF);
    if (!c.buf || !body) { rc = PKGERR_NOMEM; goto out; }

    /* resume: append to an existing partial file */
    if (req->ToFile)
    {
        if (req->Resume)
        {
            UQUAD have = pkg_file_size(req->ToFile);
            if (have != (UQUAD)-1 && have > 0) resume_from = have;
        }
        if (resume_from)
        {
            k.fh = Open(req->ToFile, MODE_READWRITE);
            if (k.fh) Seek(k.fh, 0, OFFSET_END);
        }
        else
        {
            TEXT dir[PKG_PATHLEN];
            strlcpy(dir, req->ToFile, sizeof dir);
            *(PathPart(dir)) = 0;
            if (dir[0]) pkg_mkdir_p(dir);
            k.fh = Open(req->ToFile, MODE_NEWFILE);
        }
        if (!k.fh) { pkg_set_error(ctx, "cannot open %s", req->ToFile); rc = PKGERR_IO; goto out; }
    }

    if (!SocketBase)
    {
        if (req->PostBody)
        {
            /* POST to a mirror = drop the body into the named file (a crash
               report "server" on a USB stick or a shared drive) */
            TEXT path[PKG_PATHLEN], dir[PKG_PATHLEN];
            strlcpy(path, req->URL + 7, sizeof path);
            strlcpy(dir, path, sizeof dir);
            *(PathPart(dir)) = 0;
            if (dir[0]) pkg_mkdir_p(dir);
            rc = pkg_file_write(path, req->PostBody, req->PostLen);
            if (rc) pkg_set_error(ctx, "cannot write %s", path);
            req->Status = rc ? 0 : 201;
            ctx->HttpStatus = req->Status;
            goto out;
        }
        rc = file_get(ctx, req, &k, body);
        goto out;
    }

    /* resolve + connect */
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (sa.sin_addr.s_addr == INADDR_NONE)
    {
        he = gethostbyname(host);
        if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0])
        {
            pkg_set_error(ctx, "cannot resolve %s", host);
            rc = PKGERR_NETWORK;
            goto out;
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
    }
    c.s = socket(AF_INET, SOCK_STREAM, 0);
    if (c.s < 0) { pkg_set_error(ctx, "socket() failed"); rc = PKGERR_NETWORK; goto out; }
    if (connect(c.s, (struct sockaddr *)&sa, sizeof sa) < 0)
    {
        pkg_set_error(ctx, "connect to %s:%u failed", host, (unsigned)port);
        rc = PKGERR_NETWORK;
        goto out;
    }

    /* request */
    snprintf(reqbuf, sizeof reqbuf,
             "%s %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: " PKG_USERAGENT " (%s; AROS)\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n",
             req->PostBody ? "POST" : "GET", path, host,
             ctx->Prefs.Arch[0] ? (char *)ctx->Prefs.Arch : "unknown");
    if (req->IfNoneMatch && req->IfNoneMatch[0])
    {
        snprintf(tmp, sizeof tmp, "If-None-Match: %s\r\n", req->IfNoneMatch);
        strlcat(reqbuf, tmp, sizeof reqbuf);
    }
    if (resume_from)
    {
        snprintf(tmp, sizeof tmp, "Range: bytes=%llu-\r\n", (unsigned long long)resume_from);
        strlcat(reqbuf, tmp, sizeof reqbuf);
    }
    if (req->PostBody)
    {
        snprintf(tmp, sizeof tmp, "Content-Type: %s\r\nContent-Length: %lu\r\n",
                 req->ContentType ? (char *)req->ContentType : "text/plain",
                 (unsigned long)req->PostLen);
        strlcat(reqbuf, tmp, sizeof reqbuf);
    }
    strlcat(reqbuf, "\r\n", sizeof reqbuf);
    {
        ULONG off = 0, len = strlen(reqbuf);
        while (off < len)
        {
            LONG n = send(c.s, reqbuf + off, len - off, 0);
            if (n <= 0) { pkg_set_error(ctx, "send failed"); rc = PKGERR_NETWORK; goto out; }
            off += n;
        }
        off = 0;
        while (req->PostBody && off < req->PostLen)
        {
            LONG n = send(c.s, (APTR)((CONST UBYTE *)req->PostBody + off), req->PostLen - off, 0);
            if (n <= 0) { pkg_set_error(ctx, "send failed"); rc = PKGERR_NETWORK; goto out; }
            off += n;
        }
    }

    /* status line */
    rc = read_line(ctx, &c, line, sizeof line);
    if (rc) { if (rc == PKGERR_NETWORK) pkg_set_error(ctx, "no response from %s", host); goto out; }
    if (strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12)
    {
        pkg_set_error(ctx, "malformed response from %s", host);
        rc = PKGERR_FORMAT;
        goto out;
    }
    req->Status = atoi(line + 9);
    ctx->HttpStatus = req->Status;

    /* headers */
    for (;;)
    {
        rc = read_line(ctx, &c, line, sizeof line);
        if (rc) goto out;
        if (line[0] == 0) break;
        tmp[0] = 0;
        header_value(line, "Content-Length", tmp, sizeof tmp);
        if (tmp[0]) { content_length = pkg_parse_u64(tmp); have_length = TRUE; }
        tmp[0] = 0;
        header_value(line, "Transfer-Encoding", tmp, sizeof tmp);
        if (tmp[0] && strncasecmp(tmp, "chunked", 7) == 0) chunked = TRUE;
        header_value(line, "ETag", req->ETag, sizeof req->ETag);
    }

    if (req->Status == 304)
    {
        rc = 0;
        goto out;
    }
    if (req->Status == 206 && !resume_from)
    {
        pkg_set_error(ctx, "unexpected 206 from %s", host);
        rc = PKGERR_FORMAT;
        goto out;
    }
    if (resume_from && req->Status == 200)
    {
        /* server ignored Range: start over */
        Close(k.fh);
        k.fh = Open(req->ToFile, MODE_NEWFILE);
        if (!k.fh) { rc = PKGERR_IO; goto out; }
        resume_from = 0;
    }
    if (req->Status == 416 && resume_from)
    {
        /* already complete */
        rc = 0;
        goto out;
    }
    if (req->Status != 200 && req->Status != 206 &&
        !(req->PostBody && (req->Status == 201 || req->Status == 202)))
    {
        pkg_set_error(ctx, "HTTP %ld for %s", (long)req->Status, req->URL);
        rc = req->Status == 404 ? PKGERR_NOTFOUND : PKGERR_NETWORK;
        goto out;
    }

    total_hint = have_length ? content_length + resume_from : 0;
    req->ContentLength = total_hint;
    if (req->Label)
        pkg_progress(ctx, req->Label, req->Package, 0, 0, resume_from, total_hint);

    /* body */
    if (chunked)
    {
        for (;;)
        {
            UQUAD chunk, left;
            rc = read_line(ctx, &c, line, sizeof line);
            if (rc) goto out;
            chunk = strtoull(line, NULL, 16);
            if (chunk == 0) break;
            left = chunk;
            while (left)
            {
                LONG n = read_body(ctx, &c, body, left > HTTP_BUF ? HTTP_BUF : (ULONG)left);
                if (n < 0) { rc = n; goto out; }
                if (n == 0) { rc = PKGERR_NETWORK; pkg_set_error(ctx, "truncated chunked body"); goto out; }
                rc = sink_put(&k, body, n);
                if (rc) goto out;
                left -= n;
                got += n;
            }
            rc = read_line(ctx, &c, line, sizeof line);     /* chunk CRLF */
            if (rc) goto out;
            if (req->Label && (got & 0xffff) == 0)
                pkg_progress(ctx, req->Label, req->Package, 0, 0, got, 0);
        }
    }
    else
    {
        for (;;)
        {
            ULONG want = HTTP_BUF;
            LONG n;
            if (have_length)
            {
                if (got >= content_length) break;
                if (content_length - got < want) want = (ULONG)(content_length - got);
            }
            n = read_body(ctx, &c, body, want);
            if (n < 0) { rc = n; goto out; }
            if (n == 0)
            {
                if (have_length) { rc = PKGERR_NETWORK; pkg_set_error(ctx, "connection closed early (%llu/%llu)", (unsigned long long)got, (unsigned long long)content_length); goto out; }
                break;
            }
            rc = sink_put(&k, body, n);
            if (rc) goto out;
            got += n;
            if (req->Label)
                pkg_progress(ctx, req->Label, req->Package, 0, 0, resume_from + got, total_hint);
        }
    }
    rc = 0;

out:
    if (k.fh) Close(k.fh);
    if (c.s >= 0) CloseSocket(c.s);
    free(c.buf);
    free(body);
    if (rc == 0)
    {
        if (!req->ToFile)
        {
            if (!k.mem) { k.mem = malloc(1); if (k.mem) k.mem[0] = 0; }
            req->Mem = k.mem;
            req->MemLen = k.memlen;
        }
        req->Written = k.written;
    }
    else
    {
        free(k.mem);
        if (rc == PKGERR_NOMEM) pkg_set_error(ctx, "out of memory");
    }
    if (SocketBase) CloseLibrary(SocketBase);
    return rc;
}

BOOL pkg_url_is_file(CONST_STRPTR url)
{
    return url && strncmp(url, "file://", 7) == 0 && url[7] != 0;
}

/* Public POST for tools outside libpkg (CrashReporter).  The reply body,
   if any, is returned NUL-terminated in *reply (FreeVec-free with
   pkg_free_reply()); status is the HTTP status (201 for a file:// drop). */
LONG pkg_http_post(struct PkgContext *ctx, CONST_STRPTR url, CONST_STRPTR ctype,
                   CONST_APTR body, ULONG len, STRPTR *reply, LONG *status)
{
    struct PkgHttpReq req;
    LONG rc;

    memset(&req, 0, sizeof req);
    req.URL         = url;
    req.PostBody    = body;
    req.PostLen     = len;
    req.ContentType = ctype;
    rc = pkg_http(ctx, &req);
    if (status) *status = req.Status;
    if (reply)
    {
        *reply = NULL;
        if (rc == 0 && req.Mem)
        {
            *reply = AllocVec(req.MemLen + 1, MEMF_ANY);
            if (*reply) memcpy(*reply, req.Mem, req.MemLen + 1);
        }
    }
    pkg_http_free(&req);
    return rc;
}

void pkg_free_reply(STRPTR reply)
{
    if (reply) FreeVec(reply);
}

void pkg_http_free(struct PkgHttpReq *req)
{
    free(req->Mem);
    req->Mem = NULL;
    req->MemLen = 0;
}
