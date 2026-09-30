/* Copyright 2026 The AROS Development Team. SPDX-License-Identifier: BSD-2-Clause
 * Native demo launcher: preserve stdout AND stderr on the serial DEBUG handler.
 * Does not load, replace or select a driver. Run resident-library tests only
 * after a fresh physical boot. A crashed child may never return to SystemTags;
 * its DEBUG output remains live in the external UART recorder nonetheless.
 */
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <aros/debug.h>
#include <stdio.h>
#include <string.h>

static int stream_test(void)
{
    static const char dos_error[] = "RUNLOG DOS-STDERR sentinel\n";
    BPTR error = ((struct Process *)FindTask(NULL))->pr_CES;
    int ok = error && Write(error, dos_error, sizeof(dos_error) - 1) ==
                      (LONG)(sizeof(dos_error) - 1);

    ok = (fprintf(stdout, "RUNLOG C-STDOUT sentinel\n") > 0) && ok;
    ok = (fprintf(stderr, "RUNLOG C-STDERR sentinel\n") > 0) && ok;
    ok = (fflush(stdout) == 0) && ok;
    ok = (fflush(stderr) == 0) && ok;
    bug("[RUNLOG-v1] stream-test result=%d pr_CES=%p\n", !ok,
        BADDR(error));
    return ok ? 0 : 20;
}

int main(int argc, char **argv)
{
    BPTR input = BNULL, output = BNULL, error = BNULL;
    LONG result = -1, ioerror = 0;
    int close_ok = 1;

    if (argc != 2) {
        bug("Usage: AmdgpuRunLogged \"command with arguments\"\n");
        return 20;
    }
    if (!strcmp(argv[1], "--stream-test"))
        return stream_test();

    input = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    output = Open((CONST_STRPTR)"DEBUG:", MODE_NEWFILE);
    error = Open((CONST_STRPTR)"DEBUG:", MODE_NEWFILE);
    if (!input || !output || !error) {
        ioerror = IoErr();
        bug("[RUNLOG-v1] stream open failed IoErr=%d\n", (int)ioerror);
        goto done;
    }
    /* PosixC rejects a NIL/invalid error handler and silently falls back to
     * NIL. Refuse to call that a captured run. Output/error are distinct opens.
     */
    if (!((struct FileHandle *)BADDR(output))->fh_Type ||
        !((struct FileHandle *)BADDR(error))->fh_Type) {
        bug("[RUNLOG-v1] refusing a NULL output/error handler\n");
        goto done;
    }
    bug("[RUNLOG-v1] begin command=%s output=%p error=%p stack=2097152\n",
        argv[1], BADDR(output), BADDR(error));
    result = SystemTags((CONST_STRPTR)argv[1],
                        SYS_Input, (IPTR)input,
                        SYS_Output, (IPTR)output,
                        SYS_Error, (IPTR)error,
                        SYS_UserShell, TRUE,
                        NP_StackSize, (IPTR)2097152,
                        TAG_END);
    ioerror = IoErr();
    bug("[RUNLOG-v1] returned result=%d IoErr=%d\n", (int)result, (int)ioerror);

done:
    if (error && !Close(error)) close_ok = 0;
    if (output && !Close(output)) close_ok = 0;
    if (input && !Close(input)) close_ok = 0;
    bug("[RUNLOG-v1] finished result=%d IoErr=%d close_ok=%d\n",
        (int)result, (int)ioerror, close_ok);
    return result < 0 || !close_ok ? 20 : result;
}
