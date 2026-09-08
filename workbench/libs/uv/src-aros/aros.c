/*
    AROS custom event-loop backend for libuv (the src/unix/aros.c equivalent).

    Core-first phase: no POSIX socket layer yet, so uv__io_poll() has no fds to
    wait on. It blocks up to `timeout` ms via timer.device and wakes early on
    the loop's exec async-signal (uv_async / threadpool completion). Socket fd
    readiness (WaitSelect over bsdsocket) is added in the networking phase.

    Replaces linux-core.c / kqueue.c / posix-poll.c.
*/
#include "uv.h"
#include "internal.h"

#include <proto/dos.h>
#include <proto/exec.h>
#include <exec/tasks.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <unistd.h>

/* Real AROS uv_async (src-aros/aros-async.c): drain pending async handles. */
void uv__aros_async_drain(uv_loop_t* loop);

/* bsdsocket readiness layer (src-aros/aros-net.c). */
int uv__aros_is_socket(int fd);
int uv__aros_is_dos_pipe(int fd);
int uv__aros_dos_pipe_ready(int fd);
int uv__aros_waitselect(const int* fds, const int* wanted, int* revents,
                        int nfds, int timeout_ms, int asyncsig, int* async_fired);
void uv__aros_process_reap(uv_loop_t* loop);
void uv__aros_net_loop_init(void);
void uv__aros_net_loop_delete(void);

#define UV_AROS_POLL_MAX 64   /* bsdsocket FD_SETSIZE */
#define UV_AROS_TTY_POLL_MS 10

static unsigned int uv__aros_threadpool_pending(uv_loop_t* loop)
{
    unsigned int pending;

    uv_mutex_lock(&loop->wq_mutex);
    pending = !uv__queue_empty(&loop->wq);
    uv_mutex_unlock(&loop->wq_mutex);

    return pending;
}

static int uv__aros_tty_ready(uv__io_t* watcher)
{
    /*
     * The event-loop task's fd 0 is its DOS Input() handle. KCON/CON provide
     * asynchronous device input behind that handle, and WaitForChar(0) is the
     * non-blocking readiness query DOS exposes for it. Other interactive
     * descriptors need a future descriptor-to-BPTR API; do not alias them to
     * the current process input.
     */
    if (watcher == NULL || watcher->fd != 0 || !(watcher->events & POLLIN))
        return 0;
    return WaitForChar(Input(), 0) != 0;
}

static void uv__aros_dispatch_ttys(uv_loop_t* loop,
                                   uv__io_t** watchers,
                                   int count)
{
    int i;

    for (i = 0; i < count; i++) {
        uv__io_t* watcher = watchers[i];

        if (uv__aros_tty_ready(watcher))
            watcher->cb(loop, watcher, POLLIN);
    }
}

static void uv__aros_dispatch_dos_pipes(uv_loop_t* loop,
                                        uv__io_t** watchers,
                                        int count)
{
    int i;

    for (i = 0; i < count; i++) {
        uv__io_t* watcher = watchers[i];

        if ((watcher->events & POLLIN) &&
            uv__aros_dos_pipe_ready(watcher->fd))
            watcher->cb(loop, watcher, POLLIN);
        else if (watcher->events & POLLOUT)
            watcher->cb(loop, watcher, POLLOUT);
    }
}

int uv__platform_loop_init(uv_loop_t* loop) {
    struct MsgPort* port;
    struct timerequest* tr;

    loop->aros_timerport = NULL;
    loop->aros_timerio = NULL;
    loop->aros_task = FindTask(NULL);
    loop->aros_async_sig = -1;
    loop->aros_timer_open = 0;

    port = CreateMsgPort();
    if (port == NULL)
        return UV_ENOMEM;

    tr = (struct timerequest*)CreateIORequest(port, sizeof(*tr));
    if (tr == NULL) {
        DeleteMsgPort(port);
        return UV_ENOMEM;
    }

    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                   (struct IORequest*)tr, 0) != 0) {
        DeleteIORequest((struct IORequest*)tr);
        DeleteMsgPort(port);
        return UV_ENOSYS;
    }

    loop->aros_timerport = port;
    loop->aros_timerio = tr;
    loop->aros_timer_open = 1;
    uv__aros_net_loop_init();
    return 0;
}

void uv__platform_loop_delete(uv_loop_t* loop) {
    struct timerequest* tr = (struct timerequest*)loop->aros_timerio;

    /* uv__io_poll always reaps its timer request before returning, so no
       request is ever in flight here -- just close and free. (Calling WaitIO
       on a request that was never sent removes a non-existent message and
       faults.) */
    if (loop->aros_timer_open && tr != NULL)
        CloseDevice((struct IORequest*)tr);
    if (tr != NULL)
        DeleteIORequest((struct IORequest*)tr);
    if (loop->aros_timerport != NULL)
        DeleteMsgPort((struct MsgPort*)loop->aros_timerport);

    loop->aros_timerio = NULL;
    loop->aros_timerport = NULL;
    loop->aros_timer_open = 0;
    uv__aros_net_loop_delete();
}

void uv__platform_invalidate_fd(uv_loop_t* loop, int fd) {
    (void)loop;
    (void)fd;
}

/* poll(2) is stubbed on AROS, so uv_poll_init's fd validation is permissive;
   only genuine bsdsocket fds are actually waited on (uv__io_poll below). */
int uv__io_check_fd(uv_loop_t* loop, int fd) {
    (void)loop;
    (void)fd;
    return 0;
}

void uv__io_poll(uv_loop_t* loop, int timeout) {
    struct timerequest* tr;
    struct MsgPort* port;
    struct uv__queue* q;
    uv__io_t* w;
    int fds[UV_AROS_POLL_MAX];
    int wants[UV_AROS_POLL_MAX];
    int revents[UV_AROS_POLL_MAX];
    uv__io_t* tty_watchers[UV_AROS_POLL_MAX];
    uv__io_t* pipe_watchers[UV_AROS_POLL_MAX];
    int nsock, ntty, npipe, i, fd, re;
    ULONG timermask;
    ULONG asyncmask = 0;
    ULONG got;

    /* Fold each queued watcher's pending mask into its active mask. Do this
       first, unconditionally -- it is the contract of uv__io_start/stop. */
    while (!uv__queue_empty(&loop->watcher_queue)) {
        q = uv__queue_head(&loop->watcher_queue);
        uv__queue_remove(q);
        uv__queue_init(q);
        w = uv__queue_data(q, uv__io_t, watcher_queue);
        w->events = w->pevents;
    }

    /*
     * Collect socket watchers for WaitSelect and interactive stdin separately.
     * DOS descriptors and bsdsocket descriptors are different namespaces;
     * passing KCON fd 0 to WaitSelect cannot report console.device input.
     */
    uv__aros_process_reap(loop);

    nsock = 0;
    ntty = 0;
    npipe = 0;
    for (fd = 0; fd < (int)loop->nwatchers && nsock < UV_AROS_POLL_MAX; fd++) {
        w = loop->watchers[fd];
        if (w == NULL || w->events == 0)
            continue;
        if (!uv__aros_is_socket(fd)) {
            if (uv__aros_is_dos_pipe(fd) && npipe < UV_AROS_POLL_MAX)
                pipe_watchers[npipe++] = w;
            else if (isatty(fd) && ntty < UV_AROS_POLL_MAX)
                tty_watchers[ntty++] = w;
            continue;
        }
        fds[nsock]  = fd;
        wants[nsock] = ((w->events & POLLIN)  ? 1 : 0) |
                       ((w->events & POLLOUT) ? 2 : 0);
        nsock++;
    }

    if (ntty > 0) {
        uv__aros_dispatch_ttys(loop, tty_watchers, ntty);
        /*
         * Node keeps an idle/check watcher armed while its interactive UI is
         * active, which normally asks the backend for a non-blocking poll.
         * A DOS console has no fd signal to merge into WaitSelect yet, so a
         * literal timeout zero would spin an entire CPU. Bound the adapter to
         * terminal-scale latency until KCON exposes a waitable endpoint.
         */
        if (timeout < 0 || timeout == 0 || timeout > UV_AROS_TTY_POLL_MS)
            timeout = UV_AROS_TTY_POLL_MS;
    }

    if (npipe > 0) {
        uv__aros_dispatch_dos_pipes(loop, pipe_watchers, npipe);
        if (timeout < 0 || timeout == 0 || timeout > UV_AROS_TTY_POLL_MS)
            timeout = UV_AROS_TTY_POLL_MS;
    }

    /* AROS reports child death through Exec rather than a pollable SIGCHLD fd.
       Bound the wait while process handles are active and reap each turn. */
    if (!uv__queue_empty(&loop->process_handles) &&
        (timeout < 0 || timeout > UV_AROS_TTY_POLL_MS))
        timeout = UV_AROS_TTY_POLL_MS;

    /* Socket fds present: WaitSelect covers fd readiness + timeout + the loop's
       async signal in one wait (timeout == 0 => non-blocking reap). */
    if (nsock > 0) {
        int async_fired = 0;
        int r = uv__aros_waitselect(fds, wants, revents, nsock, timeout,
                                    loop->aros_async_sig, &async_fired);
        uv__update_time(loop);

        if (async_fired)
            uv__aros_async_drain(loop);

        if (r > 0) {
            for (i = 0; i < nsock; i++) {
                re = 0;
                if (revents[i] & 1) re |= POLLIN;
                if (revents[i] & 2) re |= POLLOUT;
                if (re == 0)
                    continue;
                w = loop->watchers[fds[i]];
                if (w != NULL)
                    w->cb(loop, w, re);
            }
        }
        uv__aros_dispatch_dos_pipes(loop, pipe_watchers, npipe);
        uv__aros_dispatch_ttys(loop, tty_watchers, ntty);
        uv__aros_process_reap(loop);
        return;
    }

    /* --- No socket fds: timer.device + async-signal wait (core-first path). --- */
    tr = (struct timerequest*)loop->aros_timerio;

    if (timeout == 0) {
        /*
         * Active idle/check handles make libuv poll with timeout zero.  A
         * threadpool worker may already have signalled wq_async; returning
         * without consuming that signal starves the completed request forever.
         */
        if (loop->aros_async_sig >= 0) {
            asyncmask = 1UL << loop->aros_async_sig;
            got = SetSignal(0, asyncmask);
            if (got & asyncmask)
                uv__aros_async_drain(loop);
        }
        /*
         * If a request remains active, make the zero-timeout poll an
         * interruptible one-millisecond wait below.  This closes the race
         * where a worker raises wq_async just after the nonblocking check,
         * without taxing idle/check-only turns with a full timer tick.
         */
        if (uv__aros_threadpool_pending(loop) != 0 && loop->aros_timer_open &&
            tr != NULL && loop->aros_timerport != NULL) {
            timeout = 1;
        } else {
            uv__aros_dispatch_ttys(loop, tty_watchers, ntty);
            uv__aros_dispatch_dos_pipes(loop, pipe_watchers, npipe);
            uv__aros_process_reap(loop);
            Reschedule();
            uv__update_time(loop);
            return;
        }
    }

    if (loop->aros_async_sig >= 0)
        asyncmask = 1UL << loop->aros_async_sig;

    /* With no socket fds, an infinite wait can only be released by the async
       signal; if async isn't wired yet, don't hang the loop forever. */
    if (timeout < 0 && asyncmask == 0)
        return;

    if (!loop->aros_timer_open || tr == NULL) {
        if (asyncmask)
            Wait(asyncmask);
        uv__update_time(loop);
        uv__aros_dispatch_dos_pipes(loop, pipe_watchers, npipe);
        uv__aros_dispatch_ttys(loop, tty_watchers, ntty);
        uv__aros_process_reap(loop);
        return;
    }

    port = (struct MsgPort*)loop->aros_timerport;
    timermask = 1UL << port->mp_SigBit;

    if (timeout >= 0) {
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = (ULONG)(timeout / 1000);
        tr->tr_time.tv_micro = (ULONG)((timeout % 1000) * 1000);
        SendIO((struct IORequest*)tr);
    }

    got = Wait(timermask | asyncmask);

    if (timeout >= 0) {
        if (!(got & timermask)) {
            /* Woken early by async: cancel the still-pending timer. */
            if (!CheckIO((struct IORequest*)tr))
                AbortIO((struct IORequest*)tr);
        }
        WaitIO((struct IORequest*)tr);
    }

    /* Dispatch any pending uv_async handles woken by the async signal. */
    if (asyncmask && (got & asyncmask))
        uv__aros_async_drain(loop);

    uv__aros_dispatch_ttys(loop, tty_watchers, ntty);
    uv__aros_dispatch_dos_pipes(loop, pipe_watchers, npipe);
    uv__aros_process_reap(loop);
    uv__update_time(loop);
}
