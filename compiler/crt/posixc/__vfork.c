/*
    Copyright (C) 2008-2026, The AROS Development Team. All rights reserved.
*/

#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/stdc.h>
#include <exec/exec.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <libraries/stdc.h>
#include <aros/cpu.h>
#include <aros/startup.h>

#include <sys/types.h>
#include <unistd.h>
#include <setjmp.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include "__posixc_intbase.h"
#include "__posixc_env.h"
#include "__fdesc.h"
#include "__vfork.h"
#include "__exec.h"


#define VFORK_USE_INLINECOPY
#if defined(VFORK_USE_INLINECOPY)
/* NB: use (IPTR)(a)/(IPTR)(b), NOT (IPTR)(&a)/(&b).  jmp_buf is an array type
 * (struct __jmp_buf[1]), so a jmp_buf function parameter (e.g. __vfork's 'env')
 * decays to a pointer; &env is then the address of the pointer VARIABLE (stack
 * garbage), not the jmp_buf it points to.  An array argument decays to its data
 * address and a pointer argument already is that address, so dropping the '&'
 * is correct for every caller (arrays and decayed pointers alike).  With '&'
 * the saved context copied from a decayed 'env' was corrupt, so the subsequent
 * vfork_longjmp() jumped through a bad frame -> SP out of stack limits / SIGSEGV
 * whenever a program (e.g. git) called fork(). */
#define _VFORK_COPYENV(a,b) \
({ \
    int _i; \
    for (_i = 0; _i < sizeof(jmp_buf); _i++) \
        *((UBYTE *)((IPTR)(a) + _i)) = *((UBYTE *)((IPTR)(b) + _i)); \
})
#else
#define _VFORK_COPYENV(a,b)      *(a) = *(b)
#endif

/*****************************************************************************

    NAME
#include <unistd.h>

        pid_t vfork(

    SYNOPSIS
        void)

    FUNCTION
        Function to create a subprocess of the current process.

        This is there to ease porting of software using the fork()/vfork()
        POSIX functions. Due to a different memory and process model, fork()
        is not implemented at the moment in the C library. vfork() is provided
        with some extended functionality. In the POSIX standard the only
        guaranteed functionality for vfork() is to have an exec*() function or
        exit() called right after the vfork() in the child.

        Extra functionality for vfork():
        - The child has its own memory heap; memory allocation/deallocation
          is allowed and the heap will be removed when calling _exit() or will
          be used for the code started by the exec*() functions.
        - The child will have a copy of the file descriptors as specified by
          the POSIX standard for the fork() function. File I/O is possible in
          the child, as is file manipulation with dup() etc.

        Difference with fork():
        - The virtual memory heap is not duplicated as in POSIX but the memory
          is shared between parent and child. AROS lives in one big single
          memory region so changes to memory in the child are also seen by the
          parent.

        Behavior for other resources not described in this doc may not be
        relied on for future compatibility.

    INPUTS
        -

    RESULT
        -1: error, no child is started; errno will be set.
        0: Running in child
        >0: Running in parent, pid of child is return value.

    NOTES
        Current implementation of vfork() will only really start running things
        in parallel on an exec*() call. After vfork(), child code will run until
        _exit() or exec*(). With _exit(), the child will exit and the parent
        will continue; with exec*(), the child will be detached and the parent
        will continue.

    EXAMPLE

    BUGS

    SEE ALSO
        execl(), execve(), execlp(), execv(), execvp()

    INTERNALS

******************************************************************************/

/* The following functions are used to update the child's and parent's privdata
   for the parent pretending to be running as child and for the child to take
   over. It is called in the following sequence:
   parent_enterpretendchild() is called in vfork() so the parent pretends to be
   running as child; child_takeover() is called by child if exec*() so it can
   continue from the parent state; parent_leavepretendchild() is called by
   parent to switch back to be running as parent
*/
static void parent_enterpretendchild(struct vfork_data *udata);
static void child_takeover(struct vfork_data *udata);
static void parent_leavepretendchild(struct vfork_data *udata);
static void parent_createchild(struct vfork_data *udata);

static __attribute__((noinline)) void __vfork_exit_controlled_stack(struct vfork_data *udata);

LONG launcher()
{
    D(bug("launcher: Entered child launcher, ThisTask=%p\n", FindTask(NULL)));

    struct Task *this = FindTask(NULL);
    struct vfork_data *udata = this->tc_UserData;
    BYTE child_signal;
    struct PosixCIntBase *PosixCBase = NULL;
    jmp_buf exec_exitjmp; /* jmp_buf for when calling __exec_do */
    int exec_error; /* errno for when calling __exec_do */
    LONG ret = 0;

    /* Allocate signal for parent->child communication */
    child_signal = udata->child_signal = AllocSignal(-1);
    D(bug("launcher: Allocated child signal: %d\n", udata->child_signal));
    if (udata->child_signal == -1)
    {
        /* Lie */
        udata->child_errno = ENOMEM;
        SETCHILDSTATE(CHILD_STATE_SETUP_FAILED);
        Signal(udata->parent, 1 << udata->parent_signal);
        return -1;
    }

    /* TODO: Can we avoid opening posixc.library for child process? */
    PosixCBase = (struct PosixCIntBase *)OpenLibrary((STRPTR) "posixc.library", 0);
    if (PosixCBase)
    {
        PosixCBase->PosixCBase.StdCBase = (struct StdCBase *)OpenLibrary((STRPTR) "stdc.library", 0);
        /* posixc_startup wires StdCIOBase for regularly-started programs; this
           launcher Task has no such startup, and __exec_prepare reaches it via
           __posixc_get_environptr (which crashed on a NULL StdCIOBase). */
        PosixCBase->PosixCBase.StdCIOBase = (struct StdCIOBase *)OpenLibrary((STRPTR) "stdcio.library", 0);
        if (!PosixCBase->PosixCBase.StdCBase || !PosixCBase->PosixCBase.StdCIOBase)
        {
            if (PosixCBase->PosixCBase.StdCIOBase)
                CloseLibrary((struct Library *)PosixCBase->PosixCBase.StdCIOBase);
            if (PosixCBase->PosixCBase.StdCBase)
                CloseLibrary((struct Library *)PosixCBase->PosixCBase.StdCBase);
            CloseLibrary((struct Library *)PosixCBase);
            PosixCBase = NULL;
        }
    }
    if (!PosixCBase)
    {
        D(bug("launcher:Failed to open libraries!\n"));
        FreeSignal(child_signal);
        udata->child_errno = ENOMEM;
        SETCHILDSTATE(CHILD_STATE_SETUP_FAILED);
        Signal(udata->parent, 1 << udata->parent_signal);
        return -1;
    }
    D(bug("launcher: Opened PosixCBase: %x, StdCBase: %x\n",
          PosixCBase, PosixCBase->PosixCBase.StdCBase
    ));

    udata->child_posixcbase = PosixCBase;

    if (setjmp(exec_exitjmp) == 0)
    {
        /* Setup complete, signal parent */
        D(bug("launcher: Signaling parent that we finished setup\n"));
        SETCHILDSTATE(CHILD_STATE_SETUP_FINISHED);
        Signal(udata->parent, 1 << udata->parent_signal);

        D(bug("launcher: Child waiting for exec or exit\n"));
        Wait(1 << udata->child_signal);
        ASSERTPARENTSTATE(PARENT_STATE_EXEC_CALLED | PARENT_STATE_EXIT_CALLED);
        PRINTSTATE;

        if (udata->child_executed)
        {
            APTR exec_id;

            child_takeover(udata);

            /* Filenames passed from parent obey parent's doupath */

            PosixCBase->doupath = udata->parent_posixcbase->doupath;
            D(bug("launcher: doupath == %d for __exec_prepare()\n", PosixCBase->doupath));

            exec_id = udata->exec_id = __exec_prepare(
                udata->exec_filename,
                0,
                udata->exec_argv,
                udata->exec_envp
            );

            /* Reset handling of upath */
            PosixCBase->doupath = 0;
            udata->child_errno = errno;

            D(bug("launcher: informing parent that we have run __exec_prepare\n"));
            /* Inform parent that we have run __exec_prepare */
            SETCHILDSTATE(CHILD_STATE_EXEC_PREPARE_FINISHED);
            Signal(udata->parent, 1 << udata->parent_signal);

            /* Wait 'till __exec_do() is called on parent process */
            D(bug("launcher: Waiting parent to get the result\n"));
            Wait(1 << udata->child_signal);
            ASSERTPARENTSTATE(PARENT_STATE_EXEC_DO_FINISHED);
            PRINTSTATE;

            D(bug("launcher: informing parent that we won't use udata anymore\n"));
            /* Inform parent that we won't use udata anymore */
            SETCHILDSTATE(CHILD_STATE_UDATA_NOT_USED);
            Signal(udata->parent, 1 << udata->parent_signal);

            D(bug("launcher: waiting for parent to be after _exit()\n"));
            Wait(1 << udata->child_signal);
            ASSERTPARENTSTATE(PARENT_STATE_STOPPED_PRETENDING);
            PRINTSTATE;

            if (exec_id)
            {
                D(bug("launcher: catch _exit()\n"));
                __stdc_program_startup(exec_exitjmp, &exec_error);

D(bug("launcher: executing command\n"));
                __exec_do(exec_id);

                assert(0); /* Should not be reached */
            }
            else
            {
                D(bug("launcher: __exec_prepare returned with an error\n"));
                ret = -1;
            }
        }
        else /* !udata->child_executed */
        {
            D(bug("launcher: child not executed\n"));

            D(bug("launcher: informing parent that we won't use udata anymore\n"));
            /* Inform parent that we won't use udata anymore */
            SETCHILDSTATE(CHILD_STATE_UDATA_NOT_USED);
            Signal(udata->parent, 1 << udata->parent_signal);

            /* The parent is still pretending to be us: its descriptor calls
               resolve to OUR table until parent_leavepretendchild() runs, and
               its fflush(NULL) on the way there writes through it.  Hold our
               libraries open until it has stopped, exactly as the executed
               path above does - closing them first left the parent calling
               through a freed base (jump to PC 0 in C:Node's second
               execFile(), a bare-name exec that failed PATH lookup). */
            D(bug("launcher: waiting for parent to be after _exit()\n"));
            Wait(1 << udata->child_signal);
            ASSERTPARENTSTATE(PARENT_STATE_STOPPED_PRETENDING);
            PRINTSTATE;
        }
    }
    else
    {
        struct ETask *etask = GetETask(FindTask(NULL));

        /* child exited */
        D(bug("launcher: catched _exit(), errno=%d\n", exec_error));
        /* Return the exec'd program's status to the parent: waitpid()
           reads it from our ETask. */
        ret = exec_error;
        if (etask)
            etask->et_Result1 = exec_error;
    }

    D(bug("launcher: freeing child_signal\n"));
    FreeSignal(child_signal);

    D(bug("launcher: closing libraries\n"));
    CloseLibrary((struct Library *)PosixCBase->PosixCBase.StdCIOBase);
    CloseLibrary((struct Library *)PosixCBase->PosixCBase.StdCBase);
    CloseLibrary((struct Library *)PosixCBase);
    
    D(bug("Child Done\n"));

    return ret;
}

/* This can be good for debugging */
#ifdef __arm__
#define SP 8
#define ALT 27
#endif

#ifndef SP
#define SP  _JMPLEN - 1
#define ALT _JMPLEN - 1
#endif

pid_t __vfork(jmp_buf env)
{
    struct PosixCIntBase *PosixCBase =
        (struct PosixCIntBase *)__aros_getbase_PosixCBase();
    struct Task *this = FindTask(NULL);
    struct ETask *etask = NULL;
    struct vfork_data *udata = AllocMem(sizeof(struct vfork_data), MEMF_ANY | MEMF_CLEAR);
    if (udata == NULL)
    {
        errno = ENOMEM;
        vfork_longjmp(env, -1);
    }
    D(bug("__vfork: Parent: allocated udata %p, jmp_buf %p\n", udata, udata->vfork_jmp));
    _VFORK_COPYENV(udata->vfork_jmp,env);

    D(bug("__vfork: Parent: initial jmp_buf %p\n", env));
    D(bug("__vfork: Parent: ip: %p, stack: %p, alt: 0x%p\n", env->retaddr, env->regs[SP], env->regs[ALT]));
    D(bug("__vfork: Parent: Current altstack 0x%p\n", *((void **) this->tc_SPLower)));
    D(hexdump(env, 0, sizeof(jmp_buf) + sizeof(void *) * 4));

    udata->parent = this;
    udata->prev = __posixc_pretending();

    D(bug("__vfork: Parent: Saved old parent's vfork_data: %p\n", udata->prev));
    udata->parent_posixcbase = PosixCBase;

    parent_createchild(udata);

    D(bug("__vfork: Parent: Setting jmp_buf at %p\n", udata->parent_newexitjmp));
    if (setjmp(udata->parent_newexitjmp) == 0)
    {
        /* The old errorptr is the program's return-code cell, installed by
           __stdc_program_startup().  A Task that reaches vfork() through a
           library-opened posixc base (C:Node -> node.library -> uv1.library:
           its per-Task PosixCBase opened stdc itself, no program startup ran
           on that base) has none: there is no return code to carry across the
           pretend-child, so it starts at 0.  Restoring NULL below then
           leaves that base exactly as it was. */
        udata->parent_olderrorptr = __stdc_set_errorptr(&udata->child_error);
        udata->child_error = udata->parent_olderrorptr ? *udata->parent_olderrorptr : 0;
        __stdc_set_exitjmp(udata->parent_newexitjmp, udata->parent_oldexitjmp);

        parent_enterpretendchild(udata);

        D(bug("__vfork: ParentPretendingChild(%p) jumping to jmp_buf %p\n",
              udata->parent, &udata->vfork_jmp
        ));
        D(bug("__vfork: ip: %p, stack: %p alt: %p\n",
              udata->vfork_jmp[0].retaddr, udata->vfork_jmp[0].regs[SP],
              udata->vfork_jmp[0].regs[ALT]
        ));

        vfork_longjmp(udata->vfork_jmp, 0);
        assert(0); /* not reached */
        return (pid_t) 0;
    }
    else /* setjmp() != 0; _exit() was called */
    {
        D(bug("__vfork: ParentPretendingChild: exiting or executed\n"));

        /* Stack may have been overwritten when we return here,
         * we jump to here from a function lower in the call chain
         */
        PosixCBase = (struct PosixCIntBase *)__aros_getbase_PosixCBase();
        udata = __posixc_pretending();

        D(bug("__vfork: ParentPretendingChild: vfork_data = %x\n", udata));

        if (!udata->child_executed)
        {
            D(bug("__vfork: ParentPretendingChild: not executed\n"));

            /* et_Result is normally set in startup code but no exec was performed
               so we have to mimic the startup code
             */
            etask = GetETask(udata->child);
            if (etask)
                etask->et_Result1 = udata->child_error;

            D(bug("__vfork: ParentPretendingChild: Signaling child %p, signal %d\n", udata->child, udata->child_signal));
            SETPARENTSTATE(PARENT_STATE_EXIT_CALLED);
            Signal(udata->child, 1 << udata->child_signal);
        }

        D(bug("__vfork: ParentPretendingChild: Waiting for child to finish using udata, me=%p, signal %d\n", FindTask(NULL),
                udata->parent_signal));
        /* Wait for child to finish using udata */
        Wait(1 << udata->parent_signal);
        ASSERTCHILDSTATE(CHILD_STATE_UDATA_NOT_USED);
        PRINTSTATE;

        D(bug("__vfork: ParentPretendingChild: fflushing\n"));
        fflush(NULL);

        __vfork_exit_controlled_stack(udata);

        assert(0); /* not reached */
        return (pid_t) 1;
    }
}

/*
 * The sole purpose of this function is to enable control over allocation of dummy and env.
 * Previously they were allocated in the ending code of __vfork function. On ARM however
 * this was causing immediate allocation of space at entry to the __vfork function. Moreover
 * the jmp_buf is aligned(16) and as such is represented on the stack as a pointer to stack
 * region instead of offset from stack base.
 *
 * The exit block of __vfork function represents code that underwent a number of longjumps. The
 * stack there is not guaranteed to be preserved, thus the on-stack pointer representing dummy
 * and env were also damaged. Extracting the code below allows to control when the variables
 * are allocated (as long as the function remains not inlined).
 */
static __attribute__((noinline)) void __vfork_exit_controlled_stack(struct vfork_data *udata)
{
    jmp_buf dummy;
    jmp_buf env;
    pid_t child_pid;

    D(bug("__vfork: ParentPretendingChild: freeing parent signal\n"));
    FreeSignal(udata->parent_signal);

    /* Take the child's pid while the launcher Task is still alive: once we
       signal it below it may finish and be removed, and udata is freed
       before the final longjmp. */
    child_pid = GetETaskID(udata->child);

    /* Stop resolving to the child's descriptor table and directory before
       the launcher is told it may close them. */
    parent_leavepretendchild(udata);

    /* Hand the child's errno to the parent. */
    errno = udata->child_errno;

    /* The launcher waits for this in both the executed and the not-executed
       case before it closes the libraries our pretend-child ran on. */
    D(bug("__vfork: Inform child that we are after _exit()\n"));
    SETPARENTSTATE(PARENT_STATE_STOPPED_PRETENDING);
    Signal(udata->child, 1 << udata->child_signal);

    D(bug("__vfork: Parent: restoring startup buffer\n"));
    /* Restore parent errorptr and startup buffer */
    __stdc_set_errorptr(udata->parent_olderrorptr);
    __stdc_set_exitjmp(udata->parent_oldexitjmp, dummy);

    /* Save some data from udata before udata is being freed */
    _VFORK_COPYENV(env,udata->vfork_jmp);

    D(bug("__vfork: Parent: freeing udata\n"));
    FreeMem(udata, sizeof(struct vfork_data));

    D(bug("__vfork: Parent jumping to jmp_buf %p\n", env));
    D(bug("__vfork: ip: %p, stack: %p\n", env->retaddr, env->regs[SP]));
    vfork_longjmp(env, child_pid);
}

static void parent_createchild(struct vfork_data *udata)
{
    struct PosixCIntBase *PosixCBase =
        (struct PosixCIntBase *)__aros_getbase_PosixCBase();
    jmp_buf vfork_jmp;
    struct Task *self = FindTask(NULL);
    struct Process *parent = (struct Process *)self;
    /* Names this base as the launcher's parent for as long as it may open
       posixc; keyed by udata, the launcher's NP_UserData (__taskctx.c). */
    struct __posixc_parentctx ctx = { PosixCBase, VFORK_PARENT, NULL, udata };
    IPTR oldctx;

    /* Inherit the parent's stack size for the child process.  An exec*()ed
     * child runs via RunCommand() using the child CLI's cli_DefaultStack,
     * which DOS derives from the child process's pr_StackSize (newcliproc.c).
     * Without NP_StackSize the child gets AROS's small default stack, so a
     * stack-hungry exec'd program (e.g. a git subprocess) overflows it even
     * though the parent was given a large stack.  Unix exec preserves the
     * stack, so inherit the parent's here. */
    IPTR childstack = (IPTR)self->tc_SPUpper - (IPTR)self->tc_SPLower;

    _VFORK_COPYENV(vfork_jmp,udata->vfork_jmp);

    struct TagItem tags[] =
    {
        { NP_Entry,         (IPTR) launcher },
        { NP_StackSize,     (IPTR) childstack },
        { NP_CloseInput,    (IPTR) FALSE },
        { NP_CloseOutput,   (IPTR) FALSE },
        { NP_CloseError,    (IPTR) FALSE },
        /* The launcher opens a fresh posixc.library instance before the
           pretend-child descriptor table is installed; its OpenLib stdio hook
           needs valid DOS streams on the new Process itself.  CreateNewProc
           does not infer them for this custom NP_Entry launcher. */
        { NP_Input,         (IPTR) Input() },
        { NP_Output,        (IPTR) Output() },
        { NP_Error,         (IPTR) parent->pr_CES },
        { NP_Cli,           (IPTR) TRUE },
        { NP_Name,          (IPTR) "vfork()" },
        { NP_UserData,      (IPTR) udata },
        { NP_NotifyOnDeath, (IPTR) TRUE },
        { TAG_DONE, 0 }
    };

    D(bug("__vfork: Parent: Allocating parent signal\n"));
    /* Allocate signal for child->parent communication */
    udata->parent_signal = AllocSignal(-1);
    if (udata->parent_signal == -1)
    {
        /* Couldn't allocate the signal, return -1 */
        FreeMem(udata, sizeof(struct vfork_data));
        errno = ENOMEM;
        vfork_longjmp(vfork_jmp, -1);
    }

    /* The launcher's OpenLibrary("posixc.library") must copy from THIS base,
       which is the base this call reached posixc with - for a pthread worker
       calling through a library that carries its creator's base that is not
       the base in the worker's own per-task slot that genmodule would hand
       the launcher as parent.  Name it for the launcher (it reads its
       creator's slot and proves itself with udata) until setup is done; the
       context is dropped on every exit path, so a failed vfork() cannot leave
       a later, unrelated child inheriting this table. */
    oldctx = __posixc_setparentctx(&ctx);

    D(bug("__vfork: Parent: Creating child\n"));
    udata->child = (struct Task*) CreateNewProc(tags);

    if (udata->child == NULL)
    {
        D(bug("__vfork: Child could not be created\n"));
        /* Something went wrong, return -1 */
        __posixc_restoreparentctx(oldctx);
        FreeMem(udata, sizeof(struct vfork_data));
        errno = ENOMEM; /* Most likely */
        vfork_longjmp(vfork_jmp, -1);
    }
    D(bug("__vfork: Parent: Child created %p, waiting to finish setup\n", udata->child));

    /* Wait for child to finish setup */
    Wait(1 << udata->parent_signal);
    ASSERTCHILDSTATE(CHILD_STATE_SETUP_FAILED | CHILD_STATE_SETUP_FINISHED);
    PRINTSTATE;

    __posixc_restoreparentctx(oldctx);

    if (udata->child_errno)
    {
        /* An error occured during child setup */
        D(bug("__vfork: Child returned an error (%d)\n", udata->child_errno));
        errno = udata->child_errno;
        vfork_longjmp(vfork_jmp, -1);
    }
}

static void parent_enterpretendchild(struct vfork_data *udata)
{
    D(bug("parent_enterpretendchild(%x): entered\n", udata));

    /* Pretend to be running as the child created by vfork: from here until
       parent_leavepretendchild() this Task's descriptor calls resolve to the
       child base's table (__fd_owner()), chdir() records into the child base,
       getpid()/_exit()/the exec*() family act for the child.

       This is Task state, not base state: the base this call came in on may
       serve other Tasks at the same time (a pthread worker of C:Node reaches
       posixc through uv1.library carrying the main thread's base), and those
       must keep seeing their own table, heap and directory (__taskctx.c). */
    __posixc_setpretending(udata);

    udata->parent_curdir = CurrentDir(((struct Process *)udata->child)->pr_CurrentDir);

    D(bug("parent_enterpretendchild: leaving\n"));
}

static void child_takeover(struct vfork_data *udata)
{
    D(bug("child_takeover(%x): entered\n", udata));

    /* Set current dir to parent's current dir.  A chdir() the pretend child
       made was recorded in this base already (chdir.c). */
    CurrentDir(((struct Process *)udata->parent)->pr_CurrentDir);

    D(bug("child_takeover(): leaving\n"));
}

static void parent_leavepretendchild(struct vfork_data *udata)
{
    D(bug("parent_leavepretendchild(%x): entered\n", udata));

    /* Switch to currentdir from before vfork() call */
    BPTR pretend_curdir = CurrentDir(udata->parent_curdir);

    /* A chdir() the pretend child made left its new lock as our current
       directory and the child's own start directory in the child base
       (cd_lock, chdir.c).  When the child exec()ed, child_takeover() moved
       the new lock into the launcher, whose posixc close swaps cd_lock back
       and UnLock()s it: both locks end up freed once.  When the child
       _exit()ed instead (exec failed: uv_spawn of a name that is not on
       PATH, with a cwd), nothing took the new lock over, and the launcher
       still sits on its start directory: its __exit_chdir() then UnLock()ed
       that start directory, and the Process exit UnLock()ed it again - a
       FileLock freed twice in emul-handler, the whole guest dead (VS Code
       run 26, the Claude CLI's first spawn).  Drop the orphaned lock here
       and clear the record, so the launcher's start directory is freed once,
       by the Process exit. */
    if (!udata->child_executed && udata->child_posixcbase
        && udata->child_posixcbase->cd_changed)
    {
        UnLock(pretend_curdir);
        udata->child_posixcbase->cd_changed = FALSE;
        udata->child_posixcbase->cd_lock = BNULL;
    }

    /* Switch to previous vfork_data */
    __posixc_setpretending(udata->prev);

    D(bug("parent_leavepretendchild: leaving\n"));
}
