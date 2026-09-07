/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Give pthread worker Tasks a per-task C runtime base.

    posixc.library uses the "pertaskbase" option: its rel-linked call wrappers
    read the calling Task's own library base out of per-task storage
    (GetTaskStorageSlot) and jump through it directly, WITHOUT the slow
    __GM_GetBase() path that would create one on demand.  The main Task's slot
    is populated by the program's C startup; a pthread worker never runs that
    startup, so its slot stays NULL and the first CRT call (e.g. read()) jumps
    through a NULL base and bus-faults.

    pthread.library is C-runtime agnostic and can't know which CRT a program
    uses, so we invert the dependency with a loosely-coupled registry found by
    name in the public semaphore list: posixc registers a per-thread
    enter/leave hook here, and pthread's StarterFunc runs the registered hooks
    in each new worker.  The enter hook simply opens posixc.library for the
    worker: the pertaskbase OpenLib inherits the creator's base through
    GetParentTaskStorageSlot (the worker's et_Parent is its creator), gives the
    worker its own distinct base (own errno/stdio), cascades to posixc's
    rel-libraries (stdc/stdcio), and runs __init_fd() - which detects the
    CLI-less worker Task and routes its descriptors to the creator Process.

    The registry itself lives in compiler/crt/stdc/__crt_threadhooks.h (its
    contract is duplicated in compiler/pthread/pthread_create.c).

    Which base the worker's CRT calls actually reach depends on how the program
    is linked.  A program linked against the *_rel linklibs (relbases.o) fetches
    its posixc base per Task and so uses the base opened here.  A program linked
    against the absolute linklib jumps through its single, process-wide
    PosixCBase, and the library-side stack-call thunk stores that base in the
    calling Task's offset-table slot on EVERY call (AROS_GM_STACKCALL in
    aros/genmodule.h).  So after a worker's first CRT call its slot names the
    creator's base, not the one opened here.  The leave hook must therefore
    close the base it recorded at enter, never "whatever the slot holds now":
    closing the creator's base from a worker drops the creator's
    taskopencount to zero and tears down its descriptor table, stdio and
    internal pool while the creator is still running.
*/

#include <aros/debug.h>

#include LC_LIBDEFS_FILE

#include <proto/exec.h>
#include <proto/task.h>
#include <proto/stdc.h>
#include <proto/stdcio.h>
#include <aros/symbolsets.h>

#include "__posixc_intbase.h"
#include "__crt_threadhooks.h"

static struct CRTThreadHook __posixc_threadhook;

/* Per-Task record of the base posixc_thread_enter() opened, kept in a slot of
   our own: the offset-table slot is rewritten by every stack-call thunk. */
static LONG __posixc_threadslot;

static void posixc_thread_enter(void)
{
    /* Runs in a freshly-created pthread worker: give it its own per-task
       posixc base (inherited from the creator, descriptors routed there) and
       remember which base that was. */
    struct Library *base = OpenLibrary((STRPTR)"posixc.library", 0);

    if (base && __posixc_threadslot)
        SetTaskStorageSlot(__posixc_threadslot, (IPTR)base);
}

static void posixc_thread_leave(void)
{
    /* Runs in the worker at thread exit: release exactly the base opened
       above.  Do not consult __aros_getbase_PosixCBase(): for an absolute
       linked program it names the creator's base by now (see the header). */
    struct Library *base = NULL;

    if (__posixc_threadslot)
    {
        base = (struct Library *)GetTaskStorageSlot(__posixc_threadslot);
        SetTaskStorageSlot(__posixc_threadslot, (IPTR)NULL);
    }
    if (base)
        CloseLibrary(base);
}

static int __posixc_threadhook_init(struct PosixCIntBase *PosixCBase)
{
    if (!__posixc_threadslot)
        __posixc_threadslot = AllocTaskStorageSlot();

    /* posixc.library is a single system-wide library; the registry helper
       adds the hook exactly once even if this init runs again.  Failure is
       non-fatal: workers simply won't get a CRT base. */
    __posixc_threadhook.th_Enter = posixc_thread_enter;
    __posixc_threadhook.th_Leave = posixc_thread_leave;
    __crt_add_threadhook(&__posixc_threadhook);

    return TRUE;
}

static void __posixc_threadhook_expunge(struct PosixCIntBase *PosixCBase)
{
    __crt_rem_threadhook(&__posixc_threadhook);

    if (__posixc_threadslot)
    {
        FreeTaskStorageSlot(__posixc_threadslot);
        __posixc_threadslot = 0;
    }
}

ADD2INITLIB(__posixc_threadhook_init, 0);
ADD2EXPUNGELIB(__posixc_threadhook_expunge, 0);

/*
 * Publish posixc's own per-task stdc/stdcio rellib bases in the public base
 * fields.  __posixc_startup() (which normally does this) is only linked into
 * the absolute autoinit, so a rel-linked program never runs it and posixc's
 * public StdCBase/StdCIOBase stay NULL.  posixc uses StdCBase both directly
 * (its rel wrappers) and through the public field (the errno macro), and
 * __posixc_nixmain() registers the program exit jmp_buf through it - so the
 * public field must name the very same per-task stdc base posixc uses
 * internally, otherwise a rel-linked program's exit()/errno resolve a
 * different base (bus fault "exit without proper initialization").
 *
 * This runs as an OpenLib hook, after set_open_rellibraries() has populated
 * posixc's rellib bases, so __aros_getbase_StdCBase()/StdCIOBase() here return
 * posixc's own per-task bases.  A rel-linked program's relbases.o then adopts
 * these via PosixCBase->StdCBase rather than opening its own.  The absolute
 * path is unchanged: __posixc_startup() still overwrites the fields with the
 * program globals afterwards, and we only fill them when NULL.
 */
static int __posixc_publish_stdbases(struct PosixCIntBase *PosixCBase)
{
    /* posixc reaches stdc/stdcio through the public StdCBase/StdCIOBase fields
       (its rel wrappers read them, and so does the errno macro).  Normally
       __posixc_startup() wires them to the program's bases, but that only runs
       in the absolute autoinit; a rel-linked program never runs it, leaving
       them NULL and faulting on the first stdc/stdcio call.  Open our own
       per-task bases and publish them; a rel-linked program's relbases.o then
       adopts these so posixc and the program share one stdc/stdcio base.  In
       the absolute case __posixc_startup() overwrites the fields afterwards, so
       behaviour there is unchanged. */
    if (PosixCBase->PosixCBase.StdCBase == NULL)
        PosixCBase->PosixCBase.StdCBase =
            (struct StdCBase *)OpenLibrary((CONST_STRPTR)"stdc.library", 0);
    if (PosixCBase->PosixCBase.StdCIOBase == NULL)
        PosixCBase->PosixCBase.StdCIOBase =
            (struct StdCIOBase *)OpenLibrary((CONST_STRPTR)"stdcio.library", 0);

    return TRUE;
}

/* High priority so the public bases are set before other OpenLib hooks
   (e.g. __init_fd/__init_stdio) that touch errno/stdio. */
ADD2OPENLIB(__posixc_publish_stdbases, 90);
