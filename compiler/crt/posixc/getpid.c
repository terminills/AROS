/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    POSIX.1-2008 function getpid().
*/

#include <proto/exec.h>
#include <assert.h>

#include "__vfork.h"
#include "__posixc_intbase.h"

/*****************************************************************************

    NAME */
#include <sys/types.h>
#include <unistd.h>

        pid_t getpid (

/*  SYNOPSIS */
        )

/*  FUNCTION
        Returns the process ID of the calling process

    RESULT
        The process ID of the calling process.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

******************************************************************************/
{
    struct vfork_data *udata = __posixc_pretending();
    struct ETask *et;

    if (udata)
        et = GetETask(udata->child);
    else
        et = GetETask(FindTask(NULL));
    assert(et);
    return (pid_t) et->et_UniqueID;
} /* getpid */
