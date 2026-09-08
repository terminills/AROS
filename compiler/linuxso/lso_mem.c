/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    linuxso mmap(2) family on exec memory.  Anonymous mappings come from
    AllocMem (page aligned); private file mappings are read in with
    pread.  Protection changes are accepted and ignored: AROS memory is
    RWX and the guest only uses mprotect for W^X hygiene.
*/

#include <proto/exec.h>
#include <exec/memory.h>
#include <exec/semaphores.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "linuxso_intern.h"
#include "lso_lx.h"

#define LX_PROT_NONE    0
#define LX_MAP_SHARED   0x01
#define LX_MAP_PRIVATE  0x02
#define LX_MAP_FIXED    0x10
#define LX_MAP_ANON     0x20
#define LX_MAP_FAILED   ((void *)-1)

struct lso_mapping {
    struct MinNode node;
    APTR alloc;
    size_t allocsize;
    uintptr_t start;
    size_t len;
};

static struct MinList mappings;
static struct SignalSemaphore map_lock;
static int map_ready;

static void map_init(void)
{
    Forbid();
    if (!map_ready)
    {
        NEWLIST(&mappings);
        InitSemaphore(&map_lock);
        map_ready = 1;
    }
    Permit();
}

void *lso_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off)
{
    struct lso_mapping *m;
    size_t rlen = (len + LSO_PAGE - 1) & ~(LSO_PAGE - 1);

    (void)prot;
    map_init();
    if (!len)
    {
        lso_set_errno(LX_EINVAL);
        return LX_MAP_FAILED;
    }
    if ((flags & LX_MAP_FIXED) && addr)
    {
        /* MAP_FIXED over our own anonymous mapping is what a guest allocator
           does to commit/decommit; the memory is there already */
        struct lso_mapping *n;
        int inside = 0;

        ObtainSemaphoreShared(&map_lock);
        ForeachNode(&mappings, n)
        {
            if ((uintptr_t)addr >= n->start && (uintptr_t)addr + rlen <= n->start + n->len)
            {
                inside = 1;
                break;
            }
        }
        ReleaseSemaphore(&map_lock);
        if (inside && (flags & LX_MAP_ANON))
        {
            memset(addr, 0, rlen);
            LSO_DEBUG("mmap(FIXED %p, %lx) inside existing mapping", addr, (unsigned long)rlen);
            return addr;
        }
        LSO_DEBUG("mmap(FIXED %p, %lx) refused", addr, (unsigned long)rlen);
        lso_set_errno(LX_EINVAL);
        return LX_MAP_FAILED;
    }
    if (!(flags & LX_MAP_ANON) && (flags & LX_MAP_SHARED))
    {
        LSO_DEBUG("mmap(fd %d, MAP_SHARED) unsupported", fd);
        lso_set_errno(LX_ENOTSUP);
        return LX_MAP_FAILED;
    }

    m = AllocMem(sizeof(*m), MEMF_ANY | MEMF_CLEAR);
    if (!m)
    {
        lso_set_errno(LX_ENOMEM);
        return LX_MAP_FAILED;
    }
    m->allocsize = rlen + LSO_PAGE;
    m->alloc = AllocMem(m->allocsize, MEMF_ANY | MEMF_CLEAR);
    if (!m->alloc)
    {
        FreeMem(m, sizeof(*m));
        LSO_DEBUG("mmap(%lx) out of memory", (unsigned long)rlen);
        lso_set_errno(LX_ENOMEM);
        return LX_MAP_FAILED;
    }
    m->start = ((uintptr_t)m->alloc + LSO_PAGE - 1) & ~(LSO_PAGE - 1);
    m->len = rlen;

    if (!(flags & LX_MAP_ANON))
    {
        size_t done = 0;
        while (done < len)
        {
            ssize_t n = pread(fd, (char *)m->start + done, len - done, (off_t)(off + done));
            if (n < 0)
            {
                lso_set_errno_from_aros();
                FreeMem(m->alloc, m->allocsize);
                FreeMem(m, sizeof(*m));
                return LX_MAP_FAILED;
            }
            if (n == 0)
                break;
            done += n;
        }
        LSO_DEBUG("mmap(fd %d, off %lx, %lx) -> %p (%lu bytes read)", fd, (unsigned long)off,
                  (unsigned long)rlen, (void *)m->start, (unsigned long)done);
    }
    else
        LSO_DEBUG("mmap(anon %lx, prot %x, flags %x) -> %p", (unsigned long)rlen, prot, flags, (void *)m->start);

    ObtainSemaphore(&map_lock);
    AddHead((struct List *)&mappings, (struct Node *)&m->node);
    ReleaseSemaphore(&map_lock);
    return (void *)m->start;
}

int lso_munmap(void *addr, size_t len)
{
    struct lso_mapping *m, *found = NULL;

    map_init();
    ObtainSemaphore(&map_lock);
    ForeachNode(&mappings, m)
    {
        if (m->start == (uintptr_t)addr)
        {
            found = m;
            break;
        }
    }
    if (found)
    {
        if (len < found->len)
        {
            /* partial unmap at the front: keep the block, nothing to give back */
            LSO_DEBUG("munmap(%p, %lx) partial of %lx: kept", addr, (unsigned long)len, (unsigned long)found->len);
            found = NULL;
        }
        else
            Remove((struct Node *)&found->node);
    }
    ReleaseSemaphore(&map_lock);

    if (found)
    {
        LSO_DEBUG("munmap(%p, %lx)", addr, (unsigned long)len);
        FreeMem(found->alloc, found->allocsize);
        FreeMem(found, sizeof(*found));
        return 0;
    }
    /* unmapping a tail or the middle of a mapping: accepted, memory stays */
    return 0;
}

void *lx_mmap64(void *addr, size_t len, int prot, int flags, int fd, int64_t off)
{
    return lso_mmap(addr, len, prot, flags, fd, off);
}

int lx_munmap(void *addr, size_t len)
{
    return lso_munmap(addr, len);
}

int lx_mprotect(void *addr, size_t len, int prot)
{
    (void)addr; (void)len; (void)prot;
    return 0;
}

int lx_madvise(void *addr, size_t len, int advice)
{
    (void)addr; (void)len; (void)advice;
    return 0;
}
