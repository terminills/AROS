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
 *
 * In x2APIC mode the destination and command are one 64-bit MSR write and
 * there is no delivery status to wait for, so the window does not exist
 * there; apic_SendIPI()/apic_IPIBusy() pick the interface.
 */
#define IPI_ICR_BEGIN(flags)    do { __save_flags(flags); __cli();                                 \
                                     while (apic_IPIBusy(__APICBase)) asm volatile("pause");                  \
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
            apic_SendIPI(__APICBase, 0, cmd);
            IPI_ICR_END(flags);
        }
        else if ((IPTR)cpu_mask == TASKAFFINITY_ALL_BUT_SELF)
        {
            // Shorthand - all excluding self
            cmd |= ICR_DSH_ALLBUTSELF;

            D(bug("[Kernel:IPI] waiting for DS bit to be clear\n"));
            IPI_ICR_BEGIN(flags);
            D(bug("[Kernel:IPI] sending IPI cmd %08x\n", cmd));
            apic_SendIPI(__APICBase, 0, cmd);
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
                        apic_SendIPI(__APICBase, 0, cmd | ICR_DSH_SELF);
                    }
                    else
                    {
                        D(bug("[Kernel:IPI] sending IPI cmd %08x to destination %08x\n", cmd, (apicPrivate->cores[i].cpu_LocalID << 24)));
                        apic_SendIPI(__APICBase, apicPrivate->cores[i].cpu_LocalID, cmd);
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
static void core_IPICallHookHandle(struct ExceptionContext *regs, struct KernelBase *KernelBase);

static struct IPIHook *core_IPIHookAlloc(struct KernelBase *KernelBase)
{
    struct PlatformData *pdata = KernelBase->kb_PlatformData;
    struct IPIHook *ipi = NULL;
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
            /*
             * The pool is empty, and entries only come back when their
             * target CPUs drain them. Some may be waiting on THIS CPU,
             * which cannot take the IPI if the caller is inside Disable():
             * two CPUs storming Signal()s at each other under Disable()
             * then both spun here forever (SMP-SigRace phase 3). So drain
             * our own inbound queue inline while we wait - safe, as the
             * drain holds no list lock while it runs a hook.
             */
            core_IPICallHookHandle(NULL, KernelBase);
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
    bzero(ipi->ih_CPUClaimed, sizeof(ULONG) * ((31 + apicPrivate->apic_count) / 32));
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
 * The drain calls hooks with no list lock held (core_IPICallHookHandle), so
 * holding kb_BusyIPIHooksLock no longer waits a mid-hook core out. A matching
 * entry that no CPU has claimed never ran and goes straight back to the free
 * list; one that is claimed but not done is executing on another core right
 * now, so drop the lock, let it finish (it takes the lock to mark itself done
 * and frees the entry), and look again. Signal entries target one CPU, so a
 * claim means that call is running; a matching hook sees the task terminal and
 * returns at once.
 *
 * Async-only by construction: sync callers block in core_DoCallIPI() until
 * completion, so a sync entry cannot outlive its caller and can never be one
 * of ours.
 */
void core_CancelCallIPIs(APTR hookEntry, IPTR matchArg)
{
    struct KernelBase *KernelBase = getKernelBase();
    struct PlatformData *pdata;
    struct IPIHook *ipi, *n;
    ULONG masklongs, i;
    BOOL running;

    if (!KernelBase)
        return;
    pdata = KernelBase->kb_PlatformData;
    if (!pdata || !pdata->kb_APIC)
        return;
    masklongs = (31 + pdata->kb_APIC->apic_count) / 32;

    do
    {
        running = FALSE;

        Disable();
        KrnSpinLock(&pdata->kb_BusyIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
        ForeachNodeSafe(&pdata->kb_BusyIPIHooks, ipi, n)
        {
            BOOL claimed = FALSE;

            if ((APTR)ipi->ih_Hook.h_Entry != hookEntry ||
                !ipi->ih_Async ||
                ipi->ih_Args[1] != matchArg)
                continue;

            for (i = 0; i < masklongs; i++)
                if (ipi->ih_CPUClaimed[i])
                    claimed = TRUE;

            if (claimed)
            {
                running = TRUE;
                continue;
            }
            REMOVE(ipi);
            KrnSpinLock(&pdata->kb_FreeIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
            ADDTAIL(&pdata->kb_FreeIPIHooks, ipi);
            KrnSpinUnLock(&pdata->kb_FreeIPIHooksLock);
        }
        KrnSpinUnLock(&pdata->kb_BusyIPIHooksLock);
        Enable();

        if (running)
            asm volatile("pause");
    } while (running);
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
        bzero(ipi->ih_CPUClaimed, sizeof(ULONG)*((31 + apicPrivate->apic_count) / 32));

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

            /* The drain left a synchronous entry to us (core_IPICallHookHandle) */
            Disable();
            KrnSpinLock(&pdata->kb_FreeIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
            ADDTAIL(&pdata->kb_FreeIPIHooks, ipi);
            KrnSpinUnLock(&pdata->kb_FreeIPIHooksLock);
            Enable();
        }
    }

    return ret;
}

/*
 * Run this CPU's queued hook calls.
 *
 * The hook is called with no list lock held. On pc the hook is usually
 * signal_hook, which runs the full Signal(), and a Signal() to a task that
 * has meanwhile moved to a third CPU queues an IPI of its own through
 * core_DoCallIPI() - which takes kb_BusyIPIHooksLock. With the drain
 * holding that lock across the call, the CPU spun forever on a lock it
 * already held, interrupts off, and every other CPU that tried to send an
 * IPI queued up behind it (seven CPUs signalling one waiter that is allowed
 * on all of them and running on one: SMP-MultiWake).
 *
 * So each entry is claimed under the lock (ih_CPUClaimed, which also keeps
 * a second IPI_CALL_HOOK from running an entry twice on this CPU), called
 * unlocked, and marked done under the lock afterwards. An entry is only
 * freed once every requested CPU is done, and this CPU's done bit is set
 * after its call returns, so the entry cannot be recycled under a running
 * hook. The list is rescanned after every call: it may have changed, and
 * the hook may have queued more work for this CPU.
 */
static void core_IPICallHookHandle(struct ExceptionContext *regs, struct KernelBase *KernelBase)
{
    struct PlatformData *pdata = KernelBase->kb_PlatformData;
    struct APICData *apicPrivate = pdata->kb_APIC;
    ULONG masklongs = (31 + apicPrivate->apic_count) / 32;
    int cpunum = KrnGetCPUNumber();

    D(bug("[Kernel:IPI.CPU.%03u] %s\n", cpunum, __func__));

    for (;;)
    {
        struct IPIHook *ipi = NULL, *n = NULL, *found = NULL;
        BOOL complete = FALSE;

        KrnSpinLock(&pdata->kb_BusyIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
        ForeachNodeSafe(&pdata->kb_BusyIPIHooks, ipi, n)
        {
            if (KrnCPUInMask(cpunum, ipi->ih_CPURequested) &&
                !KrnCPUInMask(cpunum, ipi->ih_CPUClaimed))
            {
                bit_test_and_set_long(ipi->ih_CPUClaimed, cpunum);
                found = ipi;
                break;
            }
        }
        KrnSpinUnLock(&pdata->kb_BusyIPIHooksLock);

        if (!found)
            break;

        /*
            In case of asynchronous hook, the object we pass is IPIHook itself. Dangerous, but we needed to copy the
            original Hook since it could have existed on stack of caller...
        */
        D(bug("[Kernel:IPI.CPU.%03u] %s: Calling HOOK Entry %p with Data %p\n", cpunum, __func__,
            found->ih_Hook.h_Entry, found->ih_Async ? (APTR)&found->ih_Hook : found->ih_Hook.h_Data));
        CALLHOOKPKT(&found->ih_Hook, NULL, 0);

        KrnSpinLock(&pdata->kb_BusyIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);

        /* Mark this CPU as done */
        bit_test_and_set_long(found->ih_CPUDone, cpunum);

        /* Check if all requested CPUs have handled the IPI */
        if (!memcmp(found->ih_CPUDone, found->ih_CPURequested, sizeof(ULONG) * masklongs))
        {
            D(bug("[Kernel:IPI.CPU.%03u] %s: IPIHook completed. Releasing it\n", cpunum, __func__));
            REMOVE(found);
            complete = TRUE;
        }
        KrnSpinUnLock(&pdata->kb_BusyIPIHooksLock);

        if (complete)
        {
            if (found->ih_Async)
            {
                /* Put it on FreeList - never holding Free and Busy at once */
                KrnSpinLock(&pdata->kb_FreeIPIHooksLock, NULL, SPINLOCK_MODE_WRITE);
                ADDTAIL(&pdata->kb_FreeIPIHooks, found);
                KrnSpinUnLock(&pdata->kb_FreeIPIHooksLock);
            }
            else
            {
                /*
                 * The sender is waiting on the sync lock and frees the
                 * entry itself once it has it: freeing it here first let
                 * another CPU take and re-initialise the entry before this
                 * unlock, which then released the new user's lock.
                 */
                D(bug("[Kernel:IPI.CPU.%03u] %s: Releasing sync lock\n", cpunum, __func__));
                KrnSpinUnLock(&found->ih_SyncLock);
            }
        }
    }
}

int core_IPIHandle(struct ExceptionContext *regs, void *data1, struct KernelBase *KernelBase)
{
    D(int cpunum = KrnGetCPUNumber();)
    IPTR ipi_number = (IPTR)data1;
    IPTR __APICBase = core_APIC_GetBase();
    
    D(bug("[Kernel:IPI.CPU.%03u] %s: IPI%02d\n", cpunum, __func__, ipi_number));

    /*
     * Each IPI is acknowledged here, exactly once - cpu_Trap() leaves IPI
     * vectors alone. A second EOI with nothing in service is harmless, but
     * when an IPI nests inside another interrupt it retires THAT one.
     */
    switch (ipi_number)
    {
        case IPI_RESCHEDULE:
            apic_Write(__APICBase, APIC_EOI, 0);
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
            apic_Write(__APICBase, APIC_EOI, 0);
            break;

        default:
            apic_Write(__APICBase, APIC_EOI, 0);
            break;
    }

    return TRUE;
}
