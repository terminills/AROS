/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    POSIX.1-2008 function fdatasync().
*/

#include <unistd.h>

/*****************************************************************************)

    NAME */

        int fdatasync(

/*  SYNOPSIS */
        int fd)

/*  FUNCTION

    AROS DOS does not provide a data-only flush primitive. Flush the
    descriptor through fsync(), which already preserves the required
    descriptor validation and directory-lock handling.

    INPUTS

    RESULT

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

******************************************************************************/
{
    return fsync(fd);
} /* fdatasync */
