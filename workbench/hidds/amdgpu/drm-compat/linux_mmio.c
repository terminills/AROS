/*
    Copyright 2026, The AROS Development Team. All rights reserved.
*/

#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <aros/kernel.h>

/*
 * Platform registers outside the PCI windows are not mapped by default;
 * map them identity, the way the PCI driver maps its own controller.
 */
void *compat_map_mmio(unsigned long pa, unsigned long size)
{
    APTR KernelBase = OpenResource("kernel.resource");

    if (!KernelBase)
        return NULL;
#if defined(__riscv) || defined(__aarch64__) || defined(__arm__)
    if (!KrnMapGlobal((APTR)pa, (APTR)pa, size, MAP_Readable | MAP_Writable))
        return NULL;
#endif
    return (void *)pa;
}

/*
 * Write-combining for the 2 MiB pages wholly inside [va, va + size). An
 * identity mapping covers the whole range already; this only changes the
 * memory type.
 */
int compat_map_write_combining(void *va, unsigned long size)
{
#if defined(__x86_64__)
    const IPTR big = 2UL << 20, chunk = 1UL << 30;
    APTR KernelBase = OpenResource("kernel.resource");
    IPTR start = ((IPTR)va + big - 1) & ~(big - 1);
    IPTR end = ((IPTR)va + size) & ~(big - 1);

    if (!KernelBase)
        return -1;
    while (start < end)
    {
        IPTR len = end - start > chunk ? chunk : end - start;

        if (KrnMapGlobal((APTR)start, (APTR)start, len,
                         MAP_Readable | MAP_Writable | MAP_WriteCombine))
            return -1;
        start += len;
    }
#endif
    return 0;
}

/* laptop PSP diagnosis (not for commit): effective memory type of one address */
#if defined(__x86_64__)
#include <asm/cpu.h>
#include <exec/execbase.h>
#include <exec/memory.h>

void compat_dump_memtype(const char *tag, void *va)
{
    IPTR a = (IPTR)va, tmask = 0x000ffffffffff000UL;
    UQUAD cr3, l4, l3, l2 = 0, l1 = 0, cap, def, pat, base[8], mask[8];
    unsigned int i, n;
    struct MemHeader *mh;
    APTR ssp;

    asm volatile("cli" ::: "memory");
    ssp = SuperState();
    cr3 = rdcr(cr3);
    l4 = ((UQUAD *)(cr3 & tmask))[(a >> 39) & 0x1ff];
    l3 = (l4 & 1) ? ((UQUAD *)(l4 & tmask))[(a >> 30) & 0x1ff] : 0;
    if ((l3 & 1) && !(l3 & 0x80))
        l2 = ((UQUAD *)(l3 & tmask))[(a >> 21) & 0x1ff];
    if ((l2 & 1) && !(l2 & 0x80))
        l1 = ((UQUAD *)(l2 & tmask))[(a >> 12) & 0x1ff];
    cap = rdmsrq(0xFE);
    def = rdmsrq(0x2FF);
    pat = rdmsrq(0x277);
    n = cap & 0xff;
    if (n > 8)
        n = 8;
    for (i = 0; i < n; i++)
    {
        base[i] = rdmsrq(0x200 + 2 * i);
        mask[i] = rdmsrq(0x201 + 2 * i);
    }
    if (ssp)
        UserState(ssp);
    asm volatile("sti" ::: "memory");

    bug("[memtype] %s %p: pdpe %016llx pde %016llx pte %016llx (pde pwt %d pcd %d pat %d) PAT %016llx MTRRdef %llx\n",
        tag, va, l3, l2, l1, (int)((l2 >> 3) & 1), (int)((l2 >> 4) & 1),
        (int)((l2 >> 12) & 1), pat, def);
    for (i = 0; i < n; i++)
    {
        if (!(mask[i] & 0x800))
            continue;
        bug("[memtype]   mtrr%u base %013llx type %llu mask %013llx%s\n", i,
            base[i] & ~0xfffULL, base[i] & 0xff, mask[i] & ~0xfffULL,
            ((a & mask[i] & ~0xfffULL) == (base[i] & mask[i] & ~0xfffULL)) ? " <- covers" : "");
    }
    Forbid();
    ForeachNode(&SysBase->MemList, mh)
        if ((IPTR)mh->mh_Lower <= a && a < (IPTR)mh->mh_Upper)
            bug("[memtype]   inside exec memory '%s' %p-%p\n", mh->mh_Node.ln_Name, mh->mh_Lower, mh->mh_Upper);
    Permit();
}
#endif

#include <proto/dos.h>
#include <dos/dos.h>
#include <stdio.h>

/* Diagnostics: drop a buffer into SYS:amdgpu-dump/<name>. */
int compat_dump_file(const char *name, const void *data, unsigned long size)
{
    char path[64];
    BPTR lock, file;
    LONG done = 0;

    lock = Lock("SYS:amdgpu-dump", ACCESS_READ);
    if (!lock)
        lock = CreateDir("SYS:amdgpu-dump");
    if (!lock)
        return -1;
    UnLock(lock);

    snprintf(path, sizeof(path), "SYS:amdgpu-dump/%s", name);
    file = Open(path, MODE_NEWFILE);
    if (!file)
        return -1;
    while ((unsigned long)done < size) {
        LONG n = Write(file, (APTR)((const char *)data + done), size - done);
        if (n <= 0)
            break;
        done += n;
    }
    Close(file);
    return (unsigned long)done == size ? 0 : -1;
}
