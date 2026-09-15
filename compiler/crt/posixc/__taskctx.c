/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Per-Task vfork()/exec*() context of posixc.library.

    posixc keeps one base per opener, but a base is NOT one Task's: every
    library entry stores the caller's base register in the calling Task's
    offset-table slot (AROS_GM_STACKCALL), so a pthread worker calling posixc
    through a library that carries its creator's base (uv1.library's spawn
    path in C:Node, for one) runs posixc on the creator's base, concurrently
    with the creator.  Any vfork()/exec*() state kept in base fields is then
    shared between Tasks that are in different states:

    - Which base a child inherits from.  A child base created by vfork() (the
      launcher Task's OpenLibrary) or by an exec*()ed program (its startup's
      OpenLibrary on the same Task) copies the descriptor table, umask and
      environment of the base its parent ran vfork()/exec*() on.  genmodule's
      __GM_GetBaseParent() names the base in the creating Task's per-task
      slot: for the worker above that is the worker's own base (opened for it
      by the pthread thread hook, __threadhook.c), not the creator's base the
      call actually ran on, so the launcher took the plain __init_stdfiles()
      path with a three-entry table and the pretend child's first
      dup2(pipe, 0) failed with EBADF - every spawn from a node Worker thread
      (VS Code's extension host, JOB-0033, TRIG-1084).  A flag on the base
      (VFORK_PARENT/EXEC_PARENT) is no better: it would make an unrelated
      pthread created meanwhile copy the table it should borrow.

    - Whether the Task is a vfork() parent pretending to be its child.  The
      pretend window used to redirect the base's descriptor owner, swap its
      StdCBase, chdir state and upath buffers and set PRETEND_CHILD on the
      base; the creator's Tasks running meanwhile saw the child's table and
      heap through their own calls (an open() landed in the child's table and
      was closed with it, memory crossed heaps: "Semaphore in an illegal
      state" in the launcher, spawn-worker-fix1).

    Both therefore live in posixc-private task-storage slots of the Task that
    is in that state, whichever base its calls reach posixc with:

    - The parent context, set by the parent for exactly as long as a child may
      open posixc against it: vfork() around the launcher's setup, exec*()
      around RunCommand().  Only the intended child reads it: the launcher
      proves itself with the vfork_data it was created with (the creator's
      slot names it as the key), the exec*()ed program runs on the very Task
      that set the slot.  A pthread or Process created by either meanwhile
      matches neither and takes the genmodule lineage as before.

    - The pretend context, the vfork_data of a Task between vfork() and the
      child's _exit()/exec*().  __fd_owner() resolves the child's table for
      such a Task, chdir() records into the child base, getpid()/_exit()/the
      exec*() family act for the child; other Tasks on the same base are not
      affected.
*/

#include <aros/symbolsets.h>
#include <proto/exec.h>
#include <proto/task.h>

#include <dos/dosextens.h>

#include LC_LIBDEFS_FILE

#include "__posixc_intbase.h"
#include "__vfork.h"

static LONG __posixc_parentslot;
static LONG __posixc_pretendslot;

/* The base a child of the calling Task inherits from and how (VFORK_PARENT
   or EXEC_PARENT in *mode; 0 with the genmodule lineage). */
static struct PosixCIntBase *__posixc_getparentbase(struct PosixCIntBase *PosixCBase, int *mode)
{
    struct Task *me = FindTask(NULL);
    struct __posixc_parentctx *ctx = NULL;

    if (__posixc_parentslot)
    {
        /* exec*(): the program runs on the Task that set the slot.  Consumed
           by the first base opened on it: further bases opened on the Task
           while the program runs (a library carrying its own opener) borrow
           that base's table through the per-task lineage, exactly as the
           bases of a program started from the shell do. */
        ctx = (struct __posixc_parentctx *)GetTaskStorageSlot(__posixc_parentslot);
        if (ctx && ctx->task != me)
            ctx = NULL;
        else if (ctx)
            SetTaskStorageSlot(__posixc_parentslot, 0);

        /* vfork(): the launcher was created by the Task that set the slot,
           carrying that slot's vfork_data as its NP_UserData. */
        if (!ctx)
        {
            ctx = (struct __posixc_parentctx *)GetParentTaskStorageSlot(__posixc_parentslot);
            if (ctx && (!ctx->key || ctx->key != me->tc_UserData))
                ctx = NULL;
        }
    }

    if (ctx)
    {
        *mode = ctx->mode;
        return ctx->base;
    }

    *mode = 0;
    return (struct PosixCIntBase *)__GM_GetBaseParent(PosixCBase);
}

IPTR __posixc_setparentctx(struct __posixc_parentctx *ctx)
{
    IPTR old = 0;

    if (__posixc_parentslot)
    {
        old = GetTaskStorageSlot(__posixc_parentslot);
        SetTaskStorageSlot(__posixc_parentslot, (IPTR)ctx);
    }

    return old;
}

void __posixc_restoreparentctx(IPTR old)
{
    if (__posixc_parentslot)
        SetTaskStorageSlot(__posixc_parentslot, old);
}

struct vfork_data *__posixc_pretending(void)
{
    if (!__posixc_pretendslot)
        return NULL;

    return (struct vfork_data *)GetTaskStorageSlot(__posixc_pretendslot);
}

void __posixc_setpretending(struct vfork_data *udata)
{
    if (__posixc_pretendslot)
        SetTaskStorageSlot(__posixc_pretendslot, (IPTR)udata);
}

/* Resolved once, first thing at OpenLib, so the exec*() context is consumed
   by exactly one base and every later hook sees the same answer. */
static int __posixc_taskctx_open(struct PosixCIntBase *PosixCBase)
{
    PosixCBase->parentbase = __posixc_getparentbase(PosixCBase, &PosixCBase->parentmode);

    return 1;
}

static int __posixc_taskctx_init(struct PosixCIntBase *PosixCBase)
{
    if (!__posixc_parentslot)
        __posixc_parentslot = AllocTaskStorageSlot();
    if (!__posixc_pretendslot)
        __posixc_pretendslot = AllocTaskStorageSlot();

    return __posixc_parentslot && __posixc_pretendslot;
}

static void __posixc_taskctx_expunge(struct PosixCIntBase *PosixCBase)
{
    if (__posixc_parentslot)
    {
        FreeTaskStorageSlot(__posixc_parentslot);
        __posixc_parentslot = 0;
    }
    if (__posixc_pretendslot)
    {
        FreeTaskStorageSlot(__posixc_pretendslot);
        __posixc_pretendslot = 0;
    }
}

ADD2INITLIB(__posixc_taskctx_init, 0);
ADD2OPENLIB(__posixc_taskctx_open, -110);
ADD2EXPUNGELIB(__posixc_taskctx_expunge, 0);
