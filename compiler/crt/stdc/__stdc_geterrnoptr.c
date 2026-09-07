/*
    Copyright (C) 2012-2026, The AROS Development Team. All rights reserved.

    Get pointer to the calling Task's errno variable.
    In stdc.library this is a library function (the per-Task logic needs
    the library's private state); the static linklib has its own copy.
*/

#ifndef STDC_STATIC
#include <proto/exec.h>

#include "__stdc_intbase.h"

int *__stdc_taskerrnoptr(struct StdCIntBase *StdCBase);
#endif

int *__stdc_geterrnoptr(void)
{
#ifdef STDC_STATIC
    static int static_errno;

    return &static_errno;
#else
    struct StdCIntBase *StdCBase = (struct StdCIntBase *)__aros_getbase_StdCBase();

    /* The base's errno belongs to the Task that opened the base.  Another
       Task calling through it (a pthread worker of an absolutely linked
       program, see __stdc_errno.c) must not share it. */
    if (StdCBase->errno_task == FindTask(NULL))
        return &StdCBase->StdCBase._errno;

    return __stdc_taskerrnoptr(StdCBase);
#endif
}
