/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: KrnMapGlobal() remaps must reach every CPU's TLB.

    All CPUs share the kernel page tables, and each caches translations of
    them. A remap that only flushes the calling CPU leaves the others using
    the old physical page. This test maps an otherwise unused 2 MiB window to
    buffer A, has a thread pinned to every CPU read through it (so each caches
    the translation), remaps the window to buffer B, and has every thread read
    again: each must now see B.

    Usage: SMP-TLB [ROUNDS/N]
      ROUNDS   remaps to try, alternating A and B (default 20)
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
#define WINDOW       0x3000000000ULL     /* 192 GiB: above the identity map */
#define SIZE_2M      0x200000UL
#define PAT_A        0x11111111UL
#define PAT_B        0x22222222UL

APTR KernelBase;

static struct
{
    volatile ULONG *window;
    volatile int    phase;               /* bumped by main for each read pass */
    volatile int    stop;
    volatile int    done[MAX_CPUS];      /* phase each reader has finished */
    volatile ULONG  seen[MAX_CPUS];
} t;

static void *reader(void *arg)
{
    int id = (int)(IPTR)arg, last = 0;

    while (!t.stop)
    {
        int ph = t.phase;

        if (ph != last)
        {
            ULONG v = 0;
            int i;

            /* Read repeatedly, so this CPU's TLB holds the translation */
            for (i = 0; i < 1000; i++)
                v = t.window[(i * 1024) & ((SIZE_2M / 4) - 1)];
            t.seen[id] = v;
            last = ph;
            t.done[id] = ph;
        }
        else
            asm volatile("pause");
    }
    return NULL;
}

static void fill(ULONG *p, ULONG v)
{
    ULONG i;

    for (i = 0; i < SIZE_2M / 4; i++)
        p[i] = v;
}

int main(void)
{
    IPTR args[1] = { 0 };
    struct RDArgs *rda = ReadArgs(ARG_TEMPLATE, args, NULL);
    ULONG rounds = 20, cpus = 1, r, stale = 0, i;
    APTR raw;
    ULONG *bufA, *bufB;
    pthread_t th[MAX_CPUS];
    int ph = 0;

    if (rda)
    {
        if (args[0])
            rounds = *(ULONG *)args[0];
        FreeArgs(rda);
    }
    KernelBase = OpenResource("kernel.resource");
    if (KernelBase)
        cpus = KrnGetCPUCount();
    if (cpus > MAX_CPUS)
        cpus = MAX_CPUS;
    if (cpus < 2)
    {
        bug("[smptlb] SKIP: %lu CPU\n", (unsigned long)cpus);
        return RETURN_WARN;
    }

    raw = AllocMem(3 * SIZE_2M, MEMF_PUBLIC);
    if (!raw)
    {
        bug("[smptlb] FAIL: no memory\n");
        return RETURN_FAIL;
    }
    bufA = (ULONG *)(((IPTR)raw + SIZE_2M - 1) & ~(SIZE_2M - 1));
    bufB = (ULONG *)((IPTR)bufA + SIZE_2M);
    fill(bufA, PAT_A);
    fill(bufB, PAT_B);
    t.window = (volatile ULONG *)(IPTR)WINDOW;

    if (KrnMapGlobal((APTR)(IPTR)WINDOW, bufA, SIZE_2M, MAP_Readable | MAP_Writable) != 0)
    {
        bug("[smptlb] FAIL: KrnMapGlobal refused the window\n");
        FreeMem(raw, 3 * SIZE_2M);
        return RETURN_FAIL;
    }

    bug("[smptlb] begin: %lu CPUs, %lu remaps\n", (unsigned long)cpus, (unsigned long)rounds);
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
        pthread_create(&th[i], &attr, reader, (void *)(IPTR)i);
    }

    for (r = 0; r <= rounds; r++)
    {
        ULONG want = (r & 1) ? PAT_B : PAT_A;

        if (r > 0 &&
            KrnMapGlobal((APTR)(IPTR)WINDOW, (r & 1) ? bufB : bufA, SIZE_2M,
                         MAP_Readable | MAP_Writable) != 0)
        {
            bug("[smptlb] FAIL: remap %lu refused\n", (unsigned long)r);
            break;
        }
        t.phase = ++ph;
        for (i = 0; i < cpus; i++)
            while (t.done[i] != ph)
                Delay(1);
        for (i = 0; i < cpus; i++)
        {
            if (t.seen[i] != want)
            {
                stale++;
                bug("[smptlb] remap %lu: CPU %lu read %08lx, expected %08lx\n",
                    (unsigned long)r, (unsigned long)i, (unsigned long)t.seen[i], (unsigned long)want);
            }
        }
    }

    t.stop = 1;
    for (i = 0; i < cpus; i++)
        pthread_join(th[i], NULL);

    printf("SMP-TLB: %lu remaps on %lu CPUs, %lu stale reads\n",
           (unsigned long)rounds, (unsigned long)cpus, (unsigned long)stale);
    if (stale)
        bug("[smptlb] FAIL: %lu stale reads\n", (unsigned long)stale);
    else
        bug("[smptlb] PASS: %lu remaps on %lu CPUs\n", (unsigned long)rounds, (unsigned long)cpus);
    /* The window stays mapped (there is no KrnUnmapGlobal on pc); the buffers must too */
    return stale ? RETURN_FAIL : RETURN_OK;
}
