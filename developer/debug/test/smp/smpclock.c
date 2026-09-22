/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Read the timer.device clocks from every core at once and report every
          reading that is not monotonic or not plausible.

    Why this exists. Chromium's TimeTicks::Now() is clock_gettime(CLOCK_MONOTONIC)
    which is GetUpTime(); base::Time is GetSysTime(). Its schedulers (compositor
    BeginFrame, delayed task queues) compare readings taken on DIFFERENT threads
    running on DIFFERENT cores. One reading that is a second early or late does
    not crash anything - it computes a deadline a second into the future and
    the thread waits, legitimately, for a timer that fires late. From outside
    that looks like "every core idle, nobody stuck, no frames". So the question
    this test answers is not "does the clock run at the right rate" (that is
    tools/timer-clock.py, sampled from the host at 1 Hz, and it says yes) but
    "can ANY single reading, from any core, disagree with the clock".

    Shape: one reader pinned per core (master/child, as the other tests here),
    each calling GetUpTime()/GetSysTime() as fast as it can for SECS seconds
    and checking each reading against (a) its own previous reading, (b) the
    TSC elapsed since that reading, and (c) the newest reading any core has
    published. Every violation is counted per core with its worst magnitude,
    because the interesting event is rare and a mean would hide it.

    Usage: SMP-Clock [MAXCPU/N] [SECS/N] [JUMPMS/N]
      MAXCPU   cores to use (default: all, clamped to KrnGetCPUCount())
      SECS     seconds each reader runs (default 10)
      JUMPMS   a reading that is off by at least this much against the TSC
               counts as a jump (default 20)
*/

#include <aros/debug.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>
#include <proto/timer.h>

#include <string.h>

#define ARG_TEMPLATE "MAXCPU/N,SECS/N,JUMPMS/N"
#define ARG_MAXCPU      0
#define ARG_SECS        1
#define ARG_JUMPMS      2

#define MAX_CORES       256
#define DEF_SECS        10
#define DEF_JUMPMS      20
#define STACKSIZE       16384

/* Cross-core tolerance: two cores reading "at the same time" legitimately
 * differ by the PIT latch cost plus preemption; 2 ms is far above that and
 * far below the 1 s that a torn secs/micro pair produces. */
#define XCORE_TOL_US    2000

APTR KernelBase;

struct ClockStat
{
    ULONG   cs_Samples;
    ULONG   cs_Backward;        /* reading < own previous reading */
    ULONG   cs_BackwardWorstUs;
    ULONG   cs_Jump;            /* |clock delta - TSC delta| >= JUMPMS */
    ULONG   cs_JumpWorstUs;
    ULONG   cs_BadMicro;        /* tv_micro >= 1000000 */
    ULONG   cs_XCore;           /* reading below the newest reading any core published */
    ULONG   cs_XCoreWorstUs;
};

struct ClockWorker
{
    ULONG               cw_Cpu;
    ULONG               cw_Ready;       /* 0 starting, 1 running, 2 failed */
    ULONG               cw_Done;
    ULONG               cw_RanOn;       /* KrnGetCPUNumber() sampled while running */
    ULONG               cw_Moved;       /* samples taken on a core other than cw_Cpu */
    void               *cw_Mask;
    struct Task        *cw_Task;
    char                cw_Name[16];
    struct ClockStat    cw_Up;          /* GetUpTime() */
    struct ClockStat    cw_Sys;         /* GetSysTime() */
};

static struct ClockWorker g_Workers[MAX_CORES];
static ULONG   g_NumCores = 1;
static ULONG   g_Secs     = DEF_SECS;
static UQUAD   g_JumpUs   = DEF_JUMPMS * 1000ULL;
static UQUAD   g_TscHz    = 1;
static volatile UQUAD g_NewestUp;       /* newest GetUpTime() any core saw, in us */
static volatile UQUAD g_NewestSys;      /* newest GetSysTime() any core saw, in us */

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

static inline UQUAD tsc_now(void)
{
    ULONG lo, hi;
    asm volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return (((UQUAD)hi) << 32) | (UQUAD)lo;
}

/* One-time, single-threaded, before any reader exists: TSC ticks per second
 * against the EClock, same as smplatency. */
static UQUAD calibrate_tsc_hz(struct Device *TimerBase)
{
    struct EClockVal ev;
    UQUAD ehz, e0, e1, t0, t1;

    ehz = (UQUAD)ReadEClock(&ev);
    if (!ehz)
        return 0;
    e0 = (((UQUAD)ev.ev_hi) << 32) | ev.ev_lo;
    t0 = tsc_now();
    do {
        ReadEClock(&ev);
        e1 = (((UQUAD)ev.ev_hi) << 32) | ev.ev_lo;
    } while ((e1 - e0) < (ehz / 20));            /* ~50 ms */
    t1 = tsc_now();
    if (e1 == e0)
        return 0;
    return ((t1 - t0) * ehz) / (e1 - e0);
}

static inline UQUAD tv_us(const struct timeval *tv)
{
    return (UQUAD)tv->tv_secs * 1000000ULL + (UQUAD)tv->tv_micro;
}

/* Publish 'now' as the newest reading if it is; return the newest before us. */
static UQUAD publish_max(volatile UQUAD *slot, UQUAD now)
{
    UQUAD seen = *slot;

    while (now > seen)
    {
        UQUAD prev = __sync_val_compare_and_swap(slot, seen, now);
        if (prev == seen)
            break;
        seen = prev;
    }
    return seen;
}

static void check(struct ClockStat *st, const struct timeval *tv,
                  UQUAD *prevUs, UQUAD *prevTsc, volatile UQUAD *newest)
{
    UQUAD now = tv_us(tv), tsc = tsc_now(), before;

    st->cs_Samples++;

    if (tv->tv_micro >= 1000000)
        st->cs_BadMicro++;

    if (*prevTsc)
    {
        UQUAD elapsedUs = ((tsc - *prevTsc) * 1000000ULL) / g_TscHz;
        UQUAD off;

        if (now < *prevUs)
        {
            ULONG back = (ULONG)(*prevUs - now);
            st->cs_Backward++;
            if (back > st->cs_BackwardWorstUs)
                st->cs_BackwardWorstUs = back;
            off = (*prevUs - now) + elapsedUs;
        }
        else
        {
            UQUAD clockUs = now - *prevUs;
            off = (clockUs > elapsedUs) ? (clockUs - elapsedUs) : (elapsedUs - clockUs);
        }
        if (off >= g_JumpUs)
        {
            st->cs_Jump++;
            if (off > st->cs_JumpWorstUs)
                st->cs_JumpWorstUs = (ULONG)off;
        }
    }

    before = publish_max(newest, now);
    if (before > now + XCORE_TOL_US)
    {
        ULONG back = (ULONG)(before - now);
        st->cs_XCore++;
        if (back > st->cs_XCoreWorstUs)
            st->cs_XCoreWorstUs = back;
    }

    *prevUs = now;
    *prevTsc = tsc;
}

static void ClockReader(void)
{
    struct ClockWorker *w = (struct ClockWorker *)FindTask(NULL)->tc_UserData;
    struct Device *TimerBase;
    struct timerequest *tr;
    struct MsgPort *tport;
    struct timeval tv;
    UQUAD prevUp = 0, prevUpTsc = 0, prevSys = 0, prevSysTsc = 0;
    UQUAD tscEnd;
    ULONG n = 0;

    tr = open_timer(&tport);
    if (!tr)
    {
        w->cw_Ready = 2;
        Wait(0);
    }
    TimerBase = (struct Device *)tr->tr_node.io_Device;

    __sync_synchronize();
    w->cw_Ready = 1;

    tscEnd = tsc_now() + g_TscHz * (UQUAD)g_Secs;
    do
    {
        GetUpTime(&tv);
        check(&w->cw_Up, &tv, &prevUp, &prevUpTsc, &g_NewestUp);
        GetSysTime(&tv);
        check(&w->cw_Sys, &tv, &prevSys, &prevSysTsc, &g_NewestSys);

        if (KernelBase && ((++n & 0x3ff) == 0))
        {
            w->cw_RanOn = (ULONG)KrnGetCPUNumber();
            if (w->cw_RanOn != w->cw_Cpu)
                w->cw_Moved++;
        }
    } while (tsc_now() < tscEnd);

    close_timer(tr, tport);
    __sync_synchronize();
    w->cw_Done = 1;
    Wait(0);
}

static void report(const char *what, ULONG cpu, const struct ClockStat *st)
{
    bug("[smpclock] CPU #%03lu %s: samples=%lu backward=%lu (worst %luus) jump=%lu (worst %luus) xcore=%lu (worst %luus) badmicro=%lu\n",
        (unsigned long)cpu, what,
        (unsigned long)st->cs_Samples,
        (unsigned long)st->cs_Backward, (unsigned long)st->cs_BackwardWorstUs,
        (unsigned long)st->cs_Jump, (unsigned long)st->cs_JumpWorstUs,
        (unsigned long)st->cs_XCore, (unsigned long)st->cs_XCoreWorstUs,
        (unsigned long)st->cs_BadMicro);
}

int main(void)
{
    struct RDArgs *rda;
    IPTR args[3] = { 0, 0, 0 };
    struct Device *TimerBase;
    struct timerequest *tr;
    struct MsgPort *tport;
    ULONG coreCount = 1, maxCpus = 0, jumpms = DEF_JUMPMS;
    ULONG i, started = 0, spins;
    ULONG totalBad = 0, totalSamples = 0;

    KernelBase = OpenResource("kernel.resource");
    if (KernelBase)
        coreCount = (ULONG)KrnGetCPUCount();

    rda = ReadArgs(ARG_TEMPLATE, args, NULL);
    if (rda)
    {
        if (args[ARG_MAXCPU]) maxCpus = (ULONG)*(LONG *)args[ARG_MAXCPU];
        if (args[ARG_SECS])   g_Secs  = (ULONG)*(LONG *)args[ARG_SECS];
        if (args[ARG_JUMPMS]) jumpms  = (ULONG)*(LONG *)args[ARG_JUMPMS];
        FreeArgs(rda);
    }
    if (!g_Secs) g_Secs = DEF_SECS;
    if (!jumpms) jumpms = DEF_JUMPMS;
    g_JumpUs = (UQUAD)jumpms * 1000ULL;
    if (maxCpus == 0 || maxCpus > coreCount)
        maxCpus = coreCount;
    if (maxCpus > MAX_CORES)
        maxCpus = MAX_CORES;
    g_NumCores = maxCpus;

    tr = open_timer(&tport);
    if (!tr)
    {
        bug("[smpclock] cannot open timer.device\n");
        return RETURN_FAIL;
    }
    TimerBase = (struct Device *)tr->tr_node.io_Device;
    g_TscHz = calibrate_tsc_hz(TimerBase);
    if (g_TscHz < 100000000ULL || g_TscHz > 100000000000ULL)
    {
        bug("[smpclock] TSC calibration implausible (%lu Hz) - cannot judge jumps\n",
            (unsigned long)g_TscHz);
        close_timer(tr, tport);
        return RETURN_FAIL;
    }

    bug("[smpclock] start: cpus=%lu (of %lu), secs=%lu, jump>=%lums, tsc=%lu Hz\n",
        (unsigned long)maxCpus, (unsigned long)coreCount, (unsigned long)g_Secs,
        (unsigned long)jumpms, (unsigned long)g_TscHz);
    if (maxCpus < 2)
        bug("[smpclock] single core - BASELINE, not an SMP test\n");

    for (i = 0; i < maxCpus; i++)
    {
        struct ClockWorker *w = &g_Workers[i];

        w->cw_Cpu = i;
        __sprintf(w->cw_Name, "SMP-Clk.%03lu", (unsigned long)i);
        if (KernelBase)
        {
            w->cw_Mask = KrnAllocCPUMask();
            if (w->cw_Mask)
            {
                KrnClearCPUMask(w->cw_Mask);
                KrnGetCPUMask(i, w->cw_Mask);
            }
        }
        w->cw_Task = NewCreateTask(TASKTAG_NAME,      (IPTR)w->cw_Name,
                                   TASKTAG_PRI,       0,
                                   TASKTAG_PC,        (IPTR)ClockReader,
                                   TASKTAG_STACKSIZE, STACKSIZE,
                                   TASKTAG_USERDATA,  (IPTR)w,
                                   w->cw_Mask ? TASKTAG_AFFINITY : TAG_IGNORE, (IPTR)w->cw_Mask,
                                   TAG_DONE);
        if (w->cw_Task)
            started++;
        else
            bug("[smpclock] CPU #%03lu: could not create reader\n", (unsigned long)i);
    }
    bug("[smpclock] %lu/%lu readers created\n", (unsigned long)started, (unsigned long)maxCpus);

    /* The master only sleeps; the readers are the load. Bounded wait. */
    for (i = 0; i < maxCpus; i++)
    {
        for (spins = 0; g_Workers[i].cw_Task && !g_Workers[i].cw_Done && spins < (g_Secs + 10) * 50; spins++)
            Delay(1);
        if (g_Workers[i].cw_Task && !g_Workers[i].cw_Done)
            bug("[smpclock] CPU #%03lu: reader never finished (ready=%lu)\n",
                (unsigned long)i, (unsigned long)g_Workers[i].cw_Ready);
    }

    for (i = 0; i < maxCpus; i++)
    {
        struct ClockWorker *w = &g_Workers[i];

        if (!w->cw_Task)
            continue;
        report("GetUpTime ", i, &w->cw_Up);
        report("GetSysTime", i, &w->cw_Sys);
        if (w->cw_Moved)
            bug("[smpclock] CPU #%03lu: ran on CPU %lu for %lu checks - NOT pinned, figures suspect\n",
                (unsigned long)i, (unsigned long)w->cw_RanOn, (unsigned long)w->cw_Moved);
        totalSamples += w->cw_Up.cs_Samples + w->cw_Sys.cs_Samples;
        totalBad += w->cw_Up.cs_Backward + w->cw_Up.cs_Jump + w->cw_Up.cs_XCore + w->cw_Up.cs_BadMicro
                  + w->cw_Sys.cs_Backward + w->cw_Sys.cs_Jump + w->cw_Sys.cs_XCore + w->cw_Sys.cs_BadMicro;
    }

    /* Readers park in Wait(0) once done; they are ours to remove. The
     * affinity mask belongs to the task once created (CleanupETask frees it,
     * see smpstress.c), so it is only ours to free if the task never was. */
    for (i = 0; i < maxCpus; i++)
    {
        struct ClockWorker *w = &g_Workers[i];
        if (w->cw_Task && w->cw_Done)
        {
            RemTask(w->cw_Task);
            w->cw_Task = NULL;
        }
        else if (!w->cw_Task && w->cw_Mask)
            KrnFreeCPUMask(w->cw_Mask);
        w->cw_Mask = NULL;
    }

    bug("[smpclock] %s: %lu bad of %lu readings on %lu cores\n",
        totalBad ? "FAIL" : "PASS", (unsigned long)totalBad, (unsigned long)totalSamples,
        (unsigned long)maxCpus);
    close_timer(tr, tport);
    return totalBad ? RETURN_WARN : RETURN_OK;
}
