/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Cross-CPU Cause() of one software interrupt.

          A SANA-II driver's Tx softint is Cause()d by PutMsg() on the
          network stack's CPU and by the Tx-done interrupt on another, so
          the same struct Interrupt is Cause()d from two CPUs at once.
          Cause() used to test "already queued" (ln_Type == NT_SOFTINT)
          before taking the softint list's spinlock, under Disable() only:
          two CPUs could both see it unqueued and both ADDTAIL() it. The
          second add makes the node its own successor, RemHead() hands it
          back for ever, and one CPU runs that softint in a loop (Chromium
          on Twitch with e1000: two cores at 100%). The dispatcher also
          marked a removed node NT_INTERRUPT only after dropping the lock,
          so a Cause() in between was dropped.

          One worker per secondary CPU Cause()s the same softint in a
          tight loop; CPU 0 is left to the main task, which runs above the
          workers and reports progress every 2 s, so a stuck CPU shows as a
          worker whose count stops while the softint keeps running.

          - A self-linked list traps a CPU in the dispatcher: its worker
            never finishes (timeout -> FAIL), and the softint runs more
            often than it was queued (runs > causes -> FAIL).
          - After the storm a single Cause() must still run the softint;
            a node left marked queued but off the list never would
            (lost softint -> FAIL).
*/

#include <aros/config.h>

#include <exec/interrupts.h>
#include <exec/tasks.h>
#include <utility/tagitem.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>

#include <aros/asmcall.h>

#include <stdio.h>

#if defined(__AROSEXEC_SMP__)
#define DEBUG 1
#include <aros/debug.h>

APTR KernelBase;

#define STACKSIZE       16384
#define CAUSES          20000       /* per worker */
#define HANDLER_SPIN    64          /* widen the dispatch window a little */
#define WAIT_TICKS      (50 * 20)   /* 20 s: well inside smp-suite.sh's 60 s quiet window */

static struct Interrupt softint;
static volatile ULONG runs;
static volatile ULONG go;

struct CauseWorker
{
    volatile ULONG  w_Started;
    volatile ULONG  w_Done;
    volatile ULONG  w_Causes;
};

AROS_INTH1(CountSoftInt, APTR, data)
{
    AROS_INTFUNC_INIT

    int i;

    __atomic_add_fetch(&runs, 1, __ATOMIC_SEQ_CST);
    for (i = 0; i < HANDLER_SPIN; i++)
        asm volatile("pause");

    return 0;

    AROS_INTFUNC_EXIT
}

static void CauseWorker(void)
{
    struct CauseWorker *w = FindTask(NULL)->tc_UserData;
    ULONG n;

    w->w_Started = 1;
    while (!go)
        asm volatile("pause");

    for (n = 0; n < CAUSES; n++)
    {
        Cause(&softint);
        w->w_Causes++;
    }
    w->w_Done = 1;
}

static struct Task *spawn(const char *name, int cpu, struct CauseWorker *w)
{
    cpumask_t *mask = KrnAllocCPUMask();

    if (mask == NULL)
        return NULL;
    KrnClearCPUMask(mask);
    KrnGetCPUMask(cpu, mask);

    /* The mask becomes iet_CpuAffinity and is freed by exec at teardown. */
    return NewCreateTask(TASKTAG_NAME,      (IPTR)name,
                         TASKTAG_PRI,       0,
                         TASKTAG_PC,        (IPTR)CauseWorker,
                         TASKTAG_STACKSIZE, STACKSIZE,
                         TASKTAG_USERDATA,  (IPTR)w,
                         TASKTAG_AFFINITY,  (IPTR)mask,
                         TAG_DONE);
}

int main(void)
{
    static struct CauseWorker workers[64];
    static char names[64][24];
    int ncpus, cpu, i, alive = 0, done, fail = 0;
    ULONG causes = 0, before;

    KernelBase = OpenResource("kernel.resource");
    ncpus = KernelBase ? KrnGetCPUCount() : 1;
    if (ncpus > 64)
        ncpus = 64;

    bug("[smpcause] start: cpus=%ld, %ld Cause()s per secondary CPU of one "
        "softint\n", (LONG)ncpus, (LONG)CAUSES);

    if (ncpus < 2)
    {
        bug("[smpcause] only %ld CPU online - needs SMP; SKIP\n", (LONG)ncpus);
        return RETURN_WARN;
    }

    softint.is_Node.ln_Type = NT_INTERRUPT;
    softint.is_Node.ln_Pri  = 0;
    softint.is_Node.ln_Name = "smpcause";
    softint.is_Code = (VOID_FUNC)CountSoftInt;
    softint.is_Data = NULL;

    SetTaskPri(FindTask(NULL), 5);

    for (cpu = 1; cpu < ncpus; cpu++)
    {
        snprintf(names[cpu], sizeof(names[cpu]), "SMP-Cause cpu%d", cpu);
        if (spawn(names[cpu], cpu, &workers[cpu]))
            alive++;
        else
            bug("[smpcause] cpu %ld: could not create worker\n", (LONG)cpu);
    }
    for (i = 0; i < 250; i++)
    {
        int started = 0;
        for (cpu = 1; cpu < ncpus; cpu++)
            started += workers[cpu].w_Started;
        if (started == alive)
            break;
        Delay(1);
    }

    go = 1;
    for (i = 0; i < WAIT_TICKS; i++)
    {
        done = 0;
        for (cpu = 1; cpu < ncpus; cpu++)
            done += workers[cpu].w_Done;
        if (done == alive)
            break;
        if (i % 100 == 99)
        {
            ULONG c = 0;
            for (cpu = 1; cpu < ncpus; cpu++)
                c += workers[cpu].w_Causes;
            bug("[smpcause] t+%lds: %ld/%ld workers done, %ld Cause()s, "
                "%ld runs\n", (LONG)((i + 1) / 50), (LONG)done, (LONG)alive,
                (LONG)c, (LONG)runs);
        }
        Delay(1);
    }

    for (cpu = 1; cpu < ncpus; cpu++)
    {
        causes += workers[cpu].w_Causes;
        if (!workers[cpu].w_Done)
        {
            bug("[smpcause] FAIL cpu %ld: worker stuck after %ld Cause()s "
                "(its CPU is looping in the softint dispatcher)\n",
                (LONG)cpu, (LONG)workers[cpu].w_Causes);
            fail++;
        }
    }
    bug("[smpcause] storm: %ld Cause()s, softint ran %ld times\n",
        (LONG)causes, (LONG)runs);
    if (runs > causes)
    {
        bug("[smpcause] FAIL: softint ran more often than it was Cause()d "
            "(the node was queued twice)\n");
        fail++;
    }
    if (runs == 0)
    {
        bug("[smpcause] FAIL: softint never ran\n");
        fail++;
    }

    if (!fail)
    {
        before = runs;
        Cause(&softint);
        for (i = 0; i < 100 && runs == before; i++)
            Delay(1);
        if (runs == before)
        {
            bug("[smpcause] FAIL: a Cause() after the storm never ran "
                "(node still marked queued but off the list)\n");
            fail++;
        }
    }

    bug("[smpcause] DONE: %ld PASS, %ld FAIL\n", (LONG)(fail ? 0 : 1),
        (LONG)fail);
    return fail ? RETURN_FAIL : RETURN_OK;
}
#else
int main(void)
{
    return RETURN_FAIL;
}
#endif
