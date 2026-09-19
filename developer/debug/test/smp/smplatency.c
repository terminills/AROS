/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Measure how long each core takes to RECEIVE work, not just whether
          it receives it.

    Why this exists. The recorded ">32 core" SMP defect is not "some cores never
    get work" - it is that cores STALL getting it, ~1 second at a time. Every
    other test in this directory answers a pass/fail question, and a core that
    eventually completes its work after a one-second stall passes all of them.
    So the stall is invisible to the whole existing suite, and the only way to
    see it is to measure per-core latency and look at the TAIL, not the mean.

    Shape (Nick's master/child model, as in smp-test and smp-smallpt): one
    worker pinned per core, a master handing out work over MsgPorts. The master
    stamps each work unit with ReadEClock() at PutMsg time; the worker stamps it
    again the instant it comes back from WaitPort/GetMsg. The difference is the
    work-arrival latency for that core - scheduler wake-up plus dispatch - which
    is exactly the quantity that is said to stall.

    Reporting is per core and tail-weighted: count, min, median, max, and a
    count of units over the stall threshold. A mean would hide a 1-second
    outlier in a thousand microsecond samples, which is the whole failure mode.

    Usage: SMP-Latency [MAXCPU/N] [UNITS/N] [STALLMS/N]
      MAXCPU   cores to use (default: all, clamped to KrnGetCPUCount())
      UNITS    work units per core (default 200)
      STALLMS  latency at or above which a unit is counted as a stall (default 100)
*/

#include <aros/debug.h>
#include <exec/tasks.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>
#include <proto/timer.h>

#include <string.h>

#define ARG_TEMPLATE "MAXCPU/N,UNITS/N,STALLMS/N"
#define ARG_MAXCPU      0
#define ARG_UNITS       1
#define ARG_STALLMS     2

#define MAX_CORES       256
#define DEF_UNITS       200
#define DEF_STALLMS     100
#define STACKSIZE       16384       /* matches smpstress/smpdoio - proven for open_timer + bug() */

APTR KernelBase;

struct LatWorker;

/* One work unit. mn_Node is the Message; the rest is our payload. */
struct WorkMsg
{
    struct Message      wm_Msg;
    struct LatWorker   *wm_Worker;
    ULONG               wm_Seq;
    UQUAD               wm_SentAt;      /* EClock ticks, stamped by the master */
};

struct LatWorker
{
    struct Task        *lw_Task;
    struct MsgPort     *lw_Port;        /* worker's inbox, created BY the worker */
    struct MsgPort     *lw_MasterPort;  /* where replies go */
    cpumask_t          *lw_Mask;
    ULONG               lw_Cpu;
    volatile ULONG      lw_Ready;
    volatile ULONG      lw_Done;
    /* Latency stats, all in EClock ticks; the master converts to microseconds. */
    ULONG               lw_Count;
    UQUAD               lw_Min;
    UQUAD               lw_Max;
    UQUAD               lw_Total;
    ULONG               lw_Stalls;
    ULONG               lw_RanOnWrongCpu;
    ULONG               lw_Timeouts;        /* rounds this core missed the deadline */
    volatile ULONG      lw_Replied;         /* replied in the current round */
    /*
     * Round number this worker has finished. Written ONLY by its own core and
     * read by the master - one writer per cache line, so polling it costs no
     * cross-core ownership transfer. This replaces a shared reply MsgPort: with
     * 96 workers every ReplyMsg() was a PutMsg() to the SAME port, i.e. 96 cores
     * serialising on one mp_SpinLock in InternalPutMsg(). Measured in a frozen
     * guest: up to 21 cores at once inside `lock cmpxchg` on that single lock,
     * with the port's queue EMPTY. The instrument was measuring the contention
     * it created rather than work-arrival latency.
     */
    /*
     * COUNT of units this worker has finished - not the last round number.
     * It was the round number, compared by the master as lw_Seen == u, and
     * since the struct is zero-initialised ROUND 0 WAS ALREADY SATISFIED
     * before the worker ran. The master then raced ahead and re-PutMsg'd a
     * WorkMsg that was still queued; AddTail on a node already in a list
     * corrupts it, and the worker saw the same message over and over - 51224
     * deliveries for 20 sends, which the report's own cross-check caught.
     * A monotonic count compared against u+1 has no pre-satisfied state.
     */
    volatile ULONG      lw_Seen;
    UQUAD              *lw_Samples;     /* for the median */
    char                lw_Name[24];
};

static struct LatWorker g_Workers[MAX_CORES];
static ULONG            g_NumCores = 1;
static ULONG            g_Units    = DEF_UNITS;
static UQUAD            g_StallTicks;
static UQUAD            g_EFreq    = 1;     /* EClock rate (calibration only) */
static UQUAD            g_TickHz   = 1;     /* rate of whatever NOW() returns */

/* Open a private timer.device unit for this task (one per task on SMP). */
static struct timerequest *open_timer(struct MsgPort **portp)
{
    struct MsgPort *port;
    struct timerequest *tr;

    port = CreateMsgPort();
    if (!port)
        return NULL;
    tr = (struct timerequest *)CreateIORequest(port, sizeof(struct timerequest));
    if (!tr)
    {
        DeleteMsgPort(port);
        return NULL;
    }
    if (OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)tr, 0))
    {
        DeleteIORequest((struct IORequest *)tr);
        DeleteMsgPort(port);
        return NULL;
    }
    *portp = port;
    return tr;
}

static void close_timer(struct timerequest *tr, struct MsgPort *port)
{
    CloseDevice((struct IORequest *)tr);
    DeleteIORequest((struct IORequest *)tr);
    DeleteMsgPort(port);
}

static UQUAD eclock_now(struct Device *TimerBase, struct EClockVal *ev)
{
    ReadEClock(ev);
    return (((UQUAD)ev->ev_hi) << 32) | (UQUAD)ev->ev_lo;
}

/*
 * TIMESTAMPS COME FROM THE TSC, NOT timer.device, and that is not an
 * optimisation - ReadEClock() is unusable as a per-unit clock on a many-core
 * guest, three ways over:
 *
 *  1. IT IS PORT I/O. EClockUpdate() latches the 8254 with
 *     outb(CH0|ACCESS_LATCH, PIT_CONTROL) (arch/all-pc/timer/ticks.c:105).
 *     Every read is a VM exit under QEMU. Our own eclock rate prints as
 *     1193180 Hz, which is the 8254, so this is the path being taken.
 *  2. IT SHARES A CACHE LINE. It updates TimerBase->tb_ticks_total, one
 *     structure for the whole system, so N cores timestamping is N cores
 *     writing the same line - the identical mistake as the shared reply port
 *     this test already had to remove.
 *  3. IT IS NOT SMP-SAFE FOR THIS USE. ReadEClock() guards itself with
 *     Disable(), which is PER-CPU on SMP and serialises nothing against
 *     another core doing the same update.
 *
 * With two reads per work unit, the clock was plausibly a large part of what
 * the 1-CPU baseline measured (median 21us for a MsgPort round trip).
 *
 * RDTSC is ~20 cycles, needs no lock and no exit, and is a per-core register,
 * so it adds no contention at any core count. The kernel already relies on
 * invariant TSC (the boot log calibrates APs with "TSC freq inherited from BSP
 * ... [invariant TSC]"), so the rate is stable and equal across cores. HPET
 * would also beat the PIT - it is memory-mapped and the guest reports 2 units -
 * but it is still a shared device read, so TSC is the better answer here.
 *
 * LIMITATION, stated because it bounds what these numbers mean: send and
 * receive stamps are taken on DIFFERENT cores, so any cross-core TSC skew lands
 * directly in the measurement. On KVM with invariant TSC the cores are kept in
 * sync and the skew is far below the millisecond stalls being hunted, but a
 * sub-microsecond figure from this test should not be trusted on hardware whose
 * TSC sync is unknown.
 */
#if defined(__x86_64__) || defined(__i386__)
#define HAVE_TSC 1
static inline UQUAD tsc_now(void)
{
    ULONG lo, hi;
    /* lfence first: rdtsc is not ordered against earlier loads on its own. */
    asm volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return (((UQUAD)hi) << 32) | (UQUAD)lo;
}

/* One-time, single-threaded, before any worker exists - so the PIT reads here
 * cost nothing anyone else is waiting on. */
static UQUAD calibrate_tsc_hz(struct Device *TimerBase, UQUAD eclock_hz)
{
    struct EClockVal ev;
    UQUAD e0, e1, t0, t1;

    e0 = eclock_now(TimerBase, &ev);
    t0 = tsc_now();
    do {
        e1 = eclock_now(TimerBase, &ev);
    } while ((e1 - e0) < (eclock_hz / 20));      /* ~50 ms */
    t1 = tsc_now();

    if (e1 == e0)
        return 0;
    return ((t1 - t0) * eclock_hz) / (e1 - e0);
}
#endif

/* Hot-path clock: TSC where we have it, EClock otherwise. */
#if defined(HAVE_TSC)
#define NOW(tb, ev)   tsc_now()
#else
#define NOW(tb, ev)   eclock_now((tb), (ev))
#endif

/*
 * Worker: pinned to one core. Creates its OWN MsgPort, because a MsgPort
 * records mp_SigTask and the signal must be delivered to this task.
 */
static void LatencyWorker(void)
{
    struct LatWorker *w = (struct LatWorker *)FindTask(NULL)->tc_UserData;
    struct Device *TimerBase;
    struct timerequest *tr;
    struct MsgPort *tport;
    struct EClockVal ev;
    struct WorkMsg *wm;

    tr = open_timer(&tport);
    if (!tr)
    {
        w->lw_Ready = 2;    /* failed */
        Wait(0);
    }
    TimerBase = (struct Device *)tr->tr_node.io_Device;

    w->lw_Port = CreateMsgPort();
    if (!w->lw_Port)
    {
        close_timer(tr, tport);
        w->lw_Ready = 2;
        Wait(0);
    }

    w->lw_Min = ~(UQUAD)0;
    w->lw_Max = 0;

    /* Published last: the master waits on this before sending anything. */
    __sync_synchronize();
    w->lw_Ready = 1;

    for (;;)
    {
        WaitPort(w->lw_Port);
        while ((wm = (struct WorkMsg *)GetMsg(w->lw_Port)))
        {
            UQUAD now, delta;

            if (wm->wm_Seq == (ULONG)~0)
            {
                /* Shutdown unit - same slot protocol, no reply port. */
                w->lw_Done = 1;
                __sync_synchronize();
                close_timer(tr, tport);
                Wait(0);
            }

            now = NOW(TimerBase, &ev);
            delta = (now > wm->wm_SentAt) ? (now - wm->wm_SentAt) : 0;

            if (delta < w->lw_Min) w->lw_Min = delta;
            if (delta > w->lw_Max) w->lw_Max = delta;
            w->lw_Total += delta;
            if (w->lw_Samples && w->lw_Count < g_Units)
                w->lw_Samples[w->lw_Count] = delta;
            if (delta >= g_StallTicks)
                w->lw_Stalls++;
            w->lw_Count++;

            /*
             * Confirm we really ran where we were pinned. A latency figure
             * from a core other than the one under test is worse than no
             * figure at all.
             */
            if (KernelBase && (ULONG)KrnGetCPUNumber() != w->lw_Cpu)
                w->lw_RanOnWrongCpu++;

            /*
             * No ReplyMsg: publish completion in our own slot instead. Both
             * ends of this protocol are ours, and the master does not reuse the
             * WorkMsg until it has seen this store, so the message is safely
             * owned. See lw_Seen for why the reply port had to go.
             */
            __sync_synchronize();
            w->lw_Seen++;
        }
    }
}

static int cmp_uquad(const void *a, const void *b)
{
    UQUAD x = *(const UQUAD *)a, y = *(const UQUAD *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Tiny insertion sort - the sample counts are small and qsort pulls in libc. */
static void sort_samples(UQUAD *v, ULONG n)
{
    ULONG i, j;
    for (i = 1; i < n; i++)
    {
        UQUAD key = v[i];
        for (j = i; j > 0 && v[j - 1] > key; j--)
            v[j] = v[j - 1];
        v[j] = key;
    }
    (void)cmp_uquad;
}

static ULONG ticks_to_us(UQUAD ticks)
{
    if (!g_TickHz)
        return 0;
    return (ULONG)((ticks * 1000000ULL) / g_TickHz);
}

int main(void)
{
    struct RDArgs *rda;
    IPTR args[3] = { 0, 0, 0 };
    struct Device *TimerBase;
    struct timerequest *tr;
    struct MsgPort *tport, *masterPort;
    struct EClockVal ev;
    ULONG coreCount = 1, maxCpus = 0, stallms = DEF_STALLMS;
    ULONG i, u, started = 0, outstanding = 0, progressEvery;
    UQUAD roundTicks;
    struct WorkMsg *msgs;
    LONG failed = 0, stalled = 0;

    KernelBase = OpenResource("kernel.resource");
    if (KernelBase)
        coreCount = (ULONG)KrnGetCPUCount();

    rda = ReadArgs(ARG_TEMPLATE, args, NULL);
    if (rda)
    {
        if (args[ARG_MAXCPU])  maxCpus = (ULONG)*(LONG *)args[ARG_MAXCPU];
        if (args[ARG_UNITS])   g_Units = (ULONG)*(LONG *)args[ARG_UNITS];
        if (args[ARG_STALLMS]) stallms = (ULONG)*(LONG *)args[ARG_STALLMS];
        FreeArgs(rda);
    }
    if (!g_Units) g_Units = DEF_UNITS;
    if (maxCpus == 0 || maxCpus > coreCount)
        maxCpus = coreCount;
    if (maxCpus > MAX_CORES)
        maxCpus = MAX_CORES;
    g_NumCores = maxCpus;

    tr = open_timer(&tport);
    if (!tr)
    {
        bug("[smplatency] cannot open timer.device\n");
        return RETURN_FAIL;
    }
    TimerBase = (struct Device *)tr->tr_node.io_Device;
    g_EFreq = (UQUAD)ReadEClock(&ev);
    if (!g_EFreq)
        g_EFreq = 1;
    g_TickHz = g_EFreq;
#if defined(HAVE_TSC)
    {
        UQUAD hz = calibrate_tsc_hz(TimerBase, g_EFreq);

        /* Sanity-bound the calibration: a wild value would silently scale every
         * figure in the report. Anything outside 100 MHz..100 GHz means the
         * calibration itself went wrong, so fall back to EClock and say so. */
        if (hz > 100000000ULL && hz < 100000000000ULL)
            g_TickHz = hz;
        else
            bug("[smplatency] TSC calibration implausible (%lu Hz) - using EClock\n",
                (unsigned long)hz);
    }
#endif
    g_StallTicks = (g_TickHz * (UQUAD)stallms) / 1000ULL;
    /* A round's deadline is 10x the per-unit stall threshold: long enough
     * that an ordinary slow core is measured rather than accused, short
     * enough that a wedged core does not consume the capture window. */
    roundTicks = g_StallTicks * 10ULL;

    bug("[smplatency] start: cpus=%lu (of %lu), units=%lu/core, stall>=%lums, clock=%lu Hz (%s)\n",
        (unsigned long)maxCpus, (unsigned long)coreCount,
        (unsigned long)g_Units, (unsigned long)stallms, (unsigned long)g_TickHz,
        (g_TickHz == g_EFreq) ? "EClock/PIT" : "TSC");

    /*
     * Deliberately NOT skipped at one CPU. A single-core run is the
     * no-contention baseline - it says what this machine's work-arrival
     * latency looks like with the scheduler uncontended, which is the number
     * every multi-core figure has to be read against. It doubles as the
     * control that separates a defect in the system from a defect in this
     * program: if the 1-CPU run is clean and the 2-CPU run is not, the
     * instrument is sound and the machine is not.
     */
    if (maxCpus < 2)
        bug("[smplatency] single core - no-contention BASELINE, not an SMP test\n");

    masterPort = CreateMsgPort();
    msgs = AllocMem(sizeof(struct WorkMsg) * maxCpus, MEMF_PUBLIC | MEMF_CLEAR);
    if (!masterPort || !msgs)
    {
        bug("[smplatency] out of memory\n");
        close_timer(tr, tport);
        return RETURN_FAIL;
    }

    /* One worker per core, each pinned to that core. */
    for (i = 0; i < maxCpus; i++)
    {
        struct LatWorker *w = &g_Workers[i];

        w->lw_Cpu = i;
        w->lw_MasterPort = masterPort;
        w->lw_Samples = AllocMem(sizeof(UQUAD) * g_Units, MEMF_PUBLIC | MEMF_CLEAR);
        __sprintf(w->lw_Name, "SMP-Lat.%03lu", (unsigned long)i);

        if (KernelBase)
        {
            w->lw_Mask = KrnAllocCPUMask();
            if (w->lw_Mask)
            {
                KrnClearCPUMask(w->lw_Mask);
                KrnGetCPUMask(i, w->lw_Mask);
            }
        }

        w->lw_Task = NewCreateTask(TASKTAG_NAME,      (IPTR)w->lw_Name,
                                   TASKTAG_PRI,       0,
                                   TASKTAG_PC,        (IPTR)LatencyWorker,
                                   TASKTAG_STACKSIZE, STACKSIZE,
                                   TASKTAG_USERDATA,  (IPTR)w,
                                   w->lw_Mask ? TASKTAG_AFFINITY : TAG_IGNORE, (IPTR)w->lw_Mask,
                                   TAG_DONE);
        if (w->lw_Task)
            started++;
        else
            bug("[smplatency] CPU #%03lu: could not create worker\n", (unsigned long)i);
    }

    bug("[smplatency] %lu/%lu workers created; waiting for them to publish ports\n",
        (unsigned long)started, (unsigned long)maxCpus);

    /*
     * Wait for every worker to publish its port before sending anything.
     * Bounded at ~10s per worker (Delay(1) is 1/50s): a worker that never
     * arrives must be REPORTED, not waited on forever - on a machine where
     * some cores are suspected of stalling, an unbounded wait here would be
     * indistinguishable from the defect under test.
     */
    for (i = 0; i < maxCpus; i++)
    {
        ULONG spins = 0;
        while (g_Workers[i].lw_Task && g_Workers[i].lw_Ready == 0 && spins < 500)
        {
            Delay(1);
            spins++;
        }
        if (g_Workers[i].lw_Task && g_Workers[i].lw_Ready != 1)
            bug("[smplatency] CPU #%03lu: worker never became ready (lw_Ready=%lu after %lu spins)\n",
                (unsigned long)i, (unsigned long)g_Workers[i].lw_Ready, (unsigned long)spins);
    }
    progressEvery = (g_Units > 20) ? 25 : 1;   /* short runs: every round */

    /*
     * Drop the master to priority -1, as smp-test's master runs. This is not
     * cosmetic: the master POLLS (it must not Delay(), which would sleep on the
     * scheduler under test), so at equal priority it holds whatever core it is
     * on until its quantum expires and any worker sharing that core waits the
     * full slice. Measured before this line existed: a 1-CPU run reported
     * min 7044us / median 20037us - i.e. the scheduler quantum, not
     * work-arrival latency. At -1 any runnable worker preempts the master
     * immediately and the figure means what it claims again.
     */
    SetTaskPri(FindTask(NULL), -1);
    bug("[smplatency] readiness wait done; starting %lu rounds\n", (unsigned long)g_Units);

    /*
     * Round after round: hand one unit to every core, then collect every
     * reply. Keeping the rounds lockstep means a core that stalls delays only
     * its own sample, and the master is never the bottleneck.
     */
    for (u = 0; u < g_Units; u++)
    {
        outstanding = 0;
        /*
         * Bracket the send loop. Progress printed only after collection cannot
         * distinguish "hung sending" from "hung waiting for replies", and at 96
         * cores this test stopped after round 0 with no further output at all -
         * two very different bugs look identical from outside.
         */
        if ((u % progressEvery) == 0)
            bug("[smplatency] round %lu: sending to %lu cores\n",
                (unsigned long)u, (unsigned long)maxCpus);
        for (i = 0; i < maxCpus; i++)
        {
            struct LatWorker *w = &g_Workers[i];
            struct WorkMsg *wm = &msgs[i];

            if (!w->lw_Task || w->lw_Ready != 1 || !w->lw_Port)
                continue;

            wm->wm_Msg.mn_Node.ln_Type = NT_MESSAGE;
            wm->wm_Msg.mn_Length = sizeof(struct WorkMsg);
            wm->wm_Msg.mn_ReplyPort = masterPort;
            wm->wm_Worker = w;
            wm->wm_Seq = u;
            wm->wm_SentAt = NOW(TimerBase, &ev);
            PutMsg(w->lw_Port, (struct Message *)wm);
            outstanding++;
        }
        if ((u % progressEvery) == 0)
            bug("[smplatency] round %lu: sent %lu, collecting\n",
                (unsigned long)u, (unsigned long)outstanding);
        /*
         * Collect with a DEADLINE, never WaitPort() forever.
         *
         * The first version of this blocked on WaitPort until every core had
         * replied, and at 96 cores it completed round 0 and then hung in round
         * 1 - the instrument was disabled by the exact defect it exists to
         * measure, and reported nothing at all. A stall detector that stalls
         * tells you only that something is wrong, which is what we already
         * knew.
         *
         * So: poll, and when the deadline passes, record WHICH cores have not
         * answered and carry on. A core that misses its deadline is the
         * measurement, not an error - it gets a timeout counted against it and
         * the run continues, so one bad core cannot hide the other ninety-five.
         */
        {
            UQUAD deadline = NOW(TimerBase, &ev) + roundTicks;

            for (i = 0; i < maxCpus; i++)
                g_Workers[i].lw_Replied = 0;

            /*
             * Poll each worker's own completion slot. No shared port, so this
             * loop creates no cross-core contention of its own - which is the
             * entire point: the previous version had all N workers ReplyMsg to
             * one master port and then measured the convoy that produced.
             *
             * Spin rather than Delay(): Delay() sleeps on timer.device, i.e.
             * waits for the scheduler to wake us, which is the thing under
             * test. A deadline that can only fire if the scheduler is healthy
             * is no deadline at all.
             */
            while (outstanding)
            {
                outstanding = 0;
                for (i = 0; i < maxCpus; i++)
                {
                    struct LatWorker *w = &g_Workers[i];

                    if (!w->lw_Task || w->lw_Ready != 1 || !w->lw_Port)
                        continue;
                    if (w->lw_Seen > u)
                        w->lw_Replied = 1;
                    else
                        outstanding++;
                }
                if (!outstanding)
                    break;
                if (NOW(TimerBase, &ev) > deadline)
                {
                    for (i = 0; i < maxCpus; i++)
                    {
                        struct LatWorker *w = &g_Workers[i];
                        if (w->lw_Task && w->lw_Ready == 1 && w->lw_Port && !w->lw_Replied)
                            w->lw_Timeouts++;
                    }
                    bug("[smplatency] round %lu: DEADLINE - %lu core(s) did not finish within %lums\n",
                        (unsigned long)u, (unsigned long)outstanding, (unsigned long)(stallms * 10));
                    for (i = 0; i < maxCpus; i++)
                    {
                        struct LatWorker *w = &g_Workers[i];
                        if (w->lw_Task && w->lw_Ready == 1 && w->lw_Port && !w->lw_Replied)
                            bug("[smplatency]   CPU #%03lu silent\n", (unsigned long)i);
                    }
                    u = g_Units;
                    break;
                }
                __sync_synchronize();
            }
        }
        if (u >= g_Units)
            break;

        /* Progress often enough to locate a stall, whatever UNITS is. */
        if ((u % progressEvery) == 0)
            bug("[smplatency] round %lu/%lu complete\n",
                (unsigned long)u, (unsigned long)g_Units);
    }

    /* Shut the workers down. */
    for (i = 0; i < maxCpus; i++)
    {
        struct LatWorker *w = &g_Workers[i];
        struct WorkMsg *wm = &msgs[i];

        if (!w->lw_Task || w->lw_Ready != 1 || !w->lw_Port)
            continue;
        wm->wm_Msg.mn_Node.ln_Type = NT_MESSAGE;
        wm->wm_Msg.mn_Length = sizeof(struct WorkMsg);
        wm->wm_Msg.mn_ReplyPort = masterPort;
        wm->wm_Seq = (ULONG)~0;
        PutMsg(w->lw_Port, (struct Message *)wm);
        /*
         * Bounded, for the same reason the collect loop is: if this core is
         * the wedged one, blocking here would throw away the entire report.
         */
        {
            UQUAD dl = NOW(TimerBase, &ev) + roundTicks;
            while (!w->lw_Done)
            {
                if (NOW(TimerBase, &ev) > dl)
                    break;
                __sync_synchronize();
            }
        }
    }

    /* ---- report, tail first ------------------------------------------- */
    bug("[smplatency] per-core work-arrival latency, microseconds:\n");
    bug("[smplatency]   CPU   count      min   median      max   stalls  timeouts\n");
    for (i = 0; i < maxCpus; i++)
    {
        struct LatWorker *w = &g_Workers[i];
        UQUAD med = 0;
        ULONG nsamp;

        if (!w->lw_Count)
        {
            bug("[smplatency]  %03lu       0        -        -        -        -   NO WORK COMPLETED\n",
                (unsigned long)i);
            failed++;
            continue;
        }
        /*
         * CLAMP before sorting. lw_Count is written by another core and counts
         * every unit that core processed; lw_Samples only ever holds g_Units of
         * them (the store is guarded). sort_samples() WRITES as it shifts, so
         * passing the raw count sorted far past the end of the array - a
         * 45732-element sort over a 20-element buffer, which smashed the heap
         * and took the task's stack with it ("went out of stack limits", with a
         * valid task name, i.e. a real overflow rather than the corruption
         * signature). An index derived from another core's counter must be
         * bounded at the point of use, not where it is produced.
         */
        nsamp = w->lw_Count;
        if (nsamp > g_Units)
            nsamp = g_Units;
        if (w->lw_Samples && nsamp)
        {
            sort_samples(w->lw_Samples, nsamp);
            med = w->lw_Samples[nsamp / 2];
        }
        bug("[smplatency]  %03lu  %6lu  %7lu  %7lu  %7lu  %7lu  %7lu%s\n",
            (unsigned long)i,
            (unsigned long)w->lw_Count,
            (unsigned long)ticks_to_us(w->lw_Min),
            (unsigned long)ticks_to_us(med),
            (unsigned long)ticks_to_us(w->lw_Max),
            (unsigned long)w->lw_Stalls,
            (unsigned long)w->lw_Timeouts,
            w->lw_RanOnWrongCpu ? "  AFFINITY NOT HONOURED" : "");
        if (w->lw_Count > g_Units)
            bug("[smplatency]  %03lu  WARNING: processed %lu units but only %lu were sent -"
                " duplicate delivery or counter corruption\n",
                (unsigned long)i, (unsigned long)w->lw_Count, (unsigned long)g_Units);
        if (w->lw_Stalls || w->lw_Timeouts)
            stalled++;
    }

    bug("[smplatency] DONE: %lu core(s), %lu with stalls >=%lums, %lu with no work\n",
        (unsigned long)maxCpus, (unsigned long)stalled, (unsigned long)stallms,
        (unsigned long)failed);

    for (i = 0; i < maxCpus; i++)
    {
        if (g_Workers[i].lw_Samples)
            FreeMem(g_Workers[i].lw_Samples, sizeof(UQUAD) * g_Units);
    }
    FreeMem(msgs, sizeof(struct WorkMsg) * maxCpus);
    DeleteMsgPort(masterPort);
    close_timer(tr, tport);

    return (failed || stalled) ? RETURN_WARN : RETURN_OK;
}
