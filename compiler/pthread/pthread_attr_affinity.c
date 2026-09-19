/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: pthread_attr_{set,get}affinity_np() - place a thread on a CPU set.
*/

#include <errno.h>

#include "pthread.h"

/*
 * Spelled as glibc spells it so callers need no AROS-specific code. cpusetsize
 * exists for source compatibility and is not used: AROS cpumask_t is an opaque
 * object from KrnAllocCPUMask(), not a fixed-width bitmap the caller sizes.
 *
 * See the header for why this is needed at all - AROS places tasks explicitly
 * and does not migrate them, so a thread with no affinity of its own stays on
 * whichever core created it.
 */
int pthread_attr_setaffinity_np(pthread_attr_t *attr, size_t cpusetsize, const void *cpuset)
{
    (void)cpusetsize;

    if (!attr)
        return EINVAL;

    attr->cpuaffinity = (void *)cpuset;

    return 0;
}

int pthread_attr_getaffinity_np(const pthread_attr_t *attr, size_t cpusetsize, void *cpuset)
{
    (void)cpusetsize;

    if (!attr || !cpuset)
        return EINVAL;

    *(void **)cpuset = attr->cpuaffinity;

    return 0;
}
