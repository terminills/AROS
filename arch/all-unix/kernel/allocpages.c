/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: Hosted (unix/darwin) implementation of KrnAllocPages().

    The generic rom/kernel/allocpages.c serves pages out of the MMU page
    database (mm_AllocPages). The hosted ports never populate that database,
    so the generic function is a silent no-op that returns NULL.

    On hosts without W^X (Linux and friends) pages are carved out of the
    exec.library MemHeaders instead, page-aligned, exactly like the native
    kernel serves them from mm_Init()ed headers. That keeps the pages
    visible to TypeOfMem(): the DOS debug builds validate every buffer
    handed to Read()/Write() with ASSERT_VALID_PTR, and a buffer that
    exec does not know about raises a Recoverable Alert on each call.
    Host mmap() memory is invisible to exec in exactly that way.

    Darwin keeps the mmap()/munmap() backing: its W^X rules need real host
    mappings that KrnSetProtection() can flip per hunk.

    W^X note (Apple Silicon): a page may not be writable and executable at the
    same time. We therefore always hand back *writable* memory so the caller can
    populate it, and leave the transition to executable to a subsequent
    KrnSetProtection() call (host mprotect). With the process holding the
    com.apple.security.cs.allow-unsigned-executable-memory entitlement, that
    R/W -> R/X flip is permitted without MAP_JIT.
*/

#include <aros/libcall.h>
#include <aros/debug.h>

#include <inttypes.h>

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/memheaderext.h>
#include <proto/exec.h>
#include <kernel_base.h>
#include <kernel_intern.h>

#include <string.h>
#include <sys/mman.h>

#if !(defined(HOST_OS_darwin) || defined(__APPLE__))
/*
 * Take one page-aligned block of len bytes from a single MemHeader.
 * Must be called with the memory list locked (Forbid()).
 */
static void *krnAllocPagesFromHeader(struct MemHeader *mh, IPTR len, IPTR pageSize)
{
    if (IsManagedMem(mh))
    {
        struct MemHeaderExt *mhe = (struct MemHeaderExt *)mh;
        ULONG mflags = MEMF_ANY;

        if (!mhe->mhe_AllocAligned)
            return NULL;

        return mhe->mhe_AllocAligned(mhe, len, pageSize, &mflags);
    }
    else
    {
        /*
         * Classic first-fit header: Allocate() only guarantees MemChunk
         * alignment, so take one extra page and give the slack on either
         * side back. What remains is exactly [aligned, aligned + len),
         * which is what KrnFreePages() will Deallocate() later.
         */
        UBYTE *raw, *aligned;
        IPTR   head, tail;

        if (mh->mh_Free < len + pageSize)
            return NULL;

        raw = Allocate(mh, len + pageSize);
        if (!raw)
            return NULL;

        aligned = (UBYTE *)(((IPTR)raw + pageSize - 1) & ~(pageSize - 1));
        head    = aligned - raw;
        tail    = pageSize - head;

        if (head)
            Deallocate(mh, raw, head);
        if (tail)
            Deallocate(mh, aligned + len, tail);

        return aligned;
    }
}
#endif

/*****************************************************************************

    NAME */
#include <proto/kernel.h>

        AROS_LH3(void *, KrnAllocPages,

/*  SYNOPSIS */
        AROS_LHA(void *,     addr,   A0),
        AROS_LHA(uintptr_t,  length, D0),
        AROS_LHA(uint32_t,   flags,  D1),

/*  LOCATION */
        struct KernelBase *, KernelBase, 27, Kernel)

/*  FUNCTION
        Allocate page-granular memory. The region is always readable and
        writable; pass it to KrnSetProtection() afterwards to make it
        executable or read-only. On Linux the pages come from exec's RAM
        and are known to TypeOfMem(); on Darwin they are a host mapping.

    INPUTS
        addr   - Preferred start address, or NULL to let the kernel choose.
                 Only honoured on Darwin; elsewhere a non-NULL addr fails.
        length - Size in bytes (rounded up to the host page size).
        flags  - exec.library/AllocMem() style MEMF_* flags. MEMF_CLEAR and
                 the physical attribute bits are honoured; MEMF_EXECUTABLE
                 is declined on non-W^X hosts (see below).

    RESULT
        Start address of the allocated region, or NULL on failure.

******************************************************************************/
{
    AROS_LIBFUNC_INIT

    IPTR    pageSize = KernelBase->kb_PageSize ? KernelBase->kb_PageSize : 4096;
    size_t  len      = (length + pageSize - 1) & ~(pageSize - 1);

#if !(defined(HOST_OS_darwin) || defined(__APPLE__))
    /*
     * On hosts that do not enforce W^X the AROS RAM pool is already mapped
     * R/W/X, so executable hunks can (and should) come from AllocMem() like
     * any other hunk. Declining the request here makes LoadSeg()'s AllocFunc
     * fall back to AllocMem(), keeping a module's code and data hunks in the
     * same memory region. That matters on 64-bit: host mmap() places our
     * pages in the low 2GB (MAP_32BIT) while AllocMem() may serve data from
     * a RAM block above 4GB, and 32-bit RIP-relative relocations between the
     * two cannot span that distance.
     */
    if (flags & MEMF_EXECUTABLE)
        return NULL;

    /*
     * Everything else comes out of exec's RAM (see the file header). Only
     * the physical attribute bits select a header; an exact-address
     * request cannot be honoured without a non-mungwalled AllocAbs(), so
     * it fails and the caller falls back to an unhinted request.
     */
    {
        ULONG physFlags = flags & MEMF_PHYSICAL_MASK;
        struct MemHeader *mh;
        void *res = NULL;

        if (addr)
        {
            D(bug("[KrnAllocPages] exact address %p not supported\n", addr));
            return NULL;
        }

        Forbid();
        ForeachNode(&SysBase->MemList, mh)
        {
            if (physFlags & ~mh->mh_Attributes)
                continue;

            res = krnAllocPagesFromHeader(mh, len, pageSize);
            if (res)
                break;
        }
        Permit();

        if (!res)
        {
            D(bug("[KrnAllocPages] FAILED: len=%u flags=0x%x, no RAM\n",
                  (unsigned)len, (unsigned)flags));
            return NULL;
        }

        if (flags & MEMF_CLEAR)
            memset(res, 0, len);

        D(bug("[KrnAllocPages] len=%u (req %u) flags=0x%x -> %p (exec RAM)\n",
              (unsigned)len, (unsigned)length, (unsigned)flags, res));

        return res;
    }
#else
    struct KernelInterface *iface = KernelBase->kb_PlatformData->iface;
    int     prot = PROT_READ | PROT_WRITE;
    void   *map;

    /*
     * MEMF_EXECUTABLE allocations are mapped R/W/X directly where the host
     * allows it, so code is runnable as soon as it has been written - the
     * behaviour executable hunks had when they came from the (RWX) RAM pool.
     * Apple Silicon refuses a writable+executable mapping even with the
     * disable-executable-page-protection entitlement; there the RWX request
     * fails, we fall back to R/W, and the caller flips the populated region
     * to R/X with KrnSetProtection() (see CreateSegList(),
     * internalloadseg_elf.c).
     */
    if (flags & MEMF_EXECUTABLE)
        prot |= PROT_EXEC;

    /*
     * Executable regions must stay within +/-2GB of the AROS RAM block:
     * loaded code refers to data hunks (and vice versa) through 32-bit
     * RIP-relative relocations, which cannot span the gap between the
     * host's default (top of address space) mmap area and the RAM block.
     * On Linux MAP_32BIT places the mapping in the low 2GB, the same
     * neighbourhood as the RAM block; hosts without it get the default
     * placement (a passed-in addr is used as-is either way).
     */
    int flags_unix = MAP_PRIVATE | MAP_ANON;    /* Darwin has no MAP_ANONYMOUS, only MAP_ANON */
#ifdef MAP_32BIT
    if (flags & MEMF_EXECUTABLE)
        flags_unix |= MAP_32BIT;
#endif

    map = iface->mmap(addr, len, prot, flags_unix, -1, 0);
    AROS_HOST_BARRIER

#ifdef MAP_32BIT
    if ((map == MAP_FAILED) && (flags_unix & MAP_32BIT))
    {
        /* Low-2GB area exhausted - better a far mapping than none at all */
        flags_unix &= ~MAP_32BIT;
        map = iface->mmap(addr, len, prot, flags_unix, -1, 0);
        AROS_HOST_BARRIER
    }
#endif

    if ((map == MAP_FAILED) && (prot & PROT_EXEC))
    {
        /* W^X host refused R/W/X - hand out R/W; the caller must flip */
        prot &= ~PROT_EXEC;
        map = iface->mmap(addr, len, prot, flags_unix, -1, 0);
        AROS_HOST_BARRIER
    }

    if (map == MAP_FAILED)
    {
        int err = *iface->__error();
        AROS_HOST_BARRIER
        D(bug("[KrnAllocPages] FAILED: addr=%p len=%u flags=0x%x errno=%d\n",
              addr, (unsigned)len, (unsigned)flags, err));
        return NULL;
    }

    D(bug("[KrnAllocPages] addr=%p len=%u (req %u) flags=0x%x -> %p (R/W)\n",
          addr, (unsigned)len, (unsigned)length, (unsigned)flags, map));

    return map;
#endif

    AROS_LIBFUNC_EXIT
}
