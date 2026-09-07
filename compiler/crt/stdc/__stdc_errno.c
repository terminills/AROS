/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Per-Task errno for Tasks that call through another Task's StdCBase.

    errno lives in StdCBase->_errno, and stdc.library hands out one base per
    opener.  That is per-Task only as long as every Task calls through a base
    it opened itself, which a pthread worker in an absolutely linked program
    does not: its linklib stubs jump through the program's single, global
    StdCBase (see compiler/crt/posixc/__threadhook.c for the full story), so
    every thread would share the main Task's errno.  A worker then sees the
    EEXIST/EAGAIN of whatever another thread just did, and its own error is
    clobbered before it gets to read it.

    So __stdc_geterrnoptr() hands the base's _errno only to the Task that
    opened the base (StdCIntBase->errno_task) and gives every other Task a
    small cell of its own, kept in a stdc-private task-storage slot and
    allocated on first use.  The cell is released by the CRT thread-hook
    registry's leave hook when a pthread worker exits.  A Task that is not a
    pthread worker but still calls the CRT through a foreign base keeps its
    cell until it dies (a few bytes, once per such Task).
*/

#include <aros/symbolsets.h>
#include <proto/exec.h>
#include <proto/task.h>

#include "__stdc_intbase.h"
#include "__crt_threadhooks.h"

static LONG __stdc_errnoslot;
static struct CRTThreadHook __stdc_threadhook;

/* Called by __stdc_geterrnoptr() when the calling Task is not the base's
   opener.  Falls back to the base's errno (shared, as before) if a cell can
   not be provided, so errno never becomes a NULL pointer. */
int *__stdc_taskerrnoptr(struct StdCIntBase *StdCBase)
{
    int *cell = NULL;

    if (__stdc_errnoslot)
    {
        cell = (int *)GetTaskStorageSlot(__stdc_errnoslot);
        if (!cell)
        {
            cell = AllocMem(sizeof(*cell), MEMF_PUBLIC | MEMF_CLEAR);
            if (cell && !SetTaskStorageSlot(__stdc_errnoslot, (IPTR)cell))
            {
                FreeMem(cell, sizeof(*cell));
                cell = NULL;
            }
        }
    }

    return cell ? cell : &StdCBase->StdCBase._errno;
}

static void stdc_thread_leave(void)
{
    /* Runs in a pthread worker at thread exit: drop its errno cell. */
    int *cell;

    if (!__stdc_errnoslot)
        return;

    cell = (int *)GetTaskStorageSlot(__stdc_errnoslot);
    if (cell)
    {
        SetTaskStorageSlot(__stdc_errnoslot, (IPTR)NULL);
        FreeMem(cell, sizeof(*cell));
    }
}

static int __stdc_errno_init(struct StdCIntBase *StdCBase)
{
    if (!__stdc_errnoslot)
        __stdc_errnoslot = AllocTaskStorageSlot();

    /* Non-fatal when the registry is unavailable: cells then simply live
       as long as their Task. */
    __stdc_threadhook.th_Enter = NULL;
    __stdc_threadhook.th_Leave = stdc_thread_leave;
    __crt_add_threadhook(&__stdc_threadhook);

    return TRUE;
}

static void __stdc_errno_expunge(struct StdCIntBase *StdCBase)
{
    __crt_rem_threadhook(&__stdc_threadhook);

    if (__stdc_errnoslot)
    {
        FreeTaskStorageSlot(__stdc_errnoslot);
        __stdc_errnoslot = 0;
    }
}

/* Every opener gets its own base; remember who that was so that
   __stdc_geterrnoptr() can tell the opener from a Task borrowing the base. */
static int __stdc_errno_open(struct StdCIntBase *StdCBase)
{
    StdCBase->errno_task = FindTask(NULL);

    return TRUE;
}

ADD2INITLIB(__stdc_errno_init, 0);
ADD2EXPUNGELIB(__stdc_errno_expunge, 0);
/* Before every other OpenLib hook, some of which may already set errno. */
ADD2OPENLIB(__stdc_errno_open, -100);
