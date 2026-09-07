#ifndef __CRT_THREADHOOKS_H
#define __CRT_THREADHOOKS_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: C runtime per-thread hook registry, shared by stdc and posixc.

    pthread.library is C-runtime agnostic: it neither knows nor names any
    CRT.  Instead it looks up this registry by name in the public semaphore
    list and runs every registered enter hook in a freshly created worker
    Task (before the thread function) and every leave hook at thread exit
    (leave hooks run in reverse order).  A CRT registers hooks here to give
    workers whatever per-Task state they need (a library base, an errno
    cell, ...).

    This contract is duplicated in compiler/pthread/pthread_create.c and
    must be kept in sync with it.
*/

#include <exec/lists.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

#define CRT_THREADHOOKS_SEMNAME "crt.thread-hooks.v1"

struct CRTThreadHook
{
    struct MinNode  th_Node;
    void          (*th_Enter)(void);    /* either may be NULL */
    void          (*th_Leave)(void);
};

struct CRTThreadHooks
{
    struct SignalSemaphore th_Sem;   /* ss_Link.ln_Name = CRT_THREADHOOKS_SEMNAME */
    struct MinList         th_Hooks;
};

/* Find the registry; with create set, make it if it does not exist yet.
   Whichever CRT library initialises first creates it, so the order in
   which stdc/posixc come up does not matter.  Returns NULL only when
   creation was needed and failed. */
static inline struct CRTThreadHooks *__crt_get_threadhooks(BOOL create)
{
    struct CRTThreadHooks *reg, *newreg;

    Forbid();
    reg = (struct CRTThreadHooks *)FindSemaphore((STRPTR)CRT_THREADHOOKS_SEMNAME);
    Permit();
    if (reg || !create)
        return reg;

    /* Not present yet - allocate outside Forbid, then add if we still win. */
    newreg = AllocMem(sizeof(*newreg), MEMF_PUBLIC | MEMF_CLEAR);
    if (!newreg)
        return NULL;
    InitSemaphore(&newreg->th_Sem);
    newreg->th_Sem.ss_Link.ln_Name = (char *)CRT_THREADHOOKS_SEMNAME;
    NEWLIST(&newreg->th_Hooks);

    Forbid();
    reg = (struct CRTThreadHooks *)FindSemaphore((STRPTR)CRT_THREADHOOKS_SEMNAME);
    if (!reg)
    {
        AddSemaphore(&newreg->th_Sem);
        reg = newreg;
        newreg = NULL;
    }
    Permit();

    if (newreg)
        FreeMem(newreg, sizeof(*newreg));
    return reg;
}

/* Register hook once (a second call with the same, already listed hook is
   a no-op).  Returns FALSE when the registry could not be created. */
static inline BOOL __crt_add_threadhook(struct CRTThreadHook *hook)
{
    struct CRTThreadHooks *reg = __crt_get_threadhooks(TRUE);

    if (!reg)
        return FALSE;

    ObtainSemaphore(&reg->th_Sem);
    if (!hook->th_Node.mln_Succ)
        AddTail((struct List *)&reg->th_Hooks, (struct Node *)&hook->th_Node);
    ReleaseSemaphore(&reg->th_Sem);

    return TRUE;
}

static inline void __crt_rem_threadhook(struct CRTThreadHook *hook)
{
    struct CRTThreadHooks *reg = __crt_get_threadhooks(FALSE);

    if (!reg)
        return;

    ObtainSemaphore(&reg->th_Sem);
    if (hook->th_Node.mln_Succ)
    {
        Remove((struct Node *)&hook->th_Node);
        hook->th_Node.mln_Succ = NULL;
    }
    ReleaseSemaphore(&reg->th_Sem);
}

#endif /* __CRT_THREADHOOKS_H */
