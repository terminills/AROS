/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Measure TCP receive throughput without a browser.

    fast.com under Chromium never exceeds about 1 Mbps on native hardware, and
    a heavy page eventually exhausts the shared mbuf pool - at which point even
    ping fails with ENOBUFS. Both observations are made through a browser,
    which is the largest and least controllable variable in the system: it
    opens dozens of concurrent sockets, runs its own TLS, and schedules reads
    across processes.

    This does none of that. One socket, one plain HTTP GET, read as fast as the
    stack will supply, counting bytes and reporting the rate. If the ceiling is
    here too, it is AROSTCP or the driver; if this saturates the link and only
    the browser is slow, the problem is above the stack.

    Because the failure being chased is buffer starvation, errno is reported by
    name on every failing call rather than folded into a generic error - a read
    that stops with ENOBUFS is a completely different finding from one that
    stops with ECONNRESET.

    Point it at a host on the LAN or at the QEMU gateway rather than the open
    internet where possible; a local target measures the stack instead of
    measuring somebody else's server.

        nettest 10.0.2.2 PORT 8899 PATH /big.bin
        nettest example.com PATH /
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <proto/socket.h>

const TEXT version[] = "$VER: nettest 1.0 (10.09.2026)";

#define TEMPLATE "HOST/A,PORT/K/N,PATH/K,SECONDS/K/N,BUFK/K/N"

enum
{
    ARG_HOST,
    ARG_PORT,
    ARG_PATH,
    ARG_SECONDS,
    ARG_BUFK,
    ARG_COUNT
};

/* DateStamp ticks are 1/50s; ds_Tick plus ds_Minute is enough resolution for
   a transfer measured in seconds, and needs no timer.device. */
static ULONG NowTicks(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (ULONG)ds.ds_Minute * 60 * TICKS_PER_SECOND + (ULONG)ds.ds_Tick;
}

static CONST_STRPTR ErrName(int e)
{
    switch (e)
    {
        case ENOBUFS:      return "ENOBUFS (no buffer space - mbuf pool empty)";
        case ECONNRESET:   return "ECONNRESET (peer reset)";
        case ECONNREFUSED: return "ECONNREFUSED";
        case ETIMEDOUT:    return "ETIMEDOUT";
        case EHOSTUNREACH: return "EHOSTUNREACH";
        case ENETUNREACH:  return "ENETUNREACH";
        case EPIPE:        return "EPIPE";
        case EINTR:        return "EINTR";
        default:           return "";
    }
}

static void ReportErr(CONST_STRPTR what, int e)
{
    CONST_STRPTR name = ErrName(e);

    if (name[0] != '\0')
        Printf("nettest: %s failed: errno=%ld %s\n", (IPTR)what, (IPTR)e, (IPTR)name);
    else
        Printf("nettest: %s failed: errno=%ld\n", (IPTR)what, (IPTR)e);
}

/* bytes/sec -> a human rate. Integer only; no float formatting in DOS Printf. */
static void PrintRate(CONST_STRPTR label, UQUAD bytes, ULONG ticks)
{
    UQUAD bps;
    ULONG   secs10;

    if (ticks == 0)
        ticks = 1;

    secs10 = (ticks * 10) / TICKS_PER_SECOND;
    bps    = (bytes * TICKS_PER_SECOND) / ticks;

    Printf("%s: %lu KB in %lu.%lu s = %lu KB/s (%lu Mbit/s)\n",
           (IPTR)label,
           (IPTR)(ULONG)(bytes / 1024),
           (IPTR)(secs10 / 10), (IPTR)(secs10 % 10),
           (IPTR)(ULONG)(bps / 1024),
           (IPTR)(ULONG)((bps * 8) / 1000000));
}

int main(void)
{
    struct RDArgs *rdargs;
    IPTR args[ARG_COUNT] = { 0 };
    struct Library *SocketBase;
    struct sockaddr_in sa;
    struct hostent *he;
    CONST_STRPTR host, path;
    LONG port, limit, bufk;
    int sock, rc = RETURN_FAIL;
    char *buf;
    UQUAD total = 0;
    ULONG start, last, now;
    UQUAD lastbytes = 0;
    char req[512];

    rdargs = ReadArgs(TEMPLATE, args, NULL);
    if (rdargs == NULL)
    {
        PrintFault(IoErr(), "nettest");
        return RETURN_FAIL;
    }

    host  = (CONST_STRPTR)args[ARG_HOST];
    path  = args[ARG_PATH] ? (CONST_STRPTR)args[ARG_PATH] : (CONST_STRPTR)"/";
    port  = args[ARG_PORT] ? *(LONG *)args[ARG_PORT] : 80;
    limit = args[ARG_SECONDS] ? *(LONG *)args[ARG_SECONDS] : 10;
    bufk  = args[ARG_BUFK] ? *(LONG *)args[ARG_BUFK] : 32;

    if (bufk < 1)
        bufk = 1;

    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        PutStr("nettest: no bsdsocket.library - is the stack running?\n");
        FreeArgs(rdargs);
        return RETURN_FAIL;
    }

    buf = AllocVec(bufk * 1024, MEMF_ANY);
    if (buf == NULL)
    {
        PutStr("nettest: out of memory\n");
        CloseLibrary(SocketBase);
        FreeArgs(rdargs);
        return RETURN_FAIL;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((UWORD)port);
    sa.sin_addr.s_addr = inet_addr(host);

    if (sa.sin_addr.s_addr == INADDR_NONE)
    {
        he = gethostbyname((char *)host);
        if (he == NULL)
        {
            Printf("nettest: cannot resolve %s\n", (IPTR)host);
            goto done;
        }
        memcpy(&sa.sin_addr, he->h_addr, sizeof(sa.sin_addr));
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        ReportErr("socket", errno);
        goto done;
    }

    Printf("nettest: connecting to %s:%ld ...\n", (IPTR)host, (IPTR)port);

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        ReportErr("connect", errno);
        CloseSocket(sock);
        goto done;
    }

    /* HTTP/1.0 so the server closes at the end of the body and we get a clean
       EOF without having to parse chunked encoding. */
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: nettest/1.0\r\n"
             "Accept: */*\r\nConnection: close\r\n\r\n",
             path, host);

    if (send(sock, req, strlen(req), 0) < 0)
    {
        ReportErr("send", errno);
        CloseSocket(sock);
        goto done;
    }

    Printf("nettest: GET %s (reading up to %ld s, %ld KB buffer)\n",
           (IPTR)path, (IPTR)limit, (IPTR)bufk);

    start = last = NowTicks();

    for (;;)
    {
        LONG n = recv(sock, buf, bufk * 1024, 0);

        if (n == 0)
        {
            PutStr("nettest: peer closed\n");
            break;
        }

        if (n < 0)
        {
            /* The whole reason this program exists: say which errno. */
            ReportErr("recv", errno);
            break;
        }

        total += (UQUAD)n;

        now = NowTicks();
        if (now - last >= TICKS_PER_SECOND)
        {
            PrintRate("  interval", total - lastbytes, now - last);
            lastbytes = total;
            last = now;
        }

        if (limit > 0 && (now - start) >= (ULONG)limit * TICKS_PER_SECOND)
        {
            PutStr("nettest: time limit reached\n");
            break;
        }

        if (CheckSignal(SIGBREAKF_CTRL_C))
        {
            PutStr("nettest: ***Break\n");
            break;
        }
    }

    now = NowTicks();
    PrintRate("nettest: total", total, now - start);

    CloseSocket(sock);
    rc = RETURN_OK;

done:
    FreeVec(buf);
    CloseLibrary(SocketBase);
    FreeArgs(rdargs);
    return rc;
}
