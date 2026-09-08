/*
    bsdsocket-backed POSIX socket layer for the AROS libuv port.

    AROS bsdsocket sockets live in a SEPARATE fd namespace from posixc files, and
    are closed/read/written via CloseSocket/recv/send -- not close/read/write.
    libuv uses the generic ops, so we:
      - provide socket()/connect()/bind()/... as real functions calling the
        bsdsocket LVOs (via the __*_WB explicit-base macros) on a lazily-opened
        global SocketBase;
      - keep a REGISTRY of which fds are sockets (set on socket()/accept());
      - route close/read/write for registered fds to CloseSocket/recv/send via a
        linker --wrap layer (__wrap_close/read/write), falling through to
        __real_* (posixc) for file fds.

    bsdsocket and posixc have independent descriptor tables and both allocate
    from zero. Keep sockets in the upper half of WaitSelect's descriptor range
    so ordinary low-numbered files cannot alias them in the routing registry.
    Full descriptor-space unification remains a longer-term POSIX substrate job.
    SocketBase is also per-task: opened on the loop task; threadpool DNS would
    need its own (deferred).
*/
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/semaphores.h>
#include <exec/libraries.h>
#include <aros/libcall.h>

#include <sys/socket.h>          /* struct sockaddr, socklen_t */
#include <sys/types.h>
#include <netinet/in.h>          /* struct sockaddr_in (socketpair emulation) */
#include <sys/un.h>              /* struct sockaddr_un (AF_UNIX named-pipe emulation) */

/* Only the __*_WB explicit-base macros -- NOT clib's AROS_LP inline function
   definitions (which would conflict with our socket()/connect()/... below). */
#include <libraries/bsdsocket.h>
#include <defines/bsdsocket.h>

/* Drop the convenience macros (connect -> __connect_WB(SocketBase,...)) so we
   can DEFINE these names as functions; the __*_WB explicit-base macros stay. */
#undef socket
#undef connect
#undef bind
#undef listen
#undef accept
#undef send
#undef recv
#undef sendto
#undef recvfrom
#undef setsockopt
#undef getsockopt
#undef getsockname
#undef getpeername
#undef IoctlSocket
#undef CloseSocket
#undef WaitSelect

#include <errno.h>
/* fd_set / FD_SET / FD_ZERO / FD_SETSIZE come in via <sys/socket.h> above; do
   NOT re-include <sys/net_types.h> -- it resolves to a second copy and
   redefines fd_set/fd_mask. */
#include <sys/time.h>            /* struct timeval (tv_sec/tv_usec) for WaitSelect */
#include <sys/ioctl.h>           /* FIONBIO */
#include <fcntl.h>               /* F_GETFL / F_SETFL / O_NONBLOCK */
#include <unistd.h>              /* close/read/write for registry files */
#include <stdarg.h>
#include <stdio.h>               /* __get_default_file, snprintf */
#include <stdlib.h>              /* atol (AF_UNIX registry port parse) */
#include <strings.h>             /* bzero, used by FD_ZERO */

struct Library *SocketBase = NULL;          /* consumed by aros.c backend too */
static struct SignalSemaphore net_sem;
static int net_sem_ready = 0;
static int errno_wired = 0;
static unsigned int net_loop_users = 0;

/* socket-fd registry: one flag per fd (bsdsocket default dtable is small). */
#define UV_AROS_MAXSOCK 256
static unsigned char is_sock[UV_AROS_MAXSOCK];
static unsigned char is_dos_pipe[UV_AROS_MAXSOCK];
/* JOB-0033: AF_UNIX named-pipe emulation. bsdsocket has no AF_UNIX, so a
   path-based socket (e.g. VS Code's single-instance .sock) is backed by a
   127.0.0.1 loopback TCP socket -- exactly like the anonymous socketpair()
   emulation below. is_unix[fd] marks fds created as socket(AF_UNIX,...) so
   bind()/connect() route the sockaddr_un path through the loopback + a
   path->port registry file, instead of handing AF_UNIX to bsdsocket. The
   registry file at the path holds the ephemeral loopback port; the file's
   existence + content IS the AF_UNIX name (stale file -> connect refuses ->
   the caller re-binds, matching AF_UNIX stale-socket semantics). */
static unsigned char is_unix[UV_AROS_MAXSOCK];
static void ux_set(int fd) { if (fd >= 0 && fd < UV_AROS_MAXSOCK) is_unix[fd] = 1; }
static void ux_clr(int fd) { if (fd >= 0 && fd < UV_AROS_MAXSOCK) is_unix[fd] = 0; }
static int  ux_is (int fd) { return (fd >= 0 && fd < UV_AROS_MAXSOCK) ? is_unix[fd] : 0; }
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
static int ux_write_port(const char *path, unsigned short port)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    char buf[16];
    int n;
    ssize_t written;
    int saved;

    if (fd < 0)
        return -1;
    n = snprintf(buf, sizeof(buf), "%u\n", (unsigned)port);
    written = write(fd, buf, (size_t)n);
    saved = errno;
    close(fd);
    errno = saved;
    return written == n ? 0 : -1;
}
static int ux_read_port(const char *path, unsigned short *port)
{
    int fd = open(path, O_RDONLY);
    char buf[16];
    ssize_t bytes;
    long p;

    if (fd < 0)
        return -1;
    bytes = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (bytes <= 0)
        return -1;
    buf[bytes] = 0;
    p = atol(buf);
    if (p <= 0 || p > 65535)
        return -1;
    *port = (unsigned short)p;
    return 0;
}

static void reg_add(int fd) { if (fd >= 0 && fd < UV_AROS_MAXSOCK) is_sock[fd] = 1; }
static void reg_del(int fd) { if (fd >= 0 && fd < UV_AROS_MAXSOCK) is_sock[fd] = 0; }
int uv__aros_is_socket(int fd) { return (fd >= 0 && fd < UV_AROS_MAXSOCK) ? is_sock[fd] : 0; }
void uv__aros_register_dos_pipe(int fd) {
    if (fd >= 0 && fd < UV_AROS_MAXSOCK)
        is_dos_pipe[fd] = 1;
}
void uv__aros_unregister_dos_pipe(int fd) {
    if (fd >= 0 && fd < UV_AROS_MAXSOCK)
        is_dos_pipe[fd] = 0;
}
int uv__aros_is_dos_pipe(int fd) {
    return (fd >= 0 && fd < UV_AROS_MAXSOCK) ? is_dos_pipe[fd] : 0;
}

static void uv__aros_net_sem_init(void)
{
    if (net_sem_ready)
        return;

    Forbid();
    if (!net_sem_ready) {
        InitSemaphore(&net_sem);
        net_sem_ready = 1;
    }
    Permit();
}

void uv__aros_net_loop_init(void)
{
    uv__aros_net_sem_init();
    ObtainSemaphore(&net_sem);
    net_loop_users++;
    ReleaseSemaphore(&net_sem);
}

void uv__aros_net_loop_delete(void)
{
    uv__aros_net_sem_init();
    ObtainSemaphore(&net_sem);
    if (net_loop_users > 0)
        net_loop_users--;
    if (net_loop_users == 0 && SocketBase != NULL) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
        errno_wired = 0;
    }
    ReleaseSemaphore(&net_sem);
}

int uv__aros_dos_pipe_ready(int fd)
{
    long fh;

    if (!uv__aros_is_dos_pipe(fd))
        return 0;
    if (__get_default_file(fd, &fh) != 0)
        return 0;
    return WaitForChar((BPTR)fh, 0) != 0;
}

static int promote_socket(int fd)
{
    int target;
    int saved;

    for (target = FD_SETSIZE - 1; target >= FD_SETSIZE / 2; target--) {
        int promoted;

        if (target == fd || uv__aros_is_socket(target))
            continue;
        promoted = __Dup2Socket_WB(SocketBase, fd, target);
        if (promoted >= 0) {
            __CloseSocket_WB(SocketBase, fd);
            reg_add(promoted);
            return promoted;
        }
    }

    saved = errno;
    reg_add(fd);
    errno = saved;
    return fd;
}

/* Open bsdsocket once, on first use, and wire bsdsocket's errno to C errno. */
struct Library *uv__aros_socketbase(void)
{
    if (SocketBase != NULL)
        return SocketBase;

    uv__aros_net_sem_init();
    ObtainSemaphore(&net_sem);
    if (SocketBase == NULL)
        SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (SocketBase != NULL && !errno_wired) {
        struct TagItem tags[] = {
            { SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(errno))), (IPTR)&errno },
            { TAG_END, 0 }
        };
        __SocketBaseTagList_WB(SocketBase, tags);
        errno_wired = 1;
    }
    ReleaseSemaphore(&net_sem);
    return SocketBase;
}

#define BASE (SocketBase ? SocketBase : uv__aros_socketbase())

int socket(int domain, int type, int protocol)
{
    int fd;
    int posix_flags = 0;
    /* JOB-0033: AF_UNIX has no bsdsocket domain -- emulate over 127.0.0.1 TCP.
       Create the socket as AF_INET/STREAM and tag it so bind()/connect() route
       the sockaddr_un path through the loopback + path->port registry. */
    int unix_dom = (domain == AF_UNIX);
    if (unix_dom) { domain = AF_INET; protocol = 0; }

#ifdef SOCK_NONBLOCK
    posix_flags |= type & SOCK_NONBLOCK;
#endif
#ifdef SOCK_CLOEXEC
    posix_flags |= type & SOCK_CLOEXEC;
#endif

    if (BASE == NULL) { errno = ENOSYS; return -1; }
    fd = __socket_WB(SocketBase, domain, type, protocol);
    /*
     * libuv probes atomic SOCK_NONBLOCK|SOCK_CLOEXEC creation first and
     * retries the plain socket plus fcntl when the platform reports EINVAL.
     * AROSTCP reports EPROTONOSUPPORT for those POSIX type bits instead.
     * Normalize only that flagged probe so libuv reaches the existing AROS
     * fcntl adapter without hiding genuine protocol failures.
     */
    if (fd < 0 && posix_flags != 0 && errno == EPROTONOSUPPORT)
        errno = EINVAL;
    if (fd >= 0) {
        fd = promote_socket(fd);
        if (fd >= 0 && unix_dom)
            ux_set(fd);
    }
    return fd;
}

int accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    int fd;
    if (BASE == NULL) { errno = ENOSYS; return -1; }
    fd = __accept_WB(SocketBase, s, addr, addrlen);
    if (fd >= 0)
        fd = promote_socket(fd);
    return fd;
}

int connect(int s, const struct sockaddr *name, socklen_t namelen)
{
    /* JOB-0033: AF_UNIX path -> read the loopback port from the registry file
       and connect to 127.0.0.1:<port>. A missing/stale file (owner gone) makes
       connect refuse, which is exactly the AF_UNIX stale-socket contract that
       lets a single-instance client (VS Code claimInstance) re-bind. */
    if (ux_is(s) && name && name->sa_family == AF_UNIX) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)name;
        struct sockaddr_in a;
        unsigned short port;
        if (!BASE) { errno = ENOSYS; return -1; }
        if (ux_read_port(un->sun_path, &port) < 0) { errno = ENOENT; return -1; }
        bzero(&a, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = 0x0100007fUL;   /* 127.0.0.1 */
        a.sin_port        = htons(port);
        return __connect_WB(SocketBase, s, (struct sockaddr *)&a, sizeof(a));
    }
    return BASE ? __connect_WB(SocketBase, s, (struct sockaddr *)name, namelen) : -1;
}

int bind(int s, const struct sockaddr *name, socklen_t namelen)
{
    /* JOB-0033: AF_UNIX path -> bind a 127.0.0.1 ephemeral-port listener and
       publish the port in a registry file at the path. listen()/accept() then
       run unmodified on the AF_INET fd (real accept, multi-client). */
    if (ux_is(s) && name && name->sa_family == AF_UNIX) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)name;
        struct sockaddr_in a;
        socklen_t alen = sizeof(a);
        if (!BASE) { errno = ENOSYS; return -1; }
        bzero(&a, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = 0x0100007fUL;   /* 127.0.0.1 */
        a.sin_port        = 0;              /* ephemeral */
        if (__bind_WB(SocketBase, s, (struct sockaddr *)&a, sizeof(a)) < 0)
            return -1;
        if (__getsockname_WB(SocketBase, s, (struct sockaddr *)&a, &alen) < 0)
            return -1;
        if (ux_write_port(un->sun_path, ntohs(a.sin_port)) < 0)
            return -1;
        return 0;
    }
    return BASE ? __bind_WB(SocketBase, s, (struct sockaddr *)name, namelen) : -1;
}

int listen(int s, int backlog)
{ return BASE ? __listen_WB(SocketBase, s, backlog) : -1; }

ssize_t send(int s, const void *buf, size_t len, int flags)
{ return BASE ? __send_WB(SocketBase, s, (void *)buf, len, flags) : -1; }

ssize_t recv(int s, void *buf, size_t len, int flags)
{ return BASE ? __recv_WB(SocketBase, s, buf, len, flags) : -1; }

ssize_t sendto(int s, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen)
{ return BASE ? __sendto_WB(SocketBase, s, (void *)buf, len, flags,
                            (struct sockaddr *)to, tolen) : -1; }

ssize_t recvfrom(int s, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen)
{ return BASE ? __recvfrom_WB(SocketBase, s, buf, len, flags, from, fromlen) : -1; }

int setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen)
{ return BASE ? __setsockopt_WB(SocketBase, s, level, optname, (void *)optval, optlen) : -1; }

int getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)
{ return BASE ? __getsockopt_WB(SocketBase, s, level, optname, optval, optlen) : -1; }

int getsockname(int s, struct sockaddr *name, socklen_t *namelen)
{ return BASE ? __getsockname_WB(SocketBase, s, name, namelen) : -1; }

int getpeername(int s, struct sockaddr *name, socklen_t *namelen)
{ return BASE ? __getpeername_WB(SocketBase, s, name, namelen) : -1; }

/* socketpair(): bsdsocket has no AF_UNIX socketpair, so emulate it over an
   AF_INET 127.0.0.1 loopback pair -- listen on an ephemeral port, connect a
   client, accept; the connected TCP pair is bidirectional and stands in for the
   AF_UNIX pipe (this is how Windows and other AF_UNIX-less platforms do it).
   Both ends are created under uv.library's SocketBase by the wrappers above and
   registered, so uv_poll/WaitSelect can watch them. Backs uv_socketpair() and
   the anonymous uv_pipe path (Node child_process stdio). */
int socketpair(int domain, int type, int protocol, int sv[2])
{
    struct sockaddr_in a;
    socklen_t alen;
    int lst = -1, cli = -1, acc = -1, saved;
    (void)domain; (void)type; (void)protocol;

    lst = socket(AF_INET, SOCK_STREAM, 0);
    if (lst < 0) goto fail;

    bzero(&a, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = 0x0100007fUL;   /* 127.0.0.1, network order on LE host */
    a.sin_port        = 0;              /* ephemeral */
    if (bind(lst, (struct sockaddr *)&a, sizeof(a)) < 0)    goto fail;
    if (listen(lst, 1) < 0)                                 goto fail;

    alen = sizeof(a);
    if (getsockname(lst, (struct sockaddr *)&a, &alen) < 0) goto fail;

    cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli < 0) goto fail;
    if (connect(cli, (struct sockaddr *)&a, sizeof(a)) < 0) goto fail;

    alen = sizeof(a);
    acc = accept(lst, (struct sockaddr *)&a, &alen);
    if (acc < 0) goto fail;

    reg_del(lst);
    __CloseSocket_WB(SocketBase, lst);
    sv[0] = cli;
    sv[1] = acc;
    return 0;

fail:
    saved = errno;
    if (acc >= 0) { reg_del(acc); __CloseSocket_WB(SocketBase, acc); }
    if (cli >= 0) { reg_del(cli); __CloseSocket_WB(SocketBase, cli); }
    if (lst >= 0) { reg_del(lst); __CloseSocket_WB(SocketBase, lst); }
    errno = saved;
    return -1;
}

/* --- close/read/write routing via linker --wrap --- */
extern int     __real_close(int fd);
extern ssize_t __real_read(int fd, void *buf, size_t n);
extern ssize_t __real_write(int fd, const void *buf, size_t n);

int __wrap_close(int fd)
{
    if (uv__aros_is_socket(fd)) {
        int r = SocketBase ? __CloseSocket_WB(SocketBase, fd) : -1;
        reg_del(fd);
        ux_clr(fd);   /* JOB-0033: clear AF_UNIX tag (libuv unlinks the .sock path) */
        return r;
    }
    uv__aros_unregister_dos_pipe(fd);
    return __real_close(fd);
}

ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    if (uv__aros_is_socket(fd))
        return SocketBase ? __recv_WB(SocketBase, fd, buf, n, 0) : -1;
    /*
     * DOS PIPE: reads are blocking.  The event backend dispatches them only
     * after ACTION_WAIT_CHAR says data or EOF is ready, but uv__read() may
     * immediately try a second read after filling its first buffer.  Preserve
     * libuv's nonblocking stream contract for that second read.
     */
    if (uv__aros_is_dos_pipe(fd) && !uv__aros_dos_pipe_ready(fd)) {
        errno = EAGAIN;
        return -1;
    }
    return __real_read(fd, buf, n);
}

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    if (uv__aros_is_socket(fd))
        return SocketBase ? __send_WB(SocketBase, fd, (void *)buf, n, 0) : -1;
    return __real_write(fd, buf, n);
}

/* --- fcntl routing via linker --wrap ---
   libuv's uv__nonblock_fcntl() toggles O_NONBLOCK with F_GETFL/F_SETFL, which
   operate on posixc fds -- meaningless for a bsdsocket fd. Route socket fds to
   IoctlSocket(FIONBIO); non-socket fds fall through to posixc __real_fcntl. */
extern int __real_fcntl(int fd, int cmd, ...);

int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int arg;

    va_start(ap, cmd);
    arg = va_arg(ap, int);
    va_end(ap);

    if (uv__aros_is_socket(fd)) {
        if (cmd == F_GETFL)
            return 0;                       /* report "no flags set" */
        if (cmd == F_SETFL) {
            int nb = (arg & O_NONBLOCK) ? 1 : 0;
            if (SocketBase)
                __IoctlSocket_WB(SocketBase, fd, FIONBIO, (char *)&nb);
            return 0;                       /* keep uv_poll_init happy regardless */
        }
        return 0;                           /* other cmds: no-op success */
    }
    return __real_fcntl(fd, cmd, arg);
}

/* --- readiness wait over bsdsocket (drives libuv's uv__io_poll on AROS) ---
   fds[i]/wanted[i] carry per-fd interest (bit0 = readable, bit1 = writable);
   revents[i] receives the ready mask (same bits). asyncsig is an exec signal bit
   to wake on in addition to fd readiness (or < 0 for none); timeout_ms < 0 waits
   forever. Returns the ready-fd count (>= 0) or -1; *async_fired is set when the
   async signal woke the wait. WaitSelect unifies fd readiness, the timeout, and
   the loop's exec async-signal in a single blocking call. */
int uv__aros_waitselect(const int *fds, const int *wanted, int *revents,
                        int nfds, int timeout_ms, int asyncsig, int *async_fired)
{
    fd_set rd, wr;
    struct timeval tv;
    struct timeval *tvp;
    ULONG sigmask;
    int i, maxfd, n;

    *async_fired = 0;
    if (BASE == NULL)
        return -1;

    FD_ZERO(&rd);
    FD_ZERO(&wr);
    maxfd = -1;
    for (i = 0; i < nfds; i++) {
        int fd = fds[i];
        revents[i] = 0;
        if (fd < 0 || fd >= FD_SETSIZE)
            continue;
        if (wanted[i] & 1) FD_SET(fd, &rd);
        if (wanted[i] & 2) FD_SET(fd, &wr);
        if (fd > maxfd) maxfd = fd;
    }

    sigmask = (asyncsig >= 0) ? (1UL << (ULONG)asyncsig) : 0;

    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    } else {
        tvp = NULL;
    }

    n = __WaitSelect_WB(SocketBase, maxfd + 1, &rd, &wr,
                        (fd_set *)0, tvp, &sigmask);

    /*
     * AROSTCP may return zero immediately despite a positive timeout.  Do not
     * let a network-bound event loop busy-spin and starve the stack task that
     * must refill the receive queue.  A true zero-timeout poll stays nonblocking.
     */
    if (n == 0 && timeout_ms > 0)
        Delay(1);

    if (asyncsig >= 0 && (sigmask & (1UL << (ULONG)asyncsig)))
        *async_fired = 1;

    if (n > 0) {
        for (i = 0; i < nfds; i++) {
            int fd = fds[i];
            if (fd < 0 || fd >= FD_SETSIZE)
                continue;
            if (FD_ISSET(fd, &rd)) revents[i] |= 1;
            if (FD_ISSET(fd, &wr)) revents[i] |= 2;
        }
    }
    return n;
}
