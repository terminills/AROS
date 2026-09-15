/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    uv_aros_task_has_base -- can the CALLING task use uv1.library at all?

    uv1.library is a pertaskbase module: every internal call it makes into
    its rellibs (stdc, posixc) resolves the base through the current task's
    storage slot, not through the base the caller jumped in with.  A task
    that never opened uv1 (and whose creator did not either) therefore
    faults inside the first relwrapper it reaches, with CR2 = the rellib
    offset inside a NULL base.

    A caller that may be running on such a task - a library expunge, which
    lddemon runs on whichever task's AllocMem failed - needs to ask BEFORE
    calling anything else, and it cannot simply OpenLibrary("uv1.library")
    to give itself a base: LDFlush holds exec's LibList write lock across
    RemLibrary()/Expunge, and OpenLibrary takes that same lock in read mode
    (self-deadlock, spinning in KrnSpinLock on SMP; JOB-0092, pc-x86_64
    guest freeze 2026-09-14).

    This entry point is safe to call from any task: it only reads the slot
    through exec (an absolute SysBase), never a rellib.  Nothing in this
    file may call into stdc/posixc.

    The same LDFlush path is also why uv1.library vetoes its own expunge
    from any task but its last closer (hooks below).  uv1 links the static
    pthread emulation, whose EXIT-set function __pthread_Exit_Func reaps
    leftover threads with Delay(1)/pthread_join - a Wait() under exec's
    LibList WRITE lock when the expunge comes from LDFlush.  Every later
    OpenLibrary then spins under Forbid in LDRequestObject, and if the
    sleeper is pinned to the spinner's CPU it never wakes: the whole guest
    stops taking input (node.library did exactly this, rtest6 2026-09-14,
    qemu-test/logs/rtest6-deadlock.md).  The CLOSELIB set runs on the last
    per-task close, just before genmodule drops the root open count and,
    when LIBF_DELEXP is set, expunges from that same task with lddemon's
    Forbid only (no list lock).  That task is remembered by its exec unique
    ID (task pointers are recycled), and only it may expunge.  A stale
    record can only ever match the same still-living task, which is the
    same safe path.
*/
#include <aros/symbolsets.h>
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>

#include "aros-intbase.h"

/* genmodule's per-task base lookup for this module (own slot, then the
   creator's slot).  Emitted hidden into uv1_start.c; the same module may
   use it. */
extern char *__aros_getoffsettable(void);

int uv_aros_task_has_base(void)
{
    return __aros_getoffsettable() != NULL;
}

/*
 * uv_default_loop() storage for the calling task's process: the per-task
 * base (see aros-intbase.h).  src/uv-common.c reaches its two upstream
 * statics through these, so a Node process and the C:Node it spawned no
 * longer share one loop.  A task without any base (only an expunge sweep
 * running on a foreign task ever gets here without one) falls back to a
 * guest-wide pair rather than dereferencing NULL; such a task cannot run a
 * loop anyway, every relwrapper it reached would fault first.
 */
static uv_loop_t  uv_aros_orphan_default_loop_struct;
static uv_loop_t *uv_aros_orphan_default_loop_ptr;

uv_loop_t **uv_aros_default_loop_ptr(void)
{
    struct UV1IntBase *base = (struct UV1IntBase *)__aros_getoffsettable();

    if (base == NULL)
        return &uv_aros_orphan_default_loop_ptr;
    return &base->default_loop_ptr;
}

uv_loop_t *uv_aros_default_loop_struct(void)
{
    struct UV1IntBase *base = (struct UV1IntBase *)__aros_getoffsettable();

    if (base == NULL)
        return &uv_aros_orphan_default_loop_struct;
    return &base->default_loop_struct;
}

static ULONG uv_aros_closer_id;

static int uv_aros_close_last(struct Library *lh)
{
    (void)lh;
    uv_aros_closer_id = GetETaskID(FindTask(NULL));
    return 1;
}

static int uv_aros_expunge_prepare(struct Library *lh)
{
    ULONG closer = uv_aros_closer_id;

    (void)lh;
    uv_aros_closer_id = 0;
    return closer != 0 && closer == GetETaskID(FindTask(NULL));
}

ADD2CLOSELIB(uv_aros_close_last, 0)
ADD2EXPUNGELIB(uv_aros_expunge_prepare, 0)
