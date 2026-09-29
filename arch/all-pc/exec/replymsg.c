/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: Reply a message
*/

#include <aros/libcall.h>
#include <exec/ports.h>
#include <proto/exec.h>

#include "exec_intern.h"
#include "exec_util.h"

void InternalPublishMsg(struct MsgPort *port, struct Message *message, BOOL reply,
    struct ExecBase *SysBase);

/*****************************************************************************

    NAME */

        AROS_LH1(void, ReplyMsg,

/*  SYNOPSIS */
        AROS_LHA(struct Message *, message, A1),

/*  LOCATION */
        struct ExecBase *, SysBase, 63, Exec)

/*  FUNCTION
        Send a message back to where it came from. It's generally not
        wise to access the fields of a message after it has been replied.

    INPUTS
        message - a message got with GetMsg().

    RESULT

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO
        WaitPort(), GetMsg(), PutMsg()

    INTERNALS

******************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct MsgPort *port;

    /* Get replyport */
    port=message->mn_ReplyPort;

    /* Not set? Only mark the message as no longer sent. */
    if(port==NULL)
        message->mn_Node.ln_Type=NT_FREEMSG;
    else
    {
        /* Mark it replied only once it is in the port - see InternalPublishMsg() */
        InternalPublishMsg(port, message, TRUE, SysBase);
    }

    AROS_LIBFUNC_EXIT
} /* ReplyMsg() */

