/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    POSIX.1-2008 function pread().
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

        ssize_t pread (

/*  SYNOPSIS */
        int    fd,
        void * buf,
        size_t nbytes,
        off_t  offset)

/*  FUNCTION
        Read up to nbytes bytes from the file descriptor fd at the given
        offset, WITHOUT changing the file's current offset.

    INPUTS
        fd - The file descriptor to read from.
        buf - The buffer to read the bytes into.
        nbytes - Read at most this many bytes.
        offset - Absolute file offset to read from.

    RESULT
        The number of bytes read (0 at end-of-file), or -1 on error with
        errno set.

    NOTES
        AROS DOS handles are stateful (Seek()+Read()), so pread() is
        implemented as save-offset / seek / read / restore-offset. It is
        therefore NOT atomic against concurrent I/O on a handle shared by
        duplicated descriptors -- this matches posixc's documented
        per-handle serialization contract (see __fdesc.c). Offsets beyond
        2 GB currently truncate because __fcb_set_position() still casts to
        LONG; this is lifted by the posixc->dos64 wiring (the pread64/
        pwrite64 variants land with that work). See POSIXC-DOS64-WIRING-MAP.

    EXAMPLE

    BUGS

    SEE ALSO
        read(), pwrite(), lseek()

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

    /* Save the current offset; pread() must leave it unchanged. */
    if (!__fcb_get_position(fdesc->fcb, &saved))
        goto error;

    if (!__fcb_set_position(fdesc->fcb, (QUAD)offset))
        goto error;

    cnt = Read(fdesc->fcb->handle, buf, nbytes);
    ioerr = IoErr();

    /* Restore the original offset regardless of the read outcome. */
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
} /* pread */
