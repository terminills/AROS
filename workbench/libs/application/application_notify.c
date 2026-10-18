/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Notify() - hand a pop-up to the notification server.
*/

#include <aros/debug.h>
#include <exec/memory.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include <string.h>

#include "application_intern.h"
#include "application_server.h"
#include LC_LIBDEFS_FILE

static void ntf_Copy(STRPTR dst, CONST_STRPTR src, ULONG size)
{
    if (!src)
        src = "";
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/* Start the server if it is not running, and wait up to ~3 s for its port. */
static BOOL ntf_StartServer(void)
{
    struct TagItem tags[] =
    {
        { SYS_Asynch,   TRUE },
        { SYS_Input,    0    },
        { SYS_Output,   0    },
        { NP_Name,      (IPTR)"Notifications" },
        { TAG_DONE,     0    }
    };
    BPTR in, out;
    LONG tries;

    if (FindPort(NOTIFYSERVER_PORTNAME))
        return TRUE;

    in = Open("NIL:", MODE_OLDFILE);
    out = Open("NIL:", MODE_NEWFILE);
    if (!in || !out)
    {
        if (in)
            Close(in);
        if (out)
            Close(out);
        return FALSE;
    }
    tags[1].ti_Data = (IPTR)in;
    tags[2].ti_Data = (IPTR)out;

    /* With SYS_Asynch the new process owns the handles from here on. */
    if (SystemTagList(NOTIFYSERVER_PATH, tags) == -1)
    {
        Close(in);
        Close(out);
        return FALSE;
    }

    for (tries = 0; tries < 60; tries++)
    {
        if (FindPort(NOTIFYSERVER_PORTNAME))
            return TRUE;
        Delay(3);
    }
    return FALSE;
}

/*****************************************************************************

    NAME */
#include <proto/application.h>

        AROS_LH2(ULONG, NotifyA,

/*  SYNOPSIS */
        AROS_LHA(ULONG, appID, D0),
        AROS_LHA(struct TagItem *, tags, A0),

/*  LOCATION */
        struct ApplicationBase *, AppBase, 11, Application)

/*  FUNCTION
        Show a pop-up notification for a registered application. It closes
        by itself after a few seconds and asks nothing of the user.

        Double-clicking it sends APPNOTIFY_BackMsg to the application as an
        APPLIBMT_CustomMsg; a BackMsg starting with "URL:" opens the URL in
        the default browser instead.

    INPUTS
        appID - the application ID from RegisterApplication().
        tags  - APPNOTIFY_ tags. APPNOTIFY_Text is required.

    RESULT
        Non-zero if the pop-up was shown.

    NOTES
        Use notifications sparingly, keep them short, and never use one for
        an error or a question: the user may never see it.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct NotifyServerMsg *nsm;
    struct MsgPort *reply, *server;
    struct AppEntry *ae;
    CONST_STRPTR text = (CONST_STRPTR)GetTagData(APPNOTIFY_Text, 0, tags);
    ULONG result = 0;

    if (!text)
        return 0;

    if (!(nsm = AllocVec(sizeof(*nsm), MEMF_PUBLIC | MEMF_CLEAR)))
        return 0;

    ObtainSemaphore(&AppBase->ab_Lock);
    app_Reap(AppBase);
    if ((ae = app_Find(AppBase, appID)))
        ntf_Copy(nsm->nsm_AppName, ae->ae_Name, sizeof(nsm->nsm_AppName));
    ReleaseSemaphore(&AppBase->ab_Lock);

    if (!ae)
    {
        FreeVec(nsm);
        return 0;
    }

    nsm->nsm_Version = NOTIFYSERVER_VERSION;
    nsm->nsm_AppID = appID;
    nsm->nsm_CloseOnDC = GetTagData(APPNOTIFY_CloseOnDC, FALSE, tags) ? TRUE : FALSE;
    nsm->nsm_ImageVertAlign = GetTagData(APPNOTIFY_ImageVertAlign, 0, tags);
    ntf_Copy(nsm->nsm_Title, (CONST_STRPTR)GetTagData(APPNOTIFY_Title, (IPTR)nsm->nsm_AppName, tags),
             sizeof(nsm->nsm_Title));
    ntf_Copy(nsm->nsm_Text, text, sizeof(nsm->nsm_Text));
    ntf_Copy(nsm->nsm_PubScreen, (CONST_STRPTR)GetTagData(APPNOTIFY_PubScreenName, 0, tags),
             sizeof(nsm->nsm_PubScreen));
    ntf_Copy(nsm->nsm_ImageFile, (CONST_STRPTR)GetTagData(APPNOTIFY_ImageFile, 0, tags),
             sizeof(nsm->nsm_ImageFile));
    ntf_Copy(nsm->nsm_BackMsg, (CONST_STRPTR)GetTagData(APPNOTIFY_BackMsg, 0, tags),
             sizeof(nsm->nsm_BackMsg));

    if (ntf_StartServer() && (reply = CreateMsgPort()))
    {
        nsm->nsm_Msg.mn_Node.ln_Type = NT_MESSAGE;
        nsm->nsm_Msg.mn_Length = sizeof(*nsm);
        nsm->nsm_Msg.mn_ReplyPort = reply;

        /* The server may be quitting; hold the port still while posting. */
        Forbid();
        if ((server = FindPort(NOTIFYSERVER_PORTNAME)))
            PutMsg(server, &nsm->nsm_Msg);
        Permit();

        if (server)
        {
            WaitPort(reply);
            GetMsg(reply);
            result = nsm->nsm_Result;
        }
        DeleteMsgPort(reply);
    }

    FreeVec(nsm);
    return result;

    AROS_LIBFUNC_EXIT
}
