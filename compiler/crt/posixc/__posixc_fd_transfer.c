/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Pass an open file descriptor from one process to another.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <errno.h>
#include <fcntl.h>

#include "__fdesc.h"

/*
    A descriptor number means something only in the table of the process that
    owns it, so a program that hands an open file to another process (the
    AROS counterpart of passing it over a Unix socket with SCM_RIGHTS) cannot
    send the number. What it can send is the file itself: posixc's file
    control block is allocated from global memory, is reference counted and
    already shared between dup'd descriptors, so the receiving process can
    point a descriptor of its own at the same block. Both then share one open
    file - one DOS handle, one offset, one set of status flags - exactly as two
    Unix processes do after a descriptor is passed between them.

    __posixc_fd_export() takes a reference and returns it as an opaque token;
    the sender may close its own descriptor afterwards. The token is consumed
    by exactly one of __posixc_fd_import() in the receiver or
    __posixc_fd_release() when it is never delivered.
*/

/*****************************************************************************

    NAME */

        void *__posixc_fd_export (

/*  SYNOPSIS */
        int file_descriptor)

/*  FUNCTION
        Take a transferable reference to the open file behind a descriptor.

    INPUTS
        file_descriptor - an open descriptor of the calling process.

    RESULT
        An opaque token for __posixc_fd_import() or __posixc_fd_release(),
        or NULL with errno set to EBADF.

    NOTES
        The descriptor stays open in the calling process; closing it does not
        affect the token. Directory descriptors (opendir()/open() of a
        directory) are exported the same way: the shared block then holds a
        lock instead of a file handle.

    SEE ALSO
        __posixc_fd_import(), __posixc_fd_release()

******************************************************************************/
{
    fdesc *desc = __getfdesc(file_descriptor);

    if (file_descriptor < 0 || desc == NULL || desc->fcb == NULL)
    {
        errno = EBADF;
        return NULL;
    }

    __fcb_ref(desc->fcb);
    return desc->fcb;
}

/*****************************************************************************

    NAME */

        int __posixc_fd_import (

/*  SYNOPSIS */
        void *token,
        int fdflags)

/*  FUNCTION
        Give the calling process a descriptor for a file exported by another
        process (or by itself) with __posixc_fd_export().

    INPUTS
        token   - a token from __posixc_fd_export(); consumed on success.
        fdflags - descriptor flags for the new descriptor, 0 or FD_CLOEXEC.

    RESULT
        The lowest free descriptor, or -1 with errno set (the token is then
        NOT consumed and must still be imported or released).

    SEE ALSO
        __posixc_fd_export(), __posixc_fd_release()

******************************************************************************/
{
    fcb *shared = (fcb *)token;
    fdesc *desc;
    int fd;

    if (shared == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if ((desc = __alloc_fdesc()) == NULL)
    {
        errno = ENOMEM;
        return -1;
    }

    /* errno is set by __getfirstfd()/__getfdslot() on failure */
    fd = __getfirstfd(0);
    if (fd < 0 || __getfdslot(fd) < 0)
    {
        if (fd >= 0)
            __freefdslot(fd);
        __free_fdesc(desc);
        return -1;
    }

    desc->fdflags = fdflags & FD_CLOEXEC;
    desc->fcb = shared;       /* the token's reference becomes this one */
    __setfdesc(fd, desc);

    return fd;
}

/*****************************************************************************

    NAME */

        void __posixc_fd_release (

/*  SYNOPSIS */
        void *token)

/*  FUNCTION
        Drop a token from __posixc_fd_export() that will not be imported, for
        example because the message carrying it was discarded. When it held
        the last reference, the file is closed.

    INPUTS
        token - a token from __posixc_fd_export(), or NULL (ignored).

    SEE ALSO
        __posixc_fd_export(), __posixc_fd_import()

******************************************************************************/
{
    fcb *shared = (fcb *)token;

    if (shared == NULL)
        return;

    /* The same teardown as close() for the last reference: handles posixc
       does not own (_FCB_DONTCLOSE_FH) are left open, and a directory holds
       a lock rather than a file handle. */
    if (__fcb_unref(shared) == 0)
    {
        if (!(shared->privflags & _FCB_DONTCLOSE_FH) && shared->handle)
        {
            if (shared->privflags & _FCB_ISDIR)
                UnLock(shared->handle);
            else
                Close(shared->handle);
        }
        FreeVec(shared);
    }
}
