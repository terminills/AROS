/*
    Copyright (C) 2012-2026, The AROS Development Team. All rights reserved.

    This file defines the private part of PosixCBase.
    This should only be used internally in posixc.library code so
    changes can be made to this structure without breaking backwards
    compatibility.
*/
#ifndef __POSIXC_INTBASE_H
#define __POSIXC_INTBASE_H

#include <libraries/posixc.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <devices/timer.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <aros/types/sigset_t.h>
#include <pwd.h>

/* Some private structs */
struct __random_state;
struct __env_item;
struct _fdesc;
struct vfork_data;

struct PosixCIntBase
{
    struct PosixCBase PosixCBase;

    /* optional libs */
    struct Library           *PosixCUserGroupBase;
    struct Library           *PosixCFDBase;
    struct Library           *PosixCEntropyBase;    /* entropy.resource, for getentropy() */
    struct Library           *PosixCDOS64Base;      /* dos64.library, for 64-bit file I/O */

    /* common */
    APTR internalpool;

    /* __taskctx.c: the base this one inherited from at OpenLib time and how
       (VFORK_PARENT, EXEC_PARENT, or 0 for the plain per-task lineage).
       Read by __init_fd(), __umask_init() and __posixc_nixmain(). */
    struct PosixCIntBase *parentbase;
    int parentmode;

    struct Device *timerBase;
    struct timerequest timerReq;
    struct MsgPort timerPort;

    /* random.c */
    struct __random_state *rs;

    /* getpwuid.c */
    struct passwd pwd;

    /* getpass.c */
    char passbuffer[PASS_MAX];

    /* sigprocmask.c */
    sigset_t   sigmask;

    /* __exec.c */
    BPTR exec_seglist;
    BPTR exec_progdir;
    char *exec_args;
    char *exec_taskname;
    APTR exec_pool;
    char **exec_tmparray;
    BPTR exec_oldin, exec_oldout, exec_olderr;
    struct StdCBase *exec_oldstdcbase;

    /* __fdesc.c */
    /* A pthread worker Task gets its own posixc.library base but shares the
       POSIX descriptor table of the Process that created it.  fd_owner points
       at the base that actually owns the fd_array/fd_slots/internalpool used
       for descriptor lookups; it is self for an ordinary Process/CLI child.
       Resolved through __fd_owner() / FD_ARRAY() / FD_SLOTS() / FD_POOL(). */
    struct PosixCIntBase *fd_owner;
    int fd_slots;
    struct _fdesc **fd_array;
    /* Serialises fd_array/fd_slots access. The descriptor table is shared
       between the owner Process and its pthread workers (fd_owner), which
       run as independent Tasks; without this, one Task growing the table
       (realloc+free in __getfdslot) races a concurrent lookup in another
       and dereferences freed/half-swapped memory. Initialised and held in
       the owner base only; borrowers reach it through __fd_owner(). */
    struct SignalSemaphore fd_sem;
    /* Descriptor number this Task reserved in fd.library through
       __getfirstfd() but has not published yet, stored as fd + 1 (0 = none).
       __getfdslot() uses it to tell "my own fresh reservation" from "a
       number another descriptor table (vfork child/parent) still holds",
       which it must FD_Share() instead. */
    int fd_reserved;

    /* __upath.c */
    char *upathbuf;  /* Buffer that holds intermediate converted paths */
    int doupath;   /* BOOL - does the conversion need to be done?  */
    int parent_does_upath; /* BOOL - parent does upath conversion */

    /* chdir.c/fchdir.c */
    int cd_changed;
    BPTR cd_lock;

    /* flock.c */
    struct MinList _file_locks, *file_locks;

    /* umask */
    mode_t umask;

    /* __stdio.c */
    struct MinList stdio_files;
    /* Guards stdio_files: fdopen()/fclose() from concurrent Tasks would
       otherwise unlink through freed nodes. */
    struct SignalSemaphore stdio_sem;

    /* setuid.c/getuid.c */
    uid_t uid; /* Real user id of process */
    uid_t euid; /* Effective user id of process */
    /* set(e)gid.c/get(e)gid.c */
    gid_t gid; /* Real group id of process */
    gid_t egid; /* Effective group id of process */

    /* __upath.c, at the end to keep the offsets above: a program linked
       against the global PosixCBase shares it with its pthreads; those Tasks
       convert into their own buffers (upath_others), upathbuf stays with the
       Task that opened the base. */
    struct Task *upath_task;
    struct MinList upath_others;
    struct SignalSemaphore upath_sem;
};

/* __taskctx.c: per-Task vfork()/exec*() state.  None of it lives in base
   fields, because one base serves every Task that reaches posixc through a
   caller holding it (AROS_GM_STACKCALL); see __taskctx.c. */

/* How a child base inherits from its parent: started by the exec functions
   or by vfork.  The child copies the parent's descriptor table, umask and
   environment during its own initialisation. */
#define EXEC_PARENT 0x00000001
#define VFORK_PARENT 0x00000002

/* The parent names itself for the window in which its child opens posixc:
   vfork() around the launcher's setup (key = the launcher's vfork_data, its
   NP_UserData), exec*() around RunCommand() (task = the Task the program runs
   on).  Lives on the parent's stack for that window. */
struct __posixc_parentctx
{
    struct PosixCIntBase *base;
    int mode;               /* VFORK_PARENT or EXEC_PARENT */
    struct Task *task;      /* exec*(): the consuming Task */
    APTR key;               /* vfork(): the consuming launcher's tc_UserData */
};
IPTR __posixc_setparentctx(struct __posixc_parentctx *ctx);
void __posixc_restoreparentctx(IPTR old);

/* The vfork_data of a Task that is a vfork() parent running its child's code
   until the child's _exit()/exec*(): its descriptor calls resolve to the
   child base (__fd_owner()), getpid() reports the child, _exit() and the
   exec*() family hand over to the launcher.  NULL for every other Task. */
struct vfork_data *__posixc_pretending(void);
void __posixc_setpretending(struct vfork_data *udata);

/* __upath.c: frees the path buffers of other Tasks (upath_others) */
void __upath_free_others(struct PosixCIntBase *PosixCBase);

#endif //__POSIXC_INTBASE_H
