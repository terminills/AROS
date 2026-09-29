/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: A program that returns from main() while its threads still run
          must not take the system down with it.

    An emulator returning from main() with dozens of its pthreads still busy
    wedged every CPU in the scheduler: TaskReadySpinLock was left held for
    reading and no task could be switched in or out again. This test repeats
    that shape on purpose. Each child run starts WORKERS threads that keep
    hitting the paths the scheduler shares with them - FindTask() by name
    (which read-locks the task lists), AllocMem()/FreeMem(), a contended
    mutex, Signal() and yields - then returns from main() after DWELL ticks
    without joining any of them.

    The parent runs the child RUNS times and prints DONE when every run has
    come back. A wedge shows as the serial log going quiet before DONE.

    Current state (2026-09-29): the first child never finishes exiting.
    pthread.library's exit function joins every thread so the program's code
    is not unloaded under them, and these workers never stop - POSIX would
    end them with the process. That is a hang of the one program, not a
    system wedge: the other CPUs stay idle and usable. The test stays failing
    until exit ends a process's threads.

    Usage: SMP-ExitRace [RUNS/N] [WORKERS/N] [DWELL/N]
           SMP-ExitRace CHILD [WORKERS/N] [DWELL/N]   (one run, used by the parent)
*/

#include <aros/debug.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#define ARG_TEMPLATE "RUNS/N,WORKERS/N,DWELL/N,CHILD/S"
#define DEF_RUNS     50
#define DEF_WORKERS  16
#define DEF_DWELL    25     /* ticks (1/50 s) the child lets its workers run */
#define MAX_WORKERS  64

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile ULONG g_spins;

static void *worker(void *arg)
{
    struct Task *parent = (struct Task *)arg;
    ULONG n = 0;

    (void)parent;
    for (;;)
    {
        /* FindTask() by name walks TaskReady/TaskWait under their locks */
        FindTask("SMP-ExitRace no such task");

        APTR mem = AllocMem(64 + (n & 255), MEMF_ANY);
        if (mem)
            FreeMem(mem, 64 + (n & 255));

        pthread_mutex_lock(&g_lock);
        g_spins++;
        pthread_mutex_unlock(&g_lock);

        Signal(FindTask(NULL), SIGBREAKF_CTRL_F);
        SetSignal(0, SIGBREAKF_CTRL_F);

        if ((++n & 15) == 0)
            sched_yield();
    }
    return NULL;
}

static int child(ULONG workers, ULONG dwell)
{
    pthread_t tid[MAX_WORKERS];
    ULONG started = 0;

    for (ULONG i = 0; i < workers; i++)
        if (pthread_create(&tid[i], NULL, worker, FindTask(NULL)) == 0)
            started++;

    Delay(dwell);
    bug("[smpexitrace] child: %lu workers, %lu spins, returning without join\n",
        (unsigned long)started, (unsigned long)g_spins);
    /* Deliberately no pthread_join(): this return is the test. */
    return started == workers ? RETURN_OK : RETURN_WARN;
}

int main(void)
{
    IPTR args[4] = { 0 };
    struct RDArgs *rda = ReadArgs(ARG_TEMPLATE, args, NULL);
    ULONG runs    = DEF_RUNS;
    ULONG workers = DEF_WORKERS;
    ULONG dwell   = DEF_DWELL;
    BOOL is_child;
    ULONG ok = 0, bad = 0;

    if (!rda)
    {
        PrintFault(IoErr(), "SMP-ExitRace");
        return RETURN_FAIL;
    }
    if (args[0]) runs    = *(ULONG *)args[0];
    if (args[1]) workers = *(ULONG *)args[1];
    if (args[2]) dwell   = *(ULONG *)args[2];
    is_child = args[3] != 0;
    FreeArgs(rda);

    if (workers > MAX_WORKERS)
        workers = MAX_WORKERS;

    if (is_child)
        return child(workers, dwell);

    /* The child is this program; name it by full path, since the shell
       SystemTags() starts does not share our program directory. */
    char self[256], name[108];
    if (!NameFromLock(GetProgramDir(), self, sizeof(self)) ||
        !GetProgramName(name, sizeof(name)) ||
        !AddPart(self, FilePart(name), sizeof(self)))
    {
        bug("[smpexitrace] cannot name own program\n");
        return RETURN_FAIL;
    }

    for (ULONG r = 0; r < runs; r++)
    {
        char cmd[400];
        LONG rc;

        snprintf(cmd, sizeof(cmd), "\"%s\" CHILD WORKERS=%lu DWELL=%lu", self,
            (unsigned long)workers, (unsigned long)dwell);
        rc = SystemTags(cmd, SYS_Input, Open("NIL:", MODE_OLDFILE),
            SYS_Output, Open("NIL:", MODE_NEWFILE), TAG_DONE);
        if (rc == RETURN_OK)
            ok++;
        else
            bad++;
        bug("[smpexitrace] run %lu/%lu rc=%ld\n",
            (unsigned long)(r + 1), (unsigned long)runs, (long)rc);
    }

    bug("[smpexitrace] DONE: %lu PASS, %lu FAIL, 0 INVALID\n",
        (unsigned long)ok, (unsigned long)bad);
    printf("DONE: %lu PASS, %lu FAIL\n", (unsigned long)ok, (unsigned long)bad);
    return bad ? RETURN_WARN : RETURN_OK;
}
