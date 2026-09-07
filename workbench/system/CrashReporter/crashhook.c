/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CrashReporter hooks — intercept, never replace (DESIGN.md §2).

          Two hooks, both installed from user space with public APIs, so
          the ROM stays untouched and the normal "Program failed" requester
          still appears exactly as before:

          1. KrnAddExceptionHandler() observers on the CPU trap vectors —
             the classic kernel.resource API every shipped kernel has (one
             handler per vector; the vector number rides in data2 because
             the classic handler is not told which trap fired).  They run
             in trap context, copy the raw ExceptionContext for the
             trapping task into a preallocated ring, read CR2 for a #PF,
             and return 0 so the kernel forwards the trap to exec as
             usual.  Nothing here allocates, locks, or calls a library
             that needs a task-local base.  The CPU error code is not
             available through this API, so CRF_ERRORCODE is never set.

          2. A SetFunction() thunk on exec Alert() (LVO 18).  It runs in
             the failing task, in user mode, before the requester: it
             pairs the task with its trap record (if any), copies the task
             name, walks the frame chain with KrnBacktraceFromFrame() and
             signals the daemon, then jumps to the original Alert() with
             the return address intact — Exec_ExtAlert() therefore still
             reports the real caller as the alert location.

          Symbolication, file I/O and the network happen later, in the
          daemon's own task (report.c), from a copy of the record.

    AI_PHASE: CRASH_REPORTER
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: HIGH
    AI_NOTE: x86_64 only for the Alert() thunk; the trap observers are
             generic.  The thunk and cr_alert_record() must not use the C
             library: the failing task may be a bare exec task without a
             stdc base, and a fault here would recurse into Alert().
*/

#include <proto/exec.h>
#include <proto/kernel.h>

#include <aros/kernel.h>
#include <aros/libcall.h>
#include <exec/alerts.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <defines/exec_LVO.h>

#include "crashrec.h"

/* ------------------------------------------------------------------------ */

static const UBYTE trap_vectors[] = { 0, 1, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 16, 17, 18, 19 };
#define NVECTORS (sizeof trap_vectors / sizeof trap_vectors[0])

static struct CrashRing *cr_ring;
static APTR  cr_exc_handle[NVECTORS];
APTR  cr_orig_alert;                  /* referenced from the asm thunk       */
APTR KernelBase;               /* proto/kernel.h declares it extern */

/* byte copies that the compiler cannot turn into memcpy()/strcpy() calls */
static void __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
cr_copy(void *dst, const void *src, ULONG n)
{
    volatile UBYTE *d = dst;
    const volatile UBYTE *s = src;
    while (n--) *d++ = *s++;
}

static void __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
cr_copystr(TEXT *dst, CONST_STRPTR src, ULONG n)
{
    volatile TEXT *d = dst;
    ULONG i = 0;
    if (src)
        while (i + 1 < n && src[i]) { d[i] = src[i]; i++; }
    d[i] = 0;
}

static struct CrashRecord *cr_claim(struct CrashRing *ring)
{
    ULONG seq = __sync_add_and_fetch(&ring->ring_Cursor, 1);
    struct CrashRecord *rec = &ring->ring_Rec[seq % CR_RING];

    rec->rec_State = CRS_FREE;
    __sync_synchronize();
    rec->rec_Seq          = seq;
    rec->rec_Flags        = 0;
    rec->rec_Exception    = ~0U;
    rec->rec_Task         = NULL;
    rec->rec_AlertNum     = 0;
    rec->rec_Depth        = 0;
    rec->rec_Caller       = NULL;
    rec->rec_FaultAddress = NULL;
    rec->rec_ErrorCode    = 0;
    rec->rec_TaskName[0]  = 0;
    rec->rec_Program[0]   = 0;
    return rec;
}

/* ------------------------------------------------------------------------ */
/* 1. trap observer (trap context)                                           */

/* exhandler_t: ctx is the trapping task's struct ExceptionContext, data is
   our ring, data2 the vector we registered this closure for. */
static int cr_trap_observer(void *ctx, void *data, void *data2)
{
    struct CrashRing *ring = data;
    struct CrashRecord *rec;

    if (!ring)
        return 0;

    rec = cr_claim(ring);
    rec->rec_Task      = FindTask(NULL);
    rec->rec_Exception = (ULONG)(IPTR)data2;
    if (ctx)
    {
        cr_copy(&rec->rec_Ctx, ctx, sizeof rec->rec_Ctx);
        rec->rec_Ctx.FXSData = NULL;                /* not ours to keep     */
        rec->rec_Flags |= CRF_CONTEXT;
    }
#if defined(__x86_64__) || defined(__i386__)
    if (rec->rec_Exception == 14)
    {
        /* trap handlers run in ring 0; CR2 still holds the linear address
           the #PF was raised for */
        IPTR cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        rec->rec_FaultAddress = (APTR)cr2;
        rec->rec_Flags |= CRF_FAULTADDR;
    }
#endif
    __sync_synchronize();
    rec->rec_State = CRS_TRAP;
    return 0;                                       /* pass the trap on     */
}

/* ------------------------------------------------------------------------ */
/* 2. Alert() record (failing task, user mode, before the requester)         */

void cr_alert_record(ULONG alertNum, struct ExecBase *SysBaseArg, APTR caller, APTR callerfp)
{
    struct CrashRing *ring = cr_ring;
    struct CrashRecord *rec = NULL;
    struct Task *me;
    ULONG i, best = 0;

    (void)SysBaseArg;
    if (!ring)
        return;
    if (KernelBase && KrnIsSuper())
        return;                                     /* system alert: not ours */

    me = FindTask(NULL);

    /* a trap that just went through Exec_TrapHandler left a CRS_TRAP record
       for this task: complete it instead of opening a second one */
    for (i = 0; i < CR_RING; i++)
    {
        struct CrashRecord *r = &ring->ring_Rec[i];
        if (r->rec_State == CRS_TRAP && r->rec_Task == me && r->rec_Seq > best)
        {
            best = r->rec_Seq;
            rec = r;
        }
    }
    if (!rec)
    {
        rec = cr_claim(ring);
        rec->rec_Task = me;
    }

    rec->rec_AlertNum = alertNum;
    rec->rec_Caller   = caller;
    if (alertNum & AT_DeadEnd) rec->rec_Flags |= CRF_DEADEND;
    if (me)
    {
        cr_copystr(rec->rec_TaskName, me->tc_Node.ln_Name, CR_NAMELEN);
        rec->rec_TaskType = me->tc_Node.ln_Type;
        if (me->tc_Node.ln_Type == NT_PROCESS)
        {
            struct CommandLineInterface *cli = BADDR(((struct Process *)me)->pr_CLI);
#if AROS_FAST_BSTR
            if (cli && cli->cli_CommandName)
                cr_copystr(rec->rec_Program, (CONST_STRPTR)BADDR(cli->cli_CommandName), CR_PROGLEN);
#else
            (void)cli;
#endif
        }
    }

    if (KernelBase)
    {
        if (rec->rec_Flags & CRF_CONTEXT)
        {
            rec->rec_PCs[0] = (APTR)rec->rec_Ctx.rip;
            rec->rec_Depth  = 1 + KrnBacktraceFromFrame((APTR)rec->rec_Ctx.rbp,
                                                        &rec->rec_PCs[1], CR_MAX_FRAMES - 1);
        }
        else
        {
            rec->rec_PCs[0] = caller;
            rec->rec_Depth  = 1 + KrnBacktraceFromFrame(callerfp, &rec->rec_PCs[1], CR_MAX_FRAMES - 1);
        }
    }
    else
    {
        rec->rec_PCs[0] = (rec->rec_Flags & CRF_CONTEXT) ? (APTR)rec->rec_Ctx.rip : caller;
        rec->rec_Depth  = 1;
    }

    __sync_synchronize();
    rec->rec_State = CRS_ALERT;
    __sync_add_and_fetch(&ring->ring_Pending, 1);
    if (ring->ring_Daemon)
        Signal(ring->ring_Daemon, ring->ring_SigMask);
}

#if defined(__x86_64__)
/* Exec_18_Alert(ULONG alertNum [rdi], struct ExecBase *SysBase [rsi]).
   Preserve the argument registers, keep the stack 16-byte aligned for the
   call, hand over the return address and the caller's frame pointer, then
   tail-jump so the original sees the untouched return address. */
__asm__(
    "    .text\n"
    "    .globl cr_alert_thunk\n"
    "    .type  cr_alert_thunk, @function\n"
    "cr_alert_thunk:\n"
    "    push   %rdi\n"
    "    push   %rsi\n"
    "    push   %rbp\n"
    "    mov    24(%rsp), %rdx\n"          /* return address = caller      */
    "    mov    %rbp, %rcx\n"              /* caller's frame pointer       */
    "    call   cr_alert_record\n"
    "    pop    %rbp\n"
    "    pop    %rsi\n"
    "    pop    %rdi\n"
    "    jmp    *cr_orig_alert(%rip)\n"
    "    .size  cr_alert_thunk, .-cr_alert_thunk\n"
);
extern void cr_alert_thunk(void);
#define HAVE_ALERT_THUNK 1
#endif

/* ------------------------------------------------------------------------ */
/* install / remove (daemon task)                                            */

struct CrashRing *crashhook_install(struct Task *daemon, ULONG sigmask)
{
    struct CrashRing *ring;
    ULONG i;

    if (cr_ring) return NULL;                       /* one instance only    */

    KernelBase = OpenResource("kernel.resource");
    ring = AllocMem(sizeof(*ring), MEMF_ANY | MEMF_CLEAR);
    if (!ring) return NULL;
    ring->ring_Daemon  = daemon;
    ring->ring_SigMask = sigmask;
    cr_ring = ring;

    if (KernelBase)
    {
        for (i = 0; i < NVECTORS; i++)
            cr_exc_handle[i] = KrnAddExceptionHandler(trap_vectors[i],
                                                     (void *)cr_trap_observer,
                                                     ring, (void *)(IPTR)trap_vectors[i]);
    }

#ifdef HAVE_ALERT_THUNK
    Forbid();
    cr_orig_alert = SetFunction((struct Library *)SysBase, -LVOAlert * LIB_VECTSIZE,
                                (APTR)cr_alert_thunk);
    Permit();
#endif
    return ring;
}

BOOL crashhook_remove(struct CrashRing *ring)
{
    ULONG i;

    if (!ring || ring != cr_ring) return TRUE;

#ifdef HAVE_ALERT_THUNK
    Forbid();
    if (__AROS_GETVECADDR(SysBase, LVOAlert) != (APTR)cr_alert_thunk)
    {
        /* somebody patched Alert() after us: our thunk is in their chain
           and must stay resident */
        Permit();
        return FALSE;
    }
    SetFunction((struct Library *)SysBase, -LVOAlert * LIB_VECTSIZE, cr_orig_alert);
    Permit();
#endif
    if (KernelBase)
    {
        for (i = 0; i < NVECTORS; i++)
            if (cr_exc_handle[i]) KrnRemExceptionHandler(cr_exc_handle[i]);
    }
    ring->ring_Daemon = NULL;
    cr_ring = NULL;
    FreeMem(ring, sizeof(*ring));
    return TRUE;
}

/* Oldest complete record, copied out and freed.  Stale CRS_TRAP records
   (a trap whose task never reached Alert(): a handler swallowed it, or exec
   resumed the task) are reclaimed by ring reuse. */
BOOL crashhook_next(struct CrashRing *ring, struct CrashRecord *out)
{
    struct CrashRecord *rec = NULL;
    ULONG i, best = ~0U;

    if (!ring) return FALSE;
    for (i = 0; i < CR_RING; i++)
    {
        struct CrashRecord *r = &ring->ring_Rec[i];
        if (r->rec_State == CRS_ALERT && r->rec_Seq < best)
        {
            best = r->rec_Seq;
            rec = r;
        }
    }
    if (!rec) return FALSE;
    rec->rec_State = CRS_TAKEN;
    __sync_synchronize();
    cr_copy(out, rec, sizeof *out);
    __sync_synchronize();
    rec->rec_State = CRS_FREE;
    __sync_sub_and_fetch(&ring->ring_Pending, 1);
    return TRUE;
}
