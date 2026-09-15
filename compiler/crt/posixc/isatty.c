/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

#include <proto/dos.h>
#include <dos/dosextens.h>
#include <string.h>

#include "__fdesc.h"

/* A DOS PIPE: handle answers IsInteractive() with DOSTRUE: the pipe handler
   sets fh_Interactive so that dos gives the handle a stream's line buffering
   rather than a file's full buffering (workbench/fs/pipe/pipecreate.c).  That
   flag therefore means "buffer me by lines", not "I am a terminal", and POSIX
   requires isatty() to answer no for a pipe.

   Saying yes cost us the whole Claude Code panel in the desktop VS Code gate:
   node asks uv_guess_handle() -> isatty() for each of a spawned child's
   descriptors, built TTY streams over the child's PIPE: stdio, and the CLI
   then took its interactive path - it refused the SDK's non-interactive
   arguments ("--include-partial-messages requires --print and
   --output-format=stream-json") and wrote ANSI colour into the stdout the
   extension parses as JSON ("claude auth status parse failed").

   Comparing the handle's handler port with PIPE:'s is exact and needs no
   packet.  DeviceProc() is resolved per call rather than cached: it is cheap
   next to how rarely isatty() is asked, and a cached port would go stale if
   the handler were ever remounted. */
static int __posixc_is_dos_pipe(BPTR handle)
{
    struct FileHandle *fh = (struct FileHandle *)BADDR(handle);
    struct MsgPort *pipeport;

    if (fh == NULL || fh->fh_Type == NULL)
        return 0;

    pipeport = DeviceProc("PIPE:");

    return (pipeport != NULL && pipeport == fh->fh_Type);
}

/*****************************************************************************

    NAME */

#include <unistd.h>

        int isatty(

/*  SYNOPSIS */
        int fd)

/*  FUNCTION

    INPUTS

    RESULT

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

******************************************************************************/
{
    fdesc *desc = __getfdesc(fd);

    if (desc)
    {
        if (!IsInteractive(desc->fcb->handle))
            return 0;

        return __posixc_is_dos_pipe(desc->fcb->handle) ? 0 : 1;
    }

    return 0;
}

