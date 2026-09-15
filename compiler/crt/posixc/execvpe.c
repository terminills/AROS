/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    GNU/BSD function execvpe().
*/

#include <aros/debug.h>

#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include "__exec.h"

/*****************************************************************************

    NAME */
#include <unistd.h>

        int execvpe(

/*  SYNOPSIS */
        const char *file,
        char *const argv[],
        char *const envp[])

/*  FUNCTION
        Executes a file with given name, searched for like execvp() does,
        with the environment given in envp like execve() does.

    INPUTS
        file - Name of the file to execute.  A name without a directory
            separator is looked up along the caller's PATH, as with
            execvp(); the PATH in envp is for the executed file only.
        argv - Array of arguments given to main() function of the executed
            file.
        envp - Array of "NAME=value" strings making up the environment of
            the executed file.  NULL leaves the caller's environment in
            place, as execvp() would.

    RESULT
        Returns -1 and sets errno appropriately in case of error, otherwise
        doesn't return.

    NOTES
        A library that executes on behalf of several processes cannot use
        execvp() with a swapped-in environ: the environ emulation is bound
        to the stdcio base that registered it, not to the calling process.
        This function passes the environment explicitly instead.

    EXAMPLE

    BUGS

    SEE ALSO
        execve(), execvp()

    INTERNALS

******************************************************************************/
{
    APTR id = __exec_prepare(file, 1, argv, envp);
    if(!id)
        return -1;

    __exec_do(id);

    assert(0); /* not reached */
    return -1;
} /* execvpe() */
