/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Registering applications and messaging between them.
*/

#include <aros/debug.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include <string.h>

#include "application_intern.h"
#include LC_LIBDEFS_FILE

static void app_CopyStr(STRPTR dst, CONST_STRPTR src, ULONG size)
{
    if (!src)
        src = "";
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/* The program's path, from whichever of the three tags the caller gave. */
static STRPTR app_ProgramPath(struct TagItem *tags)
{
    struct WBStartup *wbs;
    STRPTR path;
    BPTR lock;
    CONST_STRPTR name = NULL;

    if ((name = (CONST_STRPTR)GetTagData(REGAPP_FileName, 0, tags)))
        return app_StrDup(name);

    if (!(path = AllocVec(1024, MEMF_ANY | MEMF_CLEAR)))
        return NULL;

    if ((lock = (BPTR)GetTagData(REGAPP_FileLock, 0, tags)))
    {
        if (NameFromLock(lock, path, 1024))
            return path;
    }
    else if ((wbs = (struct WBStartup *)GetTagData(REGAPP_WBStartup, 0, tags))
             && wbs->sm_NumArgs > 0)
    {
        if (NameFromLock(wbs->sm_ArgList[0].wa_Lock, path, 1024)
            && AddPart(path, wbs->sm_ArgList[0].wa_Name, 1024))
            return path;
    }
    else
    {
        struct Process *me = (struct Process *)FindTask(NULL);
        TEXT prog[256];

        if (me->pr_Task.tc_Node.ln_Type == NT_PROCESS && me->pr_HomeDir
            && GetProgramName(prog, sizeof(prog))
            && NameFromLock(me->pr_HomeDir, path, 1024)
            && AddPart(path, FilePart(prog), 1024))
            return path;
    }

    FreeVec(path);
    return NULL;
}

static BOOL app_SameApp(struct AppEntry *ae, CONST_STRPTR name, CONST_STRPTR url)
{
    return (Stricmp(ae->ae_Name, name) == 0) && (Stricmp(ae->ae_URL, url) == 0);
}

static BOOL app_IdentTaken(struct ApplicationBase *AppBase, CONST_STRPTR ident)
{
    struct AppEntry *ae;

    ForeachNode(&AppBase->ab_Apps, ae)
    {
        if (Stricmp(ae->ae_Ident, ident) == 0)
            return TRUE;
    }
    return FALSE;
}

/* "name.url", then "name_1.url", "name_2.url", ... for later instances. */
static void app_MakeIdent(struct ApplicationBase *AppBase, struct AppEntry *ae)
{
    ULONG n;

    for (n = 0; ; n++)
    {
        TEXT inst[16], *p = inst + sizeof(inst) - 1;
        ULONG v = n;

        *p = '\0';
        if (n)
        {
            do
                *--p = '0' + (v % 10);
            while (v /= 10);
            *--p = '_';
        }
        strcpy(ae->ae_Ident, ae->ae_Name);
        strcat(ae->ae_Ident, p);
        if (ae->ae_URL[0])
        {
            strcat(ae->ae_Ident, ".");
            strncat(ae->ae_Ident, ae->ae_URL, sizeof(ae->ae_Ident) - strlen(ae->ae_Ident) - 1);
        }
        if (!app_IdentTaken(AppBase, ae->ae_Ident))
            return;
    }
}

static ULONG app_FlagFor(Tag tag)
{
    switch (tag)
    {
    case REGAPP_UniqueApplication:      return APPF_UNIQUE;
    case REGAPP_HasIconifyFeature:
    case APPATTR_HasIconifyFeature:     return APPF_ICONIFY;
    case REGAPP_HasPrefsWindow:
    case APPATTR_HasPrefsWindow:        return APPF_PREFSWINDOW;
    case REGAPP_CanCreateNewDocs:
    case APPATTR_CanCreateNewDocs:      return APPF_NEWDOCS;
    case REGAPP_CanPrintDocs:
    case APPATTR_CanPrintDocs:          return APPF_PRINTDOCS;
    case REGAPP_AppNotifications:
    case APPATTR_AppNotifications:      return APPF_APPNOTIFY;
    case REGAPP_BlankerNotifications:
    case APPATTR_BlankerNotifications:  return APPF_BLANKNOTIFY;
    case REGAPP_AllowsBlanker:
    case APPATTR_AllowsBlanker:         return APPF_ALLOWBLANKER;
    case REGAPP_SavePrefs:
    case APPATTR_SavePrefs:             return APPF_SAVEPREFS;
    case APPATTR_Hidden:                return APPF_HIDDEN;
    }
    return 0;
}

static void app_FreeDocs(struct AppEntry *ae)
{
    struct AppDoc *ad;

    while ((ad = (struct AppDoc *)REMHEAD((struct List *)&ae->ae_LastDocs)))
        FreeVec(ad);
}

static void app_AddDoc(struct AppEntry *ae, CONST_STRPTR path)
{
    struct AppDoc *ad, *next;
    ULONG count = 0;

    if (!path || !*path)
        return;

    /* Re-opening a document moves it to the front. */
    ForeachNodeSafe(&ae->ae_LastDocs, ad, next)
    {
        if (Stricmp(ad->ad_Path, path) == 0)
        {
            REMOVE(ad);
            FreeVec(ad);
        }
    }

    if (!(ad = AllocVec(sizeof(struct AppDoc) + strlen(path), MEMF_ANY)))
        return;
    strcpy(ad->ad_Path, path);
    ADDHEAD((struct List *)&ae->ae_LastDocs, ad);

    ForeachNodeSafe(&ae->ae_LastDocs, ad, next)
    {
        if (++count > APP_LASTDOCS)
        {
            REMOVE(ad);
            FreeVec(ad);
        }
    }
}

/*****************************************************************************

    NAME */
#include <proto/application.h>

        AROS_LH2(ULONG, RegisterApplicationA,

/*  SYNOPSIS */
        AROS_LHA(CONST_STRPTR, name, A0),
        AROS_LHA(struct TagItem *, tags, A1),

/*  LOCATION */
        struct ApplicationBase *, AppBase, 5, Application)

/*  FUNCTION
        Register the calling program as an application. It gets an ID, a
        unique identifier ("name.urlidentifier", with "_1", "_2" ... added
        for further instances) and a message port, see APPATTR_Port.

    INPUTS
        name - application name, without spaces.
        tags - REGAPP_ tags.

    RESULT
        The application ID, or 0. With REGAPP_UniqueApplication, 0 is also
        returned when the application is already running; that instance is
        sent an APPLIBMT_Unique message.

    NOTES
        Call UnregisterApplication() from the same task before exiting.

    SEE ALSO
        UnregisterApplication(), GetApplicationAttrsA()

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct TagItem *tstate = tags, *tag;
    struct AppEntry *ae, *other;
    CONST_STRPTR url;
    ULONG id = 0;

    if (!name || !*name || strchr(name, ' ') || strlen(name) >= APP_NAMEMAX)
        return 0;

    if (!(ae = AllocVec(sizeof(struct AppEntry), MEMF_ANY | MEMF_CLEAR)))
        return 0;

    app_CopyStr(ae->ae_Name, name, sizeof(ae->ae_Name));
    url = (CONST_STRPTR)GetTagData(REGAPP_URLIdentifier, (IPTR)"", tags);
    if (Strnicmp(url, "www.", 4) == 0)
        url += 4;
    app_CopyStr(ae->ae_URL, url, sizeof(ae->ae_URL));

    ae->ae_Flags = APPF_ALLOWBLANKER;
    while ((tag = NextTagItem(&tstate)))
    {
        ULONG flag = app_FlagFor(tag->ti_Tag);

        if (flag)
        {
            if (tag->ti_Data)
                ae->ae_Flags |= flag;
            else
                ae->ae_Flags &= ~flag;
        }
    }

    ae->ae_Description = app_StrDup((CONST_STRPTR)GetTagData(REGAPP_Description, 0, tags));
    ae->ae_FileName = app_ProgramPath(tags);
    NEWLIST((struct List *)&ae->ae_LastDocs);
    ae->ae_Owner = FindTask(NULL);

    if (!(ae->ae_Port = CreateMsgPort()))
        goto fail;

    ObtainSemaphore(&AppBase->ab_Lock);
    app_Reap(AppBase);

    ForeachNode(&AppBase->ab_Apps, other)
    {
        if (app_SameApp(other, ae->ae_Name, ae->ae_URL)
            && ((other->ae_Flags | ae->ae_Flags) & APPF_UNIQUE))
        {
            app_Post(AppBase, other, 0, APPLIBMT_Unique, NULL);
            ReleaseSemaphore(&AppBase->ab_Lock);
            goto fail;
        }
    }

    app_MakeIdent(AppBase, ae);
    ae->ae_ID = id = AppBase->ab_NextID++;
    if (!AppBase->ab_NextID)
        AppBase->ab_NextID = 1;
    ADDTAIL((struct List *)&AppBase->ab_Apps, ae);

    ReleaseSemaphore(&AppBase->ab_Lock);

    D(bug("[application] registered %s as %lu\n", ae->ae_Ident, (unsigned long)id));
    return id;

fail:
    if (ae->ae_Port)
        DeleteMsgPort(ae->ae_Port);
    FreeVec(ae->ae_Description);
    FreeVec(ae->ae_FileName);
    FreeVec(ae);
    return 0;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH2(BOOL, UnregisterApplication,

/*  SYNOPSIS */
        AROS_LHA(ULONG, appID, D0),
        AROS_LHA(struct TagItem *, tags, A0),

/*  LOCATION */
        struct ApplicationBase *, AppBase, 6, Application)

/*  FUNCTION
        Remove an application registered with RegisterApplication().
        Messages still waiting on its port are replied.

    INPUTS
        appID - the application ID.
        tags  - none defined yet, pass NULL.

    RESULT
        FALSE if there was no such application.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct AppEntry *ae;
    struct Message *msg;

    (void)tags;

    ObtainSemaphore(&AppBase->ab_Lock);
    if (!(ae = app_Find(AppBase, appID)))
    {
        ReleaseSemaphore(&AppBase->ab_Lock);
        return FALSE;
    }
    REMOVE(ae);

    if (ae->ae_Flags & APPF_GAMEMODE)
        app_Broadcast(AppBase, ae->ae_ID, APPLIBMT_GameModeLeft);

    while ((msg = GetMsg(ae->ae_Port)))
        ReplyMsg(msg);
    app_Reap(AppBase);
    ReleaseSemaphore(&AppBase->ab_Lock);

    /*
     * The port's signal belongs to the task that registered. From any other
     * task, freeing it would release that task's signal bit here instead,
     * so the port is left allocated.
     */
    if (FindTask(NULL) == ae->ae_Owner)
        DeleteMsgPort(ae->ae_Port);

    app_FreeDocs(ae);
    FreeVec(ae->ae_Description);
    FreeVec(ae->ae_FileName);
    FreeVec(ae);
    return TRUE;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH2(BOOL, SetApplicationAttrsA,

/*  SYNOPSIS */
        AROS_LHA(ULONG, appID, D0),
        AROS_LHA(struct TagItem *, tags, A0),

/*  LOCATION */
        struct ApplicationBase *, AppBase, 7, Application)

/*  FUNCTION
        Change the attributes of an application, see APPATTR_ tags marked
        (S) in <libraries/application.h>.

    RESULT
        FALSE if there was no such application or a tag cannot be set.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct TagItem *tstate = tags, *tag;
    struct AppEntry *ae;
    BOOL ok = TRUE;

    ObtainSemaphore(&AppBase->ab_Lock);
    app_Reap(AppBase);
    if (!(ae = app_Find(AppBase, appID)))
    {
        ReleaseSemaphore(&AppBase->ab_Lock);
        return FALSE;
    }

    while ((tag = NextTagItem(&tstate)))
    {
        ULONG flag;

        switch (tag->ti_Tag)
        {
        case APPATTR_NeedsGameMode:
            if (!tag->ti_Data != !(ae->ae_Flags & APPF_GAMEMODE))
            {
                ae->ae_Flags ^= APPF_GAMEMODE;
                app_Broadcast(AppBase, ae->ae_ID,
                              tag->ti_Data ? APPLIBMT_GameModeEntered : APPLIBMT_GameModeLeft);
            }
            break;

        case APPATTR_AppOpenedDocument:
            app_AddDoc(ae, (CONST_STRPTR)tag->ti_Data);
            break;

        case APPATTR_ClearLastUsedDocs:
            if (tag->ti_Data)
                app_FreeDocs(ae);
            break;

        case APPATTR_FlushPrefs:
        case APPATTR_IconType:
        case APPATTR_MainPrefsDict:
            break;

        default:
            flag = app_FlagFor(tag->ti_Tag);
            if (flag && flag != APPF_UNIQUE)
            {
                if (tag->ti_Data)
                    ae->ae_Flags |= flag;
                else
                    ae->ae_Flags &= ~flag;
            }
            else
                ok = FALSE;
            break;
        }
    }
    ReleaseSemaphore(&AppBase->ab_Lock);

    return ok;

    AROS_LIBFUNC_EXIT
}

static struct List *app_DocList(struct AppEntry *ae)
{
    struct List *list;
    struct AppDoc *ad;

    if (!(list = AllocVec(sizeof(struct List), MEMF_ANY)))
        return NULL;
    NEWLIST(list);

    ForeachNode(&ae->ae_LastDocs, ad)
    {
        struct ApplicationNode *an;
        ULONG len = strlen(ad->ad_Path) + 1;

        if (!(an = AllocVec(sizeof(*an) + len, MEMF_ANY | MEMF_CLEAR)))
            break;
        an->an_Node.ln_Name = (STRPTR)(an + 1);
        CopyMem(ad->ad_Path, an->an_Node.ln_Name, len);
        ADDTAIL(list, an);
    }
    return list;
}

/*****************************************************************************

    NAME */
        AROS_LH2(ULONG, GetApplicationAttrsA,

/*  SYNOPSIS */
        AROS_LHA(ULONG, appID, D0),
        AROS_LHA(struct TagItem *, tags, A0),

/*  LOCATION */
        struct ApplicationBase *, AppBase, 8, Application)

/*  FUNCTION
        Read attributes of an application. ti_Data points to the storage:
        an APTR or STRPTR for pointer attributes, a ULONG for the others.
        Strings stay valid while the application is registered.

    RESULT
        The number of attributes read.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct TagItem *tstate = tags, *tag;
    struct AppEntry *ae;
    ULONG count = 0;

    ObtainSemaphore(&AppBase->ab_Lock);
    app_Reap(AppBase);
    if (!(ae = app_Find(AppBase, appID)))
    {
        ReleaseSemaphore(&AppBase->ab_Lock);
        return 0;
    }

    while ((tag = NextTagItem(&tstate)))
    {
        APTR *pstore = (APTR *)tag->ti_Data;
        ULONG *lstore = (ULONG *)tag->ti_Data;
        ULONG flag;

        if (!tag->ti_Data)
            continue;

        switch (tag->ti_Tag)
        {
        case APPATTR_Port:          *pstore = ae->ae_Port; break;
        case APPATTR_Name:          *pstore = ae->ae_Name; break;
        case APPATTR_AppIdentifier: *pstore = ae->ae_Ident; break;
        case APPATTR_URLIdentifier: *pstore = ae->ae_URL; break;
        case APPATTR_Description:   *pstore = ae->ae_Description; break;
        case APPATTR_FileName:      *pstore = ae->ae_FileName; break;
        case APPATTR_LastUsedDocs:  *pstore = app_DocList(ae); break;
        case APPATTR_NeedsGameMode: *lstore = (ae->ae_Flags & APPF_GAMEMODE) != 0; break;
        case APPATTR_UniqueApplication: *lstore = (ae->ae_Flags & APPF_UNIQUE) != 0; break;
        default:
            if (!(flag = app_FlagFor(tag->ti_Tag)))
                continue;
            *lstore = (ae->ae_Flags & flag) != 0;
            break;
        }
        count++;
    }
    ReleaseSemaphore(&AppBase->ab_Lock);

    return count;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH1(ULONG, FindApplicationA,

/*  SYNOPSIS */
        AROS_LHA(struct TagItem *, tags, A0),

/*  LOCATION */
        struct ApplicationBase *, AppBase, 9, Application)

/*  FUNCTION
        Find a registered application by FINDAPP_Name (the first instance),
        FINDAPP_AppIdentifier (one instance) or FINDAPP_FileName.

    RESULT
        The application ID, or 0.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    CONST_STRPTR name = (CONST_STRPTR)GetTagData(FINDAPP_Name, 0, tags);
    CONST_STRPTR ident = (CONST_STRPTR)GetTagData(FINDAPP_AppIdentifier, 0, tags);
    CONST_STRPTR file = (CONST_STRPTR)GetTagData(FINDAPP_FileName, 0, tags);
    struct AppEntry *ae;
    ULONG id = 0;

    ObtainSemaphore(&AppBase->ab_Lock);
    ForeachNode(&AppBase->ab_Apps, ae)
    {
        if ((name && Stricmp(ae->ae_Name, name) == 0)
            || (ident && Stricmp(ae->ae_Ident, ident) == 0)
            || (file && ae->ae_FileName && Stricmp(ae->ae_FileName, file) == 0))
        {
            id = ae->ae_ID;
            break;
        }
    }
    ReleaseSemaphore(&AppBase->ab_Lock);

    return id;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH4(BOOL, SendApplicationMsg,

/*  SYNOPSIS */
        AROS_LHA(ULONG, senderAppID, D0),
        AROS_LHA(ULONG, receiverAppID, D1),
        AROS_LHA(struct ApplicationMsg *, msg, A0),
        AROS_LHA(ULONG, type, D2),

/*  LOCATION */
        struct ApplicationBase *, AppBase, 10, Application)

/*  FUNCTION
        Send a message to an application, or with receiverAppID 0 to every
        application except the sender. The library sends a copy: msg, and
        the string of an APPLIBMT_CustomMsg, APPLIBMT_OpenDoc or
        APPLIBMT_PrintDoc, can be reused at once. msg may be NULL for the
        other types.

    RESULT
        TRUE if at least one application was sent the message.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    CONST_STRPTR text = NULL;
    struct AppEntry *ae;
    BOOL sent = FALSE;

    if (msg)
    {
        if (type == APPLIBMT_CustomMsg)
            text = ((struct ApplicationCustomMsg *)msg)->customMsg;
        else if (type == APPLIBMT_OpenDoc || type == APPLIBMT_PrintDoc)
            text = ((struct ApplicationOpenPrintDocMsg *)msg)->fileName;
    }

    ObtainSemaphore(&AppBase->ab_Lock);
    app_Reap(AppBase);
    ForeachNode(&AppBase->ab_Apps, ae)
    {
        if ((receiverAppID && ae->ae_ID != receiverAppID)
            || (!receiverAppID && ae->ae_ID == senderAppID))
            continue;
        if (app_Post(AppBase, ae, senderAppID, type, text))
            sent = TRUE;
    }
    ReleaseSemaphore(&AppBase->ab_Lock);

    return sent;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH0(struct List *, GetApplicationList,

/*  SYNOPSIS */

/*  LOCATION */
        struct ApplicationBase *, AppBase, 12, Application)

/*  FUNCTION
        A snapshot of the registered applications, as a list of
        struct ApplicationNode. Free it with FreeApplicationList().

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct List *list;
    struct AppEntry *ae;

    if (!(list = AllocVec(sizeof(struct List), MEMF_ANY)))
        return NULL;
    NEWLIST(list);

    ObtainSemaphore(&AppBase->ab_Lock);
    ForeachNode(&AppBase->ab_Apps, ae)
    {
        struct ApplicationNode *an;
        ULONG il = strlen(ae->ae_Ident) + 1, nl = strlen(ae->ae_Name) + 1;
        ULONG dl = ae->ae_Description ? strlen(ae->ae_Description) + 1 : 0;
        ULONG fl = ae->ae_FileName ? strlen(ae->ae_FileName) + 1 : 0;
        STRPTR p;

        if (!(an = AllocVec(sizeof(*an) + il + nl + dl + fl, MEMF_ANY | MEMF_CLEAR)))
            break;
        p = (STRPTR)(an + 1);
        an->an_Node.ln_Name = p;
        CopyMem(ae->ae_Ident, p, il);
        p += il;
        an->an_Name = p;
        CopyMem(ae->ae_Name, p, nl);
        p += nl;
        if (dl)
        {
            an->an_Description = p;
            CopyMem(ae->ae_Description, p, dl);
            p += dl;
        }
        if (fl)
        {
            an->an_FileName = p;
            CopyMem(ae->ae_FileName, p, fl);
        }
        an->an_AppID = ae->ae_ID;
        an->an_Hidden = (ae->ae_Flags & APPF_HIDDEN) != 0;
        ADDTAIL(list, an);
    }
    ReleaseSemaphore(&AppBase->ab_Lock);

    return list;

    AROS_LIBFUNC_EXIT
}

/*****************************************************************************

    NAME */
        AROS_LH1(VOID, FreeApplicationList,

/*  SYNOPSIS */
        AROS_LHA(struct List *, list, A0),

/*  LOCATION */
        struct ApplicationBase *, AppBase, 13, Application)

/*  FUNCTION
        Free a list from GetApplicationList() or APPATTR_LastUsedDocs.

*****************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct Node *node;

    (void)AppBase;
    if (!list)
        return;
    while ((node = REMHEAD(list)))
        FreeVec(node);
    FreeVec(list);

    AROS_LIBFUNC_EXIT
}
