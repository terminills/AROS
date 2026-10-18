#ifndef APPLICATION_INTERN_H
#define APPLICATION_INTERN_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: application.library private state.
*/

#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/lists.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <libraries/application.h>

#define APP_NAMEMAX     64
#define APP_IDENTMAX    200
#define APP_LASTDOCS    10

#define APPF_UNIQUE         (1 << 0)
#define APPF_ICONIFY        (1 << 1)
#define APPF_PREFSWINDOW    (1 << 2)
#define APPF_NEWDOCS        (1 << 3)
#define APPF_PRINTDOCS      (1 << 4)
#define APPF_APPNOTIFY      (1 << 5)
#define APPF_BLANKNOTIFY    (1 << 6)
#define APPF_ALLOWBLANKER   (1 << 7)
#define APPF_SAVEPREFS      (1 << 8)
#define APPF_HIDDEN         (1 << 9)
#define APPF_GAMEMODE       (1 << 10)

struct AppDoc
{
    struct MinNode  ad_Node;
    TEXT            ad_Path[1];     /* allocated to fit */
};

struct AppEntry
{
    struct MinNode   ae_Node;
    ULONG            ae_ID;
    ULONG            ae_Flags;
    struct MsgPort  *ae_Port;       /* created in, and owned by, ae_Owner */
    struct Task     *ae_Owner;
    STRPTR           ae_Description;
    STRPTR           ae_FileName;
    struct MinList   ae_LastDocs;
    TEXT             ae_Name[APP_NAMEMAX];
    TEXT             ae_URL[APP_NAMEMAX * 2];
    TEXT             ae_Ident[APP_IDENTMAX];
};

struct ApplicationBase
{
    struct Library          ab_Lib;
    struct SignalSemaphore  ab_Lock;
    struct MinList          ab_Apps;
    ULONG                   ab_NextID;
    /*
     * Messages the library sends come back here when the receiver replies.
     * The port never signals; replied messages are freed on the next call.
     */
    struct MsgPort          ab_Reaper;
    ULONG                   ab_Outstanding;     /* sent, not yet freed */
};

extern struct DosLibrary *DOSBase;
extern struct UtilityBase *UtilityBase;

/* Callers hold ab_Lock. */
struct AppEntry *app_Find(struct ApplicationBase *AppBase, ULONG appID);
void app_Reap(struct ApplicationBase *AppBase);
BOOL app_Post(struct ApplicationBase *AppBase, struct AppEntry *to, ULONG sender,
              ULONG type, CONST_STRPTR text);
void app_Broadcast(struct ApplicationBase *AppBase, ULONG sender, ULONG type);
STRPTR app_StrDup(CONST_STRPTR s);

#endif /* APPLICATION_INTERN_H */
