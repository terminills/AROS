/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Run a command with its CPU affinity widened to every CPU.

    A new Task inherits its creator's CPU set and is never moved, so a
    program started from a shell on CPU 0 keeps every process and thread it
    creates on CPU 0. SMP-AnyCPU widens its own set to all CPUs and runs the
    command line, which inherits that set.

    Usage: SMP-AnyCPU COMMAND/F
*/

#include <aros/debug.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/task.h>

APTR TaskResBase;

int main(void)
{
    IPTR args[1] = { 0 };
    struct RDArgs *rda;
    LONG rc;

    if (!(TaskResBase = OpenResource("task.resource")))
    {
        PutStr("SMP-AnyCPU: no task.resource\n");
        return RETURN_FAIL;
    }
    if (!(rda = ReadArgs("COMMAND/F/A", args, NULL)))
    {
        PrintFault(IoErr(), "SMP-AnyCPU");
        return RETURN_FAIL;
    }
    if (!SetTaskAffinity(NULL, (APTR)TASKAFFINITY_ANY))
    {
        PutStr("SMP-AnyCPU: SetTaskAffinity failed\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    bug("[smpanycpu] affinity widened, running: %s\n", (char *)args[0]);
    rc = SystemTags((STRPTR)args[0], SYS_Input, Input(), SYS_Output, Output(),
        TAG_DONE);
    FreeArgs(rda);
    return rc;
}
