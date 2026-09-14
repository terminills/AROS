/*
    Copyright (C) 2025-2026, The AROS Development Team. All rights reserved.
*/

#include <aros/libcall.h>
#include <proto/exec.h>
#include <errno.h>

#include "fd_private.h"
#include LC_LIBDEFS_FILE

static LONG fd_ensure_capacity(struct fd_base *base, ULONG min_slots)
{
    if (min_slots <= base->fd_Slots)
        return 0;

    if (min_slots == 0)
        return EINVAL;

    fd_entry *new_table = AllocVec(min_slots * sizeof(*new_table), MEMF_ANY | MEMF_CLEAR);
    if (!new_table)
        return ENOMEM;

    if (base->fd_Table) {
        CopyMem(base->fd_Table, new_table, base->fd_Slots * sizeof(*new_table));
        FreeVec(base->fd_Table);
    }

    base->fd_Table = new_table;
    base->fd_Slots = min_slots;
    return 0;
}

static fd_entry *fd_get_entry(struct fd_base *base, LONG fd)
{
    if (fd < 0 || (ULONG)fd >= base->fd_Slots)
        return NULL;
    return &base->fd_Table[fd];
}

/*****************************************************************************

    NAME */
        AROS_LH4(LONG, FD_Alloc,

/*  SYNOPSIS */
        AROS_LHA(LONG, startfd, D0),
        AROS_LHA(fd_owner_t, owner, D1),
        AROS_LHA(APTR, data, A0),
        AROS_LHA(LONG *, outfd, A1),

/*  LOCATION */
        struct fd_base *, FDBase, 5, FD)

/*  FUNCTION
        Allocate a file descriptor slot for the specified owner.

    INPUTS
        startfd - Starting descriptor number to search from.
        owner   - Descriptor owner identifier.
        data    - Optional owner data.
        outfd   - Pointer that receives the allocated descriptor.

    RESULT
        0 on success, or an errno-style error code.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        FD_Reserve()

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    LONG error = 0;
    LONG fd = startfd;
    fd_entry *entry = NULL;

    if (!outfd)
        return EINVAL;

    if (owner == FD_OWNER_NONE)
        return EINVAL;

    ObtainSemaphore(&FDBase->fd_Lock);

    if (fd < 0) {
        error = EBADF;
        goto done;
    }

    for (;; fd++) {
        if ((ULONG)fd >= FDBase->fd_Slots) {
            error = fd_ensure_capacity(FDBase, fd + 1);
            if (error)
                goto done;
        }

        entry = &FDBase->fd_Table[fd];
        if (entry->owner == FD_OWNER_NONE) {
            entry->owner = owner;
            entry->refs = 1;
            entry->data = data;
            *outfd = fd;
            goto done;
        }
    }

 done:
    ReleaseSemaphore(&FDBase->fd_Lock);
    return error;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH3(LONG, FD_Reserve,

/*  SYNOPSIS */
        AROS_LHA(LONG, fd, D0),
        AROS_LHA(fd_owner_t, owner, D1),
        AROS_LHA(APTR, data, A0),

/*  LOCATION */
        struct fd_base *, FDBase, 6, FD)

/*  FUNCTION
        Reserve a specific file descriptor slot for the specified owner.

    INPUTS
        fd     - Descriptor number to reserve.
        owner  - Descriptor owner identifier.
        data   - Optional owner data.

    RESULT
        0 on success, or an errno-style error code.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        FD_Alloc()

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    LONG error = 0;
    fd_entry *entry = NULL;

    if (owner == FD_OWNER_NONE)
        return EINVAL;

    ObtainSemaphore(&FDBase->fd_Lock);

    if (fd < 0) {
        error = EBADF;
        goto done;
    }

    error = fd_ensure_capacity(FDBase, fd + 1);
    if (error)
        goto done;

    entry = &FDBase->fd_Table[fd];
    if (entry->owner != FD_OWNER_NONE) {
        error = EBUSY;
        goto done;
    }

    entry->owner = owner;
    entry->refs = 1;
    entry->data = data;

 done:
    ReleaseSemaphore(&FDBase->fd_Lock);
    return error;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH2(LONG, FD_Free,

/*  SYNOPSIS */
        AROS_LHA(LONG, fd, D0),
        AROS_LHA(fd_owner_t, owner, D1),

/*  LOCATION */
        struct fd_base *, FDBase, 7, FD)

/*  FUNCTION
        Drop one holder of a descriptor slot owned by a specific consumer.
        The slot becomes free again when its last holder lets go (see
        FD_Share()); a slot that was only ever allocated or reserved has a
        single holder and is released by this call.

    INPUTS
        fd     - Descriptor number to release.
        owner  - Descriptor owner identifier.

    RESULT
        0 on success, or an errno-style error code.

    NOTES
        Descriptor numbers are system-wide, but a POSIX process and the
        vfork() child that inherited a copy of its descriptor table both
        hold the same numbers.  Before FD_Share() existed, the first of
        the two to close() a shared number freed it for everyone: the other
        table's slot turned stale (EBADF), and the DOS handle behind it was
        never closed - a pipe whose write end was such a slot never reached
        EOF.

    EXAMPLE

    BUGS

    SEE ALSO
        FD_Reserve(), FD_Share()

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    LONG error = 0;
    fd_entry *entry = NULL;

    if (owner == FD_OWNER_NONE)
        return EINVAL;

    ObtainSemaphore(&FDBase->fd_Lock);

    entry = fd_get_entry(FDBase, fd);
    if (!entry || entry->owner == FD_OWNER_NONE || entry->owner != owner) {
        error = EBADF;
        goto done;
    }

    if (entry->refs > 1) {
        entry->refs--;
        goto done;
    }

    entry->owner = FD_OWNER_NONE;
    entry->refs = 0;
    entry->data = NULL;

 done:
    ReleaseSemaphore(&FDBase->fd_Lock);
    return error;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH2(LONG, FD_Share,

/*  SYNOPSIS */
        AROS_LHA(LONG, fd, D0),
        AROS_LHA(fd_owner_t, owner, D1),

/*  LOCATION */
        struct fd_base *, FDBase, 14, FD)

/*  FUNCTION
        Add a holder to a descriptor slot that the given consumer already
        owns.  Each holder must later drop the number with FD_Free(); the
        slot is released when the last one does.

    INPUTS
        fd     - Descriptor number to share.
        owner  - Descriptor owner identifier; must match the current owner.

    RESULT
        0 on success, or an errno-style error code (EBADF when the slot is
        free or owned by someone else).

    NOTES
        Used by posixc.library when a vfork() child copies its parent's
        descriptor table, and when dup2() targets a number another table
        still holds.

    EXAMPLE

    BUGS

    SEE ALSO
        FD_Free()

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    LONG error = 0;
    fd_entry *entry = NULL;

    if (owner == FD_OWNER_NONE)
        return EINVAL;

    ObtainSemaphore(&FDBase->fd_Lock);

    entry = fd_get_entry(FDBase, fd);
    if (!entry || entry->owner == FD_OWNER_NONE || entry->owner != owner) {
        error = EBADF;
        goto done;
    }

    entry->refs++;

 done:
    ReleaseSemaphore(&FDBase->fd_Lock);
    return error;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH1(LONG, FD_Check,

/*  SYNOPSIS */
        AROS_LHA(LONG, fd, D0),

/*  LOCATION */
        struct fd_base *, FDBase, 8, FD)

/*  FUNCTION
        Check whether a descriptor slot is available.

    INPUTS
        fd - Descriptor number to check.

    RESULT
        0 if the slot is free, or an errno-style error code.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        FD_GetOwner()

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    LONG error = 0;
    fd_entry *entry = NULL;

    ObtainSemaphore(&FDBase->fd_Lock);

    if (fd < 0) {
        error = EBADF;
        goto done;
    }

    entry = fd_get_entry(FDBase, fd);
    if (entry && entry->owner != FD_OWNER_NONE)
        error = EBUSY;

 done:
    ReleaseSemaphore(&FDBase->fd_Lock);
    return error;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH1(fd_owner_t, FD_GetOwner,

/*  SYNOPSIS */
        AROS_LHA(LONG, fd, D0),

/*  LOCATION */
        struct fd_base *, FDBase, 9, FD)

/*  FUNCTION
        Query the owner of a descriptor slot.

    INPUTS
        fd - Descriptor number to query.

    RESULT
        Owner identifier or FD_OWNER_NONE.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        FD_Check()

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    fd_owner_t owner = FD_OWNER_NONE;
    fd_entry *entry = NULL;

    ObtainSemaphore(&FDBase->fd_Lock);

    entry = fd_get_entry(FDBase, fd);
    if (entry)
        owner = entry->owner;

    ReleaseSemaphore(&FDBase->fd_Lock);
    return owner;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH1(APTR, FD_GetData,

/*  SYNOPSIS */
        AROS_LHA(LONG, fd, D0),

/*  LOCATION */
        struct fd_base *, FDBase, 10, FD)

/*  FUNCTION
        Query the owner data for a descriptor slot.

    INPUTS
        fd - Descriptor number to query.

    RESULT
        Owner data pointer or NULL.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        FD_SetData()

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    APTR data = NULL;
    fd_entry *entry = NULL;

    ObtainSemaphore(&FDBase->fd_Lock);

    entry = fd_get_entry(FDBase, fd);
    if (entry)
        data = entry->data;

    ReleaseSemaphore(&FDBase->fd_Lock);
    return data;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH3(LONG, FD_SetData,

/*  SYNOPSIS */
        AROS_LHA(LONG, fd, D0),
        AROS_LHA(fd_owner_t, owner, D1),
        AROS_LHA(APTR, data, A0),

/*  LOCATION */
        struct fd_base *, FDBase, 11, FD)

/*  FUNCTION
        Update the owner data for a descriptor slot.

    INPUTS
        fd    - Descriptor number to update.
        owner - Descriptor owner identifier.
        data  - Owner data pointer.

    RESULT
        0 on success, or an errno-style error code.

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        FD_GetData()

    INTERNALS

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    LONG error = 0;
    fd_entry *entry = NULL;

    if (owner == FD_OWNER_NONE)
        return EINVAL;

    ObtainSemaphore(&FDBase->fd_Lock);

    entry = fd_get_entry(FDBase, fd);
    if (!entry || entry->owner != owner) {
        error = EBADF;
        goto done;
    }

    entry->data = data;

 done:
    ReleaseSemaphore(&FDBase->fd_Lock);
    return error;

    AROS_LIBFUNC_EXIT
}
