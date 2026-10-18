/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: application.library test. Prints one PASS/FAIL line per check;
          with WAITBACK=n it then waits n seconds for the pop-up's
          double-click message.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/application.h>
#include <rexx/storage.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/application.h>
#include <proto/rexxsyslib.h>

#include <stdio.h>
#include <string.h>

struct Library *ApplicationBase;
struct RxsLib *RexxSysBase;

static int passed, failed;

static void check(BOOL ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok)
        passed++;
    else
        failed++;
}

/* Take every message waiting on a port; return how many had 'type'. */
static int drain(struct MsgPort *port, ULONG type, char *text, int textsize)
{
    struct ApplicationMsg *am;
    int n = 0;

    while ((am = (struct ApplicationMsg *)GetMsg(port)))
    {
        if (am->type == type)
        {
            n++;
            if (text && type == APPLIBMT_CustomMsg)
            {
                strncpy(text, ((struct ApplicationCustomMsg *)am)->customMsg, textsize - 1);
                text[textsize - 1] = '\0';
            }
        }
        ReplyMsg(&am->msg);
    }
    return n;
}

/*
 * Send one command to an ARexx port the way Hollywood's SendRexxCommand()
 * does: a RexxMsg built with rexxsyslib, no interpreter involved.
 * Returns rm_Result1, or -1 if the port is missing.
 */
static LONG rexx(CONST_STRPTR portname, CONST_STRPTR command)
{
    struct MsgPort *reply, *port;
    struct RexxMsg *rm;
    LONG rc = -1;

    if (!RexxSysBase || !(reply = CreateMsgPort()))
        return -1;
    if ((rm = CreateRexxMsg(reply, NULL, NULL)))
    {
        rm->rm_Args[0] = (IPTR)CreateArgstring((STRPTR)command, strlen(command));
        rm->rm_Action = RXCOMM;
        Forbid();
        if ((port = FindPort(portname)))
            PutMsg(port, &rm->rm_Node);
        Permit();
        if (port)
        {
            WaitPort(reply);
            GetMsg(reply);
            rc = rm->rm_Result1;
        }
        DeleteArgstring((UBYTE *)rm->rm_Args[0]);
        DeleteRexxMsg(rm);
    }
    DeleteMsgPort(reply);
    return rc;
}

int main(void)
{
    struct RDArgs *rda;
    IPTR args[2] = { 0, 0 };
    LONG waitback = 0;
    STRPTR image = NULL;
    ULONG id1, id2, id3, id4;
    struct MsgPort *p1 = NULL, *p2 = NULL, *p3 = NULL;
    STRPTR ident = NULL;
    struct ApplicationCustomMsg cm;
    struct List *list;
    struct Node *n;
    char text[64];
    int count;

    if (!(rda = ReadArgs("WAITBACK/K/N,IMAGE/K", args, NULL)))
    {
        PrintFault(IoErr(), "AppTest");
        return RETURN_FAIL;
    }
    if (args[0])
        waitback = *(LONG *)args[0];
    image = (STRPTR)args[1];

    if (!(ApplicationBase = OpenLibrary("application.library", 1)))
    {
        printf("FAIL cannot open application.library\n");
        return RETURN_FAIL;
    }

    id1 = RegisterApplication("AppTest", REGAPP_URLIdentifier, (IPTR)"www.aros.org",
                              REGAPP_Description, (IPTR)"application.library test", TAG_DONE);
    check(id1 != 0, "register first instance");
    GetApplicationAttrs(id1, APPATTR_AppIdentifier, (IPTR)&ident, APPATTR_Port, (IPTR)&p1, TAG_DONE);
    check(ident && strcmp(ident, "AppTest.aros.org") == 0, "identifier name.url, www. stripped");
    check(p1 != NULL, "first instance has a port");

    id2 = RegisterApplication("AppTest", REGAPP_URLIdentifier, (IPTR)"aros.org", TAG_DONE);
    ident = NULL;
    GetApplicationAttrs(id2, APPATTR_AppIdentifier, (IPTR)&ident, APPATTR_Port, (IPTR)&p2, TAG_DONE);
    check(id2 && id2 != id1 && ident && strcmp(ident, "AppTest_1.aros.org") == 0,
          "second instance is AppTest_1.aros.org");

    check(FindApplication(FINDAPP_Name, (IPTR)"apptest", TAG_DONE) == id1, "find by name (first instance)");
    check(FindApplication(FINDAPP_AppIdentifier, (IPTR)"AppTest_1.aros.org", TAG_DONE) == id2,
          "find by identifier");
    check(FindApplication(FINDAPP_Name, (IPTR)"NoSuchApp", TAG_DONE) == 0, "find unknown is 0");

    memset(&cm, 0, sizeof(cm));
    strcpy(text, "hello");
    cm.customMsg = text;
    check(SendApplicationMsg(id1, id2, &cm.almsg, APPLIBMT_CustomMsg), "send custom message");
    strcpy(text, "clobbered");          /* the library must have copied it */
    text[0] = '\0';
    count = drain(p2, APPLIBMT_CustomMsg, text, sizeof(text));
    check(count == 1 && strcmp(text, "hello") == 0, "custom message arrives, string copied");

    check(SendApplicationMsg(id1, 0, NULL, APPLIBMT_ToFront), "broadcast");
    check(drain(p2, APPLIBMT_ToFront, NULL, 0) == 1 && drain(p1, APPLIBMT_ToFront, NULL, 0) == 0,
          "broadcast skips the sender");

    id3 = RegisterApplication("UniqApp", REGAPP_UniqueApplication, TRUE, TAG_DONE);
    GetApplicationAttrs(id3, APPATTR_Port, (IPTR)&p3, TAG_DONE);
    id4 = RegisterApplication("UniqApp", TAG_DONE);
    check(id3 && !id4, "second instance of a unique application refused");
    check(p3 && drain(p3, APPLIBMT_Unique, NULL, 0) == 1, "running instance told APPLIBMT_Unique");

    SetApplicationAttrs(id1, APPATTR_NeedsGameMode, TRUE, TAG_DONE);
    check(drain(p2, APPLIBMT_GameModeEntered, NULL, 0) == 1 && drain(p3, APPLIBMT_GameModeEntered, NULL, 0) == 1,
          "game mode broadcast to the others");
    SetApplicationAttrs(id1, APPATTR_NeedsGameMode, FALSE, TAG_DONE);
    drain(p2, APPLIBMT_GameModeLeft, NULL, 0);
    drain(p3, APPLIBMT_GameModeLeft, NULL, 0);

    SetApplicationAttrs(id1, APPATTR_AppOpenedDocument, (IPTR)"RAM:a.txt", TAG_DONE);
    SetApplicationAttrs(id1, APPATTR_AppOpenedDocument, (IPTR)"RAM:b.txt", TAG_DONE);
    SetApplicationAttrs(id1, APPATTR_AppOpenedDocument, (IPTR)"RAM:a.txt", TAG_DONE);
    list = NULL;
    GetApplicationAttrs(id1, APPATTR_LastUsedDocs, (IPTR)&list, TAG_DONE);
    count = 0;
    if (list)
        ForeachNode(list, n)
        {
            if ((count == 0 && strcmp(n->ln_Name, "RAM:a.txt") == 0)
                || (count == 1 && strcmp(n->ln_Name, "RAM:b.txt") == 0))
                count++;
            else
                count = -100;
        }
    FreeApplicationList(list);
    check(count == 2, "last used documents: most recent first, no duplicates");

    list = GetApplicationList();
    count = 0;
    if (list)
        ForeachNode(list, n)
        {
            if (strcmp(n->ln_Name, "AppTest.aros.org") == 0 || strcmp(n->ln_Name, "AppTest_1.aros.org") == 0
                || strcmp(n->ln_Name, "UniqApp") == 0)
                count++;
        }
    FreeApplicationList(list);
    check(count == 3, "application list has all three");

    check(Notify(id1,
                 APPNOTIFY_Title,     (IPTR)"application.library",
                 APPNOTIFY_Text,      (IPTR)"Notifications work. Double-click to send a message back.",
                 APPNOTIFY_BackMsg,   (IPTR)"back-from-popup",
                 APPNOTIFY_CloseOnDC, TRUE,
                 TAG_DONE) != 0, "Notify() shows a pop-up (server started on demand)");
    check(Notify(id1, APPNOTIFY_Title, (IPTR)"no text", TAG_DONE) == 0, "Notify() without text is refused");

    if (waitback > 0)
    {
        ULONG left = waitback * 50;
        BOOL got = FALSE;

        text[0] = '\0';
        while (left-- && !got)
        {
            Delay(1);
            got = drain(p1, APPLIBMT_CustomMsg, text, sizeof(text)) > 0;
        }
        check(got && strcmp(text, "back-from-popup") == 0, "double-click sent APPNOTIFY_BackMsg back");
    }

    RexxSysBase = (struct RxsLib *)OpenLibrary("rexxsyslib.library", 0);
    check(rexx("RINGHIO", "REGISTERAPP APP=RexxTest") == 0, "ARexx REGISTERAPP at the RINGHIO port");
    check(FindApplication(FINDAPP_Name, (IPTR)"RexxTest", TAG_DONE) != 0, "ARexx registration is a real application");
    check(rexx("RINGHIO", "RINGHIO APP=RexxTest CLOSEONDC SCREEN=\"FRONT\" TITLE=\"ARexx\" IMGVALIGN=1 "
                          "TEXT=Hello from the RINGHIO port, as Hollywood sends it") == 0,
          "ARexx RINGHIO pop-up");
    check(rexx("RINGHIO", "NOSUCHCOMMAND") != 0, "ARexx unknown command fails");
    if (RexxSysBase)
        CloseLibrary((struct Library *)RexxSysBase);

    check(UnregisterApplication(id3, NULL) && UnregisterApplication(id2, NULL)
          && UnregisterApplication(id1, NULL), "unregister");
    check(FindApplication(FINDAPP_Name, (IPTR)"AppTest", TAG_DONE) == 0, "gone after unregister");
    check(!UnregisterApplication(id1, NULL), "unregister twice is refused");

    if (image)
    {
        ULONG id5 = RegisterApplication("AppTestImage", TAG_DONE);

        check(Notify(id5, APPNOTIFY_Title, (IPTR)"With an image",
                     APPNOTIFY_Text, (IPTR)"The picture is scaled to 32x32 and drawn with its alpha.",
                     APPNOTIFY_ImageFile, (IPTR)image, APPNOTIFY_ImageVertAlign, 1, TAG_DONE) != 0,
              "pop-up with an image");
        check(Notify(id5, APPNOTIFY_Title, (IPTR)"Stacked",
                     APPNOTIFY_Text, (IPTR)"A second pop-up stacks below the first.", TAG_DONE) != 0,
              "second pop-up stacks");
        UnregisterApplication(id5, NULL);
    }

    printf("APPTEST %s %d passed, %d failed\n", failed ? "FAILED" : "OK", passed, failed);

    CloseLibrary(ApplicationBase);
    FreeArgs(rda);
    return failed ? RETURN_ERROR : RETURN_OK;
}
