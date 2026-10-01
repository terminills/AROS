/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: timer.device wake precision. A task asks UNIT_MICROHZ for one
          60 Hz frame (16667 us) again and again and records how late each
          request comes back, measured with GetUpTime(). Chromium paces its
          frames with exactly such delays; if they return a tick late, every
          frame misses its deadline while the CPUs sit idle.
          Run once on the creator's CPU and once from a task on another CPU,
          whose wakeup crosses CPUs (timer IRQ on one, Signal to the other).
*/

#include <aros/config.h>

#include <exec/memory.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>
#include <proto/timer.h>

#include <stdlib.h>
#include <string.h>

#if defined(__AROSEXEC_SMP__)
#define DEBUG 1
#include <aros/debug.h>

APTR KernelBase;

#define ROUNDS      300
#define FRAME_US    16667
#define STACKSIZE   16384
#define LATE_OK_US  2000    /* median lateness above this fails */

struct run
{
    ULONG           cpu;            /* CPU the measuring task ran on */
    LONG            late[ROUNDS];   /* microseconds past the request */
    BOOL            ok;
};

static LONG tv_us(const struct timeval *a, const struct timeval *b)
{
    return (LONG)((b->tv_secs - a->tv_secs) * 1000000 +
                  ((LONG)b->tv_micro - (LONG)a->tv_micro));
}

static void measure(struct run *r)
{
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *tr = port ?
        (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : NULL;
    struct Device *TimerBase;
    int i;

    r->ok = FALSE;
    r->cpu = KrnGetCPUNumber();
    if (!tr || OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)tr, 0))
    {
        bug("[smptimerjitter] cannot open timer.device\n");
        if (tr) DeleteIORequest((struct IORequest *)tr);
        if (port) DeleteMsgPort(port);
        return;
    }
    TimerBase = tr->tr_node.io_Device;

    for (i = 0; i < ROUNDS; i++)
    {
        struct timeval t0, t1;

        GetUpTime(&t0);
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = FRAME_US;
        DoIO((struct IORequest *)tr);
        GetUpTime(&t1);
        r->late[i] = tv_us(&t0, &t1) - FRAME_US;
    }

    CloseDevice((struct IORequest *)tr);
    DeleteIORequest((struct IORequest *)tr);
    DeleteMsgPort(port);
    r->ok = TRUE;
}

static int cmp_long(const void *a, const void *b)
{
    LONG x = *(const LONG *)a, y = *(const LONG *)b;
    return (x > y) - (x < y);
}

/* Prints the distribution; returns TRUE if the median is within LATE_OK_US. */
static BOOL report(const char *what, struct run *r)
{
    LONG s[ROUNDS];
    ULONG b1 = 0, b5 = 0, b10 = 0, b17 = 0, bmore = 0, early = 0;
    int i;

    if (!r->ok)
    {
        bug("[smptimerjitter] %s: no data\n", what);
        return FALSE;
    }
    for (i = 0; i < ROUNDS; i++)
    {
        LONG l = r->late[i];
        s[i] = l;
        if (l < 0) early++;
        else if (l < 1000) b1++;
        else if (l < 5000) b5++;
        else if (l < 10000) b10++;
        else if (l < FRAME_US) b17++;
        else bmore++;
    }
    qsort(s, ROUNDS, sizeof(s[0]), cmp_long);
    bug("[smptimerjitter] %s cpu=%lu: %lu x %lu us, late us min=%ld median=%ld p90=%ld max=%ld\n",
        what, r->cpu, (ULONG)ROUNDS, (ULONG)FRAME_US,
        (long)s[0], (long)s[ROUNDS / 2], (long)s[ROUNDS * 9 / 10], (long)s[ROUNDS - 1]);
    bug("[smptimerjitter] %s late: early=%lu <1ms=%lu 1-5ms=%lu 5-10ms=%lu 10-16.7ms=%lu >=16.7ms=%lu\n",
        what, early, b1, b5, b10, b17, bmore);
    return s[ROUNDS / 2] < LATE_OK_US;
}

static struct run remote_run;
static struct Task *parent;
static ULONG donesig;

static void remote_entry(void)
{
    measure(&remote_run);
    Signal(parent, 1UL << donesig);
}

int main(void)
{
    static struct run local_run;
    ULONG pass = 0, fail = 0, cpus;
    BYTE sig;

    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
    {
        bug("[smptimerjitter] no kernel.resource\n");
        return RETURN_FAIL;
    }
    cpus = KrnGetCPUCount();
    bug("[smptimerjitter] start: cpus=%lu rounds=%lu request=%lu us\n",
        cpus, (ULONG)ROUNDS, (ULONG)FRAME_US);

    measure(&local_run);
    if (report("local", &local_run)) pass++; else fail++;

    if (cpus > 1 && (sig = AllocSignal(-1)) != -1)
    {
        void *affinity = KrnAllocCPUMask();
        struct Task *t;

        parent = FindTask(NULL);
        donesig = sig;
        SetSignal(0, 1UL << sig);
        KrnClearCPUMask(affinity);
        KrnGetCPUMask((KrnGetCPUNumber() + 1) % cpus, affinity);
        t = NewCreateTask(TASKTAG_NAME, (IPTR)"smptimerjitter remote",
                          TASKTAG_PC, (IPTR)remote_entry,
                          TASKTAG_STACKSIZE, STACKSIZE,
                          TASKTAG_AFFINITY, (IPTR)affinity,
                          TAG_DONE);
        if (t)
        {
            Wait(1UL << sig);
            if (report("remote", &remote_run)) pass++; else fail++;
        }
        else
        {
            bug("[smptimerjitter] cannot create remote task\n");
            fail++;
            /* Only ours to free when no task took it: a created task's
             * mask becomes its iet_CpuAffinity, which exec frees when the
             * task ends - freeing it here as well is a double free. */
            KrnFreeCPUMask(affinity);
        }
        FreeSignal(sig);
    }

    bug("[smptimerjitter] DONE: %lu PASS, %lu FAIL, 0 INVALID\n", pass, fail);
    return fail ? RETURN_WARN : RETURN_OK;
}

#else

int main(void)
{
    return RETURN_OK;
}

#endif
