/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    C99 function malloc().
*/

#include "__stdc_intbase.h"

#include <errno.h>
#include <stdint.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <aros/symbolsets.h>
#include <aros/debug.h>

#include "debug.h"

/*****************************************************************************

    NAME */
#include <stdlib.h>

        void *malloc (

/*  SYNOPSIS */
        size_t size)

/*  FUNCTION
        Allocate size bytes of memory and return the address of the
        first byte.

    INPUTS
        size - How much memory to allocate.

    RESULT
        A pointer to the allocated memory or NULL. If you don't need the
        memory anymore, you can pass this pointer to free(). If you don't,
        the memory will be freed for you when the application exits.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        free()

    INTERNALS

******************************************************************************/
{
    struct StdCIntBase *StdCBase = (struct StdCIntBase *)__aros_getbase_StdCBase();
    UBYTE *mem = NULL;

    /* Guard against (size + header) wrapping size_t, which would otherwise
       under-allocate the block and let the caller overrun it. */
    if (size > SIZE_MAX - AROS_ALIGN(sizeof(size_t)))
    {
        errno = ENOMEM;
        return NULL;
    }

    /* Allocate the memory */
    mem = AllocPooled (StdCBase->mempool, size + AROS_ALIGN(sizeof(size_t)));
    if (mem)
    {
        *((size_t *)mem) = size;
        mem += AROS_ALIGN(sizeof(size_t));
    }
    else
        errno = ENOMEM;

    return mem;

} /* malloc */


/*
 * ONE malloc pool shared by every stdc opener in the address space.
 *
 * stdc.library hands each opener its own base, and pthread_create()
 * spawns each POSIX thread as a separate AROS Process (its own base).
 * A per-base pool therefore made malloc() and free() disagree whenever
 * a block allocated on one thread was freed on another — normal for
 * Chromium (PartitionAlloc/cppgc/Skia allocate on one thread, free on
 * another) — and FreePooled() then hit a pool the block never came
 * from ("FREE outside TLSF area", signal 6). POSIX malloc/free is
 * process-global; a single shared pool restores that. The pool is
 * MEMF_SEM_PROTECTED so concurrent alloc/free across Processes is
 * safe; the refcount+create/delete are guarded by stdc_pool_sem,
 * initialised once at library load (before any open). The pool
 * outlives individual openers and is deleted only when the last one
 * closes — so a Process may still free memory another Process
 * allocated, which is exactly the contract Chromium relies on.
 */
static struct SignalSemaphore stdc_pool_sem;
static APTR                   stdc_shared_pool = NULL;
static LONG                   stdc_pool_refs   = 0;

static int __init_poolguard(struct StdCIntBase *StdCBase)
{
    (void)StdCBase;
    InitSemaphore(&stdc_pool_sem);
    return 1;
}

int __init_memstuff(struct StdCIntBase *StdCBase)
{
    D(bug("[%s] %s: task(0x%p), StdCBase(0x%p)\n", STDCNAME, __func__,
          FindTask(NULL), StdCBase
    ));

    ObtainSemaphore(&stdc_pool_sem);
    if (stdc_pool_refs == 0)
    {
        stdc_shared_pool =
            CreatePool(MEMF_ANY | MEMF_SEM_PROTECTED, 65536L, 4096L);
        if (!stdc_shared_pool)
        {
            ReleaseSemaphore(&stdc_pool_sem);
            return 0;
        }
    }
    stdc_pool_refs++;
    StdCBase->mempool = stdc_shared_pool;
    ReleaseSemaphore(&stdc_pool_sem);

    D(bug("[%s] %s: shared mempool(0x%p) refs=%ld\n", STDCNAME, __func__,
          StdCBase->mempool, (long)stdc_pool_refs));

    return 1;
}


void __exit_memstuff(struct StdCIntBase *StdCBase)
{
    D(bug("[%s] %s: task(0x%p), StdCBase(0x%p), mempool(0x%p)\n", STDCNAME,
          __func__, FindTask(NULL), StdCBase, StdCBase->mempool
    ));

    ObtainSemaphore(&stdc_pool_sem);
    StdCBase->mempool = NULL;
    if (stdc_pool_refs > 0 && --stdc_pool_refs == 0 && stdc_shared_pool)
    {
        DeletePool(stdc_shared_pool);
        stdc_shared_pool = NULL;
    }
    ReleaseSemaphore(&stdc_pool_sem);
}

ADD2INITLIB(__init_poolguard, 0);
ADD2OPENLIB(__init_memstuff, 0);
ADD2CLOSELIB(__exit_memstuff, 0);
