/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Read from a file at a given offset.
*/
#include <errno.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "__fdesc.h"
#include "__dos64.h"

/*****************************************************************************

    NAME */
#include <unistd.h>

        ssize_t pread64 (

/*  SYNOPSIS */
        int        d,
        void      *buf,
        size_t     nbytes,
        __off64_t  offset)

/*  FUNCTION
        Read up to nbytes from the file at offset, leaving the file offset
        of the descriptor unchanged.

    INPUTS
        d      - file descriptor to read from
        buf    - where to put the data
        nbytes - number of bytes to read
        offset - position in the file to read from

    RESULT
        The number of bytes read, 0 at or beyond the end of the file, or -1
        with errno set.

    NOTES
        The file offset is saved, moved and restored under the file's I/O
        lock, so read() and write() on the same file never see it move. An
        offset at or beyond the end returns 0.

    EXAMPLE

    BUGS

    SEE ALSO
        pread(), read(), lseek64()

    INTERNALS

******************************************************************************/
{
    ssize_t cnt;
    QUAD saved;
    LONG ioerr;
    fdesc *fdesc = __getfdesc(d);

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

    /* Save the current offset; pread64() must leave it unchanged.  Hold
       the fcb's io_lock across the whole seek/transfer/seek sequence so a
       concurrent read()/write() on the same handle cannot interleave. */
    __fcb_lock(fdesc->fcb);

    saved = __fcb_getpos(fdesc->fcb);
    if (saved == -1)
        goto error;

    /* An offset at or past EOF is a legal read position that returns 0 */
    if (__fcb_seek(fdesc->fcb, (QUAD)offset, OFFSET_BEGINNING) == -1)
        goto error;

    cnt = __fcb_settle(fdesc->fcb, FALSE);
    if (cnt == 0)
        cnt = __dos64_read(fdesc->fcb, buf, nbytes);
    else if (cnt > 0)
        cnt = 0;
    ioerr = IoErr();

    /* Restore the original offset regardless of the transfer outcome. */
    __fcb_seek(fdesc->fcb, saved, OFFSET_BEGINNING);
    __fcb_unlock(fdesc->fcb);

    if (cnt == -1)
    {
        errno = __stdc_ioerr2errno(ioerr);
        return -1;
    }

    return cnt;

error:
    ioerr = IoErr();
    __fcb_unlock(fdesc->fcb);
    errno = __stdc_ioerr2errno(ioerr);
    return -1;
} /* pread64 */
