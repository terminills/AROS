/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: Hosted (unix/darwin) implementation of KrnFreePages().

    Pages served from exec's RAM (see allocpages.c) go back to the
    MemHeader they came from; anything exec does not know about is a host
    mapping and is munmap()ed.
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

/*
 * Find the MemHeader an address lives in, exactly the way exec's
 * TypeOfMem() does. Must be called with the memory list locked.
 */
static struct MemHeader *krnFindMemHeader(void *addr)
{
    struct MemHeader *mh;

    ForeachNode(&SysBase->MemList, mh)
    {
        if (IsManagedMem(mh))
        {
            struct MemHeaderExt *mhe = (struct MemHeaderExt *)mh;

            if (mhe->mhe_InBounds && mhe->mhe_InBounds(mhe, addr, addr))
                return mh;
        }
        else if (addr >= mh->mh_Lower && addr < mh->mh_Upper)
            return mh;
    }

    return NULL;
}

AROS_LH2(void, KrnFreePages,
         AROS_LHA(void *, phy_addr, A0),
         AROS_LHA(uint32_t, length, D0),
         struct KernelBase *, KernelBase, 28, Kernel)
{
    AROS_LIBFUNC_INIT

    IPTR   pageSize = KernelBase->kb_PageSize ? KernelBase->kb_PageSize : 4096;
    size_t len      = (length + pageSize - 1) & ~(pageSize - 1);
    struct MemHeader *mh;

    Forbid();
    mh = krnFindMemHeader(phy_addr);
    if (mh)
    {
        if (IsManagedMem(mh))
        {
            struct MemHeaderExt *mhe = (struct MemHeaderExt *)mh;

            if (mhe->mhe_Free)
                mhe->mhe_Free(mhe, phy_addr, len);
        }
        else
            Deallocate(mh, phy_addr, len);
    }
    Permit();

    if (mh)
    {
        D(bug("[KrnFreePages] %p len=%u -> %s\n", phy_addr, (unsigned)len,
              mh->mh_Node.ln_Name));
        return;
    }

    KernelBase->kb_PlatformData->iface->munmap(phy_addr, len);
    AROS_HOST_BARRIER

    AROS_LIBFUNC_EXIT
}
