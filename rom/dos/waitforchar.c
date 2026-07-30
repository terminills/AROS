/*
    Copyright (C) 1995-2014, The AROS Development Team. All rights reserved.

    Desc: Waits for a character to arrive at a filehandle.
*/
#include <proto/exec.h>
#include "dos_intern.h"

/*****************************************************************************

    NAME */
#include <proto/dos.h>
#include <exec/types.h>

        AROS_LH2(LONG, WaitForChar,

/*  SYNOPSIS */
        AROS_LHA(BPTR, file, D1),
        AROS_LHA(LONG, timeout, D2),

/*  LOCATION */
        struct DosLibrary *, DOSBase, 34, Dos)

/*  FUNCTION
        Wait for a character to arrive at a filehandle. The filehandle
        can be either a console handle, or a regular file. For a regular
        file most filesystems will return a character immediately, but
        sometimes (for example, a network handler) the character may not
        have arrived.

    INPUTS
        file    - File to wait for a character on.
        timeout - Number of microseconds to wait for the character
                  to arrive.

    RESULT
        != 0    if a character arrived before the timeout expired
        == 0    if no character arrived

    NOTES
        Many filesystems do not implement this function.

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct FileHandle *fh = (struct FileHandle *)BADDR(file);
    LONG status;

    /* NULL-pointers are okay. */
    if(file == BNULL)
        return 0;

    /*
     * Arg1 is the traditional timeout.  Pass the file-handle key as Arg2 as
     * well so handlers which multiplex several streams in one process (PIPE:
     * is the important example) can answer for the right stream.  Existing
     * handlers only inspect Arg1, so this is backward compatible.
     */
    status = dopacket2(DOSBase, NULL, fh->fh_Type, ACTION_WAIT_CHAR,
                       timeout, fh->fh_Arg1);
    return status;

    AROS_LIBFUNC_EXIT
} /* WaitForChar */
