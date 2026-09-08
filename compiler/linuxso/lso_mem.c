/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    linuxso mmap(2) family on exec memory.

    Ordinary anonymous mappings come from AllocMem (page aligned) and
    private file mappings are read in with pread; for those, protection
    changes are accepted and ignored (AROS memory is RWX and the guest
    only uses mprotect for W^X hygiene).

    Address-space reservations are different: a guest allocator such as
    PartitionAlloc reserves tens of GiB PROT_NONE up front and commits
    small pieces later with mprotect(PROT_READ|PROT_WRITE), decommitting
    them again with madvise(MADV_DONTNEED).  No amount of exec memory
    satisfies that literally, so a reservation is a window that is backed
    on demand:

      hosted  - the host kernel does it; the window is a real host mmap
                and mprotect/madvise/munmap on it go to the host too.
      native  - the window lives above the kernel's identity map
                (>= LSO_WINDOW_BASE, nothing else maps there) and each
                2 MiB chunk is backed the first time it is committed,
                with physical memory from AllocMem mapped in through
                KrnMapGlobal.  Decommit zeroes the chunk (the MADV_DONTNEED
                contract) but keeps it mapped: unmapping would need a TLB
                shootdown on the other CPUs, and the address space is
                reused by the allocator soon anyway.

    Windows are handed out from a cursor shared by every process on the
    machine (a named public semaphore), so no window is ever given out
    twice per boot: a stale TLB entry left on another CPU by a finished
    process can then never alias a live window.  At process exit the
    windows still held are unmapped and their backing chunks returned.
*/

#include <proto/exec.h>
#include <proto/kernel.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <aros/symbolsets.h>

#define __HOSTLIB_NOLIBBASE__
#define __HOSTLIB_LIBBASE HostLibBase
#include <proto/hostlib.h>

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
#define LX_MADV_DONTNEED 4

/* anonymous PROT_NONE mappings at least this big are reservations */
#define LSO_RESERVE_MIN   (64UL << 20)

/* native windows: 2 MiB chunks (KrnMapGlobal's page size) carved from
   arenas so that every chunk is physically 2 MiB aligned */
#define LSO_CHUNK_SHIFT   21
#define LSO_CHUNK         (1UL << LSO_CHUNK_SHIFT)
#define LSO_ARENA_CHUNKS  16
#define LSO_WINDOW_BASE   (2UL << 40)     /* 2 TiB: above the identity map and 64-bit BARs */
#define LSO_WINDOW_LIMIT  (64UL << 40)    /* 64 TiB: stay well inside canonical space */

struct lso_mapping {
    struct MinNode node;
    APTR alloc;
    size_t allocsize;
    uintptr_t start;
    size_t len;
};

struct lso_reserve {
    struct MinNode node;
    uintptr_t start;        /* current extent, trimmed by partial munmap */
    size_t len;
    uintptr_t base;         /* the window as handed out (native) */
    size_t span;
    int hosted;
    APTR *chunks;           /* native: backing of chunk i of the window, NULL = uncommitted */
    unsigned nchunks;
};

/* one per arena of chunks, so they can be given back at exit */
struct lso_arena {
    struct MinNode node;
    APTR raw;
    size_t size;
};

/* the machine-wide window cursor, a named public semaphore */
#define LSO_WINDOWS_NAME "linuxso.windows"
struct lso_windows {
    struct SignalSemaphore sem;
    char name[sizeof(LSO_WINDOWS_NAME)];
    uintptr_t next;
};

static struct MinList mappings;
static struct MinList reserves;
static struct MinList arenas;
static struct SignalSemaphore map_lock;
static int map_ready;

static struct lso_windows *windows;

/* host mmap family (hosted only) */
static APTR HostLibBase;
static void *host_libc;
static struct {
    void *(*mmap)(void *, size_t, int, int, int, long);
    int (*munmap)(void *, size_t);
    int (*mprotect)(void *, size_t, int);
    int (*madvise)(void *, size_t, int);
} host;

/* free chunk pool (native) */
static APTR *chunk_free;
static unsigned chunk_free_n, chunk_free_cap;
static APTR mem_KernelBase;

static void map_init(void)
{
    Forbid();
    if (!map_ready)
    {
        NEWLIST(&mappings);
        NEWLIST(&reserves);
        NEWLIST(&arenas);
        InitSemaphore(&map_lock);
        map_ready = 1;
    }
    Permit();
}

/* the shared window cursor, created by the first process that needs it */
static struct lso_windows *windows_get(void)
{
    struct lso_windows *w;

    if (windows)
        return windows;
    Forbid();
    w = (struct lso_windows *)FindSemaphore(LSO_WINDOWS_NAME);
    if (!w)
    {
        /* outlives this process, so the name lives inside it */
        w = AllocMem(sizeof(*w), MEMF_PUBLIC | MEMF_CLEAR);
        if (w)
        {
            strcpy(w->name, LSO_WINDOWS_NAME);
            w->sem.ss_Link.ln_Name = w->name;
            w->next = LSO_WINDOW_BASE;
            AddSemaphore(&w->sem);
        }
    }
    Permit();
    windows = w;
    return w;
}

/* ---- hosted: host mmap through hostlib.resource --------------------------- */

static int host_init(void)
{
    char *err = NULL;

    if (host.mmap)
        return 1;
    if (!HostLibBase)
        HostLibBase = OpenResource("hostlib.resource");
    if (!HostLibBase)
        return 0;
    host_libc = HostLib_Open("libc.so.6", &err);
    if (!host_libc)
    {
        LSO_DEBUG("HostLib_Open(libc.so.6) failed: %s", err ? err : "?");
        if (err)
            HostLib_FreeErrorStr(err);
        return 0;
    }
    host.munmap = HostLib_GetPointer(host_libc, "munmap", NULL);
    host.mprotect = HostLib_GetPointer(host_libc, "mprotect", NULL);
    host.madvise = HostLib_GetPointer(host_libc, "madvise", NULL);
    host.mmap = HostLib_GetPointer(host_libc, "mmap", NULL);
    if (!host.munmap || !host.mprotect || !host.madvise || !host.mmap)
    {
        LSO_DEBUG("host libc lacks the mmap family");
        host.mmap = NULL;
        return 0;
    }
    return 1;
}

/* ---- native: on-demand backed windows ------------------------------------- */

static APTR chunk_alloc(void)
{
    APTR chunk = NULL;

    Forbid();
    if (chunk_free_n)
        chunk = chunk_free[--chunk_free_n];
    Permit();
    if (chunk)
        return chunk;

    /* a new arena: LSO_ARENA_CHUNKS aligned chunks plus alignment slack;
       the first aligned chunk is returned, the rest go to the pool */
    {
        size_t size = (LSO_ARENA_CHUNKS + 1) * LSO_CHUNK;
        APTR raw = AllocMem(size, MEMF_ANY | MEMF_CLEAR);
        struct lso_arena *arena;
        uintptr_t first, i;

        if (!raw)
            return NULL;
        arena = AllocMem(sizeof(*arena), MEMF_ANY);
        if (!arena)
        {
            FreeMem(raw, size);
            return NULL;
        }
        arena->raw = raw;
        arena->size = size;
        first = ((uintptr_t)raw + LSO_CHUNK - 1) & ~(LSO_CHUNK - 1);

        Forbid();
        if (chunk_free_n + LSO_ARENA_CHUNKS - 1 > chunk_free_cap)
        {
            unsigned cap = chunk_free_cap ? chunk_free_cap * 2 : 64;
            APTR *n;
            while (cap < chunk_free_n + LSO_ARENA_CHUNKS - 1)
                cap *= 2;
            n = AllocMem(cap * sizeof(APTR), MEMF_ANY);
            if (!n)
            {
                Permit();
                FreeMem(arena, sizeof(*arena));
                FreeMem(raw, size);
                return NULL;
            }
            if (chunk_free)
            {
                memcpy(n, chunk_free, chunk_free_n * sizeof(APTR));
                FreeMem(chunk_free, chunk_free_cap * sizeof(APTR));
            }
            chunk_free = n;
            chunk_free_cap = cap;
        }
        for (i = 1; i < LSO_ARENA_CHUNKS; i++)
            chunk_free[chunk_free_n++] = (APTR)(first + i * LSO_CHUNK);
        AddTail((struct List *)&arenas, (struct Node *)&arena->node);
        Permit();
        LSO_DEBUG("chunk arena %p (+%lu MiB)", raw, (unsigned long)(size >> 20));
        return (APTR)first;
    }
}

static void chunk_release(APTR chunk)
{
    /* the chunk stays mapped in its window; it only returns to the pool
       when the whole window goes away, and then it is zeroed first */
    memset(chunk, 0, LSO_CHUNK);
    Forbid();
    if (chunk_free_n < chunk_free_cap)
        chunk_free[chunk_free_n++] = chunk;
    Permit();
}

/* back every chunk of the window touching [addr, addr+len) */
static int window_commit(struct lso_reserve *r, uintptr_t addr, size_t len)
{
    uintptr_t first = (addr - r->base) >> LSO_CHUNK_SHIFT;
    uintptr_t last = (addr + len - 1 - r->base) >> LSO_CHUNK_SHIFT;
    uintptr_t i;
    APTR KernelBase;

    if (!mem_KernelBase)
        mem_KernelBase = OpenResource("kernel.resource");
    KernelBase = mem_KernelBase;
    if (!KernelBase)
        return -1;

    for (i = first; i <= last && i < r->nchunks; i++)
    {
        APTR chunk, phys;
        void *virt = (void *)(r->base + (i << LSO_CHUNK_SHIFT));

        if (r->chunks[i])
            continue;
        chunk = chunk_alloc();
        if (!chunk)
        {
            LSO_DEBUG("window %p: no memory for chunk %lu", (void *)r->base, (unsigned long)i);
            return -1;
        }
        phys = KrnVirtualToPhysical(chunk);
        if (KrnMapGlobal(virt, phys, LSO_CHUNK, MAP_Readable | MAP_Writable | MAP_Executable) != 0)
        {
            LSO_DEBUG("window %p: KrnMapGlobal(%p <- %p) failed", (void *)r->base, virt, phys);
            chunk_release(chunk);
            return -1;
        }
        r->chunks[i] = chunk;
    }
    return 0;
}

/* MADV_DONTNEED: the pages read as zero afterwards */
static void window_discard(struct lso_reserve *r, uintptr_t addr, size_t len)
{
    uintptr_t end = addr + len, cur = addr;

    while (cur < end)
    {
        uintptr_t i = (cur - r->base) >> LSO_CHUNK_SHIFT;
        uintptr_t cend = r->base + ((i + 1) << LSO_CHUNK_SHIFT);
        size_t n = (cend < end ? cend : end) - cur;

        if (i < r->nchunks && r->chunks[i])
            memset((void *)cur, 0, n);
        cur += n;
    }
}

static void *reserve_alloc(size_t len, int prot, int flags)
{
    struct lso_reserve *r = AllocMem(sizeof(*r), MEMF_ANY | MEMF_CLEAR);

    if (!r)
    {
        lso_set_errno(LX_ENOMEM);
        return LX_MAP_FAILED;
    }

    if (host_init())
    {
        void *p;

        HostLib_Lock();
        p = host.mmap(NULL, len, prot, flags & ~LX_MAP_FIXED, -1, 0);
        HostLib_Unlock();
        if (p == LX_MAP_FAILED)
        {
            FreeMem(r, sizeof(*r));
            LSO_DEBUG("host mmap(%lx) failed", (unsigned long)len);
            lso_set_errno(LX_ENOMEM);
            return LX_MAP_FAILED;
        }
        r->hosted = 1;
        r->start = r->base = (uintptr_t)p;
        r->len = r->span = len;
        LSO_DEBUG("reserve(%lx) -> host %p", (unsigned long)len, p);
    }
    else
    {
        /* hand out windows aligned to their own size (up to 64 GiB) so an
           allocator's aligned-reservation dance succeeds first time */
        size_t span = (len + LSO_CHUNK - 1) & ~(LSO_CHUNK - 1);
        size_t align = LSO_CHUNK;
        struct lso_windows *w = windows_get();
        uintptr_t base;

        if (!w)
        {
            FreeMem(r, sizeof(*r));
            lso_set_errno(LX_ENOMEM);
            return LX_MAP_FAILED;
        }
        while (align < span && align < (64UL << 30))
            align <<= 1;

        ObtainSemaphore(&w->sem);
        base = (w->next + align - 1) & ~(align - 1);
        if (base + span > LSO_WINDOW_LIMIT)
        {
            ReleaseSemaphore(&w->sem);
            FreeMem(r, sizeof(*r));
            LSO_DEBUG("reserve(%lx): window space exhausted", (unsigned long)len);
            lso_set_errno(LX_ENOMEM);
            return LX_MAP_FAILED;
        }
        w->next = base + span;
        ReleaseSemaphore(&w->sem);

        r->nchunks = span >> LSO_CHUNK_SHIFT;
        r->chunks = AllocMem(r->nchunks * sizeof(APTR), MEMF_ANY | MEMF_CLEAR);
        if (!r->chunks)
        {
            FreeMem(r, sizeof(*r));
            lso_set_errno(LX_ENOMEM);
            return LX_MAP_FAILED;
        }
        r->start = r->base = base;
        r->len = r->span = span;
        LSO_DEBUG("reserve(%lx) -> window %p (%u chunks)", (unsigned long)len, (void *)base, r->nchunks);

        if (prot != LX_PROT_NONE && window_commit(r, base, span) != 0)
        {
            FreeMem(r->chunks, r->nchunks * sizeof(APTR));
            FreeMem(r, sizeof(*r));
            lso_set_errno(LX_ENOMEM);
            return LX_MAP_FAILED;
        }
    }

    ObtainSemaphore(&map_lock);
    AddHead((struct List *)&reserves, (struct Node *)&r->node);
    ReleaseSemaphore(&map_lock);
    return (void *)r->start;
}

/* the reservation containing [addr, addr+len), or NULL; map_lock held */
static struct lso_reserve *reserve_find(uintptr_t addr, size_t len)
{
    struct lso_reserve *r;

    ForeachNode(&reserves, r)
        if (addr >= r->start && addr + len <= r->start + r->len)
            return r;
    return NULL;
}

static void reserve_free(struct lso_reserve *r)
{
    if (r->hosted)
    {
        HostLib_Lock();
        host.munmap((void *)r->start, r->len);
        HostLib_Unlock();
    }
    else
    {
        unsigned i;
        for (i = 0; i < r->nchunks; i++)
            if (r->chunks[i])
                chunk_release(r->chunks[i]);
        FreeMem(r->chunks, r->nchunks * sizeof(APTR));
    }
    FreeMem(r, sizeof(*r));
}

/* ---- mmap ----------------------------------------------------------------- */

void *lso_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off)
{
    struct lso_mapping *m;
    size_t rlen = (len + LSO_PAGE - 1) & ~(LSO_PAGE - 1);

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
        struct lso_reserve *r;
        int inside = 0;

        ObtainSemaphoreShared(&map_lock);
        r = reserve_find((uintptr_t)addr, rlen);
        ForeachNode(&mappings, n)
        {
            if ((uintptr_t)addr >= n->start && (uintptr_t)addr + rlen <= n->start + n->len)
            {
                inside = 1;
                break;
            }
        }
        ReleaseSemaphore(&map_lock);
        if (r && (flags & LX_MAP_ANON))
        {
            if (r->hosted)
            {
                void *p;
                HostLib_Lock();
                p = host.mmap(addr, rlen, prot, flags, -1, 0);
                HostLib_Unlock();
                LSO_DEBUG("mmap(FIXED %p, %lx) in host reservation -> %p", addr, (unsigned long)rlen, p);
                if (p == LX_MAP_FAILED)
                    lso_set_errno(LX_ENOMEM);
                return p;
            }
            if (prot != LX_PROT_NONE && window_commit(r, (uintptr_t)addr, rlen) != 0)
            {
                lso_set_errno(LX_ENOMEM);
                return LX_MAP_FAILED;
            }
            window_discard(r, (uintptr_t)addr, rlen);
            LSO_DEBUG("mmap(FIXED %p, %lx) in window", addr, (unsigned long)rlen);
            return addr;
        }
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
    if ((flags & LX_MAP_ANON) && prot == LX_PROT_NONE && rlen >= LSO_RESERVE_MIN)
        return reserve_alloc(rlen, prot, flags);

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
    struct lso_reserve *r, *rfree = NULL;
    uintptr_t a = (uintptr_t)addr;
    size_t rlen = (len + LSO_PAGE - 1) & ~(LSO_PAGE - 1);

    map_init();
    ObtainSemaphore(&map_lock);
    r = reserve_find(a, rlen);
    if (r)
    {
        if (a == r->start && rlen >= r->len)
        {
            Remove((struct Node *)&r->node);
            rfree = r;
        }
        else if (a == r->start)
        {
            /* head trim (an allocator aligning its reservation) */
            r->start += rlen;
            r->len -= rlen;
        }
        else if (a + rlen == r->start + r->len)
            r->len -= rlen;       /* tail trim */
        /* a hole in the middle: the range stays reserved, nothing lost */
        if (!rfree && r->hosted)
        {
            HostLib_Lock();
            host.munmap(addr, rlen);
            HostLib_Unlock();
        }
        ReleaseSemaphore(&map_lock);
        LSO_DEBUG("munmap(%p, %lx) %s reservation", addr, (unsigned long)rlen, rfree ? "frees" : "trims");
        if (rfree)
            reserve_free(rfree);
        return 0;
    }
    ForeachNode(&mappings, m)
    {
        if (m->start == a)
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
    struct lso_reserve *r;
    int ret = 0;

    map_init();
    ObtainSemaphore(&map_lock);
    r = reserve_find((uintptr_t)addr, len);
    if (r)
    {
        if (r->hosted)
        {
            HostLib_Lock();
            ret = host.mprotect(addr, len, prot);
            HostLib_Unlock();
        }
        else if (prot != LX_PROT_NONE)
            ret = window_commit(r, (uintptr_t)addr, len);
    }
    ReleaseSemaphore(&map_lock);
    if (ret != 0)
    {
        LSO_DEBUG("mprotect(%p, %lx, %x) failed", addr, (unsigned long)len, prot);
        lso_set_errno(LX_ENOMEM);
        return -1;
    }
    return 0;
}

int lx_madvise(void *addr, size_t len, int advice)
{
    struct lso_reserve *r;

    map_init();
    ObtainSemaphore(&map_lock);
    r = reserve_find((uintptr_t)addr, len);
    if (r)
    {
        if (r->hosted)
        {
            HostLib_Lock();
            host.madvise(addr, len, advice);
            HostLib_Unlock();
        }
        else if (advice == LX_MADV_DONTNEED)
            window_discard(r, (uintptr_t)addr, len);
    }
    ReleaseSemaphore(&map_lock);
    return 0;
}

/* ---- process exit --------------------------------------------------------- */

/*
 * Whatever the guest still holds goes back: windows are unmapped (the
 * present bit cleared; the address range is never handed out again this
 * boot, so no shootdown is needed) before their arenas return to exec.
 */
static void lso_mem_exit(void)
{
    struct lso_reserve *r, *r2;
    struct lso_mapping *m, *m2;
    struct lso_arena *a, *a2;
    APTR KernelBase = mem_KernelBase;

    if (!map_ready)
        return;

    ForeachNodeSafe(&reserves, r, r2)
    {
        if (!r->hosted && KernelBase)
        {
            unsigned i;
            for (i = 0; i < r->nchunks; i++)
                if (r->chunks[i])
                    KrnMapGlobal((void *)(r->base + (i << LSO_CHUNK_SHIFT)), NULL, LSO_CHUNK, 0);
        }
        Remove((struct Node *)&r->node);
        reserve_free(r);
    }
    ForeachNodeSafe(&mappings, m, m2)
    {
        Remove((struct Node *)&m->node);
        if (m->alloc)
            FreeMem(m->alloc, m->allocsize);
        FreeMem(m, sizeof(*m));
    }
    ForeachNodeSafe(&arenas, a, a2)
    {
        Remove((struct Node *)&a->node);
        FreeMem(a->raw, a->size);
        FreeMem(a, sizeof(*a));
    }
    if (chunk_free)
        FreeMem(chunk_free, chunk_free_cap * sizeof(APTR));
    chunk_free = NULL;
    chunk_free_n = chunk_free_cap = 0;
}

ADD2EXIT(lso_mem_exit, 0);
