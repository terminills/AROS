/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>
#include <asm/cpu.h>
#include <exec/memory.h>
#include <proto/exec.h>
#define __KERNEL_NOLIBBASE__
#include <proto/kernel.h>

#include "kernel_base.h"
#include "kernel_intern.h"
#if defined(__AROSEXEC_SMP__)
#include "kernel_ipi.h"
#include "apic.h"
#endif

#define MAP_2M_SHIFT 21
#define MAP_2M_SIZE  (1UL << MAP_2M_SHIFT)
#define MAP_2M_MASK  (MAP_2M_SIZE - 1)
#define MAP_4K_MASK  (PAGE_SIZE - 1)

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

/*
 * The tables are live: other CPUs walk them while an entry is filled in.
 * Build each entry aside and store it with one 64-bit write, so no CPU
 * ever sees a present entry whose address is not written yet.
 */
#define PUBLISH(entry, value) \
    __atomic_store_n((UQUAD *)(entry), *(UQUAD *)&(value), __ATOMIC_RELEASE)

static void set_pml4e(struct PML4E *slot, APTR table)
{
    struct PML4E e, *entry = &e;

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
    PUBLISH(slot, e);
}

static void set_pdpe(struct PDPE *slot, APTR table)
{
    struct PDPE e, *entry = &e;

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
    PUBLISH(slot, e);
}

/* A page directory entry pointing at a table of 4 KiB pages */
static void set_pde_4k(struct PDE4K *slot, APTR table)
{
    struct PDE4K e, *entry = &e;

    entry->p = 1;
    entry->rw = 1;
    entry->us = 1;
    entry->pwt = 0;
    entry->pcd = 0;
    entry->a = 0;
    entry->__pad0 = 0;
    entry->ps = 0;
    entry->_pad1 = 0;
    entry->avl = 0;
    entry->base_low = (IPTR)table >> 12;
    entry->base_high = ((IPTR)table >> 32) & 0x000FFFFF;
    entry->avail = 0;
    entry->nx = 0;
    PUBLISH(slot, e);
}

static void set_pte_4k(struct PTE *slot, IPTR phys, KRN_MapAttr flags)
{
    struct PTE e, *entry = &e;

    entry->p = (flags & (MAP_Readable | MAP_Writable | MAP_Executable)) ? 1 : 0;
    entry->rw = (flags & MAP_Writable) ? 1 : 0;
    entry->us = (flags & MAP_Supervisor) ? 0 : 1;
    entry->pwt = (flags & MAP_WriteThrough) ? 1 : 0;
    entry->pcd = (flags & MAP_CacheInhibit) ? 1 : 0;
    entry->a = 0;
    entry->d = 0;
    entry->pat = 0;
    entry->g = 0;
    entry->avl = 0;
    entry->base_low = phys >> 12;
    entry->base_high = (phys >> 32) & 0x000FFFFF;
    entry->avail = 0;
    entry->nx = 0;
    PUBLISH(slot, e);
}

static void set_pde_2m(struct PDE2M *slot, IPTR phys, KRN_MapAttr flags)
{
    struct PDE2M e, *entry = &e;

    entry->p = (flags & (MAP_Readable | MAP_Writable | MAP_Executable)) ? 1 : 0;
    entry->rw = (flags & MAP_Writable) ? 1 : 0;
    entry->us = 1;
    /*
     * Memory type is selected by PAT:PCD:PWT indexing IA32_PAT's 8 entries.
     * core_CPUSetup() leaves indices 0-3 at their architectural defaults and
     * programs index 4 (PAT=1, PCD=0, PWT=0) to write-combining. Nothing else
     * in the tree sets the PAT bit, so indices 4-7 were previously unused and
     * this cannot change the type of any existing mapping.
     */
    if (flags & MAP_WriteCombine)
    {
        entry->pat = 1;                 /* -> PAT index 4 == WC */
        entry->pcd = 0;
        entry->pwt = 0;
    }
    else
    {
        entry->pat = 0;
        entry->pwt = (flags & MAP_WriteThrough) ? 1 : 0;
        entry->pcd = (flags & MAP_CacheInhibit) ? 1 : 0;
    }
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
    PUBLISH(slot, e);
}

/*
 * Map 4 KiB pages: unlike the 2 MiB path this can place any page-aligned
 * physical page at any page-aligned virtual address, which sparse users
 * (an emulator's guest memory) need. An existing 2 MiB mapping is never
 * split - callers wanting aliases use an unused virtual range, so live RAM
 * or MMIO mappings cannot be replaced by accident.
 */
static int map_global_4k(void *virt, void *phys, uint32_t length,
    KRN_MapAttr flags, BOOL *replaced)
{
    struct CPUMMUConfig *MMU = &__KernBootPrivate->MMU;
    struct PML4E *pml4 = MMU->mmu_PML4;
    IPTR vaddr = (IPTR)virt;
    IPTR paddr = phys ? (IPTR)phys : vaddr;
    IPTR vcur;
    IPTR pcur;
    IPTR vend;

    if (!pml4 || !length || ((vaddr ^ paddr) & MAP_4K_MASK))
        return -1;

    vcur = vaddr & ~(IPTR)MAP_4K_MASK;
    pcur = paddr & ~(IPTR)MAP_4K_MASK;
    vend = (vaddr + length + MAP_4K_MASK) & ~(IPTR)MAP_4K_MASK;

    while (vcur < vend) {
        ULONG pml4_off = (vcur >> 39) & 0x1ff;
        ULONG pdpe_off = (vcur >> 30) & 0x1ff;
        ULONG pde_off  = (vcur >> 21) & 0x1ff;
        ULONG pte_off  = (vcur >> 12) & 0x1ff;
        struct PDPE *pdpe;
        struct PDE4K *pde;
        struct PTE *pte;

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

        if (pde[pde_off].p && pde[pde_off].ps)
            return -1;

        if (!pde[pde_off].p) {
            pte = alloc_page_table();
            if (!pte)
                return -1;
            set_pde_4k(&pde[pde_off], pte);
        } else {
            pte = table_addr(pde[pde_off].base_low,
                pde[pde_off].base_high);
        }

        if (pte[pte_off].p)
            *replaced = TRUE;
        set_pte_4k(&pte[pte_off], pcur, flags);
        asm volatile ("invlpg (%0)" :: "r"(vcur) : "memory");

        vcur += PAGE_SIZE;
        pcur += PAGE_SIZE;
    }

    wrcr(cr3, MMU->mmu_PML4);
    return 0;
}

static int map_global_2m(void *virt, void *phys, uint32_t length,
    KRN_MapAttr flags, BOOL *replaced)
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

        if (pde[pde_off].p)
            *replaced = TRUE;
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

    IPTR vcur = hook->ih_Args[0], vend = hook->ih_Args[1], step = hook->ih_Args[2];

    for (; vcur < vend; vcur += step)
        asm volatile ("invlpg (%0)" :: "r"(vcur) : "memory");

    return 0;

    AROS_USERFUNC_EXIT
}

/*
 * Flush [virt, virt+length) on every other running CPU, and wait for it.
 * step is the size of the pages mapped: one invlpg covers one page.
 */
static void tlb_shootdown(struct KernelBase *KernelBase, void *virt, uint32_t length,
    IPTR step)
{
    struct PlatformData *pdata = KernelBase->kb_PlatformData;
    struct Hook h;
    IPTR args[3];

    /* Nothing to do (and nobody to wait for) until a second CPU runs */
    if (!pdata || !pdata->kb_APIC || pdata->kb_APIC->apic_count < 2)
        return;

    args[0] = (IPTR)virt & ~(step - 1);
    args[1] = ((IPTR)virt + length + step - 1) & ~(step - 1);
    args[2] = step;
    h.h_Entry = (HOOKFUNC)tlbflush_hook;
    h.h_SubEntry = NULL;
    h.h_Data = NULL;
    core_DoCallIPI(&h, (void *)TASKAFFINITY_ALL_BUT_SELF, 0, 3, args, KernelBase);
}
#endif

#if defined(__AROSEXEC_SMP__)
static spinlock_t mapglobal_lock = SPINLOCK_INIT_UNLOCKED;
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
    UQUAD rflags = 0;
    BOOL replaced = FALSE;
    IPTR step;
    int ret;

    /*
     * SuperState() leaves its return frame on this CPU's kernel stack and
     * carries on at ring 0 on the task's stack. Preempted there, the frame
     * is overwritten by the next task's interrupts (or the task resumes on
     * another CPU), and UserState() returns through garbage - the task can
     * come back still in ring 0. Keep interrupts off until UserState().
     */
    if (!Kernel_13_KrnIsSuper()) {
        rflags = __builtin_ia32_readeflags_u64();
        asm volatile("cli" ::: "memory");
        ssp = SuperState();
        if (!ssp) {
            if (rflags & (1 << 9))
                asm volatile("sti" ::: "memory");
            return -1;
        }
    }

    /* 2 MiB pages where the range allows them, else 4 KiB pages */
    step = (!(((IPTR)virt | (IPTR)(phys ? phys : virt) | length) & MAP_2M_MASK)
            && length >= MAP_2M_SIZE) ? MAP_2M_SIZE : PAGE_SIZE;

#if defined(__AROSEXEC_SMP__)
    /* Two CPUs mapping at once must not both add the same missing table */
    KrnSpinLock(&mapglobal_lock, NULL, SPINLOCK_MODE_WRITE);
#endif
    if (step == MAP_2M_SIZE)
        ret = map_global_2m(virt, phys, length, flags, &replaced);
    else
        ret = map_global_4k(virt, phys, length, flags, &replaced);
#if defined(__AROSEXEC_SMP__)
    KrnSpinUnLock(&mapglobal_lock);

    /*
     * Only a changed present entry can be cached by another CPU: no TLB or
     * paging-structure cache entry is made from a not-present one (Intel SDM
     * 4.10.4.3), so filling a hole - demand paging - needs no shootdown.
     * After the unlock: the shootdown waits for every other CPU.
     */
    if (ret == 0 && replaced)
        tlb_shootdown(KernelBase, virt, length, step);
#endif

    if (ssp) {
        UserState(ssp);
        if (rflags & (1 << 9))
            asm volatile("sti" ::: "memory");
    }

    return ret;

    AROS_LIBFUNC_EXIT
}
