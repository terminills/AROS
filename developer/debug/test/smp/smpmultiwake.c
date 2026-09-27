/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Many CPUs Signal() one waiting task at once.

    Signal() decides under the target's tc_SpinLock that the task is waiting,
    drops the lock, and only then asks the kernel to move it to TaskReady
    (krnSysCallReschedTask). With several CPUs signalling the same waiter,
    several of those requests arrive for one wake-up, and the later ones
    find the task already ready - or already running again. A reschedule
    that trusts a tc_State read without the task lock removes the node from
    the wrong list and closes TaskReady into a cycle: every CPU then spins on
    TaskReadySpinLock and the machine stops.

    One waiter is allowed on every CPU, so each signaller takes the local
    path (the task is in its CPU mask) rather than an IPI; one signaller is
    pinned to each other CPU and signals with a short random pause between
    signals - long enough that the locks are not simply saturated (seven
    cores in a tight loop starve the scheduler of TaskReadySpinLock), short
    enough that wake-ups from different cores overlap. The test passes if it
    finishes and the waiter kept waking. A progress line per second goes
    to the debug output, so a hang shows when it stopped.

    Usage: SMP-MultiWake [SECS/N]
      SECS   seconds to run (default 10)
*/

#include <aros/debug.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define ARG_TEMPLATE "SECS/N"
#define MAX_SIGNALLERS 64

APTR KernelBase;

static struct
{
    struct Task *waiter;
    ULONG wake_sig;
    ULONG stop_sig;
    volatile int ready;
    volatile int stop;
    volatile ULONG wakes;
    volatile ULONG sends[MAX_SIGNALLERS];
} mw;

/* An affinity attribute for one CPU, or for all of them (cpu < 0). */
static void attr_affinity(pthread_attr_t *attr, int cpu, int cpus)
{
    void *mask = KrnAllocCPUMask();
    int i;

    pthread_attr_init(attr);
    if (!mask)
        return;
    KrnClearCPUMask(mask);
    if (cpu >= 0)
        KrnGetCPUMask(cpu, mask);
    else
        for (i = 0; i < cpus; i++)
            KrnGetCPUMask(i, mask);
    pthread_attr_setaffinity_np(attr, 0, mask);
}

static void *waiter(void *arg)
{
    BYTE wake = AllocSignal(-1), stop = AllocSignal(-1);
    volatile ULONG spin;

    (void)arg;
    mw.wake_sig = 1UL << wake;
    mw.stop_sig = 1UL << stop;
    mw.waiter = FindTask(NULL);
    mw.ready = 1;

    for (;;)
    {
        ULONG got = Wait(mw.wake_sig | mw.stop_sig);

        if (got & mw.stop_sig)
            break;
        mw.wakes++;
        /* A little work, so signals also land while we run or are ready */
        for (spin = 0; spin < (mw.wakes & 255); spin++)
            ;
    }
    FreeSignal(wake);
    FreeSignal(stop);
    return NULL;
}

static void *signaller(void *arg)
{
    int id = (int)(IPTR)arg;
    ULONG seed = 0x9e3779b9 * (id + 1), spin;

    while (!mw.stop)
    {
        Signal(mw.waiter, mw.wake_sig);
        mw.sends[id]++;
        /* About 1-30 us between signals */
        seed = seed * 1664525 + 1013904223;
        for (spin = 200 + (seed >> 20); spin > 0; spin--)
            asm volatile("pause");
    }
    return NULL;
}

int main(void)
{
    IPTR args[1] = { 0 };
    struct RDArgs *rda = ReadArgs(ARG_TEMPLATE, args, NULL);
    ULONG secs = 10, cpus = 1, i, n = 0, total = 0, last = 0;
    pthread_t wt, st[MAX_SIGNALLERS];
    pthread_attr_t attr;

    if (rda)
    {
        if (args[0])
            secs = *(ULONG *)args[0];
        FreeArgs(rda);
    }

    KernelBase = OpenResource("kernel.resource");
    if (KernelBase)
        cpus = KrnGetCPUCount();
    if (cpus < 2)
    {
        printf("SMP-MultiWake: needs 2 or more CPUs (have %lu)\n", (unsigned long)cpus);
        bug("[smpmultiwake] SKIP: %lu CPU\n", (unsigned long)cpus);
        return RETURN_WARN;
    }

    bug("[smpmultiwake] begin: %lu CPUs, %lu signallers, %lu s\n",
        (unsigned long)cpus, (unsigned long)(cpus - 1), (unsigned long)secs);

    attr_affinity(&attr, -1, cpus);
    if (pthread_create(&wt, &attr, waiter, NULL) != 0)
    {
        bug("[smpmultiwake] FAIL: cannot create the waiter\n");
        return RETURN_FAIL;
    }
    while (!mw.ready)
        Delay(1);

    for (i = 1; i < cpus && n < MAX_SIGNALLERS; i++, n++)
    {
        attr_affinity(&attr, i, cpus);
        if (pthread_create(&st[n], &attr, signaller, (void *)(IPTR)n) != 0)
            break;
    }

    for (i = 0; i < secs; i++)
    {
        ULONG w;

        Delay(50);
        w = mw.wakes;
        bug("[smpmultiwake] %lus: wakes=%lu (+%lu)\n",
            (unsigned long)(i + 1), (unsigned long)w, (unsigned long)(w - last));
        last = w;
    }

    mw.stop = 1;
    for (i = 0; i < n; i++)
    {
        pthread_join(st[i], NULL);
        total += mw.sends[i];
    }
    Signal(mw.waiter, mw.stop_sig);
    pthread_join(wt, NULL);

    printf("SMP-MultiWake: %lu signallers sent %lu signals, waiter woke %lu times\n",
           (unsigned long)n, (unsigned long)total, (unsigned long)mw.wakes);
    if (mw.wakes == 0)
    {
        bug("[smpmultiwake] FAIL: the waiter never woke\n");
        return RETURN_FAIL;
    }
    bug("[smpmultiwake] PASS: sends=%lu wakes=%lu\n", (unsigned long)total, (unsigned long)mw.wakes);
    return RETURN_OK;
}
