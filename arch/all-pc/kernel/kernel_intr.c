/*
    Copyright (C) 2011-2026, The AROS Development Team. All rights reserved.

    Desc: High-level scheduler calling code (PC version)
*/

#include <exec/execbase.h>
#include <hardware/intbits.h>
#include <proto/exec.h>

#include <kernel_base.h>
#include <kernel_debug.h>
#include <kernel_intr.h>
#include <kernel_scheduler.h>
#include <kernel_syscall.h>

#define D(x)

/*
 * Leave the interrupt. This function receives the interrupt register frame
 * and runs task scheduler if needed.
 *
 * You should call it only if you're returning to user mode (i.e. not from
 * within nested interrupt).
 *
 * It relies on CPU-specific cpu_Switch() and cpu_Dispatch() implementations
 * which save and restore CPU context. cpu_Dispatch() is allowed just to
 * jump to the saved context and not return here.
 */
void core_ExitInterrupt(regs_t *regs)
{
    /* Soft interrupt requested? It's high time to do it */
    if (SysBase->SysFlags & SFF_SoftInt)
    {
        core_Cause(INTB_SOFTINT, 1L << INTB_SOFTINT);
        /*
         * SoftIntDispatch returns with KrnSti(). core_Schedule() below takes
         * TaskReadySpinLock for reading, and a device IRQ landing on this CPU
         * while it is held runs handlers that Signal() a task (timer.device
         * ReplyMsg), which wants the same lock for writing: the CPU spins on
         * its own read lock and every other CPU queues behind it in
         * core_Switch. Seen with Chromium playing YouTube on 4 cores.
         */
        __asm__ __volatile__("cli");
    }

    /* If task switching is disabled, do nothing */
    if (TDNESTCOUNT_GET < 0)
    {
        /*
         * Do not disturb task if it's not necessary.
         * Reschedule only if switch pending flag is set. Exit otherwise.
         */
        if (FLAG_SCHEDSWITCH_ISSET)
        {
            D(bug("[Kernel] %s: Triggering Schedule\n", __func__);)
            /* Run task scheduling sequence */
            if (core_Schedule())
            {
                cpu_Switch(regs);
                cpu_Dispatch(regs);
            }
        }
    }
}

/*
 * This routine dispatches scheduler's SysCall, when some task wants to give up
 * the CPU time explicitly.
 * Similar to above, it should be called only when you're returning to user mode.
 */
void core_SysCall(int sc, regs_t *regs)
{
    switch (sc)
    {
    case SC_CAUSE:
        core_ExitInterrupt(regs);
        break;

    case SC_SCHEDULE:
        if (!core_Schedule())
            break;
        /* Fallthrough */

    case SC_SWITCH:
        cpu_Switch(regs);
        /* Fallthrough */

    case SC_DISPATCH:
        cpu_Dispatch(regs);
        break;
    }
}
