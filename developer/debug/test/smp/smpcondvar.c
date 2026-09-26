/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Measure what a pthread condition-variable handoff costs.

    Thread pools hand work over with a mutex and a condition variable: the
    producer signals, a worker wakes, does the work and signals back. When
    that round trip is slow, a pool with more threads gets slower, not
    faster - which is what llvmpipe showed under Chromium (8 rasterizer
    threads ran at a sixth of the speed of one).

    Three shapes are timed, each reported as median and worst round trip in
    microseconds:
      exec       two tasks bounce a signal with Signal()/Wait() - the kernel's
                 own wake-up and switch cost, with no pthread layer - on the
                 creator's CPU and on another CPU;
      ping-pong  two threads alternate through one mutex/condvar pair, first
                 with both on the creator's CPU, then with the second thread
                 on another CPU;
      fan-out    one thread wakes WORKERS threads and waits until all of them
                 report done (llvmpipe's per-flush pattern), workers on CPUs
                 1..N-1.

    Usage: SMP-CondVar [ROUNDS/N] [WORKERS/N] [MODE/K]
      ROUNDS   round trips per measurement (default 2000)
      WORKERS  fan-out workers (default: CPU count - 1, at least 1)
      MODE     run one measurement only: EXECSAME, EXECOTHER, PINGSAME,
               PINGOTHER or FANOUT (default: all), e.g. to sample the
               hypervisor's exit counters over a long run of one pattern
*/

#include <aros/debug.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <proto/utility.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_WORKERS 64

APTR KernelBase;

static long long now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int compare_ll(const void *a, const void *b)
{
    long long x = *(const long long *)a, y = *(const long long *)b;

    return (x > y) - (x < y);
}

static void report(const char *what, long long *samples, int count)
{
    qsort(samples, count, sizeof(*samples), compare_ll);
    printf("%-28s median %8lld us   max %8lld us   (%d round trips)\n", what,
           samples[count / 2] / 1000, samples[count - 1] / 1000, count);
    bug("[smpcondvar] %s median %lld us max %lld us n=%d\n", what,
        samples[count / 2] / 1000, samples[count - 1] / 1000, count);
}

/* Place the thread about to be created on one CPU (-1: leave it on the
 * creator's CPU). */
static void attr_on_cpu(pthread_attr_t *attr, int cpu)
{
    void *mask;

    pthread_attr_init(attr);
    if (cpu < 0 || !KernelBase)
        return;
    mask = KrnAllocCPUMask();
    if (!mask)
        return;
    KrnClearCPUMask(mask);
    KrnGetCPUMask(cpu, mask);
    pthread_attr_setaffinity_np(attr, 0, mask);
}

/* ---- exec Signal/Wait ping-pong ----------------------------------------- */

static struct
{
    struct Task *main_task;
    struct Task *partner_task;
    ULONG main_sig;
    ULONG partner_sig;
    volatile int ready;
    int rounds;
} ex;

static void *exec_partner(void *arg)
{
    BYTE bit = AllocSignal(-1);
    int i;

    (void)arg;
    ex.partner_sig = 1UL << bit;
    ex.partner_task = FindTask(NULL);
    ex.ready = 1;
    for (i = 0; i < ex.rounds; i++)
    {
        Wait(ex.partner_sig);
        Signal(ex.main_task, ex.main_sig);
    }
    FreeSignal(bit);
    return NULL;
}

static void run_exec(const char *what, int cpu, int rounds)
{
    long long *samples = malloc(rounds * sizeof(*samples));
    BYTE bit = AllocSignal(-1);
    pthread_attr_t attr;
    pthread_t partner;
    int i;

    if (!samples || bit < 0)
    {
        free(samples);
        return;
    }
    ex.main_task = FindTask(NULL);
    ex.main_sig = 1UL << bit;
    ex.ready = 0;
    ex.rounds = rounds;

    attr_on_cpu(&attr, cpu);
    if (pthread_create(&partner, &attr, exec_partner, NULL) != 0)
    {
        FreeSignal(bit);
        free(samples);
        return;
    }
    while (!ex.ready)
        Delay(1);

    for (i = 0; i < rounds; i++)
    {
        long long start = now_ns();

        Signal(ex.partner_task, ex.partner_sig);
        Wait(ex.main_sig);
        samples[i] = now_ns() - start;
    }

    pthread_join(partner, NULL);
    FreeSignal(bit);
    report(what, samples, rounds);
    free(samples);
}

/* ---- ping-pong ---------------------------------------------------------- */

static struct
{
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int turn;       /* 0: main's turn, 1: partner's */
    int rounds;
} pp;

static void *pingpong_partner(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < pp.rounds; i++)
    {
        pthread_mutex_lock(&pp.lock);
        while (pp.turn != 1)
            pthread_cond_wait(&pp.cond, &pp.lock);
        pp.turn = 0;
        pthread_cond_signal(&pp.cond);
        pthread_mutex_unlock(&pp.lock);
    }
    return NULL;
}

static void run_pingpong(const char *what, int cpu, int rounds)
{
    long long *samples = malloc(rounds * sizeof(*samples));
    pthread_attr_t attr;
    pthread_t partner;
    int i;

    if (!samples)
        return;
    pthread_mutex_init(&pp.lock, NULL);
    pthread_cond_init(&pp.cond, NULL);
    pp.turn = 0;
    pp.rounds = rounds;

    attr_on_cpu(&attr, cpu);
    if (pthread_create(&partner, &attr, pingpong_partner, NULL) != 0)
    {
        printf("%s: pthread_create failed\n", what);
        free(samples);
        return;
    }

    for (i = 0; i < rounds; i++)
    {
        long long start = now_ns();

        pthread_mutex_lock(&pp.lock);
        pp.turn = 1;
        pthread_cond_signal(&pp.cond);
        while (pp.turn != 0)
            pthread_cond_wait(&pp.cond, &pp.lock);
        pthread_mutex_unlock(&pp.lock);
        samples[i] = now_ns() - start;
    }

    pthread_join(partner, NULL);
    report(what, samples, rounds);
    pthread_cond_destroy(&pp.cond);
    pthread_mutex_destroy(&pp.lock);
    free(samples);
}

/* ---- fan-out ------------------------------------------------------------ */

static struct
{
    pthread_mutex_t lock;
    pthread_cond_t work;    /* main -> workers */
    pthread_cond_t done;    /* workers -> main */
    int generation;         /* bumped per round */
    int finished;           /* workers done with this generation */
    int quit;
} fo;

static void *fanout_worker(void *arg)
{
    int seen = 0;

    (void)arg;
    pthread_mutex_lock(&fo.lock);
    for (;;)
    {
        while (fo.generation == seen && !fo.quit)
            pthread_cond_wait(&fo.work, &fo.lock);
        if (fo.quit)
            break;
        seen = fo.generation;
        fo.finished++;
        pthread_cond_signal(&fo.done);
    }
    pthread_mutex_unlock(&fo.lock);
    return NULL;
}

static void run_fanout(int workers, int cpus, int rounds)
{
    long long *samples = malloc(rounds * sizeof(*samples));
    pthread_t threads[MAX_WORKERS];
    char what[64];
    int i;

    if (!samples)
        return;
    memset(&fo, 0, sizeof(fo));
    pthread_mutex_init(&fo.lock, NULL);
    pthread_cond_init(&fo.work, NULL);
    pthread_cond_init(&fo.done, NULL);

    for (i = 0; i < workers; i++)
    {
        pthread_attr_t attr;

        attr_on_cpu(&attr, cpus > 1 ? 1 + i % (cpus - 1) : -1);
        pthread_create(&threads[i], &attr, fanout_worker, NULL);
    }

    for (i = 0; i < rounds; i++)
    {
        long long start = now_ns();

        pthread_mutex_lock(&fo.lock);
        fo.finished = 0;
        fo.generation++;
        pthread_cond_broadcast(&fo.work);
        while (fo.finished < workers)
            pthread_cond_wait(&fo.done, &fo.lock);
        pthread_mutex_unlock(&fo.lock);
        samples[i] = now_ns() - start;
    }

    pthread_mutex_lock(&fo.lock);
    fo.quit = 1;
    pthread_cond_broadcast(&fo.work);
    pthread_mutex_unlock(&fo.lock);
    for (i = 0; i < workers; i++)
        pthread_join(threads[i], NULL);

    snprintf(what, sizeof(what), "fan-out to %d workers", workers);
    report(what, samples, rounds);
    free(samples);
}

int main(void)
{
    IPTR args[3] = {0, 0, 0};
    struct RDArgs *rda = ReadArgs("ROUNDS/N,WORKERS/N,MODE/K", args, NULL);
    char mode[16] = "";
    int rounds = 2000, cpus = 1, workers;

    if (!rda)
    {
        PrintFault(IoErr(), "SMP-CondVar");
        return RETURN_FAIL;
    }
    if (args[0])
        rounds = *(LONG *)args[0];
    if (rounds < 10)
        rounds = 10;

    KernelBase = OpenResource("kernel.resource");
    if (KernelBase)
        cpus = KrnGetCPUCount();
    workers = args[1] ? *(LONG *)args[1] : (cpus > 1 ? cpus - 1 : 1);
    if (workers < 1)
        workers = 1;
    if (workers > MAX_WORKERS)
        workers = MAX_WORKERS;
    if (args[2])
        strncpy(mode, (const char *)args[2], sizeof(mode) - 1);
    FreeArgs(rda);

    printf("SMP-CondVar: %d CPUs, %d rounds%s%s\n", cpus, rounds,
           mode[0] ? ", mode " : "", mode);
    bug("[smpcondvar] begin %s\n", mode[0] ? mode : "ALL");
#define WANT(m) (!mode[0] || !Stricmp(mode, m))
    if (WANT("EXECSAME"))
        run_exec("exec Signal/Wait, same CPU", -1, rounds);
    if (cpus > 1 && WANT("EXECOTHER"))
        run_exec("exec Signal/Wait, other CPU", 1, rounds);
    if (WANT("PINGSAME"))
        run_pingpong("ping-pong, same CPU", -1, rounds);
    if (cpus > 1 && WANT("PINGOTHER"))
        run_pingpong("ping-pong, other CPU", 1, rounds);
    if (WANT("FANOUT"))
        run_fanout(workers, cpus, rounds);
#undef WANT
    bug("[smpcondvar] end\n");

    return RETURN_OK;
}
