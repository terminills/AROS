/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnSetProtection() from several CPUs at once must not corrupt the
          shared page tables.

    Changing the protection of a 4 KiB page inside a 2 MiB mapping splits that
    mapping into 512 small pages, using a page from a shared pool. Every CPU
    edits the same tables. This test has a thread pinned to every CPU take a
    fresh 2 MiB-aligned block, release them all together so each splits its
    own block at the same moment, then tag every page of its block and check
    the tags. A block that shares its page table with another block sees the
    other thread's tags.

    Usage: SMP-ProtRace [ROUNDS/N]
      ROUNDS   rounds of concurrent splits (default 40); each round uses one
               pool page per CPU
*/

#include <aros/debug.h>
#include <aros/kernel.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>

#include <pthread.h>
#include <stdio.h>

#define ARG_TEMPLATE "ROUNDS/N"
#define MAX_CPUS     64
#define SIZE_2M      0x200000UL
#define PAGE         4096UL
#define PAGES        (SIZE_2M / PAGE)

APTR KernelBase;

static struct
{
    ULONG          cpus;
    ULONG          rounds;
    volatile ULONG arrived;
    volatile ULONG generation;
    volatile ULONG bad[MAX_CPUS];
    volatile ULONG nomem[MAX_CPUS];
} t;

static void barrier(void)
{
    ULONG gen = __atomic_load_n(&t.generation, __ATOMIC_ACQUIRE);

    if (__atomic_add_fetch(&t.arrived, 1, __ATOMIC_ACQ_REL) == t.cpus)
    {
        __atomic_store_n(&t.arrived, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&t.generation, gen + 1, __ATOMIC_RELEASE);
    }
    else
        while (__atomic_load_n(&t.generation, __ATOMIC_ACQUIRE) == gen)
            asm volatile("pause");
}

static void *worker(void *arg)
{
    ULONG id = (ULONG)(IPTR)arg, r, p;

    for (r = 0; r < t.rounds; r++)
    {
        APTR raw = AllocMem(2 * SIZE_2M, MEMF_PUBLIC);
        volatile ULONG *blk = NULL;

        if (raw)
            blk = (volatile ULONG *)(((IPTR)raw + SIZE_2M - 1) & ~(SIZE_2M - 1));
        else
            t.nomem[id]++;

        barrier();
        if (blk)
            KrnSetProtection((APTR)(blk + (id % PAGES) * (PAGE / 4)), PAGE,
                             MAP_Readable | MAP_Writable);
        barrier();
        if (blk)
            for (p = 0; p < PAGES; p++)
                blk[p * (PAGE / 4)] = (id << 24) | (r << 12) | p;
        barrier();
        if (blk)
        {
            for (p = 0; p < PAGES; p++)
            {
                ULONG v = blk[p * (PAGE / 4)];

                if (v != ((id << 24) | (r << 12) | p))
                {
                    if (t.bad[id]++ < 4)
                        bug("[smpprotrace] round %lu CPU %lu page %lu at 0x%p: read %08lx\n",
                            (unsigned long)r, (unsigned long)id, (unsigned long)p,
                            &blk[p * (PAGE / 4)], (unsigned long)v);
                }
            }
        }
        barrier();
        if (raw)
            FreeMem(raw, 2 * SIZE_2M);
    }
    return NULL;
}

int main(void)
{
    IPTR args[1] = { 0 };
    struct RDArgs *rda = ReadArgs(ARG_TEMPLATE, args, NULL);
    ULONG cpus = 1, i, bad = 0, nomem = 0;
    pthread_t th[MAX_CPUS];

    t.rounds = 40;
    if (rda)
    {
        if (args[0])
            t.rounds = *(ULONG *)args[0];
        FreeArgs(rda);
    }
    KernelBase = OpenResource("kernel.resource");
    if (KernelBase)
        cpus = KrnGetCPUCount();
    if (cpus > MAX_CPUS)
        cpus = MAX_CPUS;
    if (cpus < 2)
    {
        bug("[smpprotrace] SKIP: %lu CPU\n", (unsigned long)cpus);
        return RETURN_WARN;
    }
    t.cpus = cpus;

    bug("[smpprotrace] begin: %lu CPUs, %lu rounds\n", (unsigned long)cpus,
        (unsigned long)t.rounds);
    for (i = 0; i < cpus; i++)
    {
        pthread_attr_t attr;
        void *mask = KrnAllocCPUMask();

        pthread_attr_init(&attr);
        if (mask)
        {
            KrnClearCPUMask(mask);
            KrnGetCPUMask(i, mask);
            pthread_attr_setaffinity_np(&attr, 0, mask);
        }
        pthread_create(&th[i], &attr, worker, (void *)(IPTR)i);
    }
    for (i = 0; i < cpus; i++)
    {
        pthread_join(th[i], NULL);
        bad += t.bad[i];
        nomem += t.nomem[i];
    }

    printf("SMP-ProtRace: %lu rounds on %lu CPUs, %lu bad pages, %lu blocks unallocated\n",
           (unsigned long)t.rounds, (unsigned long)cpus, (unsigned long)bad,
           (unsigned long)nomem);
    if (bad)
        bug("[smpprotrace] FAIL: %lu bad pages\n", (unsigned long)bad);
    else
        bug("[smpprotrace] PASS: %lu rounds on %lu CPUs\n", (unsigned long)t.rounds,
            (unsigned long)cpus);
    return bad ? RETURN_FAIL : RETURN_OK;
}
