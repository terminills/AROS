/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: One errno per Task, whichever StdCBase the Task calls through.

    stdc.library hands out one base per opener, and the public StdCBase
    carries an _errno field.  Neither is a usable errno home:

    - A pthread worker in an absolutely linked program never opens a base;
      its linklib stubs jump through the program's single, global StdCBase
      (see compiler/crt/posixc/__threadhook.c for the full story).  Every
      thread would share the main Task's errno: a thread reading errno after
      readdir() returned NULL could see the EEXIST of another thread's
      mkdir(), and its own error could be clobbered before it got to read
      it.

    - One Task can also hold SEVERAL bases.  A pertaskbase module with
      "rellib stdc" (posixc, uv1, z1, node) opens stdc on the task that opens
      the module, so a C:Node process owns one stdc base per such module.
      Giving each base its own _errno then splits errno by call path: posixc's
      fopen() sets ENOENT through posixc's base, and the caller reading errno
      through node's base sees 0 (OpenSSL's config loader took that "No
      error" for a fatal error, JOB-0033 spawn-env-control7, 2026-09-15).

    So errno is a small cell of the calling Task, kept in a stdc-private
    task-storage slot, allocated on first use and handed to every base the
    Task opens.  The cell counts the bases that use it: it is released when
    the last of them closes on its opener, or - for a Task that only ever
    borrowed foreign bases - by the CRT thread-hook registry's leave hook
    when a pthread worker exits.  A plain Task that never opened stdc itself
    keeps its cell until it dies (a few bytes, once per such Task).  The
    public _errno field only backs a Task whose cell could not be allocated.
*/

#include <aros/symbolsets.h>
#include <proto/exec.h>
#include <proto/task.h>

#include "__stdc_intbase.h"
#include "__crt_threadhooks.h"

struct StdCErrnoCell
{
    int     value;
    ULONG   bases;      /* stdc bases of the owning Task using this cell */
};

static LONG __stdc_errnoslot;
static struct CRTThreadHook __stdc_threadhook;

/* The calling Task's cell, made on first use. */
static struct StdCErrnoCell *__stdc_errno_taskcell(BOOL create)
{
    struct StdCErrnoCell *cell;

    if (!__stdc_errnoslot)
        return NULL;

    cell = (struct StdCErrnoCell *)GetTaskStorageSlot(__stdc_errnoslot);
    if (!cell && create)
    {
        cell = AllocMem(sizeof(*cell), MEMF_PUBLIC | MEMF_CLEAR);
        if (cell && !SetTaskStorageSlot(__stdc_errnoslot, (IPTR)cell))
        {
            FreeMem(cell, sizeof(*cell));
            cell = NULL;
        }
    }

    return cell;
}

/* Called by __stdc_geterrnoptr().  Falls back to the base's _errno if a
   cell can not be provided, so errno never becomes a NULL pointer. */
int *__stdc_taskerrnoptr(struct StdCIntBase *StdCBase)
{
    struct StdCErrnoCell *cell = __stdc_errno_taskcell(TRUE);

    return cell ? &cell->value : &StdCBase->StdCBase._errno;
}

static void stdc_thread_leave(void)
{
    /* Runs in a pthread worker at thread exit: drop its errno cell unless a
       base the worker opened still uses it - that base's close drops it. */
    struct StdCErrnoCell *cell = __stdc_errno_taskcell(FALSE);

    if (cell && cell->bases == 0)
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

/* Every opener gets its own base; bind it to the opener's cell so that all
   of one Task's bases, and every foreign base it borrows, share one errno. */
static int __stdc_errno_open(struct StdCIntBase *StdCBase)
{
    struct StdCErrnoCell *cell = __stdc_errno_taskcell(TRUE);

    StdCBase->errno_task = FindTask(NULL);
    StdCBase->errno_cell = cell;
    if (cell)
        cell->bases++;

    return TRUE;
}

static int __stdc_errno_close(struct StdCIntBase *StdCBase)
{
    struct StdCErrnoCell *cell = StdCBase->errno_cell;

    if (!cell)
        return TRUE;

    StdCBase->errno_cell = NULL;
    if (--cell->bases == 0)
    {
        /* Only the opener can clear its own slot.  A base closed by some
           other Task (an expunge sweep) leaves the cell to its owner, which
           reuses it on its next open or takes it to the grave: a stale
           slot pointer must never be freed under the owner. */
        if (StdCBase->errno_task == FindTask(NULL)
            && __stdc_errno_taskcell(FALSE) == cell)
        {
            SetTaskStorageSlot(__stdc_errnoslot, (IPTR)NULL);
            FreeMem(cell, sizeof(*cell));
        }
    }

    return TRUE;
}

ADD2INITLIB(__stdc_errno_init, 0);
ADD2EXPUNGELIB(__stdc_errno_expunge, 0);
/* Before every other OpenLib hook, some of which may already set errno; the
   CloseLib set runs in reverse, so the same priority puts the close last. */
ADD2OPENLIB(__stdc_errno_open, -100);
ADD2CLOSELIB(__stdc_errno_close, -100);
