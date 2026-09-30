/*
    Copyright (C) 2004-2026, The AROS Development Team. All rights reserved.

    POSIX.1-2008 function times().
*/

#include <errno.h>
#include <time.h>

/*****************************************************************************

    NAME */

#include <sys/times.h>

        clock_t times(

/*  SYNOPSIS */
        struct tms *tms)

/*  FUNCTION

    INPUTS

    RESULT

    NOTES
        The result counts sysconf(_SC_CLK_TCK) (CLOCKS_PER_SEC) units of
        monotonic time. AROS keeps no user/system split and has no child
        accounting: tms_utime is clock(), the other three fields are 0.

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

******************************************************************************/
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return (clock_t)-1;

    if (tms)
    {
        tms->tms_utime  = clock();
        tms->tms_stime  = 0;
        tms->tms_cutime = 0;
        tms->tms_cstime = 0;
    }

    return (clock_t)now.tv_sec * CLOCKS_PER_SEC +
           (clock_t)now.tv_nsec / (1000000000 / CLOCKS_PER_SEC);
}
