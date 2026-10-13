/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    C99 function fclose().
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "__stdio.h"

#include "__posixc_intbase.h"

/*****************************************************************************

    NAME */
#include <stdio.h>

        int fclose (

/*  SYNOPSIS */
        FILE * stream)

/*  FUNCTION
        Closes a stream.

    INPUTS
        stream - Stream to close.

    RESULT
        Upon successful completion 0 is returned. Otherwise, EOF is
        returned and the global variable errno is set to indicate the
        error. In either case no further access to the stream is possible.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        fopen(), open(), close()

    INTERNALS

******************************************************************************/
{
    struct PosixCIntBase *PosixCBase =
        (struct PosixCIntBase *)__aros_getbase_PosixCBase();
    FILENODE * fn;
    int error = 0;

    /* Close() flushes owned DOS handles, but inherited standard handles are
       kept open by close(). Flush output before releasing either kind. */
    if ((stream->flags & __POSIXC_STDIO_WRITE) && fflush(stream) == EOF)
        error = errno;
    if (close(stream->fd) == -1)
    {
        if (!error)
            error = errno;
    }

    fn = FILE2FILENODE (stream);
    ObtainSemaphore(&PosixCBase->stdio_sem);
    Remove ((struct Node *)fn);
    ReleaseSemaphore(&PosixCBase->stdio_sem);

    FreePooled(PosixCBase->internalpool, fn, sizeof(FILENODE));

    if (error)
    {
        errno = error;
        return EOF;
    }
    return 0;
} /* fclose */
