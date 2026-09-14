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
*/
#include <exec/types.h>
#include <proto/exec.h>

/* genmodule's per-task base lookup for this module (own slot, then the
   creator's slot).  Emitted hidden into uv1_start.c; the same module may
   use it. */
extern char *__aros_getoffsettable(void);

int uv_aros_task_has_base(void)
{
    return __aros_getoffsettable() != NULL;
}
