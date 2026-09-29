/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: Send a message to a port.
*/

#define DEBUG 0
#include <aros/debug.h>

#include <aros/libcall.h>
#include <exec/ports.h>
#include <proto/exec.h>

#include "exec_intern.h"
#include "exec_util.h"

/*****************************************************************************

    NAME */

        AROS_LH2(void, PutMsg,

/*  SYNOPSIS */
        AROS_LHA(struct MsgPort *, port,    A0),
        AROS_LHA(struct Message *, message, A1),

/*  LOCATION */
        struct ExecBase *, SysBase, 61, Exec)

/*  FUNCTION
        Sends a message to a given message port. Messages are not copied
        from one task to another but must lie in shared memory instead.
        Therefore the owner of the message may generally not reuse it before
        it is returned. But this depends on the two tasks sharing the message.

    INPUTS
        port    - Pointer to messageport.
        message - Pointer to message.

    RESULT

    NOTES
        It is legal to send a message from within interrupts.

        Messages may either trigger a signal at the owner of the messageport
        or raise a software interrupt, depending on port->mp_Flags&PF_ACTION.

    EXAMPLE

    BUGS

    SEE ALSO
        WaitPort(), GetMsg()

    INTERNALS

******************************************************************************/
{
    AROS_LIBFUNC_INIT
    ASSERT_VALID_PTR(message);
    ASSERT_VALID_PTR(port);

    /* Set the node type to NT_MESSAGE == sent message. */
    message->mn_Node.ln_Type = NT_MESSAGE;

    InternalPutMsg(port, message, SysBase);

    AROS_LIBFUNC_EXIT
} /* PutMsg() */

/*
 * Add a message to a port and notify its owner - SMP-safe for the case
 * that matters on pc: the port may live on the owner's stack. As soon as
 * the owner can see the message (in the list, or - for a reply - as
 * NT_REPLYMSG via CheckIO()), it may return and reuse that stack, so after
 * that point the sender must not touch the port or the message again.
 * Everything the notification needs is read under the port lock before the
 * message is published; a reply is marked NT_REPLYMSG as the very last
 * store to the message. Messages may be sent from interrupts, so the list
 * is protected by Disable() on this CPU and the port's spinlock.
 */
void InternalPublishMsg(struct MsgPort *port, struct Message *message, BOOL reply,
    struct ExecBase *SysBase)
{
    struct Task *sigtask;
    APTR target;
    UBYTE flags, sigbit;

    D(bug("[EXEC] PutMsg: Port @ 0x%p, Msg @ 0x%p\n", port, message);)

    Disable();
#if defined(__AROSEXEC_SMP__)
    EXEC_SPINLOCK_LOCK(&port->mp_SpinLock, NULL, SPINLOCK_MODE_WRITE);
#endif
    sigtask = port->mp_SigTask;
    target  = port->mp_SoftInt;     /* same storage as mp_SigTask (PA_CALL/PA_SOFTINT) */
    flags   = port->mp_Flags;
    sigbit  = port->mp_SigBit;
    AddTail(&port->mp_MsgList, &message->mn_Node);
#if defined(__AROSEXEC_SMP__)
    EXEC_SPINLOCK_UNLOCK(&port->mp_SpinLock);
#endif
    Enable();

    if (reply)
        __atomic_store_n(&message->mn_Node.ln_Type, NT_REPLYMSG, __ATOMIC_RELEASE);

    if (sigtask)
    {
        switch (flags & PF_ACTION)
        {
            case PA_SIGNAL:
                D(bug("[EXEC] PutMsg: PA_SIGNAL -> Task 0x%p, Signal %08x\n", sigtask, (1 << sigbit));)
                Signal(sigtask, (1 << sigbit));
                break;

            case PA_SOFTINT:
                Cause((struct Interrupt *)target);
                break;

            case PA_IGNORE:
                break;

            case PA_CALL:
                /* The called function gets the port; it must be SMP safe. */
                AROS_UFC2NR(void, target,
                    AROS_UFCA(struct MsgPort *,  port,    D0),
                    AROS_UFCA(struct ExecBase *, SysBase, A6));
                break;
        }
    }
}

void InternalPutMsg(struct MsgPort *port, struct Message *message, struct ExecBase *SysBase)
{
    InternalPublishMsg(port, message, FALSE, SysBase);
}
