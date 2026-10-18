/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: application.library init, expunge and shared helpers.
*/

#include <aros/symbolsets.h>
#include <aros/debug.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include <string.h>

#include "application_intern.h"
#include LC_LIBDEFS_FILE

struct DosLibrary *DOSBase = NULL;
struct UtilityBase *UtilityBase = NULL;

static int Application_Init(LIBBASETYPEPTR AppBase)
{
    InitSemaphore(&AppBase->ab_Lock);
    NEWLIST((struct List *)&AppBase->ab_Apps);
    AppBase->ab_NextID = 1;

    AppBase->ab_Reaper.mp_Node.ln_Type = NT_MSGPORT;
    AppBase->ab_Reaper.mp_Flags = PA_IGNORE;
    NEWLIST(&AppBase->ab_Reaper.mp_MsgList);

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    UtilityBase = (struct UtilityBase *)OpenLibrary("utility.library", 36);

    return (DOSBase && UtilityBase);
}

static int Application_Expunge(LIBBASETYPEPTR AppBase)
{
    BOOL idle;

    ObtainSemaphore(&AppBase->ab_Lock);
    app_Reap(AppBase);
    idle = IsListEmpty((struct List *)&AppBase->ab_Apps) && !AppBase->ab_Outstanding;
    ReleaseSemaphore(&AppBase->ab_Lock);

    /* Messages still out with receivers point back at ab_Reaper. */
    if (!idle)
        return FALSE;

    CloseLibrary((struct Library *)UtilityBase);
    CloseLibrary((struct Library *)DOSBase);
    UtilityBase = NULL;
    DOSBase = NULL;
    return TRUE;
}

ADD2INITLIB(Application_Init, 0)
ADD2EXPUNGELIB(Application_Expunge, 0)

STRPTR app_StrDup(CONST_STRPTR s)
{
    STRPTR d;
    ULONG len;

    if (!s)
        return NULL;
    len = strlen(s) + 1;
    if ((d = AllocVec(len, MEMF_ANY)))
        CopyMem((APTR)s, d, len);
    return d;
}

struct AppEntry *app_Find(struct ApplicationBase *AppBase, ULONG appID)
{
    struct AppEntry *ae;

    if (!appID)
        return NULL;
    ForeachNode(&AppBase->ab_Apps, ae)
    {
        if (ae->ae_ID == appID)
            return ae;
    }
    return NULL;
}

void app_Reap(struct ApplicationBase *AppBase)
{
    struct Message *msg;

    while ((msg = GetMsg(&AppBase->ab_Reaper)))
    {
        FreeVec(msg);
        AppBase->ab_Outstanding--;
    }
}

/*
 * Send a copy of a message to one application. The copy carries its own
 * string, so the sender's buffers can go away at once.
 */
BOOL app_Post(struct ApplicationBase *AppBase, struct AppEntry *to, ULONG sender,
              ULONG type, CONST_STRPTR text)
{
    struct ApplicationMsg *am;
    ULONG size, textlen = text ? strlen(text) + 1 : 0;

    if (!to || !to->ae_Port)
        return FALSE;

    switch (type)
    {
    case APPLIBMT_CustomMsg:
        size = sizeof(struct ApplicationCustomMsg);
        break;
    case APPLIBMT_OpenDoc:
    case APPLIBMT_PrintDoc:
        size = sizeof(struct ApplicationOpenPrintDocMsg);
        break;
    default:
        size = sizeof(struct ApplicationMsg);
        textlen = 0;
        break;
    }

    if (!(am = AllocVec(size + textlen, MEMF_PUBLIC | MEMF_CLEAR)))
        return FALSE;

    am->msg.mn_Node.ln_Type = NT_MESSAGE;
    am->msg.mn_Length = size;
    am->msg.mn_ReplyPort = &AppBase->ab_Reaper;
    am->senderAppID = sender;
    am->type = type;

    if (textlen)
    {
        STRPTR copy = (STRPTR)am + size;

        CopyMem((APTR)text, copy, textlen);
        if (type == APPLIBMT_CustomMsg)
            ((struct ApplicationCustomMsg *)am)->customMsg = copy;
        else
            ((struct ApplicationOpenPrintDocMsg *)am)->fileName = copy;
    }

    AppBase->ab_Outstanding++;
    PutMsg(to->ae_Port, &am->msg);
    return TRUE;
}

void app_Broadcast(struct ApplicationBase *AppBase, ULONG sender, ULONG type)
{
    struct AppEntry *ae;

    ForeachNode(&AppBase->ab_Apps, ae)
    {
        if (ae->ae_ID != sender)
            app_Post(AppBase, ae, sender, type, NULL);
    }
}
