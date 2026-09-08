/*
    AROS uv_async implementation for the libuv port.

    Replaces libuv's fd-based async wakeup (eventfd / self-pipe) with AROS exec
    signals. uv_async_send() sets the handle's atomic pending flag and Signal()s
    the loop task's async signal bit; the custom event-loop backend
    (src/unix/aros.c uv__io_poll) calls uv__aros_async_drain() when that signal
    arrives. This is required for uv_loop_init to succeed -- the loop creates an
    internal work-queue async handle (loop->wq_async) -- and is the wakeup path
    for threadpool completions and cross-thread uv_async_send().

    The queue-drain and spin logic mirror libuv's async.c verbatim; only the
    wakeup primitive differs.
*/
#include "uv.h"
#include "internal.h"

#include <proto/exec.h>
#include <exec/tasks.h>

#include <stdatomic.h>

/* Called by the backend (aros.c) when the loop's async signal fires. */
void uv__aros_async_drain(uv_loop_t* loop);

static void uv__async_spin(uv_async_t* handle) {
    _Atomic int* pending = (_Atomic int*) &handle->pending;
    _Atomic int* busy = (_Atomic int*) &handle->u.fd;
    int i;

    /* Set pending first so no new events are added after this returns. */
    atomic_store(pending, 1);

    for (;;) {
        for (i = 0; i < 997; i++) {
            if (atomic_load(busy) == 0)
                return;
            /* Another thread holds the handle briefly; spin. */
        }
        /* Give the other task a chance to finish its critical section. */
        Forbid();
        Permit();
    }
}

static int uv__async_start(uv_loop_t* loop) {
    BYTE sig;

    if (loop->aros_async_sig >= 0)
        return 0;

    sig = AllocSignal(-1);
    if (sig == -1)
        return UV_EMFILE;

    loop->aros_async_sig = sig;
    if (loop->aros_task == NULL)
        loop->aros_task = FindTask(NULL);
    return 0;
}

int uv_async_init(uv_loop_t* loop, uv_async_t* handle, uv_async_cb async_cb) {
    int err = uv__async_start(loop);
    if (err)
        return err;

    uv__handle_init(loop, (uv_handle_t*)handle, UV_ASYNC);
    handle->async_cb = async_cb;
    handle->pending = 0;
    handle->u.fd = 0;   /* used as a busy flag */

    uv__queue_insert_tail(&loop->async_handles, &handle->queue);
    uv__handle_start(handle);

    return 0;
}

int uv_async_send(uv_async_t* handle) {
    _Atomic int* pending = (_Atomic int*) &handle->pending;
    _Atomic int* busy = (_Atomic int*) &handle->u.fd;
    uv_loop_t* loop = handle->loop;

    if (atomic_load_explicit(pending, memory_order_relaxed) != 0)
        return 0;

    atomic_fetch_add(busy, 1);

    if (atomic_exchange(pending, 1) == 0) {
        if (loop->aros_task != NULL && loop->aros_async_sig >= 0)
            Signal((struct Task*)loop->aros_task, 1UL << loop->aros_async_sig);
    }

    atomic_fetch_add(busy, -1);
    return 0;
}

void uv__aros_async_drain(uv_loop_t* loop) {
    struct uv__queue queue;
    struct uv__queue* q;
    uv_async_t* h;
    _Atomic int* pending;

    uv__queue_move(&loop->async_handles, &queue);
    while (!uv__queue_empty(&queue)) {
        q = uv__queue_head(&queue);
        h = uv__queue_data(q, uv_async_t, queue);

        uv__queue_remove(q);
        uv__queue_insert_tail(&loop->async_handles, q);

        pending = (_Atomic int*) &h->pending;
        if (atomic_exchange(pending, 0) == 0)
            continue;
        if (h->async_cb == NULL)
            continue;
        h->async_cb(h);
    }
}

void uv__async_close(uv_async_t* handle) {
    uv__async_spin(handle);
    uv__queue_remove(&handle->queue);
    uv__handle_stop(handle);
}

void uv__async_stop(uv_loop_t* loop) {
    if (loop->aros_async_sig >= 0) {
        FreeSignal(loop->aros_async_sig);
        loop->aros_async_sig = -1;
    }
}

int uv__async_fork(uv_loop_t* loop) {
    (void)loop;
    return 0;
}
