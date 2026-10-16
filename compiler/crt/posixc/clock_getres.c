/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    GetSysTime() and GetUpTime(), used by clock_gettime(), expose time in
    whole microseconds. This reports their representation's resolution,
    not the precision with which a task can be scheduled or awakened.
*/

#include <time.h>

int clock_getres(clockid_t clk_id, struct timespec *res)
{
    struct timespec now;

    /* Keep clock availability and error handling identical to clock_gettime.
       A NULL result is permitted, but an unsupported clock is still an error. */
    if (clock_gettime(clk_id, &now) != 0)
        return -1;

    if (res)
    {
        res->tv_sec = 0;
        res->tv_nsec = 1000;
    }
    return 0;
}
