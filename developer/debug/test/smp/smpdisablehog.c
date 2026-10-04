/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Holds Disable() on one CPU for MS milliseconds at a time, GAP ticks (20 ms)
          apart, until CTRL-C. Run it next to the other SMP tests to see how
          the rest of the system copes with a core that keeps interrupts off.
*/

#include <aros/debug.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>
#include <proto/timer.h>

APTR KernelBase;
struct Device *TimerBase;

struct HogArgs
{
    struct Task *ha_Parent;
    ULONG        ha_MS;
    ULONG        ha_Gap;
    ULONG        ha_CPU;
    volatile BOOL ha_Stop;
    volatile ULONG ha_Cycles;
};

static void HogTask(struct HogArgs *ha)
{
    struct EClockVal ev;
    ULONG efreq = ReadEClock(&ev);
    UQUAD ticks = ((UQUAD)efreq * ha->ha_MS) / 1000;
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *gap = port ?
        (struct timerequest *)CreateIORequest(port, sizeof(struct timerequest)) : NULL;

    if (!gap || OpenDevice("timer.device", UNIT_MICROHZ, &gap->tr_node, 0) != 0)
    {
        bug("[smpdisablehog] no timer for the gap\n");
        ha->ha_Stop = TRUE;
    }

    bug("[smpdisablehog] holding Disable() %lu ms on CPU %lu (ran on CPU %lu)\n",
        ha->ha_MS, ha->ha_CPU, (ULONG)KrnGetCPUNumber());

    while (!ha->ha_Stop)
    {
        UQUAD start, now;

        Disable();
        ReadEClock(&ev);
        start = ((UQUAD)ev.ev_hi << 32) | ev.ev_lo;
        do {
            ReadEClock(&ev);
            now = ((UQUAD)ev.ev_hi << 32) | ev.ev_lo;
        } while (now - start < ticks);
        Enable();

        if ((++ha->ha_Cycles % 100) == 0)
            bug("[smpdisablehog] CPU %lu: %lu windows\n", ha->ha_CPU, ha->ha_Cycles);

        gap->tr_node.io_Command = TR_ADDREQUEST;
        gap->tr_time.tv_secs = 0;
        gap->tr_time.tv_micro = ha->ha_Gap * 20000;
        DoIO(&gap->tr_node);
    }
    if (gap)
    {
        if (gap->tr_node.io_Device)
            CloseDevice(&gap->tr_node);
        DeleteIORequest(&gap->tr_node);
    }
    if (port)
        DeleteMsgPort(port);
    Signal(ha->ha_Parent, SIGBREAKF_CTRL_F);
}

int main(void)
{
    IPTR args[3] = { 0, 0, 0 };
    struct RDArgs *rda;
    struct timerequest tr;
    struct HogArgs ha = { 0 };
    void *mask;

    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
        return RETURN_FAIL;

    rda = ReadArgs("CPU/N,MS/N,GAP/N", args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "SMP-DisableHog");
        return RETURN_FAIL;
    }
    ha.ha_CPU = args[0] ? *(LONG *)args[0] : 1;
    ha.ha_MS = args[1] ? *(LONG *)args[1] : 20;
    ha.ha_Gap = args[2] ? *(LONG *)args[2] : 1;
    FreeArgs(rda);

    if (ha.ha_CPU >= KrnGetCPUCount())
    {
        Printf("SMP-DisableHog: CPU %lu does not exist\n", ha.ha_CPU);
        return RETURN_FAIL;
    }

    if (OpenDevice("timer.device", UNIT_ECLOCK, &tr.tr_node, 0) != 0)
        return RETURN_FAIL;
    TimerBase = tr.tr_node.io_Device;

    ha.ha_Parent = FindTask(NULL);
    mask = KrnAllocCPUMask();
    KrnGetCPUMask(ha.ha_CPU, mask);
    SetSignal(0, SIGBREAKF_CTRL_F);

    if (!NewCreateTask(TASKTAG_NAME, "SMP-DisableHog",
                       TASKTAG_AFFINITY, mask,
                       TASKTAG_PRI, 0,
                       TASKTAG_PC, HogTask,
                       TASKTAG_ARG1, &ha,
                       TAG_DONE))
    {
        KrnFreeCPUMask(mask);
        CloseDevice(&tr.tr_node);
        return RETURN_FAIL;
    }

    Printf("SMP-DisableHog: %lu ms windows on CPU %lu, CTRL-C to stop\n", ha.ha_MS, ha.ha_CPU);
    Wait(SIGBREAKF_CTRL_C);
    ha.ha_Stop = TRUE;
    Wait(SIGBREAKF_CTRL_F);
    bug("[smpdisablehog] stopped after %lu windows\n", ha.ha_Cycles);

    CloseDevice(&tr.tr_node);
    return RETURN_OK;
}
