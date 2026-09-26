/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

/*
 * Place llvmpipe's rasterizer threads on their own CPUs.
 *
 * AROS starts a new task on its creator's CPU and never migrates it, so the
 * rasterizer threads the driver starts from one thread would all share that
 * thread's core. Each thread moves itself when it starts: thread i to CPU
 * 1 + i mod (N - 1), keeping CPU 0 for the application's own threads.
 *
 * Setting the environment variable SYS/llvmpipe.placement to "off" leaves
 * the threads where they start, for comparing the two layouts.
 *
 * Kept out of lp_rast.c so the AROS kernel headers stay away from Mesa's.
 */

#include <exec/types.h>
#include <dos/var.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <proto/task.h>

void lp_aros_place_rast_thread(unsigned thread_index);

static BOOL placement_disabled(void)
{
    struct Library *DOSBase = OpenLibrary("dos.library", 36);
    char value[8];
    BOOL off = FALSE;

    if (DOSBase)
    {
        if (GetVar("SYS/llvmpipe.placement", value, sizeof(value),
                   GVF_GLOBAL_ONLY) > 0)
            off = (value[0] == 'o' || value[0] == 'O') &&
                  (value[1] == 'f' || value[1] == 'F');
        CloseLibrary(DOSBase);
    }
    return off;
}

void lp_aros_place_rast_thread(unsigned thread_index)
{
    APTR KernelBase = OpenResource("kernel.resource");
    APTR TaskResBase = OpenResource("task.resource");
    unsigned int cpus;
    void *mask;

    if (!KernelBase || !TaskResBase || placement_disabled())
        return;

    cpus = KrnGetCPUCount();
    if (cpus < 2)
        return;

    mask = KrnAllocCPUMask();
    if (!mask)
        return;

    KrnClearCPUMask(mask);
    KrnGetCPUMask(1 + thread_index % (cpus - 1), mask);
    /* The mask is copied; the move happens at the CPU's next scheduling
       point. Fails harmlessly on a non-SMP build. */
    SetTaskAffinity(NULL, mask);
    KrnFreeCPUMask(mask);
}
