/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    PrintQueue - the jobs printspool.device holds: their printer unit, size and
    state, with the spooler's progress, and Cancel.
*/

#include <aros/asmcall.h>
#include <clib/alib_protos.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <exec/memory.h>
#include <libraries/mui.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/utility.h>

#include <stdio.h>
#include <string.h>

#define REFRESH_MS 1000

const char version[] = "$VER: PrintQueue 1.0 (15.10.2026)";

struct Job
{
    TEXT name[64];      /* without the suffix */
    TEXT unit[8];
    TEXT size[16];
    TEXT state[128];
    BOOL writing;       /* still being spooled */
};

static Object *app, *win, *list, *status, *cancel, *cancel_all;
static TEXT spool_dir[256];

AROS_UFH3(APTR, construct_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(APTR, pool, A2), AROS_UFHA(struct Job *, job, A1))
{
    AROS_USERFUNC_INIT
    struct Job *copy = AllocPooled(pool, sizeof(*copy));

    if (copy != NULL)
        *copy = *job;
    return copy;
    AROS_USERFUNC_EXIT
}

AROS_UFH3(void, destruct_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(APTR, pool, A2), AROS_UFHA(struct Job *, job, A1))
{
    AROS_USERFUNC_INIT
    FreePooled(pool, job, sizeof(*job));
    AROS_USERFUNC_EXIT
}

AROS_UFH3(void, display_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(STRPTR *, columns, A2), AROS_UFHA(struct Job *, job, A1))
{
    AROS_USERFUNC_INIT
    if (job == NULL)
    {
        columns[0] = "\033bJob";
        columns[1] = "\033bUnit";
        columns[2] = "\033bSize";
        columns[3] = "\033bState";
    }
    else
    {
        columns[0] = job->name;
        columns[1] = job->unit;
        columns[2] = job->size;
        columns[3] = job->state;
    }
    AROS_USERFUNC_EXIT
}

static struct Hook construct_hook, destruct_hook, display_hook;

static void read_spool_dir(void)
{
    if (GetVar("PrintSpool/Dir", spool_dir, sizeof(spool_dir), GVF_GLOBAL_ONLY) <= 0)
        strcpy(spool_dir, "T:PrintSpool");
}

/* The spooler's state line, and the job it names ("" when idle). */
static void spooler_state(STRPTR line, LONG size, STRPTR active, LONG asize)
{
    STRPTR colon;

    if (GetVar("PrintSpool/Status", line, size, GVF_GLOBAL_ONLY) <= 0)
        strcpy(line, "idle");
    active[0] = '\0';
    colon = strchr(line, ':');
    if (colon != NULL && colon - line < asize)
    {
        CopyMem(line, active, colon - line);
        active[colon - line] = '\0';
    }
}

static void refresh(void)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    TEXT line[200], active[64], selected[64] = "";
    struct Job *sel = NULL;
    BPTR lock;
    LONG count = 0, i, n;

    read_spool_dir();
    spooler_state(line, sizeof(line), active, sizeof(active));
    DoMethod(list, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &sel);
    if (sel != NULL)
        strcpy(selected, sel->name);

    set(list, MUIA_List_Quiet, TRUE);
    DoMethod(list, MUIM_List_Clear);
    lock = Lock(spool_dir, SHARED_LOCK);
    if (lock != BNULL && fib != NULL && Examine(lock, fib))
    {
        while (ExNext(lock, fib))
        {
            struct Job job;
            LONG len = strlen(fib->fib_FileName);
            STRPTR u;

            memset(&job, 0, sizeof(job));
            if (len > 4 && strcmp(fib->fib_FileName + len - 4, ".job") == 0)
                job.writing = FALSE;
            else if (len > 4 && strcmp(fib->fib_FileName + len - 4, ".tmp") == 0)
                job.writing = TRUE;
            else
                continue;
            if (len - 4 >= (LONG)sizeof(job.name))
                continue;
            CopyMem(fib->fib_FileName, job.name, len - 4);
            job.name[len - 4] = '\0';
            u = strstr(job.name, "-u");
            snprintf(job.unit, sizeof(job.unit), "%s", u != NULL ? (const char *)u + 2 : "?");
            snprintf(job.size, sizeof(job.size), "%ld KB", (long)((fib->fib_Size + 1023) / 1024));
            if (job.writing)
                strcpy(job.state, "being spooled");
            else if (strcmp(job.name, active) == 0)
                snprintf(job.state, sizeof(job.state), "%s", line + strlen(active) + 2);
            else
                strcpy(job.state, "queued");
            /* Sorted by name, which is the order the spooler prints in. */
            n = 0;
            get(list, MUIA_List_Entries, &n);
            for (i = 0; i < n; i++)
            {
                struct Job *other = NULL;

                DoMethod(list, MUIM_List_GetEntry, i, &other);
                if (other != NULL && strcmp(job.name, other->name) < 0)
                    break;
            }
            DoMethod(list, MUIM_List_InsertSingle, &job, i);
            count++;
        }
    }
    if (lock != BNULL)
        UnLock(lock);
    FreeDosObject(DOS_FIB, fib);

    n = 0;
    get(list, MUIA_List_Entries, &n);
    for (i = 0; i < n && selected[0] != '\0'; i++)
    {
        struct Job *other = NULL;

        DoMethod(list, MUIM_List_GetEntry, i, &other);
        if (other != NULL && strcmp(other->name, selected) == 0)
            set(list, MUIA_List_Active, i);
    }
    set(list, MUIA_List_Quiet, FALSE);

    snprintf(line + strlen(line), sizeof(line) - strlen(line), "  -  %ld job%s in %s",
        (long)count, count == 1 ? "" : "s", spool_dir);
    set(status, MUIA_Text_Contents, line);
}

/* The timer input handler calls one method without arguments: this
   object's only method refreshes the list. */
#define MUIM_PrintQueue_Refresh (TAG_USER | 0x50510001)

static struct MUI_CustomClass *refresher_class;

BOOPSI_DISPATCHER(IPTR, refresher_dispatcher, cl, obj, msg)
{
    if (msg->MethodID == MUIM_PrintQueue_Refresh)
    {
        refresh();
        return 0;
    }
    return DoSuperMethodA(cl, obj, msg);
}
BOOPSI_DISPATCHER_END

/* A job is dropped by the spooler when <job>.cancel exists. */
static void cancel_job(struct Job *job)
{
    TEXT path[300];
    BPTR file;

    strcpy(path, spool_dir);
    AddPart(path, job->name, sizeof(path));
    strcat(path, ".cancel");
    file = Open(path, MODE_NEWFILE);
    if (file != BNULL)
        Close(file);
}

int main(void)
{
    struct MUI_InputHandlerNode timer;
    Object *refresher;
    ULONG sigs = 0;
    BOOL running = TRUE;

    construct_hook.h_Entry = (HOOKFUNC)construct_func;
    destruct_hook.h_Entry = (HOOKFUNC)destruct_func;
    display_hook.h_Entry = (HOOKFUNC)display_func;

    app = ApplicationObject,
        MUIA_Application_Title, (IPTR)"PrintQueue",
        MUIA_Application_Version, (IPTR)version,
        MUIA_Application_Description, (IPTR)"Jobs waiting for the printer",
        MUIA_Application_Base, (IPTR)"PRINTQUEUE",
        SubWindow, (IPTR)(win = WindowObject,
            MUIA_Window_Title, (IPTR)"Print Queue",
            MUIA_Window_ID, MAKE_ID('P', 'R', 'Q', 'U'),
            MUIA_Window_Width, MUIV_Window_Width_Screen(40),
            MUIA_Window_Height, MUIV_Window_Height_Screen(35),
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(ListviewObject,
                    MUIA_Listview_List, (IPTR)(list = ListObject,
                        InputListFrame,
                        MUIA_List_Format, (IPTR)"BAR,BAR P=\033r,BAR P=\033r,",
                        MUIA_List_Title, TRUE,
                        MUIA_List_ConstructHook, (IPTR)&construct_hook,
                        MUIA_List_DestructHook, (IPTR)&destruct_hook,
                        MUIA_List_DisplayHook, (IPTR)&display_hook,
                        End),
                    End),
                Child, (IPTR)(status = TextObject, TextFrame, MUIA_Background, MUII_TextBack, End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(cancel = SimpleButton("_Cancel job")),
                    Child, (IPTR)(cancel_all = SimpleButton("Cancel _all")),
                    End),
                End),
            End),
        End;
    if (app == NULL)
        return RETURN_FAIL;

    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2,
        MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(cancel, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, 1);
    DoMethod(cancel_all, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, 2);

    refresher_class = MUI_CreateCustomClass(NULL, (ClassID)MUIC_Notify, NULL, 0, refresher_dispatcher);
    refresher = refresher_class != NULL ? NewObject(refresher_class->mcc_Class, NULL, TAG_END) : NULL;
    if (refresher == NULL)
    {
        MUI_DisposeObject(app);
        return RETURN_FAIL;
    }
    memset(&timer, 0, sizeof(timer));
    timer.ihn_Object = refresher;
    timer.ihn_Flags = MUIIHNF_TIMER;
    timer.ihn_Millis = REFRESH_MS;
    timer.ihn_Method = MUIM_PrintQueue_Refresh;
    DoMethod(app, MUIM_Application_AddInputHandler, (IPTR)&timer);

    refresh();
    set(win, MUIA_Window_Open, TRUE);
    while (running)
    {
        IPTR id = DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs);

        switch (id)
        {
        case MUIV_Application_ReturnID_Quit:
            running = FALSE;
            break;
        case 1:
        {
            struct Job *job = NULL;

            DoMethod(list, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &job);
            if (job != NULL && !job->writing)
                cancel_job(job);
            refresh();
            break;
        }
        case 2:
        {
            LONG n = 0, i;

            get(list, MUIA_List_Entries, &n);
            for (i = 0; i < n; i++)
            {
                struct Job *job = NULL;

                DoMethod(list, MUIM_List_GetEntry, i, &job);
                if (job != NULL && !job->writing)
                    cancel_job(job);
            }
            refresh();
            break;
        }
        default:
            break;
        }
        if (running && sigs)
        {
            sigs = Wait(sigs | SIGBREAKF_CTRL_C);
            if (sigs & SIGBREAKF_CTRL_C)
                running = FALSE;
        }
    }
    DoMethod(app, MUIM_Application_RemInputHandler, (IPTR)&timer);
    MUI_DisposeObject(app);
    DisposeObject(refresher);
    MUI_DeleteCustomClass(refresher_class);
    return RETURN_OK;
}
