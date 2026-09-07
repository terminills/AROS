/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CrashTest — deliberately fail in a chosen way so CrashReporter's
          capture path can be exercised end to end (DESIGN.md §5).

          CrashTest MODE/A,DELAY/N

          MODE:  NULL     read through a NULL pointer (#PF, AT_DeadEnd)
                 WRITE    write to an unmapped high address (#PF)
                 ILLEGAL  execute ud2 (#UD)
                 BREAK    execute int3 (#BP)
                 DIV      integer divide by zero (#DE)
                 ALERT    Alert(AN_StackProbe) — recoverable, no trap
                 DEADEND  Alert(AN_MemCorrupt | AT_DeadEnd) — no trap
                 DEEP     NULL deref five calls deep (backtrace check)

          DELAY: seconds to sleep first (lets you watch from LogView)

    AI_PHASE: CRASH_REPORTER
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: LOW
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/alerts.h>
#include <dos/dos.h>
#include <string.h>
#include <stdio.h>

const char *version = "$VER: CrashTest 1.0 (06.09.2026)";

static volatile ULONG *null_ptr = NULL;
static volatile ULONG  sink;
static volatile ULONG  zero = 0;

/* The recursion is through volatile calls so the compiler cannot flatten
   it; each level leaves a real frame for KrnBacktraceFromFrame(). */
static void __attribute__((noinline)) deep5(void) { sink = *null_ptr; }
static void __attribute__((noinline)) deep4(void) { deep5(); sink++; }
static void __attribute__((noinline)) deep3(void) { deep4(); sink++; }
static void __attribute__((noinline)) deep2(void) { deep3(); sink++; }
static void __attribute__((noinline)) deep1(void) { deep2(); sink++; }

int main(void)
{
    struct RDArgs *rda;
    IPTR args[2] = { 0, 0 };
    CONST_STRPTR mode;
    int rc = RETURN_OK;

    rda = ReadArgs("MODE/A,DELAY/N", args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "CrashTest");
        return RETURN_FAIL;
    }
    mode = (CONST_STRPTR)args[0];

    if (args[1] && *(LONG *)args[1] > 0)
    {
        printf("CrashTest: %s in %ld s...\n", mode, (long)*(LONG *)args[1]);
        fflush(stdout);
        Delay(*(LONG *)args[1] * TICKS_PER_SECOND);
    }
    printf("CrashTest: failing with %s\n", mode);
    fflush(stdout);

    if (!stricmp(mode, "NULL"))
        sink = *null_ptr;
    else if (!stricmp(mode, "WRITE"))
        *(volatile ULONG *)(IPTR)0x7ff0deadbeefULL = 1;
    else if (!stricmp(mode, "ILLEGAL"))
        __asm__ __volatile__("ud2");
    else if (!stricmp(mode, "BREAK"))
        __asm__ __volatile__("int3");
    else if (!stricmp(mode, "DIV"))
        sink = 100 / zero;
    else if (!stricmp(mode, "ALERT"))
        Alert(AN_StackProbe);
    else if (!stricmp(mode, "DEADEND"))
        Alert(AN_MemCorrupt);           /* AN_MemCorrupt carries AT_DeadEnd */
    else if (!stricmp(mode, "DEEP"))
        deep1();
    else
    {
        printf("CrashTest: unknown MODE %s\n", mode);
        rc = RETURN_ERROR;
    }

    /* only the recoverable ALERT mode gets here (or an unknown mode) */
    printf("CrashTest: survived (%s)\n", mode);
    FreeArgs(rda);
    return rc;
}
