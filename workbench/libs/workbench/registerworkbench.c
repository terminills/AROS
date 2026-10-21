/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

#include "workbench_intern.h"
#include "support.h"
#include <workbench/workbench.h>

/*****************************************************************************

    NAME */

        #include <proto/workbench.h>

        AROS_LH1(BOOL, RegisterWorkbench,

/*  SYNOPSIS */
        AROS_LHA(struct MsgPort *, messageport, A0),

/*  LOCATION */
        struct WorkbenchBase *, WorkbenchBase, 23, Workbench)

/*  FUNCTION
        The workbench application uses this function to register itself with
        the library. When it has done this, the library sends messages to the
        specified port about actions the application is supposed to carry out.
        
        All messages sent to the message port are of struct WBHandlerMessage,
        which is specified in <workbench/handler.h>. The wbhm_Type field
        identifies the type of message and which part of the wbhm_Data union
        is relevant. The following types are currently defined:
        
        WBHM_TYPE_SHOW
            Intuition has (re)opened the Workbench Screen, and request that
            you open all your windows. When the message is replied, Intuition
            assumes that the windows have been opened.

        WBHM_TYPE_HIDE
            Intuition is about to close the Workbench Screen, and request that
            you close all your windows. When the message is replied, Intuition
            assumes that the windows have been closed and will try to close the
            screen.
            
        WBHM_TYPE_OPEN
            Request to open the specified drawer.
            
        WBHM_TYPE_UPDATE
            The state of the specified disk object has changed, and this
            message serves as a notification and suggestion that you should
            update its visual representation to the user. For example, it
            might have been deleted or renamed.

    INPUTS
        messageport - The message port to send the to.

    RESULT
        TRUE if the message port was successfully registered, FALSE otherwise.
        The registration will fail if an other message port has already been
        registered earlier or if a NULL pointer was passed in.

    NOTES
        As you can read above, only one workbench application can be registered
        at a time. This is intentional. Note that "workbench application" in
        this context means the program that is the file manager and handles
        the GUI, not a program that is started using OpenWorkbenchObjectA()!

        The command search path of the calling process becomes the one
        returned by WorkbenchControl(WBCTRLA_DuplicateSearchPath).

    EXAMPLE

    BUGS

    SEE ALSO
        UnregisterWorkbench()

    INTERNALS

******************************************************************************/
{
    AROS_LIBFUNC_INIT

    BOOL success = FALSE;

    if (messageport != NULL)
    {
        ObtainSemaphore(&(WorkbenchBase->wb_WorkbenchPortSemaphore));
    
        if (WorkbenchBase->wb_WorkbenchPort == NULL)
        {
            struct Process *me = (struct Process *)FindTask(NULL);
            struct CommandLineInterface *cli;

            WorkbenchBase->wb_WorkbenchPort = messageport;
            success = TRUE;

            if (me->pr_Task.tc_Node.ln_Type == NT_PROCESS && (cli = Cli()) != NULL
                && cli->cli_CommandDir != BNULL)
            {
                BPTR old = WorkbenchBase->wb_SearchPath;

                WorkbenchBase->wb_SearchPath = DuplicateSearchPath(cli->cli_CommandDir);
                FreeSearchPath(old);
            }
        }
        
        ReleaseSemaphore(&(WorkbenchBase->wb_WorkbenchPortSemaphore));
    }
    
    return success;

    AROS_LIBFUNC_EXIT
} /* RegisterWorkbench() */
