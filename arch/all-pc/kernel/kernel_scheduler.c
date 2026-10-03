/*
    Copyright (C) 2017-2026, The AROS Development Team. All rights reserved.
*/

#include <exec/alerts.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <proto/exec.h>

#define __KERNEL_NOLIBBASE__
#include <proto/kernel.h>

#include <kernel_base.h>
#include <kernel_debug.h>
#include <kernel_scheduler.h>

#include "kernel_cpu.h"

#include <exec_platform.h>

#include <aros/types/spinlock_s.h>

#include <etask.h>

#define __AROS_KERNEL__

#include "exec_intern.h"

#include "apic.h"

#define LOWSTACKWARN
#define SCHEDULERASCII_DEBUG

#ifdef DEBUG
#undef DEBUG
#endif
#define DEBUG 0

#if (DEBUG > 0)
#define DSCHED(x) x
#else
#define DSCHED(x)
#endif

#if (DEBUG > 0) && defined(SCHEDULERASCII_DEBUG)
#define DEBUGCOLOR_SET       "\033[31m"
#define DEBUGFUNCCOLOR_SET   "\033[31;1m"
#define DEBUGCOLOR_RESET     "\033[0m"
#else
#define DEBUGCOLOR_SET
#define DEBUGFUNCCOLOR_SET
#define DEBUGCOLOR_RESET
#endif

#if defined(__AROSEXEC_SMP__)
void core_InitScheduleData(struct X86SchedulerPrivate *schedData)
{
    DSCHED(bug("[Kernel]" DEBUGFUNCCOLOR_SET " %s(0x%p)" DEBUGCOLOR_RESET "\n", __func__, schedData);)
    schedData->Granularity = SCHEDGRAN_VALUE;
    schedData->Quantum = SCHEDQUANTUM_VALUE;
}
#endif

/* Check if the currently running task on this cpu should be rescheduled.. */
BOOL core_Schedule(void)
{
#if defined(__AROSEXEC_SMP__) || (DEBUG > 0)
    cpuid_t cpuNo;
#endif
    struct Task *task;
    BOOL corereschedule = TRUE;

    DSCHED(bug("[Kernel]" DEBUGFUNCCOLOR_SET " %s()" DEBUGCOLOR_RESET "\n", __func__);)

    task = GET_THIS_TASK;
#if defined(__AROSEXEC_SMP__) || (DEBUG > 0)
    cpuNo = KrnGetCPUNumber();
#endif

    DSCHED(
        bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: running Task @ 0x%p" DEBUGCOLOR_RESET "\n", cpuNo, __func__, task);
        bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: '%s', state %08X" DEBUGCOLOR_RESET "\n", cpuNo, __func__, task->tc_Node.ln_Name, task->tc_State);
    )

    FLAG_SCHEDSWITCH_CLEAR;

    if (task)
    {
        if (
#if defined(__AROSEXEC_SMP__)
            (task->tc_State == TS_TOMBSTONED) ||
#endif
            (task->tc_State == TS_REMOVED))
        {
            /* always let finalising tasks finish... */
            corereschedule = FALSE;
#if defined(__AROSEXEC_SMP__) || (DEBUG > 0)
            bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: letting finalising task run.." DEBUGCOLOR_RESET "\n", cpuNo, __func__);
#endif
        }
#if defined(__AROSEXEC_SMP__)
        else if (GetIntETask(task) && GetIntETask(task)->iet_StopReq)
            corereschedule = TRUE;
#endif
        else if (!(task->tc_Flags & TF_EXCEPT))
        {
#if defined(__AROSEXEC_SMP__)
            KrnSpinLock(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL,
                        SPINLOCK_MODE_READ);
#endif
            /* Is the TaskReady empty? If yes, then the running task is the only one. Let it work */
            if (IsListEmpty(&SysBase->TaskReady))
                corereschedule = FALSE;
            else
            {
                struct Task *nexttask;
                /*
                        If there are tasks ready for this cpu that have equal or lower priority,
                        and the current task has used its alloted time - reschedule so they can run
                    */
                for (nexttask = (struct Task *)GetHead(&SysBase->TaskReady); nexttask != NULL; nexttask = (struct Task *)GetSucc(nexttask))
                {
#if defined(__AROSEXEC_SMP__)
                    if (!(PrivExecBase(SysBase)->IntFlags & EXECF_CPUAffinity) || (GetIntETask(nexttask) && core_APIC_CPUInMask(cpuNo, GetIntETask(nexttask)->iet_CpuAffinity)))
                    {
#endif
                     if (
#if defined(__AROSEXEC_SMP__)
                            (task->tc_State != TS_SPIN) &&

#endif
                            (nexttask->tc_Node.ln_Pri <= task->tc_Node.ln_Pri))
                        {
                            /* If the running task did not used it's whole quantum yet, let it work */
                            if (!FLAG_SCHEDQUANTUM_ISSET)
                                corereschedule = FALSE;
                        }
                        break;
#if defined(__AROSEXEC_SMP__)
                    }
#endif
                }
            }
#if defined(__AROSEXEC_SMP__)
            KrnSpinUnLock(&PrivExecBase(SysBase)->TaskReadySpinLock);
#endif
        }

#if defined(__AROSEXEC_SMP__)
        if ((!corereschedule) && (task->tc_State == TS_SPIN))
            task->tc_State = TS_RUN;
#endif
    }

    DSCHED
        (
            if (corereschedule)
                bug("[Kernel:%03u]" DEBUGCOLOR_SET " '%s' @ 0x%p needs rescheduled .." DEBUGCOLOR_RESET "\n", cpuNo, task->tc_Node.ln_Name, task);
        )

    return corereschedule;
}

/*
    Switch the currently running task on this cpu
    if it is running , switch it to the ready state
    on SMP builds, if exec has set TS_SPIN,
    switch it to the spinning list.
*/
#if defined(__AROSEXEC_SMP__)
/*
 * The scheduler can run inside a syscall entered with interrupts enabled
 * (Reschedule() from a task). While it holds a task's tc_SpinLock an
 * interrupt on this CPU whose handler Signal()s that task would spin on a
 * lock its own CPU holds (e1000 Rx IRQ during core_Dispatch, SMP=8), so
 * interrupts stay off here for as long as a task lock is held - as they are
 * for every other tc_SpinLock holder, which runs under Disable().
 */
static inline IPTR sched_IntsOff(void)
{
    IPTR flags;

    asm volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void sched_IntsRestore(IPTR flags)
{
    asm volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
}
#endif

void core_Switch(void)
{
    cpuid_t cpuNo;
    struct Task *task;
    ULONG showAlert = 0;
    BOOL doSwitch = TRUE;
#if defined(__AROSEXEC_SMP__)
    BOOL parkedByWait = FALSE;
    IPTR intFlags = 0;
#endif
    struct ETask * taskEtask = NULL;

    DSCHED(bug("[Kernel]" DEBUGFUNCCOLOR_SET " %s()" DEBUGCOLOR_RESET "\n", __func__);)

    cpuNo = KrnGetCPUNumber();
    task = GET_THIS_TASK;

    DSCHED(
        bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Current running Task @ 0x%p" DEBUGCOLOR_RESET "\n", cpuNo, __func__, task);
    )
    if ((!task) || (task->tc_State == TS_INVALID))
    {
        bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: called on invalid task!" DEBUGCOLOR_RESET "\n", cpuNo, __func__);
        doSwitch = FALSE;
    }
    DSCHED(
        bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Task name '%s'" DEBUGCOLOR_RESET "\n", cpuNo, __func__, task->tc_Node.ln_Name);
        bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Task state = %08X" DEBUGCOLOR_RESET "\n", cpuNo, __func__, task->tc_State);
    )
#if defined(__AROSEXEC_SMP__)
    if (doSwitch && task->tc_State == TS_TOMBSTONED)
        doSwitch = FALSE;
#endif
    if (!doSwitch)
    {
        bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Letting Task continue to run.." DEBUGCOLOR_RESET "\n", cpuNo, __func__);
        return;
    }

    DSCHED(
        bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Switching away from Task" DEBUGCOLOR_RESET "\n", cpuNo, __func__);
    )

#if defined(__AROSEXEC_SMP__)
    /*
     * A task that reaches us in TS_WAIT was moved from TaskRunning to
     * TaskWait by Wait() itself, under its tc_SpinLock, before the
     * KrnSwitch (rom/exec/wait.c). It is not ours to relink: removing it
     * here would unlink it from TaskWait under the *running* list lock,
     * racing Signal()'s Remove()/Enqueue() on that list, and the Enqueue
     * below would splice the node into TaskWait a second time - one node
     * on two lists, and TaskReady ends up headed by a TS_WAIT task that
     * core_Dispatch()/core_Switch() then bounce between forever
     * (Chromium with pinned threads at -smp 4, 2026-09-21: 428 nested
     * core_Dispatch frames ate the BSP TLS). Leave the lists alone and
     * only do the bookkeeping that is still ours (stack check, IDNestCnt).
     */
    if (task->tc_State == TS_WAIT)
        parkedByWait = TRUE;
    else
    {
        /*
         * Every other writer of (tc_State, list membership) holds the
         * task's tc_SpinLock (settaskpri.c, and X86_HandleReschedTask).
         * Without it a reschedule request on another CPU could read the
         * TS_READY written below before the node is on TaskReady, take it
         * off a list it is not on and queue it itself - and then the
         * Enqueue below put it on TaskReady a second time, a cycle that
         * Enqueue() then walks forever holding TaskReadySpinLock (SMP-
         * MultiWake). Nothing on this CPU holds it here: exec's locked
         * sections run under Disable(), and a Wait() that does arrives as
         * parkedByWait.
         */
        intFlags = sched_IntsOff();
        KrnSpinLock(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
        KrnSpinLock(&PrivExecBase(SysBase)->TaskRunningSpinLock, NULL,
                    SPINLOCK_MODE_WRITE);
        REMOVE(&task->tc_Node);
        KrnSpinUnLock(&PrivExecBase(SysBase)->TaskRunningSpinLock);
        if (task->tc_State == TS_REMOVED)
            task->tc_State = TS_TOMBSTONED;
    }
#else
    if (task->tc_State != TS_RUN && task->tc_State != TS_REMOVED)
    REMOVE(&task->tc_Node);
#endif

    DSCHED(bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Task removed from list, state = %08X" DEBUGCOLOR_RESET "\n", cpuNo, __func__, task->tc_State);)

    if ((task->tc_State != TS_WAIT) &&
#if defined(__AROSEXEC_SMP__)
        (task->tc_State != TS_SPIN) &&
        (task->tc_State != TS_TOMBSTONED) &&
#endif
        (task->tc_State != TS_REMOVED))
        task->tc_State = TS_READY;
#if defined(__AROSEXEC_SMP__)
    if ((task->tc_State == TS_READY) && GetIntETask(task) &&
        GetIntETask(task)->iet_StopReq)
    {
        task->tc_SigWait = 0;
        task->tc_State = TS_WAIT;
    }
#endif

    /* if the current task has gone out of stack bounds, suspend it to prevent further damage to the system */
    if (task->tc_SPReg <= task->tc_SPLower || task->tc_SPReg > task->tc_SPUpper)
    {
        bug("[Kernel:%03u]" DEBUGCOLOR_SET " ERROR! Task (%s) went out of stack limits" DEBUGCOLOR_RESET "\n", cpuNo, task->tc_Node.ln_Name);
        bug("[Kernel:%03u]" DEBUGCOLOR_SET "  - Lower Bound = 0x%p, Upper Bound = 0x%p" DEBUGCOLOR_RESET "\n", cpuNo, task->tc_SPLower, task->tc_SPUpper);
        bug("[Kernel:%03u]" DEBUGCOLOR_SET "  - SP = 0x%p" DEBUGCOLOR_RESET "\n", cpuNo, task->tc_SPReg);

        task->tc_SigWait    = 0;
        task->tc_State      = TS_WAIT;

        showAlert = AN_StackProbe;
#if defined(LOWSTACKWARN)
    }
    else if (task->tc_SPReg <= task->tc_SPLower + 1024)
    {
        bug("[Kernel:%03u]" DEBUGCOLOR_SET " WARNING - Task (%s) close to stack limits" DEBUGCOLOR_RESET "\n", cpuNo, task->tc_Node.ln_Name);
#endif
    }
    task->tc_IDNestCnt = IDNESTCOUNT_GET;

    if (task->tc_State == TS_READY)
    {
        if (task->tc_Flags & TF_SWITCH)
            AROS_UFC1NR(void, task->tc_Switch, AROS_UFCA(struct ExecBase *, SysBase, A6));

        DSCHED(bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Setting '%s' @ 0x%p as ready" DEBUGCOLOR_RESET "\n", cpuNo, __func__, task->tc_Node.ln_Name, task);)
#if defined(__AROSEXEC_SMP__)
        KrnSpinLock(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL,
                SPINLOCK_MODE_WRITE);
#endif
        Enqueue(&SysBase->TaskReady, &task->tc_Node);
#if defined(__AROSEXEC_SMP__)
        KrnSpinUnLock(&PrivExecBase(SysBase)->TaskReadySpinLock);
#endif
    }
#if defined(__AROSEXEC_SMP__)
    else if (task->tc_State == TS_SPIN)
    {
        DSCHED(bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Setting '%s' @ 0x%p to spin" DEBUGCOLOR_RESET "\n", cpuNo, __func__, task->tc_Node.ln_Name, task);)
        KrnSpinLock(&PrivExecBase(SysBase)->TaskSpinningLock, NULL,
                    SPINLOCK_MODE_WRITE);
        Enqueue(&PrivExecBase(SysBase)->TaskSpinning, &task->tc_Node);
        KrnSpinUnLock(&PrivExecBase(SysBase)->TaskSpinningLock);
    }
#endif
    else if(
#if defined(__AROSEXEC_SMP__)
        (!parkedByWait) &&              /* already on TaskWait, see above */
        (task->tc_State != TS_TOMBSTONED) &&
#endif
        (task->tc_State != TS_REMOVED))
    {
        DSCHED(bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Setting '%s' @ 0x%p to wait" DEBUGCOLOR_RESET "\n", cpuNo, __func__, task->tc_Node.ln_Name, task);)
#if defined(__AROSEXEC_SMP__)
        KrnSpinLock(&PrivExecBase(SysBase)->TaskWaitSpinLock, NULL,
                    SPINLOCK_MODE_WRITE);
#endif
        Enqueue(&SysBase->TaskWait, &task->tc_Node);
#if defined(__AROSEXEC_SMP__)
        KrnSpinUnLock(&PrivExecBase(SysBase)->TaskWaitSpinLock);
#endif
    }
#if defined(__AROSEXEC_SMP__)
    if (!parkedByWait)
    {
        KrnSpinUnLock(&task->tc_SpinLock);
        sched_IntsRestore(intFlags);
    }
#endif
    if (showAlert)
        Alert(showAlert);
}

/* Dispatch a "new" ready task on this cpu */
struct Task *core_Dispatch(void)
{
    struct Task *newtask;
    struct Task *task = GET_THIS_TASK;
#if defined(__AROSEXEC_SMP__)
    BOOL newtaskLocked = FALSE;
    IPTR intFlags;
#endif
#if defined(__AROSEXEC_SMP__) || (DEBUG > 0)
    cpuid_t cpuNo = KrnGetCPUNumber();
#endif

    DSCHED(bug("[Kernel:%03u]" DEBUGFUNCCOLOR_SET " %s()" DEBUGCOLOR_RESET "\n", cpuNo, __func__);)

#if defined(__AROSEXEC_SMP__)
    intFlags = sched_IntsOff();
    KrnSpinLock(&PrivExecBase(SysBase)->TaskReadySpinLock, NULL,
                SPINLOCK_MODE_WRITE);
#endif
    for (newtask = (struct Task *)GetHead(&SysBase->TaskReady); newtask != NULL; newtask = (struct Task *)GetSucc(newtask))
    {
#if defined(__AROSEXEC_SMP__)
        if (!(PrivExecBase(SysBase)->IntFlags & EXECF_CPUAffinity) || (GetIntETask(newtask) && core_APIC_CPUInMask(cpuNo, GetIntETask(newtask)->iet_CpuAffinity)))
        {
            /*
             * The task lock keeps (tc_State, list) together until the task
             * is TS_RUN on TaskRunning (see core_Switch). We already hold
             * TaskReadySpinLock, and the order everywhere else is task lock
             * first, so only try it: a task whose lock is busy is being
             * changed on another CPU - leave it and take the next.
             */
            if (!KrnSpinTryLock(&newtask->tc_SpinLock, SPINLOCK_MODE_WRITE))
                continue;
            newtaskLocked = TRUE;
#endif
            REMOVE(&newtask->tc_Node);
            break;
#if defined(__AROSEXEC_SMP__)
        }
#endif
    }
#if defined(__AROSEXEC_SMP__)
    KrnSpinUnLock(&PrivExecBase(SysBase)->TaskReadySpinLock);
    if (!newtaskLocked)
        sched_IntsRestore(intFlags);
#endif

    if ((!newtask) && (task) && (task->tc_State != TS_WAIT))
    {
#if defined(__AROSEXEC_SMP__)
        if (task->tc_State == TS_SPIN)
        {
            DSCHED(bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Waking spinning task '%s' @ 0x%p" DEBUGCOLOR_RESET "\n",
                cpuNo, __func__newtask->tc_Node.ln_Name, newtask);)
#if 0
bug("----> such unspinning should not take place!" DEBUGCOLOR_RESET "\n");
            KrnSpinLock(&PrivExecBase(SysBase)->TaskSpinningLock, NULL,
                    SPINLOCK_MODE_WRITE);
            REMOVE(&task->tc_Node);
            KrnSpinUnLock(&PrivExecBase(SysBase)->TaskSpinningLock);
#endif
            task->tc_State = TS_READY;
        }
#endif
        newtask = task;
    }

    if (newtask != NULL)
    {
        BOOL launchtask = FALSE, sleeptask = FALSE;

        if (newtask->tc_State == TS_READY)
        {
            DSCHED(bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Preparing to run '%s' @ 0x%p" DEBUGCOLOR_RESET "\n",
                cpuNo, __func__, newtask->tc_Node.ln_Name, newtask);)

            IDNESTCOUNT_SET(newtask->tc_IDNestCnt);
            SET_THIS_TASK(newtask);
            SCHEDELAPSED_SET(SCHEDQUANTUM_GET);
            FLAG_SCHEDQUANTUM_CLEAR;
        }
        if  ((newtask->tc_State == TS_READY) || (newtask->tc_State == TS_RUN))
        {
            /* Check the stack of the task we are about to launch. */
            if ((newtask->tc_SPReg <= newtask->tc_SPLower) ||
                (newtask->tc_SPReg > newtask->tc_SPUpper))
            {
                newtask->tc_State     = TS_WAIT;
                sleeptask = TRUE;
            }
            else
            {
                newtask->tc_State     = TS_RUN;
                launchtask = TRUE;
            }
        }
        else if (
#if defined(__AROSEXEC_SMP__)
            (task->tc_State == TS_TOMBSTONED ) ||
#endif
            (task->tc_State == TS_REMOVED))
        {
#if defined(__AROSEXEC_SMP__) || (DEBUG > 0)
            // The task is on its way out ...
            bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: --> Dispatching finalizing/tombstoned task?" DEBUGCOLOR_RESET "\n", cpuNo, __func__);
            bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: --> Task @ 0x%p '%s', state %08X" DEBUGCOLOR_RESET "\n", cpuNo, __func__, task, task->tc_Node.ln_Name, newtask->tc_State);
#endif
        }

        if (sleeptask)
        {
            DSCHED(bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Moving '%s' @ 0x%p to wait queue" DEBUGCOLOR_RESET "\n", cpuNo, __func__, newtask->tc_Node.ln_Name, newtask);)
#if defined(__AROSEXEC_SMP__)
            KrnSpinLock(&PrivExecBase(SysBase)->TaskWaitSpinLock, NULL,
                        SPINLOCK_MODE_WRITE);
#endif
            Enqueue(&SysBase->TaskWait, &newtask->tc_Node);
#if defined(__AROSEXEC_SMP__)
            KrnSpinUnLock(&PrivExecBase(SysBase)->TaskWaitSpinLock);
#endif
        }
#if defined(__AROSEXEC_SMP__)
        /* tc_State and the list now agree; core_Switch (below) takes it again */
        if (newtaskLocked)
        {
            KrnSpinUnLock(&newtask->tc_SpinLock);
            newtaskLocked = FALSE;
            sched_IntsRestore(intFlags);
        }
#endif

        if (!launchtask)
        {
            /* if the new task shouldn't run - force a reschedule */
            DSCHED(bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Skipping '%s' @ 0x%p (state %08X)" DEBUGCOLOR_RESET "\n", cpuNo, __func__, newtask->tc_Node.ln_Name, newtask, newtask->tc_State);)

            core_Switch();
            newtask = core_Dispatch();
        }
        else
        {
#if defined(AROS_NO_ATOMIC_OPERATIONS)
            SysBase->DispCount++;
#else
            AROS_ATOMIC_INC(SysBase->DispCount);
#endif
            DSCHED(bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: Launching '%s' @ 0x%p (state %08X)" DEBUGCOLOR_RESET "\n", cpuNo, __func__, newtask->tc_Node.ln_Name, newtask, newtask->tc_State);)
        }
    }
    else
    {
        /* Go idle if there is nothing to do ... */
        DSCHED(bug("[Kernel:%03u]" DEBUGCOLOR_SET " %s: No ready Task(s) - entering sleep mode" DEBUGCOLOR_RESET "\n", cpuNo, __func__);)

        /*
         * Idle counter is incremented every time when we enter here,
         * not only once. This is correct.
         */
#if defined(AROS_NO_ATOMIC_OPERATIONS)
        SysBase->IdleCount++;
#else
        AROS_ATOMIC_INC(SysBase->IdleCount);
#endif
        FLAG_SCHEDSWITCH_SET;
    }

    return newtask;
}
