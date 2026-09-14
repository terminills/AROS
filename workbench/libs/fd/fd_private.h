#ifndef FD_PRIVATE_H
#define FD_PRIVATE_H

/*
    Copyright (C) 2025-2026, The AROS Development Team. All rights reserved.
*/

#include <exec/libraries.h>
#include <exec/semaphores.h>

#include <libraries/fd.h>

typedef struct fd_entry {
    fd_owner_t owner;
    /* Number of holders of this descriptor number (FD_Alloc()/FD_Reserve()
       start at 1, FD_Share() adds one, FD_Free() drops one); the slot is
       released when it reaches zero. */
    ULONG refs;
    APTR data;
} fd_entry;

struct fd_base {
    struct Library fd_LibNode;
    struct SignalSemaphore fd_Lock;
    fd_entry *fd_Table;
    ULONG fd_Slots;
    const struct fd_hooks *fd_OwnerHooks[FD_OWNER_MAX];
};

#endif /* FD_PRIVATE_H */
