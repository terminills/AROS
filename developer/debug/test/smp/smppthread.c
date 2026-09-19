/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Prove pthread_attr_setaffinity_np() actually places threads.

    AROS does not balance tasks: a new task inherits an affinity mask holding
    only the CPU that created it (rom/exec/exec_util.c, Exec_InitETask) and
    nothing migrates it afterwards. Every pthread therefore used to land on the
    creating core no matter how many cores the machine had - which is why
    Chromium, whose thread pool is sized correctly from KrnGetCPUCount(), still
    ran on one core.

    This is the acceptance test for the plumbing that fixes that:
      pthread_attr_setaffinity_np -> pthread_attr_t.cpuaffinity
        -> NP_Affinity -> NewAddTask(TASKTAG_AFFINITY) -> PrepareContext
    Each thread reports the CPU it actually runs on, repeatedly, and the test
    fails if any thread is ever seen anywhere other than where it was pinned.
    Reporting "it was pinned" is not evidence; reporting where it RAN is.

    Usage: SMP-PThread [MAXCPU/N] [ITER/N]
*/

#include <aros/debug.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/kernel.h>

#include <pthread.h>

#define ARG_TEMPLATE "MAXCPU/N,ITER/N,SAMECPU/S"
#define MAX_CORES 256
#define DEF_ITER  2000

APTR KernelBase;

struct PinWorker
{
    ULONG            pw_Cpu;        /* where it was pinned      */
    volatile ULONG   pw_Samples;    /* how often it checked     */
    volatile ULONG   pw_Wrong;      /* seen on the wrong CPU    */
    volatile ULONG   pw_LastSeen;   /* most recent CPU observed */
    volatile ULONG   pw_Done;
};

static struct PinWorker g_Workers[MAX_CORES];
static ULONG            g_Iter = DEF_ITER;

static void *PinThread(void *arg)
{
    struct PinWorker *w = (struct PinWorker *)arg;
    ULONG n;

    for (n = 0; n < g_Iter; n++)
    {
        ULONG cpu = (ULONG)KrnGetCPUNumber();

        w->pw_LastSeen = cpu;
        if (cpu != w->pw_Cpu)
            w->pw_Wrong++;
        w->pw_Samples++;
    }

    w->pw_Done = 1;
    return NULL;
}

int main(void)
{
    struct RDArgs *rda;
    IPTR args[3] = { 0, 0, 0 };
    ULONG sameCpu;
    pthread_t tid[MAX_CORES];
    ULONG coreCount = 1, maxCpus = 0, i, created = 0, bad = 0, unpinned = 0;

    KernelBase = OpenResource("kernel.resource");
    if (KernelBase)
        coreCount = (ULONG)KrnGetCPUCount();

    rda = ReadArgs(ARG_TEMPLATE, args, NULL);
    if (rda)
    {
        if (args[0]) maxCpus = (ULONG)*(LONG *)args[0];
        if (args[1]) g_Iter  = (ULONG)*(LONG *)args[1];
        FreeArgs(rda);
    }
    sameCpu = args[2] ? 1 : 0;
    if (!g_Iter) g_Iter = DEF_ITER;
    if (maxCpus == 0 || maxCpus > coreCount) maxCpus = coreCount;
    if (maxCpus > MAX_CORES) maxCpus = MAX_CORES;

    bug("[smppthread] start: %lu thread(s), %lu checks each, mode=%s\n",
        (unsigned long)maxCpus, (unsigned long)g_Iter,
        sameCpu ? "SAMECPU (all on the creating core)" : "spread (one per core)");

    if (maxCpus < 2)
    {
        bug("[smppthread] only %lu CPU online - needs SMP; SKIP\n", (unsigned long)maxCpus);
        return RETURN_OK;
    }

    for (i = 0; i < maxCpus; i++)
    {
        struct PinWorker *w = &g_Workers[i];
        pthread_attr_t attr;
        void *mask = NULL;

        /*
         * SAMECPU pins every thread to the CREATING core instead of spreading
         * them. That separates two very different failures: if SAMECPU works
         * and the spread run does not, the affinity plumbing is sound and what
         * breaks is cross-CPU placement (newaddtask.c:90-102 fires
         * KrnScheduleCPU(), i.e. an IPI, when the new task may not run here).
         * If SAMECPU also hangs, the bug is in this plumbing.
         */
        w->pw_Cpu = sameCpu ? (ULONG)KrnGetCPUNumber() : i;

        pthread_attr_init(&attr);
        if (KernelBase)
        {
            /* One mask per thread: exec takes ownership and frees it with the
             * task, so a shared mask would be freed out from under the others. */
            mask = KrnAllocCPUMask();
            if (mask)
            {
                KrnClearCPUMask(mask);
                KrnGetCPUMask(w->pw_Cpu, mask);
                pthread_attr_setaffinity_np(&attr, 0, mask);
            }
        }

        bug("[smppthread] creating thread for CPU #%03lu ...\n", (unsigned long)w->pw_Cpu);
        if (pthread_create(&tid[i], &attr, PinThread, w) == 0)
            created++;
        else
            bug("[smppthread] CPU #%03lu: pthread_create failed\n", (unsigned long)i);

        pthread_attr_destroy(&attr);
    }

    for (i = 0; i < maxCpus; i++)
        pthread_join(tid[i], NULL);

    bug("[smppthread] %lu/%lu threads created\n", (unsigned long)created, (unsigned long)maxCpus);

    for (i = 0; i < maxCpus; i++)
    {
        struct PinWorker *w = &g_Workers[i];

        if (!w->pw_Samples)
        {
            bug("[smppthread]  CPU #%03lu: NEVER RAN\n", (unsigned long)i);
            bad++;
            continue;
        }
        if (w->pw_Wrong)
        {
            bug("[smppthread]  CPU #%03lu: *** NOT PINNED *** %lu/%lu samples on the wrong CPU (last seen %lu)\n",
                (unsigned long)i, (unsigned long)w->pw_Wrong,
                (unsigned long)w->pw_Samples, (unsigned long)w->pw_LastSeen);
            unpinned++;
        }
    }

    bug("[smppthread] DONE: %lu pinned correctly, %lu not pinned, %lu never ran\n",
        (unsigned long)(maxCpus - unpinned - bad), (unsigned long)unpinned, (unsigned long)bad);

    return (unpinned || bad) ? RETURN_WARN : RETURN_OK;
}
