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

    REPEAT makes N sequential connections, one after another, never more than
    one at a time.  That separates two things a browser conflates: how many
    connections are alive *at once*, and how many have been made *in total*.
    A browser speed test leaves the whole stack unable to connect - every
    later connect() in every process fails - and capping Chromium to four
    simultaneous sockets did not stop it, so the suspect is a per-connection
    leak rather than concurrency.  CONNECTONLY connects and closes again
    without sending a byte, which removes the data path from the question
    entirely: if that alone kills the stack, nothing above socket/connect/
    close is involved.

        nettest 10.0.2.2 PORT 8901 REPEAT 500 CONNECTONLY
        nettest 10.0.2.2 PORT 8901 PATH /small.bin REPEAT 500

    PARALLEL opens N connections at once and pushes on all of them together.
    That is the shape a browser produces and REPEAT deliberately does not:
    many sockets transmitting simultaneously.  A captured failing browser run
    showed the guest stop transmitting outright - last outbound packet 5m23s
    before the last inbound one, RX still flowing, and the next connect()
    never putting a SYN on the wire - which is the same Tx wedge as the SMP
    upload stall.  A browser cannot be debugged with debug=serial (the kernel
    logs once per 4 KiB page while PartitionAlloc reserves gigabytes), so the
    point of this mode is to reproduce that wedge from a program small enough
    to run under serial debugging with the driver instrumented.

        nettest 10.0.2.2 PORT 8901 PATH /drain PARALLEL 64 UPLOAD 4
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <proto/socket.h>
#include <bsdsocket/socketbasetags.h>
#include <sys/ioctl.h>

const TEXT version[] = "$VER: nettest 1.2 (11.09.2026)";

#define TEMPLATE "HOST/A,PORT/K/N,PATH/K,SECONDS/K/N,BUFK/K/N,UPLOAD/K/N,NODELAY/S,REPEAT/K/N,CONNECTONLY/S,PARALLEL/K/N"

enum
{
    ARG_HOST,
    ARG_PORT,
    ARG_PATH,
    ARG_SECONDS,
    ARG_BUFK,
    ARG_UPLOAD,
    ARG_NODELAY,
    ARG_REPEAT,
    ARG_CONNECTONLY,
    ARG_PARALLEL,
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
    LONG port, limit, bufk, upmb, reps, conly, par;
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
    upmb  = args[ARG_UPLOAD] ? *(LONG *)args[ARG_UPLOAD] : 0;
    reps  = args[ARG_REPEAT] ? *(LONG *)args[ARG_REPEAT] : 0;
    conly = args[ARG_CONNECTONLY] ? 1 : 0;
    par   = args[ARG_PARALLEL] ? *(LONG *)args[ARG_PARALLEL] : 0;

    if (bufk < 1)
        bufk = 1;

    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        PutStr("nettest: no bsdsocket.library - is the stack running?\n");
        FreeArgs(rdargs);
        return RETURN_FAIL;
    }

    /* nettest opens bsdsocket itself, so the net.lib autoinit that registers
       the errno pointer never runs and errno stays 0 no matter what fails.
       Register it here or every failure reports "errno=0" and says nothing. */
    if (SocketBaseTags(SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(errno))), (IPTR)&errno,
                       TAG_END))
        PutStr("nettest: warning: could not register errno pointer\n");

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

    if (par > 0)
    {
        /* N sockets open at once, all transmitting.  Non-blocking, so one
           stalled peer cannot hide a wedge on the others: if the Tx path dies
           every socket goes EWOULDBLOCK for ever and the idle counter below
           says so, instead of the program simply hanging in send(). */
        int *socks;
        UQUAD *sent;
        UQUAD want = (UQUAD)(upmb > 0 ? upmb : 1) * 1024 * 1024;
        LONG i, live = 0;
        ULONG idle_since = 0;
        ULONG t0;

        socks = AllocVec(par * sizeof(int), MEMF_ANY | MEMF_CLEAR);
        sent  = AllocVec(par * sizeof(UQUAD), MEMF_ANY | MEMF_CLEAR);
        if (socks == NULL || sent == NULL)
        {
            PutStr("nettest: out of memory for the socket table\n");
            FreeVec(socks); FreeVec(sent);
            goto done;
        }

        memset(buf, 'x', bufk * 1024);
        Printf("nettest: opening %ld parallel connections, %lu MB each\n",
               (IPTR)par, (IPTR)(ULONG)(want / (1024 * 1024)));

        for (i = 0; i < par; i++)
        {
            ULONG nb = 1;

            socks[i] = socket(AF_INET, SOCK_STREAM, 0);
            if (socks[i] < 0)
            {
                Printf("nettest: socket %ld: ", (IPTR)i);
                ReportErr("socket", errno);
                break;
            }
            if (connect(socks[i], (struct sockaddr *)&sa, sizeof(sa)) < 0)
            {
                Printf("nettest: socket %ld: ", (IPTR)i);
                ReportErr("connect", errno);
                CloseSocket(socks[i]);
                socks[i] = -1;
                break;
            }
            snprintf(req, sizeof(req),
                     "POST %s HTTP/1.0\r\nHost: %s\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                     path, host, (unsigned long)want);
            send(socks[i], req, strlen(req), 0);
            IoctlSocket(socks[i], FIONBIO, (char *)&nb);
            live++;
        }

        Printf("nettest: %ld connections established\n", (IPTR)live);
        t0 = last = NowTicks();

        while (live > 0)
        {
            LONG moved = 0;

            for (i = 0; i < par; i++)
            {
                LONG n;
                ULONG piece = bufk * 1024;

                if (socks[i] <= 0)
                    continue;
                if (sent[i] >= want)
                {
                    CloseSocket(socks[i]);
                    socks[i] = -1;
                    live--;
                    continue;
                }
                if ((UQUAD)piece > want - sent[i])
                    piece = (ULONG)(want - sent[i]);

                n = send(socks[i], buf, piece, 0);
                if (n > 0)
                {
                    sent[i] += (UQUAD)n;
                    total   += (UQUAD)n;
                    moved++;
                }
                else if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN)
                {
                    Printf("nettest: socket %ld died after %lu KB: ",
                           (IPTR)i, (IPTR)(ULONG)(sent[i] / 1024));
                    ReportErr("send", errno);
                    CloseSocket(socks[i]);
                    socks[i] = -1;
                    live--;
                }
            }

            /* A wedge is measured in TIME, not in loop iterations.  Counting
               passes reports a wedge the instant one straggler socket is left,
               because a handful of non-blocking sends spin through hundreds of
               thousands of rounds in well under a second - which produced
               "TX WEDGED - 1 sockets open" on a run that had just delivered
               129 of 131 MB perfectly. */
            if (moved == 0)
            {
                if (idle_since == 0)
                    idle_since = NowTicks();
                else if (NowTicks() - idle_since >= 10 * TICKS_PER_SECOND)
                {
                    Printf("nettest: TX WEDGED - %ld sockets open, none accepting"
                           " data for 10 s, %lu KB sent total\n",
                           (IPTR)live, (IPTR)(ULONG)(total / 1024));
                    break;
                }
            }
            else
                idle_since = 0;

            now = NowTicks();
            if (now - last >= TICKS_PER_SECOND)
            {
                PrintRate("  interval", total - lastbytes, now - last);
                lastbytes = total;
                last = now;
            }
            if (limit > 0 && (now - t0) >= (ULONG)limit * TICKS_PER_SECOND)
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
        PrintRate("nettest: sent", total, now - t0);
        for (i = 0; i < par; i++)
            if (socks[i] > 0)
                CloseSocket(socks[i]);
        FreeVec(socks);
        FreeVec(sent);
        rc = RETURN_OK;
        goto done;
    }

    if (reps > 0 || conly)
    {
        /* Sequential connections: never more than one socket alive at a time,
           so anything that breaks here is driven by how many connections have
           been made, not by how many are open at once. */
        LONG cyc, ok = 0;
        ULONG t0;

        if (reps <= 0)
            reps = 1;

        Printf("nettest: %ld sequential connections to %s:%ld%s\n",
               (IPTR)reps, (IPTR)host, (IPTR)port,
               (IPTR)(conly ? " (connect/close only, no data)" : ""));
        t0 = NowTicks();

        for (cyc = 1; cyc <= reps; cyc++)
        {
            int s2 = socket(AF_INET, SOCK_STREAM, 0);

            if (s2 < 0)
            {
                Printf("nettest: cycle %ld: ", (IPTR)cyc);
                ReportErr("socket", errno);
                break;
            }

            if (connect(s2, (struct sockaddr *)&sa, sizeof(sa)) < 0)
            {
                Printf("nettest: cycle %ld: ", (IPTR)cyc);
                ReportErr("connect", errno);
                CloseSocket(s2);
                break;
            }

            if (!conly)
            {
                snprintf(req, sizeof(req),
                         "GET %s HTTP/1.0\r\nHost: %s\r\n"
                         "Connection: close\r\n\r\n", path, host);
                if (send(s2, req, strlen(req), 0) < 0)
                {
                    Printf("nettest: cycle %ld: ", (IPTR)cyc);
                    ReportErr("send", errno);
                    CloseSocket(s2);
                    break;
                }
                for (;;)
                {
                    LONG k = recv(s2, buf, bufk * 1024, 0);
                    if (k < 0)
                    {
                        Printf("nettest: cycle %ld: ", (IPTR)cyc);
                        ReportErr("recv", errno);
                        break;
                    }
                    if (k == 0)
                        break;
                    total += (UQUAD)k;
                }
            }

            CloseSocket(s2);
            ok = cyc;

            if ((cyc % 25) == 0)
                Printf("  %ld connections ok (%lu KB)\n",
                       (IPTR)cyc, (IPTR)(ULONG)(total / 1024));

            if (CheckSignal(SIGBREAKF_CTRL_C))
            {
                PutStr("nettest: ***Break\n");
                break;
            }
        }

        now = NowTicks();
        Printf("nettest: %ld of %ld connections completed, %lu KB, %lu ticks\n",
               (IPTR)ok, (IPTR)reps, (IPTR)(ULONG)(total / 1024),
               (IPTR)(now - t0));
        rc = RETURN_OK;
        goto done;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        ReportErr("socket", errno);
        goto done;
    }

    if (args[ARG_NODELAY])
    {
        /* Disable Nagle. A sub-MSS tail is withheld while unacked data is
           outstanding, which is invisible on a continuous stream and fatal at
           the end of a buffer - exactly the shape of the observed stall. This
           switch exists to test that without touching the stack. */
        int on = 1;
        if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0)
            ReportErr("setsockopt(TCP_NODELAY)", errno);
        else
            PutStr("nettest: TCP_NODELAY set\n");
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
    if (upmb > 0)
    {
        snprintf(req, sizeof(req),
                 "POST %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: nettest/1.0\r\n"
                 "Content-Type: application/octet-stream\r\n"
                 "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                 path, host, (unsigned long)upmb * 1024UL * 1024UL);
    }
    else
    {
        snprintf(req, sizeof(req),
                 "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: nettest/1.0\r\n"
                 "Accept: */*\r\nConnection: close\r\n\r\n",
                 path, host);
    }

    if (send(sock, req, strlen(req), 0) < 0)
    {
        ReportErr("send", errno);
        CloseSocket(sock);
        goto done;
    }

    if (upmb > 0)
    {
        /* Upload mode. The receive path was the first thing fixed here; the
           send path has its own sbspace() accounting on so_snd and its own
           chance to go wrong, and a browser upload failing with "a socket
           error occurred" says nothing about which layer gave up. Push a
           known number of bytes and report exactly where it stops. */
        UQUAD want = (UQUAD)upmb * 1024 * 1024;

        memset(buf, 'x', bufk * 1024);
        Printf("nettest: POST %s, sending %ld MB\n", (IPTR)path, (IPTR)upmb);
        start = last = NowTicks();

        while (total < want)
        {
            ULONG piece = bufk * 1024;
            LONG n;

            if ((UQUAD)piece > want - total)
                piece = (ULONG)(want - total);

            n = send(sock, buf, piece, 0);
            if (n < 0)
            {
                ReportErr("send", errno);
                break;
            }
            if (n == 0)
            {
                PutStr("nettest: send returned 0\n");
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
        PrintRate("nettest: sent", total, now - start);
        CloseSocket(sock);
        rc = RETURN_OK;
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
