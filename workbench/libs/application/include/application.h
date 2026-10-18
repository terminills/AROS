#ifndef LIBRARIES_APPLICATION_H
#define LIBRARIES_APPLICATION_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: application.library - registered applications, messages between
          them, and pop-up notifications.

    The names follow the AmigaOS 4 application.library, so its programs
    build unchanged. The tag and message type VALUES are AROS's own.
*/

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/lists.h>
#include <utility/tagitem.h>

#define APPLICATIONNAME "application.library"

/* RegisterApplication() */
#define REGAPP_Dummy                    (TAG_USER + 0x4A0000)
#define REGAPP_URLIdentifier            (REGAPP_Dummy + 1)  /* STRPTR: "supercoders.com" */
#define REGAPP_WBStartup                (REGAPP_Dummy + 2)  /* struct WBStartup *        */
#define REGAPP_FileName                 (REGAPP_Dummy + 3)  /* STRPTR: program path      */
#define REGAPP_FileLock                 (REGAPP_Dummy + 4)  /* BPTR: lock on the program */
#define REGAPP_Description              (REGAPP_Dummy + 5)  /* STRPTR, fixed once set    */
#define REGAPP_UniqueApplication        (REGAPP_Dummy + 6)  /* BOOL, fixed once set      */
#define REGAPP_HasIconifyFeature        (REGAPP_Dummy + 7)  /* BOOL                      */
#define REGAPP_HasPrefsWindow           (REGAPP_Dummy + 8)  /* BOOL                      */
#define REGAPP_CanCreateNewDocs         (REGAPP_Dummy + 9)  /* BOOL                      */
#define REGAPP_CanPrintDocs             (REGAPP_Dummy + 10) /* BOOL                      */
#define REGAPP_AppNotifications         (REGAPP_Dummy + 11) /* BOOL                      */
#define REGAPP_BlankerNotifications     (REGAPP_Dummy + 12) /* BOOL                      */
#define REGAPP_AllowsBlanker            (REGAPP_Dummy + 13) /* BOOL, default TRUE        */
#define REGAPP_SavePrefs                (REGAPP_Dummy + 14) /* BOOL                      */

/* Get/SetApplicationAttrs(). Pointer values are read into an APTR or
   STRPTR, everything else into a ULONG. */
#define APPATTR_Dummy                   (TAG_USER + 0x4A1000)
#define APPATTR_Port                    (APPATTR_Dummy + 1)  /* (G)  struct MsgPort *     */
#define APPATTR_Name                    (APPATTR_Dummy + 2)  /* (G)  STRPTR               */
#define APPATTR_AppIdentifier           (APPATTR_Dummy + 3)  /* (G)  STRPTR               */
#define APPATTR_URLIdentifier           (APPATTR_Dummy + 4)  /* (G)  STRPTR               */
#define APPATTR_Description             (APPATTR_Dummy + 5)  /* (G)  STRPTR               */
#define APPATTR_FileName                (APPATTR_Dummy + 6)  /* (G)  STRPTR               */
#define APPATTR_UniqueApplication       (APPATTR_Dummy + 7)  /* (G)  BOOL                 */
#define APPATTR_Hidden                  (APPATTR_Dummy + 8)  /* (SG) BOOL                 */
#define APPATTR_HasIconifyFeature       (APPATTR_Dummy + 9)  /* (SG) BOOL                 */
#define APPATTR_HasPrefsWindow          (APPATTR_Dummy + 10) /* (SG) BOOL                 */
#define APPATTR_CanCreateNewDocs        (APPATTR_Dummy + 11) /* (SG) BOOL                 */
#define APPATTR_CanPrintDocs            (APPATTR_Dummy + 12) /* (SG) BOOL                 */
#define APPATTR_AppNotifications        (APPATTR_Dummy + 13) /* (SG) BOOL                 */
#define APPATTR_BlankerNotifications    (APPATTR_Dummy + 14) /* (SG) BOOL                 */
#define APPATTR_AllowsBlanker           (APPATTR_Dummy + 15) /* (SG) BOOL                 */
#define APPATTR_NeedsGameMode           (APPATTR_Dummy + 16) /* (SG) BOOL                 */
#define APPATTR_SavePrefs               (APPATTR_Dummy + 17) /* (SG) BOOL                 */
#define APPATTR_AppOpenedDocument       (APPATTR_Dummy + 18) /* (S)  STRPTR: document path*/
#define APPATTR_ClearLastUsedDocs       (APPATTR_Dummy + 19) /* (S)  BOOL                 */
#define APPATTR_LastUsedDocs            (APPATTR_Dummy + 20) /* (G)  struct List * (copy, */
                                                             /*      FreeApplicationList) */
#define APPATTR_FlushPrefs              (APPATTR_Dummy + 21) /* (S)  reserved             */
#define APPATTR_IconType                (APPATTR_Dummy + 22) /* (SG) reserved             */
#define APPATTR_MainPrefsDict           (APPATTR_Dummy + 23) /* (SG) reserved             */

/* FindApplication() */
#define FINDAPP_Dummy                   (TAG_USER + 0x4A2000)
#define FINDAPP_Name                    (FINDAPP_Dummy + 1)  /* "AudioMonster"                   */
#define FINDAPP_AppIdentifier           (FINDAPP_Dummy + 2)  /* "AudioMonster_1.supercoders.com" */
#define FINDAPP_FileName                (FINDAPP_Dummy + 3)  /* "Work:Utils/AudioMonster"        */

/* Notify() */
#define APPNOTIFY_Dummy                 (TAG_USER + 0x4A3000)
#define APPNOTIFY_Title                 (APPNOTIFY_Dummy + 1) /* STRPTR, up to 64 chars       */
#define APPNOTIFY_Text                  (APPNOTIFY_Dummy + 2) /* STRPTR, up to 160 chars      */
#define APPNOTIFY_PubScreenName         (APPNOTIFY_Dummy + 3) /* STRPTR, "FRONT" = frontmost  */
#define APPNOTIFY_ImageFile             (APPNOTIFY_Dummy + 4) /* STRPTR, full path, 32x32     */
#define APPNOTIFY_BackMsg               (APPNOTIFY_Dummy + 5) /* STRPTR, "URL:..." opens it   */
#define APPNOTIFY_CloseOnDC             (APPNOTIFY_Dummy + 6) /* BOOL                         */
#define APPNOTIFY_ImageVertAlign        (APPNOTIFY_Dummy + 7) /* ULONG 0 top, 1 centre, 2 bottom */

#define APPNOTIFY_TITLE_MAX             64
#define APPNOTIFY_TEXT_MAX              160

/* Message types, in struct ApplicationMsg.type */
#define APPLIBMT_Quit                   1
#define APPLIBMT_ForceQuit              2
#define APPLIBMT_Hide                   3
#define APPLIBMT_Unhide                 4
#define APPLIBMT_ToFront                5
#define APPLIBMT_OpenPrefs              6
#define APPLIBMT_ReloadPrefs            7
#define APPLIBMT_NewBlankDoc            8
#define APPLIBMT_OpenDoc                9   /* struct ApplicationOpenPrintDocMsg */
#define APPLIBMT_PrintDoc               10  /* struct ApplicationOpenPrintDocMsg */
#define APPLIBMT_Unique                 11
#define APPLIBMT_GameModeEntered        12
#define APPLIBMT_GameModeLeft           13
#define APPLIBMT_CustomMsg              14  /* struct ApplicationCustomMsg */

/*
 * Messages arrive on the port from APPATTR_Port. Reply every one with
 * ReplyMsg(); the library frees it then, so copy anything to be kept.
 */
struct ApplicationMsg
{
    struct Message msg;
    ULONG          senderAppID;     /* 0 = the system */
    ULONG          type;
};

struct ApplicationOpenPrintDocMsg
{
    struct ApplicationMsg almsg;
    STRPTR                fileName;
};

struct ApplicationCustomMsg
{
    struct ApplicationMsg almsg;
    STRPTR                customMsg;
};

/* GetApplicationList(): ln_Name is the application identifier. For
   APPATTR_LastUsedDocs only ln_Name (the path) is set. */
struct ApplicationNode
{
    struct Node an_Node;
    ULONG       an_AppID;
    STRPTR      an_Name;
    STRPTR      an_Description;
    STRPTR      an_FileName;
    BOOL        an_Hidden;
};

#endif /* LIBRARIES_APPLICATION_H */
