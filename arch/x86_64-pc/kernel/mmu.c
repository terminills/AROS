/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

#include <asm/cpu.h>
#include <exec/types.h>
#include "kernel_base.h"
#include "kernel_intern.h"
#include "kernel_bootmem.h"
#include "kernel_debug.h"
#include "apic.h"

#define D(x)
#define DMMU(x)

/*
 * Split-PTE pool: how many 2MB PDEs core_ProtPage() can demote to 512x 4K pages
 * for fine-grained protection. Each slot = one 2MB region, so total coverage is
 * mmu_PTEPageCount * 2MB. core_ProtKernelArea() protects the whole
 * kickstart-module region page-by-page, so this pool must exceed that region's
 * PDE count.
 *
 * 32 slots (64MB) sufficed historically, but large embedded payloads push the
 * protected region well past that - measured here 2026-09-19 with a native
 * Chromium on the Live CD, which exhausted the pool and then emitted an ungated
 * bug() PER 4K PAGE: 25,862 lines of "split PTE pool exhausted" over serial,
 * which is itself what stalls the boot. 64 slots = 128MB coverage.
 *
 * 64 then ran out too, on the same day, with a larger Chromium (911MB stripped,
 * PDF and printing enabled): one "pool exhausted at 0x138400000" and the region
 * left at 2MB granularity. That silent outcome is the dangerous one. Failing to
 * demote does not fail the request - core_ProtPage() returns having protected
 * nothing, so a PartitionAlloc decommit or a PROT_NONE guard page quietly stays
 * readable and writable, and the fault surfaces much later as a CHECK inside
 * Chromium with no obvious connection to the MMU.
 *
 * 512 was still not enough, and the "sized to stop being a variable" claim
 * above was wrong twice over. It ran out on EVERY Chromium run - single- and
 * multi-process alike, always around 0x16e600000 - because the protected
 * region scales with the payload (a ~900MB browser binary) AND with the number
 * of processes loading one. Multi-process doubles the demand outright.
 *
 * 4096 slots = 8GB of coverage for 16MB of boot memory. That is chosen to
 * exceed several concurrent Chromium processes rather than to fit one, because
 * the failure mode is silent (see core_ProtPage below: the protection is simply
 * not applied) and a silently wrong page table is far more expensive to chase
 * than 16MB is to spend against a 6GB minimum.
 *
 * The real fix is to stop sizing this at boot and demote on demand, since no
 * constant is correct for "however many large processes the user runs". Until
 * then this is deliberately generous, and the refusal counter below exists so
 * that running out is never again mistaken for a one-off warning.
 *
 * The pool comes from the boot reserve behind the kickstart, and a fixed 4096
 * made the kernel panic whenever the bootstrap could only leave its minimum
 * there (GRUB 2 loading the modules near the top of 32-bit RAM). The count is
 * therefore decided in core_SetupMMU(): the preferred size when the reserve
 * holds it, never less than the historic 32.
 */
#define MMU_SPLIT_PTE_PAGE_PREFERRED 4096
#define MMU_SPLIT_PTE_PAGE_MIN       32
#define MMU_SPLIT_PTE_SLOT_SIZE      (sizeof(struct PTE) * 512)

void core_InitMMU(struct CPUMMUConfig *MMU)
{
    struct PML4E *PML4;
    struct PDPE  *PDP;
    struct PDE2M *PDE;
    unsigned int i;

    PML4 = MMU->mmu_PML4;
    PDP  = MMU->mmu_PDP;
    PDE  = MMU->mmu_PDE;

    /* PML4 Entry - we need only the first out of 16 entries */
    PML4[0].p  = 1; /* present */
    PML4[0].rw = 1; /* read/write */
    PML4[0].us = 1; /* accessible for user */
    PML4[0].pwt= 0; /* write-through cache */
    PML4[0].pcd= 0; /* cache enabled */
    PML4[0].a  = 0; /* not yet accessed */
    PML4[0].mbz= 0; /* must be zero */
    PML4[0].base_low = ((IPTR)PDP) >> 12;
    PML4[0].avl= 0;
    PML4[0].nx = 0;
    PML4[0].avail = 0;
    PML4[0].base_high = (((IPTR)PDP) >> 32) & 0x000FFFFF;

    for (i = 0; i < MMU->mmu_PDEPageCount; i++)
    {
        /* For every 512th page create the directory entry */
        if ((i % 512) == 0)
        {
            IPTR pdes = (IPTR)&PDE[i];
            int idx = i / 512;

            /* Set the PDP entry up and point to the PDE table */
            PDP[idx].p  = 1;
            PDP[idx].rw = 1;
            PDP[idx].us = 1;
            PDP[idx].pwt= 0;
            PDP[idx].pcd= 0;
            PDP[idx].a  = 0;
            PDP[idx].mbz= 0;
            PDP[idx].base_low = pdes >> 12;

            PDP[idx].nx = 0;
            PDP[idx].avail = 0;
            PDP[idx].base_high = (pdes >> 32) & 0x000FFFFF;
        }

        /* Set PDE entries - use 2MB memory pages, with full supervisor and user access */
        unsigned long base = (((IPTR)i) << 21);

        PDE[i].p  = 1;
        PDE[i].rw = 1;
        PDE[i].us = 1;
        PDE[i].pwt= 0;  // 1
        PDE[i].pcd= 0;  // 1
        PDE[i].a  = 0;
        PDE[i].d  = 0;
        PDE[i].g  = 0;
        PDE[i].pat= 0;
        PDE[i].ps = 1;
        PDE[i].base_low = base >> 13;

        PDE[i].avail = 0;
        PDE[i].nx = 0;
        PDE[i].base_high = (base >> 32) & 0x000FFFFF;
    }

#if 0
    /* PDP Entries. There are four of them used in order to define 2048 pages of 2MB each. */
    for (i = 0; i < 4; i++)
    {
        struct PDE2M *pdes = &PDE[512 * i];
        unsigned int j;

        /* Set the PDP entry up and point to the PDE table */
        PDP[i].p  = 1;
        PDP[i].rw = 1;
        PDP[i].us = 1;
        PDP[i].pwt= 0;
        PDP[i].pcd= 0;
        PDP[i].a  = 0;
        PDP[i].mbz= 0;
        PDP[i].base_low = (unsigned long)pdes >> 12;

        PDP[i].nx = 0;
        PDP[i].avail = 0;
        PDP[i].base_high = ((unsigned long)pdes >> 32) & 0x000FFFFF;

        for (j=0; j < 512; j++)
        {
            /* Set PDE entries - use 2MB memory pages, with full supervisor and user access */
            unsigned long base = (i << 30) + (j << 21);

            pdes[j].p  = 1;
            pdes[j].rw = 1;
            pdes[j].us = 1;
            pdes[j].pwt= 0;  // 1
            pdes[j].pcd= 0;  // 1
            pdes[j].a  = 0;
            pdes[j].d  = 0;
            pdes[j].g  = 0;
            pdes[j].pat= 0;
            pdes[j].ps = 1;
            pdes[j].base_low = base >> 13;

            pdes[j].avail = 0;
            pdes[j].nx = 0;
            pdes[j].base_high = (base >> 32) & 0x000FFFFF;
        }
    }
#endif

    MMU->mmu_PDEPageUsed = 0;
}

void core_LoadMMU(struct CPUMMUConfig *MMU)
{
    D(bug("[Kernel] %s: Registering PML4 @ 0x%p\n", __func__, MMU->mmu_PML4));
    wrcr(cr3, MMU->mmu_PML4);
}

void core_SetupMMU(struct CPUMMUConfig *MMU, IPTR memtop, IPTR maptop)
{
    if (!MMU->mmu_PML4)
    {
        /*
         * First (cold) start: decide how large the identity map is and
         * allocate the paging structures for it. Both survive a warm
         * reboot in KernBootPrivate, so the size must not be recomputed
         * afterwards: core_InitMMU() fills exactly mmu_PDEPageCount
         * entries of the PDE table allocated here.
         */
        IPTR top = memtop;
        IPTR needed, fixed, avail, spare, must, room;

        if (maptop > top)
            top = maptop;

        /*
         * How many PDE entries shall be created?
         * The map must cover physical RAM and the boot framebuffer (GOP
         * framebuffers can sit far above 4 GiB), and preferably a wide
         * window so 64-bit MMIO BARs (ReBAR, large VRAM) are reachable
         * during early driver init.
         */
        needed = (top + (1 << 21) - 1) >> 21;
        if (needed > 262144)                     /* cap at 512 GiB */
            needed = 262144;

        MMU->mmu_PDEPageCount = needed;
        if (MMU->mmu_PDEPageCount < 65536)      /* prefer at least 128 GiB mapped */
            MMU->mmu_PDEPageCount = 65536;

        /*
         * The bootstrap only guarantees a bounded reserve behind the
         * kickstart (KICKSTART_BOOTMEM_RESERVE, or its minimum when the
         * memory map is tight). Fit the tables into what is actually there,
         * in order of importance:
         *   1. the identity map over RAM and the boot framebuffer, and the
         *      minimum split page pool - without these we cannot run;
         *   2. the split page pool up to its preferred size, because running
         *      out silently drops protection changes (see core_ProtPage());
         *   3. the identity map widened towards the preferred window.
         * Later boot allocations (GDT, TSS, IDT, TLS) are small; leave room
         * for them and for page alignment.
         */
        fixed = sizeof(struct PML4E) * 512 + sizeof(struct PDPE) * 512
              + 8 * PAGE_SIZE;
        avail = (IPTR)BootMemLimit - AROS_ROUNDUP2((IPTR)BootMemPtr, PAGE_SIZE);
        spare = (avail > fixed) ? avail - fixed : 0;

        must = needed * sizeof(struct PDE2M)
             + MMU_SPLIT_PTE_PAGE_MIN * MMU_SPLIT_PTE_SLOT_SIZE;
        if (spare < must)
        {
            krnPanic(NULL, "Not enough boot memory for the MMU tables\n"
                           "%lu MiB must be identity mapped (top 0x%p):\n"
                           "%lu KiB needed, %lu KiB free behind the kickstart\n"
                           "Increase reserved space in bootstrap",
                           needed << 1, top, (must + fixed) >> 10, avail >> 10);
        }
        spare -= must;

        MMU->mmu_PTEPageCount = MMU_SPLIT_PTE_PAGE_MIN;
        room = spare / MMU_SPLIT_PTE_SLOT_SIZE;
        if (room > MMU_SPLIT_PTE_PAGE_PREFERRED - MMU_SPLIT_PTE_PAGE_MIN)
            room = MMU_SPLIT_PTE_PAGE_PREFERRED - MMU_SPLIT_PTE_PAGE_MIN;
        MMU->mmu_PTEPageCount += room;
        spare -= room * MMU_SPLIT_PTE_SLOT_SIZE;
        if (MMU->mmu_PTEPageCount < MMU_SPLIT_PTE_PAGE_PREFERRED)
        {
            bug("[Kernel] core_SetupMMU: split page pool trimmed from %u to %u slots to fit boot memory\n",
                MMU_SPLIT_PTE_PAGE_PREFERRED, MMU->mmu_PTEPageCount);
        }

        room = needed + spare / sizeof(struct PDE2M);
        if (MMU->mmu_PDEPageCount > room)
        {
            bug("[Kernel] core_SetupMMU: identity map trimmed from %lu MiB to %lu MiB to fit boot memory\n",
                MMU->mmu_PDEPageCount << 1, room << 1);
            MMU->mmu_PDEPageCount = room;
        }

        D(bug("[Kernel] core_SetupMMU: Re-creating the MMU pages for first %dMB area\n", MMU->mmu_PDEPageCount << 1));

        /*
         * Allocate MMU paging structures for the configured identity-mapped
         * low physical address window.
         */
        MMU->mmu_PML4 = krnAllocBootMemAligned(sizeof(struct PML4E) * 512, PAGE_SIZE);
        MMU->mmu_PDP  = krnAllocBootMemAligned(sizeof(struct PDPE)  * 512, PAGE_SIZE);
        MMU->mmu_PDE  = krnAllocBootMemAligned(sizeof(struct PDE2M) * MMU->mmu_PDEPageCount, PAGE_SIZE);
        /* MUST use the macro. This site used to hardcode 32 while the guard
         * below tested MMU_SPLIT_PTE_PAGE_COUNT, so raising the macro alone
         * would have let core_ProtPage() hand out slots past the end of this
         * allocation - a heap overrun instead of the graceful 2MB-granularity
         * degradation the guard intends. The count is now mmu_PTEPageCount,
         * sized above, and the guard reads the same field. */
        MMU->mmu_PTE  = krnAllocBootMemAligned(MMU_SPLIT_PTE_SLOT_SIZE * MMU->mmu_PTEPageCount, PAGE_SIZE);

        D(bug("[Kernel] Allocated PML4 0x%p, PDP 0x%p, PDE 0x%p PTE 0x%p\n", MMU->mmu_PML4, MMU->mmu_PDP, MMU->mmu_PDE, MMU->mmu_PTE));
    }

    core_InitMMU(MMU);

    core_LoadMMU(MMU);

    D(bug("[Kernel] core_SetupMMU: Done\n"));
}

static volatile int protpage_lock;

void core_ProtPage(intptr_t addr, char p, char rw, char us)
{
    struct CPUMMUConfig *MMU;
    struct PML4E *pml4;
    struct PDPE  *pdpe;
    struct PDE4K *pde;
    struct PTE   *Pages4K;
    struct PTE   *pte;

    unsigned long pml4_off = (addr >> 39) & 0x1ff;
    unsigned long pdpe_off = (addr >> 30) & 0x1ff;
    unsigned long pde_off  = (addr >> 21) & 0x1ff;
    unsigned long pte_off  = (addr >> 12) & 0x1ff;

    DMMU(bug("[Kernel] Marking page 0x%p as read-only\n", addr));

    MMU = &__KernBootPrivate->MMU;
    pml4 = MMU->mmu_PML4;
    pdpe = (struct PDPE *)((((IPTR)pml4[pml4_off].base_low) << 12) | (((IPTR)pml4[pml4_off].base_high) << 32));
    pde  = (struct PDE4K *)((((IPTR)pdpe[pdpe_off].base_low) << 12) | (((IPTR)pdpe[pdpe_off].base_high) << 32));
    Pages4K = MMU->mmu_PTE;

    while (__atomic_exchange_n(&protpage_lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&protpage_lock, __ATOMIC_RELAXED))
            asm volatile("pause");

    if (pde[pde_off].ps)
    {
        /* work on local copy of the affected PDE */
        struct PDE4K tmp_pde = pde[pde_off];
        struct PDE2M *pde2 = (struct PDE2M *)pde;
        intptr_t base = ((IPTR)pde2[pde_off].base_low << 13) | ((IPTR)pde2[pde_off].base_high << 32);
        int i;

        if (MMU->mmu_PDEPageUsed >= MMU->mmu_PTEPageCount)
        {
            /* EXHAUSTION IS NOT SAFE, and an earlier version of this comment
             * said it was. It is safe only when the caller merely wants a
             * mapping to exist - the region does stay mapped, at 2MB rather
             * than 4K. It is NOT safe when the caller is changing PROTECTION
             * on a sub-2MB range, which is what core_ProtPage() is for: the
             * requested protection is then silently not applied and we return
             * success. A PartitionAlloc decommit leaves the page readable and
             * writable; worse, a later set-access leaves it PROT_NONE, and the
             * next legitimate read takes a privilege-violation #GP nowhere
             * near the MMU. Both were observed in Chromium - a deterministic
             * #GP in OPENSSL_free on every run, and a PartitionAlloc FATAL in
             * every child process once multi-process was enabled.
             *
             * Still rate-limited, because core_ProtKernelArea() calls this for
             * every page in the protected region and an ungated bug() emitted
             * 25,862 lines and stalled the boot - the flood is its own failure.
             * But report on a doubling schedule rather than once: a single line
             * reads like a one-off curiosity, when the true count is what says
             * whether the pool is marginally or hopelessly small. */
            static unsigned refused = 0;
            static unsigned next_report = 1;
            if (++refused >= next_report)
            {
                next_report *= 2;
                bug("[Kernel] core_ProtPage: split PTE pool exhausted (%u slots) at 0x%p"
                    " - PROTECTION NOT APPLIED, %u refused so far;"
                    " raise KICKSTART_BOOTMEM_RESERVE\n",
                    (unsigned)MMU->mmu_PTEPageCount, addr, refused);
            }
            __atomic_store_n(&protpage_lock, 0, __ATOMIC_RELEASE);
            return;
        }

        pte = &Pages4K[512 * MMU->mmu_PDEPageUsed++];

        D(bug("[Kernel] The page for address 0x%p was a big one. Splitting it into 4K pages\n", addr));
        D(bug("[Kernel] Base=0x%p, pte=0x%p\n", base, pte));

        for (i = 0; i < 512; i++)
        {
            pte[i].p = 1;
            pte[i].rw        = pde2[pde_off].rw;
            pte[i].us        = pde2[pde_off].us;
            pte[i].pwt       = pde2[pde_off].pwt;
            pte[i].pcd       = pde2[pde_off].pcd;
            pte[i].base_low  = base >> 12;
            pte[i].base_high = (base >> 32) & 0x0FFFFF;

            base += PAGE_SIZE;
        }

        tmp_pde.ps = 0;
        tmp_pde.base_low = (intptr_t)pte >> 12;
        tmp_pde.base_high = ((intptr_t)pte >> 32) & 0x0FFFFF;

        pde[pde_off] = tmp_pde;
    }
            
    pte = (struct PTE *)((((IPTR)pde[pde_off].base_low) << 12) | (((IPTR)pde[pde_off].base_high) << 32));

    pte[pte_off].rw = rw ? 1:0;
    pte[pte_off].us = us ? 1:0;
    pte[pte_off].p = p ? 1:0;
    asm volatile ("invlpg (%0)"::"r"(addr));

    __atomic_store_n(&protpage_lock, 0, __ATOMIC_RELEASE);
}

void core_ProtKernelArea(intptr_t addr, intptr_t length, char p, char rw, char us)
{
    D(bug("[Kernel] Protecting area 0x%p - 0x%p\n", addr, addr + length - 1));

    while (length > 0)
    {
        core_ProtPage(addr, p, rw, us);
        addr += 4096;
        length -= 4096;
    }
}
