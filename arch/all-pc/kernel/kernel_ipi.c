/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

#include <asm/cpu.h>
#include <strings.h>

#define __KERNEL_NOLIBBASE__
#include <proto/kernel.h>


#include "kernel_base.h"
#include "kernel_globals.h"     /* getKernelBase() for the claim/commit API */
#include "kernel_intern.h"
#include "kernel_debug.h"

#include "kernel_ipi.h"

#include <kernel_scheduler.h>
#include <kernel_intr.h>

#include "apic_ia32.h"

#define D(x)

/*
 * The local APIC has ONE interrupt command register per CPU and a send is two
 * MMIO stores: the destination into ICRH, then the command into ICRL, which
 * is the write that actually fires it. Nothing in the APIC ties the two
 * together. Any interrupt taken on this CPU between them whose handler also
 * sends an IPI - Signal() from an interrupt server or a nested signal_hook,
 * KrnScheduleCPU() from a scheduler tick - overwrites ICRH with ITS
 * destination, and when the interrupted sender resumes its ICRL store fires
 * OUR interrupt at THAT core. The intended target never gets an interrupt,
 * so the IPIHook queued for it by core_DoCallIPI() stays on kb_BusyIPIHooks
 * until some unrelated IPI happens to reach that core and its drain loop
 * finds the entry. Nothing is corrupted and nothing is lost, only delayed
 * by "whenever the next IPI lands there" - which on a core running one
 * pinned, idle-in-Wait thread is seconds to minutes.
 *
 * That was the multi-core Chromium stall of 2026-09-21/22 (JOB-0104): every
 * thread TS_WAIT, tc_SigRecvd empty, kernel idle, and walk-ipihooks.py showed
 * signal_hook entries for CPU 2 pending 8, 41 and 50 seconds with that
 * LAPIC's IRR/ISR empty. Signal()'s cross-CPU path calls this with interrupts
 * enabled (core_DoCallIPI Enable()s before the send), so the window is open
 * on every cross-core wakeup. It bites harder the fewer threads share a core:
 * pinned over 4 cores the stragglers are freed within a frame or two by the
 * neighbours' traffic, over 8 there is nobody to do it.
 *
 * The busy-wait on ICR_DS and both stores are one transaction, and the whole
 * sequence runs with interrupts off. Per destination, not around the loop:
 * an IPI to a 64-core mask must not hold interrupts off for 64 sends.
 * __save_flags/__cli/__restore_flags rather than Disable()/Enable() because
 * this is reached from interrupt context with interrupts already off and
 * must leave them exactly as found.
 */
#define IPI_ICR_BEGIN(flags)    do { __save_flags(flags); __cli();                                 \
                                     while (APIC_REG(__APICBase, APIC_ICRL) & ICR_DS) asm volatile("pause"); \
                                } while (0)
#define IPI_ICR_END(flags)      __restore_flags(flags)

void core_DoIPI(uint8_t ipi_number, void *cpu_mask, struct KernelBase *KernelBase)
{
    int cpunum = KrnGetCPUNumber();
    ULONG cmd = APIC_CPU_EXCEPT_TO_VECTOR(APIC_EXCEPT_IPI_NOP + ipi_number) | ICR_INT_ASSERT;
    struct PlatformData *kernPlatD = (struct PlatformData *)KernelBase->kb_PlatformData;
    struct APICData *apicPrivate = kernPlatD->kb_APIC;
    IPTR __APICBase = apicPrivate->lapicBase;
    unsigned long flags;

    D(bug("[Kernel:IPI] Sending IPI %02d from CPU.%03u to target mask @ 0x%p\n", ipi_number, cpunum, cpu_mask));

    asm volatile("sfence");

    if ((cmd & 0xff) <= APIC_CPU_EXCEPT_TO_VECTOR(APIC_EXCEPT_IPI_CAUSE))
    {
        // special case - send IPI to all
        if ((IPTR)cpu_mask == TASKAFFINITY_ANY)
        {
            // Shorthand - all including self
            cmd |= ICR_DSH_ALL;

            D(bug("[Kernel:IPI] waiting for DS bit to be clear\n"));
            IPI_ICR_BEGIN(flags);
            D(bug("[Kernel:IPI] sending IPI cmd %08x\n", cmd));
            APIC_REG(__APICBase, APIC_ICRL) = cmd;
            IPI_ICR_END(flags);
        }
        else if ((IPTR)cpu_mask == TASKAFFINITY_ALL_BUT_SELF)
        {
            // Shorthand - all excluding self
            cmd |= ICR_DSH_ALLBUTSELF;

            D(bug("[Kernel:IPI] waiting for DS bit to be clear\n"));
            IPI_ICR_BEGIN(flags);
            D(bug("[Kernel:IPI] sending IPI cmd %08x\n", cmd));
            APIC_REG(__APICBase, APIC_ICRL) = cmd;
            IPI_ICR_END(flags);
        }
        else
        {
            int i;

            // No shortcut, send IPI to each CPU one after another
            for (i=0; i < apicPrivate->apic_count; i++)
            {
                if (KrnCPUInMask(i, cpu_mask))
                {
                    D(bug("[Kernel:IPI] waiting for DS bit to be clear\n"));
                    IPI_ICR_BEGIN(flags);
                    if (cpunum == i)
                    {
                        D(bug("[Kernel:IPI] sending IPI cmd %08x to destination %08x (self-ipi)\n", cmd, (apicPrivate->cores[i].cpu_LocalID << 24)));
                        APIC_REG(__APICBase, APIC_ICRL) = cmd | ICR_DSH_SELF;
                    }
                    else
                    {
                        D(bug("[Kernel:IPI] sending IPI cmd %08x to destination %08x\n", cmd, (apicPrivate->cores[i].cpu_LocalID << 24)));
                        APIC_REG(__APICBase, APIC_ICRH) = (apicPrivate->cores[i].cpu_LocalID << 24);
                        APIC_REG(__APICBase, APIC_ICRL) = cmd;
                    }
                    IPI_ICR_END(flags);
                }
            }
        }
    }
    else
    {
        D(bug("[Kernel:IPI] unhandled IPI cmd %08x\n", cmd));
    }
}

/*
 * Take an IPIHook off the free list, spinning until one is available.
 *
 * Factored out of core_DoCallIPI() so the claim/commit path below reuses the
 * exact same loop rather than carrying a second copy: the ordering here
 * (Disable() before the list spinlock, and never holding Free and Busy at
 * once) is load-bearing, and two copies would drift.
 */
static struct IPIHook *core_IPIHookAlloc(struct KernelBase *KernelBase)
{
    struct PlatformData *pdata = KernelBase->kb_PlatformData;
    struct APICData *apicPrivate = pdata->kb_APIC;
    struct IPIHook *ipi = NULL, *n = NULL;
    D(int cpunum = KrnGetCPUNumber();)

    do
    {
        Disable();
        KrnSpinLock(&pdata->kb_FreeIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
        ipi = (struct IPIHook *)REMHEAD(&pdata->kb_FreeIPIHooks);
        KrnSpinUnLock(&pdata->kb_FreeIPIHooksLock);
        Enable();
        if (ipi == NULL)
        {
            D(bug("[Kernel:IPI.CPU.%03u] %s: Failed to allocate IPIHook entry\n", cpunum, __func__));
            Disable();
            KrnSpinLock(&pdata->kb_BusyIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
            ForeachNodeSafe(&pdata->kb_BusyIPIHooks, ipi, n)
            {
                if (!memcmp(ipi->ih_CPUDone, ipi->ih_CPURequested, sizeof(ULONG) * ((31 + apicPrivate->apic_count)/32)))
                {
                    D(bug("[Kernel:IPI.CPU.%03u] %s: Busy IPI @ %p appears to be finished?\n", cpunum, __func__, ipi));
                }
                else
                {
                    D(bug("[Kernel:IPI.CPU.%03u] %s: Busy IPI @ %p\n", cpunum, __func__, ipi));
                }
            }
            KrnSpinUnLock(&pdata->kb_BusyIPIHooksLock);
            Enable();
            ipi = NULL;
            /* Tell the CPU we are idling around a lock... */
            asm volatile("pause");
        }
    } while (ipi == NULL);

    return ipi;
}

/*
 * Claim an entry without publishing it. The caller commits or aborts it.
 *
 * Claiming can spin (the free list may be empty), so exec calls this BEFORE
 * taking the target task's tc_SpinLock - see the comment in rom/exec/signal.c.
 * A claimed entry is on no list, so a concurrent core_CancelCallIPIs() cannot
 * see it; that is the point of the split, since the commit then happens under
 * tc_SpinLock after tc_State has been re-checked.
 */
struct CallIPIEntry *core_ClaimCallIPI(int cpu)
{
    (void)cpu;
    return (struct CallIPIEntry *)core_IPIHookAlloc(getKernelBase());
}

/* Give a claimed entry back unused - the target died before we committed. */
void core_AbortCallIPI(struct CallIPIEntry *cie, int cpu)
{
    struct KernelBase *KernelBase = getKernelBase();
    struct PlatformData *pdata = KernelBase->kb_PlatformData;
    struct IPIHook *ipi = (struct IPIHook *)cie;

    (void)cpu;
    if (!ipi)
        return;

    Disable();
    KrnSpinLock(&pdata->kb_FreeIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
    ADDTAIL(&pdata->kb_FreeIPIHooks, ipi);
    KrnSpinUnLock(&pdata->kb_FreeIPIHooksLock);
    Enable();
}

/*
 * Publish a claimed entry to exactly one CPU and poke it.
 *
 * Always asynchronous: the only caller is Signal(), which may run from an
 * interrupt and must not wait for the target to drain.
 */
void core_CommitCallIPI(struct CallIPIEntry *cie, int cpu, struct Hook *hook,
                        int nargs, IPTR *args)
{
    struct KernelBase *KernelBase = getKernelBase();
    struct PlatformData *pdata = KernelBase->kb_PlatformData;
    struct APICData *apicPrivate = pdata->kb_APIC;
    struct IPIHook *ipi = (struct IPIHook *)cie;
    /* Stack mask: Signal() may run in an interrupt, so KrnAllocCPUMask()
     * (which allocates) is not available here. 8 longs = 256 CPUs. */
    ULONG mask[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    int i;

    if (!ipi)
        return;

    if (nargs > IPI_CALL_HOOK_MAX_ARGS)
        nargs = IPI_CALL_HOOK_MAX_ARGS;

    ipi->ih_Hook.h_Entry = hook->h_Entry;
    ipi->ih_Hook.h_SubEntry = hook->h_SubEntry;
    ipi->ih_Hook.h_Data = hook->h_Data;

    for (i = 0; i < nargs; i++)
        ipi->ih_Args[i] = args[i];

    ipi->ih_Async = 1;

    bzero(ipi->ih_CPUDone, sizeof(ULONG) * ((31 + apicPrivate->apic_count) / 32));
    bzero(ipi->ih_CPURequested, sizeof(ULONG) * ((31 + apicPrivate->apic_count) / 32));
    bit_test_and_set_long(ipi->ih_CPURequested, cpu);

    bit_test_and_set_long(mask, cpu);

    Disable();
    KrnSpinLock(&pdata->kb_BusyIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
    ADDTAIL(&pdata->kb_BusyIPIHooks, ipi);
    KrnSpinUnLock(&pdata->kb_BusyIPIHooksLock);
    Enable();

    core_DoIPI(IPI_CALL_HOOK, mask, KernelBase);
}

/*
 * Drop every queued call matching (hookEntry, args[1]) and make sure none is
 * still executing anywhere once we return. Called from RemTask() via
 * Exec_CancelSignalIPIs() once the task is terminal, before its memory is
 * freed.
 *
 * Why no separate "wait for the executing call" loop, unlike arm-native: on pc
 * core_IPICallHookHandle() holds kb_BusyIPIHooksLock ACROSS the CALLHOOKPKT(),
 * so a core that is mid-hook still owns this lock. Acquiring it therefore
 * already waits that core out, and once we hold it no core can start a new
 * hook. The lock IS the barrier.
 *
 * An entry we unlink here was allocated by core_ClaimCallIPI() and never ran,
 * so returning it straight to the free list is the same bookkeeping the drain
 * would have done, minus the CPUDone/SyncLock steps which only apply to a call
 * that actually executed. Async-only by construction: sync callers block in
 * core_DoCallIPI() until completion, so a sync entry cannot outlive its caller
 * and can never be one of ours.
 */
void core_CancelCallIPIs(APTR hookEntry, IPTR matchArg)
{
    struct KernelBase *KernelBase = getKernelBase();
    struct PlatformData *pdata;
    struct IPIHook *ipi, *n;

    if (!KernelBase)
        return;
    pdata = KernelBase->kb_PlatformData;
    if (!pdata || !pdata->kb_APIC)
        return;

    Disable();
    KrnSpinLock(&pdata->kb_BusyIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
    ForeachNodeSafe(&pdata->kb_BusyIPIHooks, ipi, n)
    {
        if ((APTR)ipi->ih_Hook.h_Entry == hookEntry &&
            ipi->ih_Async &&
            ipi->ih_Args[1] == matchArg)
        {
            REMOVE(ipi);
            KrnSpinLock(&pdata->kb_FreeIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
            ADDTAIL(&pdata->kb_FreeIPIHooks, ipi);
            KrnSpinUnLock(&pdata->kb_FreeIPIHooksLock);
        }
    }
    KrnSpinUnLock(&pdata->kb_BusyIPIHooksLock);
    Enable();
}

int core_DoCallIPI(struct Hook *hook, void *cpu_mask, int async, int nargs, IPTR *args, APTR _KB)
{
    struct KernelBase *KernelBase = _KB;
    struct PlatformData *pdata = KernelBase->kb_PlatformData;
    struct IPIHook *ipi = NULL, *n = NULL;
    struct APICData *apicPrivate = pdata->kb_APIC;
    int cpunum = KrnGetCPUNumber();
    int ret = FALSE;
    int i;

    D(bug("[Kernel:IPI.CPU.%03u] %s: Hook @ 0x%p, async=%d\n", cpunum, __func__, hook, async));

    if (nargs > IPI_CALL_HOOK_MAX_ARGS)
        return ret;

    if (hook)
    {
        /*
            Allocate IPIHook just by removing it form the Free list:
            First Disable() so that we are not interrupted on this CPU core, then use SpinLock to protect us from
            other cores accessing the list.

            If the FreeIPIHooks list is empty, just do busyloop wait - other cores shall free the hook sooner or later
        */
        ipi = core_IPIHookAlloc(KernelBase);

        D(bug("[Kernel:IPI.CPU.%03u] %s: Allocated IPIHook %p\n", cpunum, __func__, ipi));
    
        /*
            Copy IPI data from struct Hook provided by caller into allocated ipi
        */
        ipi->ih_Hook.h_Entry = hook->h_Entry;
        ipi->ih_Hook.h_SubEntry = hook->h_SubEntry;
        ipi->ih_Hook.h_Data = hook->h_Data;
        
        /*
            Copy call hook arguments
        */
        for (i=0; i < nargs; i++)
            ipi->ih_Args[i] = args[i];

        if (async)
        {
            ipi->ih_Async = 1;
        }
        else
        {
            ipi->ih_Async = 0;

            KrnSpinInit(&ipi->ih_SyncLock);
            KrnSpinLock(&ipi->ih_SyncLock, NULL, SPINLOCK_MODE_WRITE);
        }

        /* Clear CPU done mask */
        bzero(ipi->ih_CPUDone, sizeof(ULONG)*((31 + apicPrivate->apic_count) / 32));

        /* Copy CPU mask */
        if (cpu_mask && cpu_mask != (void*)TASKAFFINITY_ANY && cpu_mask != (void*)TASKAFFINITY_ALL_BUT_SELF)
            bcopy(cpu_mask, ipi->ih_CPURequested, sizeof(ULONG)*((31 + apicPrivate->apic_count) / 32));
        else
        {
            int i;

            bzero(ipi->ih_CPURequested, sizeof(ULONG)*((31 + apicPrivate->apic_count) / 32));

            for (i=0; i < apicPrivate->apic_count; i++)
            {
                if ((cpu_mask == (APTR)TASKAFFINITY_ALL_BUT_SELF) && (i == cpunum))
                    continue;

                bit_test_and_set_long(ipi->ih_CPURequested, i);
            }
        }

        /*
            Put the IPIHook on the BusyIPIHooks list, so that it gets processed once IPIs are called
        */
        Disable();
        KrnSpinLock(&pdata->kb_BusyIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
        ADDTAIL(&pdata->kb_BusyIPIHooks, ipi);
        KrnSpinUnLock(&pdata->kb_BusyIPIHooksLock);
        Enable();

        D(bug("[Kernel:IPI.CPU.%03u] %s: Sending IPI message\n", cpunum, __func__, ipi));

        ret = TRUE;

        /* Issue IPI_CALL_HOOK to requested CPUs */
        core_DoIPI(IPI_CALL_HOOK, cpu_mask, KernelBase);

        /* If synchronous IPI, wait for completion */
        if (!async)
        {
            D(bug("[Kernel:IPI.CPU.%03u] %s: Synchronous IPI, waiting for completion\n", cpunum, __func__));
            KrnSpinLock(&ipi->ih_SyncLock, NULL, SPINLOCK_MODE_WRITE);
            KrnSpinUnLock(&ipi->ih_SyncLock);
            D(bug("[Kernel:IPI.CPU.%03u] %s: Synchronous IPI completed\n", cpunum, __func__));
        }
    }

    return ret;
}

static void core_IPICallHookHandle(struct ExceptionContext *regs, struct KernelBase *KernelBase)
{
    struct PlatformData *pdata = KernelBase->kb_PlatformData;
    struct IPIHook *ipi = NULL;
    struct IPIHook *n = NULL;
    struct APICData *apicPrivate = pdata->kb_APIC;
    int cpunum = KrnGetCPUNumber();

    D(bug("[Kernel:IPI.CPU.%03u] %s\n", cpunum, __func__));

    KrnSpinLock(&pdata->kb_BusyIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);

    ForeachNodeSafe(&pdata->kb_BusyIPIHooks, ipi, n)
    {
        D(bug("[Kernel:IPI.CPU.%03u] %s: Checking node %p\n", cpunum, __func__, ipi));
        if (KrnCPUInMask(cpunum, ipi->ih_CPURequested))
        {
            
            /*
            In case of asynchronous hook, the object we pass is IPIHook itself. Dangerous, but we needed to copy the
            original Hook since it could have existed on stack of caller...
            */
            if (ipi->ih_Async)
            {
                D(bug("[Kernel:IPI.CPU.%03u] %s: Calling HOOK Entry %p with Data %p\n", cpunum, __func__,
                    ipi->ih_Hook.h_Entry, &ipi->ih_Hook));

                CALLHOOKPKT(&ipi->ih_Hook, NULL, 0);
            }
            else
            {
                D(bug("[Kernel:IPI.CPU.%03u] %s: Calling HOOK Entry %p with Data %p\n", cpunum, __func__,
                    ipi->ih_Hook.h_Entry, ipi->ih_Hook.h_Data));

                CALLHOOKPKT(&ipi->ih_Hook, NULL, 0);
            }

            /* This part operates on locked IPIHook */
            KrnSpinLock(&ipi->ih_Lock, NULL, SPINLOCK_MODE_WRITE);

            /* Mark this CPU as done */
            bit_test_and_set_long(ipi->ih_CPUDone, cpunum);

            /* Check if all requested CPUs have handled the IPI */
            if (!memcmp(ipi->ih_CPUDone, ipi->ih_CPURequested, sizeof(ULONG) * ((31 + apicPrivate->apic_count)/32)))
            {
                D(bug("[Kernel:IPI.CPU.%03u] %s: IPIHook completed. Releasing it\n", cpunum, __func__));
                int async = ipi->ih_Async;

                /* Yes, remove this ipi from BusyList */
                REMOVE(ipi);

                /* Put it on FreeList */
                KrnSpinLock(&pdata->kb_FreeIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
                ADDTAIL(&pdata->kb_FreeIPIHooks, ipi);
                KrnSpinUnLock(&pdata->kb_FreeIPIHooksLock);

                if (!async)
                {
                    D(bug("[Kernel:IPI.CPU.%03u] %s: Releasing sync lock\n", cpunum, __func__));
                    KrnSpinUnLock(&ipi->ih_SyncLock);
                }
            }
            /* End of IPIHook critical section */
            KrnSpinUnLock(&ipi->ih_Lock);
        }
    }

    KrnSpinUnLock(&pdata->kb_BusyIPIHooksLock);
}

int core_IPIHandle(struct ExceptionContext *regs, void *data1, struct KernelBase *KernelBase)
{
    D(int cpunum = KrnGetCPUNumber();)
    IPTR ipi_number = (IPTR)data1;
    IPTR __APICBase = core_APIC_GetBase();
    
    D(bug("[Kernel:IPI.CPU.%03u] %s: IPI%02d\n", cpunum, __func__, ipi_number));

    switch (ipi_number)
    {
        case IPI_RESCHEDULE:
            APIC_REG(__APICBase, APIC_EOI) = 0;
            // If IPI was called when CPU was in user mode, and not in forbid state,
            // perform task switch - otherwise set delayed schedule flag.
            if (INTR_FROMUSERMODE && (TDNESTCOUNT_GET < 0))
            {
                if (core_Schedule())
                {
                    cpu_Switch(regs);
                    cpu_Dispatch(regs);
                }
            }
            else
            {
                FLAG_SCHEDSWITCH_SET;
            }
            break;

        case IPI_CALL_HOOK:
            core_IPICallHookHandle(regs, KernelBase);
            APIC_REG(__APICBase, APIC_EOI) = 0;
            break;
    }

    return TRUE;
}
