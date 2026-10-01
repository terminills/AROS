/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: SMP semaphore storm. One task pinned to each CPU hammers a single
          SignalSemaphore and a MEMF_SEM_PROTECTED pool (what stdc malloc
          uses) for a fixed time, the way RPCS3's LLVM compiler threads do.
          A watchdog checks progress every second; if no task moves for
          STALL_SECS it reports every worker's state and signals, and flags
          a task that is WAIT with a signal it waits for already received:
          a lost wakeup. RPCS3/GTA V stalled that way on 8 CPUs, with
          SIGF_SINGLE (semaphore hand-over) received and nobody woken.
*/

#include <aros/config.h>

#include <exec/memory.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <utility/tagitem.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>

#include <string.h>

#if defined(__AROSEXEC_SMP__)
#define DEBUG 1
#include <aros/debug.h>

APTR KernelBase;

#define MAX_WORKERS 64
#define STACKSIZE   16384
#define RUN_SECS    60
#define STALL_SECS  5

struct worker
{
    struct Task    *task;
    volatile ULONG  ops;
    volatile BOOL   done;
    ULONG           cpu;
};

static struct SignalSemaphore g_sem;
static APTR                   g_pool;
static volatile ULONG         g_shared;
static volatile BOOL          g_stop;
static struct worker          g_workers[MAX_WORKERS];
static struct Task           *g_parent;
static ULONG                  g_donesig;

static void worker_entry(void)
{
    struct Task *me = FindTask(NULL);
    struct worker *w = (struct worker *)me->tc_UserData;
    ULONG n = 0;

    while (!g_stop)
    {
        APTR p;

        ObtainSemaphore(&g_sem);
        g_shared++;
        ReleaseSemaphore(&g_sem);

        /* Varying sizes, as a compiler's allocations are. */
        p = AllocPooled(g_pool, 16 + ((n * 37) & 1023));
        if (p)
            FreePooled(g_pool, p, 16 + ((n * 37) & 1023));

        w->ops = ++n;
    }
    w->done = TRUE;
    Signal(g_parent, 1UL << g_donesig);
}

static int report_stall(ULONG workers)
{
    ULONG i;
    int lost = 0;

    for (i = 0; i < workers; i++)
    {
        struct Task *t = g_workers[i].task;
        ULONG wait = t->tc_SigWait, recvd = t->tc_SigRecvd;
        BOOL is_lost = (t->tc_State == TS_WAIT) && (wait & recvd);

        if (is_lost)
            lost++;
        bug("[smpsemstorm] worker %lu cpu=%lu ops=%lu state=%u wait=%08lx recvd=%08lx%s\n",
            i, g_workers[i].cpu, g_workers[i].ops, (unsigned)t->tc_State,
            wait, recvd, is_lost ? " <== LOST WAKEUP" : "");
    }
    bug("[smpsemstorm] semaphore owner=%p nest=%d queue=%d\n",
        g_sem.ss_Owner, (int)g_sem.ss_NestCount, (int)g_sem.ss_QueueCount);
    return lost;
}

int main(void)
{
    ULONG cpus, workers, i, sec, fail = 0;
    ULONG last_total = 0, idle_secs = 0;
    BYTE sig;

    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
        return RETURN_FAIL;
    cpus = KrnGetCPUCount();
    workers = cpus > MAX_WORKERS ? MAX_WORKERS : cpus;
    if (workers < 2)
    {
        bug("[smpsemstorm] DONE: 0 PASS, 0 FAIL, 1 INVALID (needs 2+ CPUs)\n");
        return RETURN_OK;
    }

    InitSemaphore(&g_sem);
    g_pool = CreatePool(MEMF_ANY | MEMF_SEM_PROTECTED, 65536, 4096);
    sig = AllocSignal(-1);
    if (!g_pool || sig == -1)
    {
        bug("[smpsemstorm] setup failed\n");
        return RETURN_FAIL;
    }
    g_parent = FindTask(NULL);
    g_donesig = sig;
    bug("[smpsemstorm] start: %lu workers pinned one per CPU, %us\n",
        workers, RUN_SECS);

    for (i = 0; i < workers; i++)
    {
        void *mask = KrnAllocCPUMask();

        g_workers[i].cpu = i;
        KrnClearCPUMask(mask);
        KrnGetCPUMask(i, mask);
        g_workers[i].task = NewCreateTask(TASKTAG_NAME, (IPTR)"smpsemstorm worker",
                                          TASKTAG_PC, (IPTR)worker_entry,
                                          TASKTAG_STACKSIZE, STACKSIZE,
                                          TASKTAG_AFFINITY, (IPTR)mask,
                                          TASKTAG_USERDATA, (IPTR)&g_workers[i],
                                          TAG_DONE);
        /*
         * A created task keeps the mask as its iet_CpuAffinity and exec
         * frees it when the task ends. Freeing it here left every worker
         * with a dangling affinity: READY, never scheduled, ops=0.
         */
        if (!g_workers[i].task)
            KrnFreeCPUMask(mask);
        if (!g_workers[i].task)
        {
            bug("[smpsemstorm] cannot create worker %lu\n", i);
            g_stop = TRUE;
            workers = i;
            fail++;
            break;
        }
    }

    for (sec = 0; sec < RUN_SECS && !g_stop; sec++)
    {
        ULONG total = 0;

        Delay(50);
        for (i = 0; i < workers; i++)
            total += g_workers[i].ops;
        if (total == last_total)
        {
            if (++idle_secs >= STALL_SECS)
            {
                bug("[smpsemstorm] STALL after %lus: no progress for %us\n", sec + 1, STALL_SECS);
                if (report_stall(workers))
                    bug("[smpsemstorm] lost wakeup(s) in memory - see above\n");
                fail++;
                /* The stuck workers never finish; leave them. */
                bug("[smpsemstorm] DONE: 0 PASS, %lu FAIL, 0 INVALID\n", fail);
                return RETURN_FAIL;
            }
        }
        else
            idle_secs = 0;
        last_total = total;
        if ((sec + 1) % 10 == 0)
            bug("[smpsemstorm] t=%lus ops=%lu shared=%lu\n", sec + 1, total, g_shared);
    }

    g_stop = TRUE;
    for (;;)
    {
        ULONG finished = 0;

        for (i = 0; i < workers; i++)
            if (g_workers[i].done)
                finished++;
        if (finished == workers)
            break;
        Wait(1UL << sig);
    }
    bug("[smpsemstorm] finished: ops=%lu shared=%lu\n", last_total, g_shared);
    DeletePool(g_pool);
    FreeSignal(sig);
    bug("[smpsemstorm] DONE: %lu PASS, %lu FAIL, 0 INVALID\n", fail ? 0UL : 1UL, fail);
    return fail ? RETURN_FAIL : RETURN_OK;
}

#else

int main(void)
{
    return RETURN_OK;
}

#endif
