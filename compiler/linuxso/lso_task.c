/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    linuxso per-task state: glibc-numbered errno, the guest's TLS blocks
    (__tls_get_addr) and the errno/return-code translation tables.
*/

#include <proto/exec.h>
/* keep the resource base private to the linklib (no exported TaskResBase) */
#define TaskResBase lso_TaskResBase
static APTR lso_TaskResBase;
#include <proto/task.h>
#include <exec/memory.h>

#include <errno.h>
#include <string.h>

#include "linuxso_intern.h"
#include "lso_lx.h"

static LONG lso_slot = -1;

static void slot_init(void)
{
    Forbid();
    if (lso_slot < 0)
    {
        if (!TaskResBase)
            TaskResBase = OpenResource("task.resource");
        if (TaskResBase)
            lso_slot = AllocTaskStorageSlot();
    }
    Permit();
}

struct lso_task *lso_task_get(void)
{
    struct lso_task *lt;

    if (lso_slot < 0)
        slot_init();
    if (lso_slot < 0)
        return NULL;
    lt = (struct lso_task *)GetTaskStorageSlot(lso_slot);
    if (!lt)
    {
        lt = AllocMem(sizeof(*lt), MEMF_ANY | MEMF_CLEAR);
        if (!lt)
            return NULL;
        lt->task = FindTask(NULL);
        SetTaskStorageSlot(lso_slot, (IPTR)lt);
        LSO_DEBUG("task %p '%s': shim context %p", lt->task, lt->task->tc_Node.ln_Name, lt);
    }
    return lt;
}

void lso_tls_release(struct lso_task *lt)
{
    int i;

    for (i = 0; i < LSO_MAX_TLS_MODULES; i++)
    {
        if (lt->tls[i])
        {
            FreeVec(lt->tls[i]);
            lt->tls[i] = NULL;
        }
    }
}

/* Called on the exiting task itself (pthread start trampoline). */
void lso_task_free(struct Task *task)
{
    struct lso_task *lt;

    if (lso_slot < 0)
        return;
    lt = (struct lso_task *)GetTaskStorageSlot(lso_slot);
    if (!lt || lt->task != task)
        return;
    SetTaskStorageSlot(lso_slot, 0);
    lso_tls_release(lt);
    FreeMem(lt, sizeof(*lt));
}

/* ---- errno ---------------------------------------------------------------- */

int lso_errno_to_linux(int e)
{
    switch (e)
    {
    case 0:          return 0;
    case EPERM:      return LX_EPERM;
    case ENOENT:     return LX_ENOENT;
    case ESRCH:      return LX_ESRCH;
    case EINTR:      return LX_EINTR;
    case EIO:        return LX_EIO;
    case EBADF:      return LX_EBADF;
    case EAGAIN:     return LX_EAGAIN;
    case ENOMEM:     return LX_ENOMEM;
    case EACCES:     return LX_EACCES;
    case EFAULT:     return LX_EFAULT;
    case EBUSY:      return LX_EBUSY;
    case EEXIST:     return LX_EEXIST;
    case ENOTDIR:    return LX_ENOTDIR;
    case EISDIR:     return LX_EISDIR;
    case EINVAL:     return LX_EINVAL;
    case ENFILE:     return LX_ENFILE;
    case EMFILE:     return LX_EMFILE;
    case ENOSPC:     return LX_ENOSPC;
    case ESPIPE:     return LX_ESPIPE;
    case EPIPE:      return LX_EPIPE;
    case ERANGE:     return LX_ERANGE;
    case EDEADLK:    return LX_EDEADLK;
    case ENOSYS:     return LX_ENOSYS;
    case ENOTSUP:    return LX_ENOTSUP;
    case ETIMEDOUT:  return LX_ETIMEDOUT;
    default:         return LX_EIO;
    }
}

void lso_set_errno(int lx)
{
    struct lso_task *lt = lso_task_get();
    if (lt)
        lt->lx_errno = lx;
}

void lso_set_errno_from_aros(void)
{
    lso_set_errno(lso_errno_to_linux(errno));
}

int lso_pthread_ret(int r)
{
    return r ? lso_errno_to_linux(r) : 0;
}

int *lso_errno_location(void)
{
    static int fallback;
    struct lso_task *lt = lso_task_get();
    return lt ? &lt->lx_errno : &fallback;
}

/* ---- TLS ------------------------------------------------------------------ */

struct lso_tls_index {
    unsigned long module;
    unsigned long offset;
};

static struct LinuxSO *module_by_id(unsigned int id)
{
    struct LinuxSO *so, *found = NULL;

    ObtainSemaphoreShared(&lso_lock);
    ForeachNode(&lso_objects, so)
    {
        if (so->tls_modid == id)
        {
            found = so;
            break;
        }
    }
    ReleaseSemaphore(&lso_lock);
    return found;
}

void *lso_tls_get_addr(struct lso_tls_index *ti)
{
    struct lso_task *lt = lso_task_get();
    unsigned int id = (unsigned int)ti->module;
    void *block;

    if (!lt || id == 0 || id > LSO_MAX_TLS_MODULES)
        return NULL;
    block = lt->tls[id - 1];
    if (!block)
    {
        struct LinuxSO *so = module_by_id(id);
        size_t align, size;
        char *raw;

        if (!so)
            return NULL;
        align = so->tls_align < 16 ? 16 : so->tls_align;
        size = so->tls_memsz + align;
        raw = AllocVec(size, MEMF_ANY | MEMF_CLEAR);
        if (!raw)
            return NULL;
        block = (void *)(((uintptr_t)raw + align - 1) & ~(align - 1));
        memcpy(block, so->tls_image, so->tls_filesz);
        /* keep the raw pointer for FreeVec: the aligned block never moves
           more than align-1 bytes, and AllocVec's own header sits below raw */
        lt->tls[id - 1] = raw;
        LSO_DEBUG("task %p: TLS block for module %u at %p (%lu bytes)",
                  lt->task, id, block, (unsigned long)so->tls_memsz);
        return (char *)block + ti->offset;
    }
    {
        struct LinuxSO *so = module_by_id(id);
        size_t align = (so && so->tls_align > 16) ? so->tls_align : 16;
        return (char *)(((uintptr_t)block + align - 1) & ~(align - 1)) + ti->offset;
    }
}
