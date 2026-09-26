/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

/*
 * Run gallivm's LLVM compile on a stack big enough for it.
 *
 * LLVM's code generation recurses deeply and runs on the calling task's
 * stack. An AROS task started from a Shell gets 40 KiB by default, and a GL
 * program compiling its first shader through llvmpipe then went out of stack
 * limits. When less than LP_AROS_MIN_FREE is left, the compile runs under
 * NewStackSwap() on a stack of its own, which also moves the task's stack
 * bounds so the kernel's stack check follows it.
 */

#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#define LP_AROS_MIN_FREE    (256 * 1024)
#define LP_AROS_BIG_STACK   (1024 * 1024)

void lp_aros_run_on_big_stack(void (*fn)(void *), void *arg);

void lp_aros_run_on_big_stack(void (*fn)(void *), void *arg)
{
    struct Task *me = FindTask(NULL);
    struct StackSwapStruct sss;
    struct StackSwapArgs args;
    UBYTE here;
    APTR stack;

    if (!me->tc_SPLower ||
        (IPTR)&here - (IPTR)me->tc_SPLower >= LP_AROS_MIN_FREE)
    {
        fn(arg);
        return;
    }

    stack = AllocMem(LP_AROS_BIG_STACK, MEMF_ANY);
    if (!stack)
    {
        fn(arg);
        return;
    }

    sss.stk_Lower = stack;
    sss.stk_Upper = (APTR)((IPTR)stack + LP_AROS_BIG_STACK);
    sss.stk_Pointer = sss.stk_Upper;
    args.Args[0] = (IPTR)arg;
    NewStackSwap(&sss, (LONG_FUNC)fn, &args);

    FreeMem(stack, LP_AROS_BIG_STACK);
}
