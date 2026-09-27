/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>
#include <asm/cpu.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "kernel_base.h"
#include "kernel_intern.h"
#if defined(__AROSEXEC_SMP__)
#include "kernel_ipi.h"
#include "apic.h"
#endif

#define MAP_2M_SHIFT 21
#define MAP_2M_SIZE  (1UL << MAP_2M_SHIFT)
#define MAP_2M_MASK  (MAP_2M_SIZE - 1)

extern int Kernel_13_KrnIsSuper(void);

static APTR alloc_page_table(void)
{
    IPTR raw;

    raw = (IPTR)AllocMem(PAGE_SIZE * 2, MEMF_PUBLIC | MEMF_CLEAR);
    if (!raw)
        return NULL;

    return (APTR)((raw + PAGE_SIZE - 1) & ~(IPTR)(PAGE_SIZE - 1));
}

static APTR table_addr(ULONG base_low, ULONG base_high)
{
    return (APTR)(((IPTR)base_low << 12) | ((IPTR)base_high << 32));
}

static void set_pml4e(struct PML4E *entry, APTR table)
{
    entry->p = 1;
    entry->rw = 1;
    entry->us = 1;
    entry->pwt = 0;
    entry->pcd = 0;
    entry->a = 0;
    entry->mbz = 0;
    entry->avl = 0;
    entry->base_low = (IPTR)table >> 12;
    entry->base_high = ((IPTR)table >> 32) & 0x000FFFFF;
    entry->avail = 0;
    entry->nx = 0;
}

static void set_pdpe(struct PDPE *entry, APTR table)
{
    entry->p = 1;
    entry->rw = 1;
    entry->us = 1;
    entry->pwt = 0;
    entry->pcd = 0;
    entry->a = 0;
    entry->__pad0 = 0;
    entry->mbz = 0;
    entry->avl = 0;
    entry->base_low = (IPTR)table >> 12;
    entry->base_high = ((IPTR)table >> 32) & 0x000FFFFF;
    entry->avail = 0;
    entry->nx = 0;
}

static void set_pde_2m(struct PDE2M *entry, IPTR phys, KRN_MapAttr flags)
{
    entry->p = (flags & (MAP_Readable | MAP_Writable | MAP_Executable)) ? 1 : 0;
    entry->rw = (flags & MAP_Writable) ? 1 : 0;
    entry->us = 1;
    entry->pwt = (flags & MAP_WriteThrough) ? 1 : 0;
    entry->pcd = (flags & MAP_CacheInhibit) ? 1 : 0;
    entry->a = 0;
    entry->d = 0;
    entry->ps = 1;
    entry->g = 0;
    entry->avl = 0;
    entry->pat = 0;
    entry->base_low = phys >> 13;
    entry->base_high = (phys >> 32) & 0x000FFFFF;
    entry->avail = 0;
    entry->nx = 0;
}

static int map_global_2m(void *virt, void *phys, uint32_t length,
    KRN_MapAttr flags)
{
    struct CPUMMUConfig *MMU = &__KernBootPrivate->MMU;
    struct PML4E *pml4 = MMU->mmu_PML4;
    IPTR vaddr = (IPTR)virt;
    IPTR paddr = phys ? (IPTR)phys : vaddr;
    IPTR vcur;
    IPTR pcur;
    IPTR vend;

    if (!pml4 || !length)
        return -1;

    if ((vaddr ^ paddr) & MAP_2M_MASK)
        return -1;

    vcur = vaddr & ~(IPTR)MAP_2M_MASK;
    pcur = paddr & ~(IPTR)MAP_2M_MASK;
    vend = (vaddr + length + MAP_2M_MASK) & ~(IPTR)MAP_2M_MASK;

    while (vcur < vend) {
        ULONG pml4_off = (vcur >> 39) & 0x1ff;
        ULONG pdpe_off = (vcur >> 30) & 0x1ff;
        ULONG pde_off  = (vcur >> 21) & 0x1ff;
        struct PDPE *pdpe;
        struct PDE2M *pde;

        if (!pml4[pml4_off].p) {
            pdpe = alloc_page_table();
            if (!pdpe)
                return -1;
            set_pml4e(&pml4[pml4_off], pdpe);
        } else {
            pdpe = table_addr(pml4[pml4_off].base_low,
                pml4[pml4_off].base_high);
        }

        if (!pdpe[pdpe_off].p) {
            pde = alloc_page_table();
            if (!pde)
                return -1;
            set_pdpe(&pdpe[pdpe_off], pde);
        } else {
            pde = table_addr(pdpe[pdpe_off].base_low,
                pdpe[pdpe_off].base_high);
        }

        set_pde_2m(&pde[pde_off], pcur, flags);
        asm volatile ("invlpg (%0)" :: "r"(vcur) : "memory");

        vcur += MAP_2M_SIZE;
        pcur += MAP_2M_SIZE;
    }

    wrcr(cr3, MMU->mmu_PML4);
    return 0;
}

#if defined(__AROSEXEC_SMP__)
/*
 * All CPUs share these page tables, and each caches translations of them:
 * a changed entry must be flushed on every CPU, not only the caller's (a
 * BAR remapped uncached, say, must not keep a write-back translation on
 * another core). invlpg rather than a CR3 reload, because the entries
 * replaced may have been global. Runs from the IPI drain on each other CPU.
 */
static AROS_UFH3(IPTR, tlbflush_hook,
    AROS_UFHA(struct IPIHook *, hook, A0),
    AROS_UFHA(APTR, object, A2),
    AROS_UFHA(APTR, message, A1))
{
    AROS_USERFUNC_INIT

    IPTR vcur = hook->ih_Args[0], vend = hook->ih_Args[1];

    for (; vcur < vend; vcur += MAP_2M_SIZE)
        asm volatile ("invlpg (%0)" :: "r"(vcur) : "memory");

    return 0;

    AROS_USERFUNC_EXIT
}

/* Flush [virt, virt+length) on every other running CPU, and wait for it */
static void tlb_shootdown(struct KernelBase *KernelBase, void *virt, uint32_t length)
{
    struct PlatformData *pdata = KernelBase->kb_PlatformData;
    struct Hook h;
    IPTR args[2];

    /* Nothing to do (and nobody to wait for) until a second CPU runs */
    if (!pdata || !pdata->kb_APIC || pdata->kb_APIC->apic_count < 2)
        return;

    args[0] = (IPTR)virt & ~(IPTR)MAP_2M_MASK;
    args[1] = ((IPTR)virt + length + MAP_2M_MASK) & ~(IPTR)MAP_2M_MASK;
    h.h_Entry = (HOOKFUNC)tlbflush_hook;
    h.h_SubEntry = NULL;
    h.h_Data = NULL;
    core_DoCallIPI(&h, (void *)TASKAFFINITY_ALL_BUT_SELF, 0, 2, args, KernelBase);
}
#endif

/* See rom/kernel/mapglobal.c for documentation. */
AROS_LH4I(int, KrnMapGlobal,
    AROS_LHA(void *, virt, A0),
    AROS_LHA(void *, phys, A1),
    AROS_LHA(uint32_t, length, D0),
    AROS_LHA(KRN_MapAttr, flags, D1),
    struct KernelBase *, KernelBase, 16, Kernel)
{
    AROS_LIBFUNC_INIT

    APTR ssp = NULL;
    int ret;

    if (!Kernel_13_KrnIsSuper()) {
        ssp = SuperState();
        if (!ssp)
            return -1;
    }

    ret = map_global_2m(virt, phys, length, flags);
#if defined(__AROSEXEC_SMP__)
    if (ret == 0)
        tlb_shootdown(KernelBase, virt, length);
#endif

    if (ssp)
        UserState(ssp);

    return ret;

    AROS_LIBFUNC_EXIT
}
