/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    POSIX.1-2008 function pwrite().
*/

#include <errno.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "__fdesc.h"

/*****************************************************************************

    NAME */
#include <unistd.h>

        ssize_t pwrite (

/*  SYNOPSIS */
        int          fd,
        const void * buf,
        size_t       nbytes,
        off_t        offset)

/*  FUNCTION
        Write up to nbytes bytes to the file descriptor fd at the given
        offset, WITHOUT changing the file's current offset.

    INPUTS
        fd - The file descriptor to write to.
        buf - The buffer holding the bytes to write.
        nbytes - Write at most this many bytes.
        offset - Absolute file offset to write at.

    RESULT
        The number of bytes written, or -1 on error with errno set.

    NOTES
        Implemented as save-offset / seek / write / restore-offset (AROS
        DOS handles are stateful), so it is NOT atomic against concurrent
        I/O on a shared handle -- matching posixc's per-handle
        serialization contract (see __fdesc.c). O_APPEND is not given the
        Linux "always append" special case here. Offsets beyond 2 GB
        truncate until the posixc->dos64 wiring lands (__fcb_set_position
        casts to LONG); see POSIXC-DOS64-WIRING-MAP.

    EXAMPLE

    BUGS

    SEE ALSO
        write(), pread(), lseek()

    INTERNALS

******************************************************************************/
{
    ssize_t cnt;
    QUAD saved;
    LONG ioerr;
    fdesc *fdesc = __getfdesc(fd);

    if (!fdesc)
    {
        errno = EBADF;
        return -1;
    }

    if (fdesc->fcb->privflags & _FCB_ISDIR)
    {
        errno = EISDIR;
        return -1;
    }

    if (offset < 0)
    {
        errno = EINVAL;
        return -1;
    }

    /* Save the current offset; pwrite() must leave it unchanged. */
    if (!__fcb_get_position(fdesc->fcb, &saved))
        goto error;

    if (!__fcb_set_position(fdesc->fcb, (QUAD)offset))
        goto error;

    cnt = Write(fdesc->fcb->handle, (APTR)buf, nbytes);
    ioerr = IoErr();

    /* Restore the original offset regardless of the write outcome. */
    __fcb_set_position(fdesc->fcb, saved);

    if (cnt == -1)
    {
        errno = __stdc_ioerr2errno(ioerr);
        return -1;
    }

    return cnt;

error:
    errno = __stdc_ioerr2errno(IoErr());
    return -1;
} /* pwrite */
