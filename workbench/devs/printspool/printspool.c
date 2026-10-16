/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    printspool.device - takes printer output into job files and sends them to
    the printer in the background, so an application's print returns as soon
    as its data is spooled.

    Unit n sends its jobs to the device named by the variable PrintSpool/Unit<n>
    ("parallel.device 0" when it is not set). Jobs go to the directory in
    PrintSpool/Dir (default T:PrintSpool) as <sequence>-u<unit>.tmp while they
    are written and .job once the last opener has closed the unit. The spooler
    sends .job files in sequence order, deletes them when done, writes its state
    ("<job>: printing on <device> unit <n>, <sent>/<size> KB", "...: waiting, ...",
    "idle") to PrintSpool/Status and drops a job when <job>.cancel exists. What
    printer.device says is being printed (PRD_SETJOBINFO) is kept in <job>.info
    as Application= and Document= lines.
*/

#include <aros/debug.h>

#include <dos/dostags.h>
#include <dos/var.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <devices/parallel.h>
#include <devices/serial.h>
#include <devices/printer.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include <string.h>

#include LC_LIBDEFS_FILE

#include "printspool_intern.h"

#define CHUNK_SIZE      65536
#define RETRY_TICKS     (5 * TICKS_PER_SECOND)
#define JOB_SUFFIX      ".job"
#define TMP_SUFFIX      ".tmp"

/* A job whose writer has closed the unit; the spooler finishes it. */
struct FinishedJob
{
    struct MinNode fj_Node;
    BPTR fj_File;
    TEXT fj_Name[64];
    TEXT fj_Application[64];
    TEXT fj_Document[128];
};

static struct MinList finished;

static void spool_dir(struct PrintSpoolBase *PrintSpoolBase, STRPTR buf, LONG size)
{
    if (GetVar("PrintSpool/Dir", buf, size, GVF_GLOBAL_ONLY) <= 0)
        strcpy(buf, "T:PrintSpool");
}

static void job_path(struct PrintSpoolBase *PrintSpoolBase, CONST_STRPTR name,
    CONST_STRPTR suffix, STRPTR buf, LONG size)
{
    spool_dir(PrintSpoolBase, buf, size);
    AddPart(buf, name, size);
    if (strlen(buf) + strlen(suffix) < (size_t)size)
        strcat(buf, suffix);
}

/* SetVar() makes the variable's file but not its directory. */
static void set_status(struct PrintSpoolBase *PrintSpoolBase, CONST_STRPTR text)
{
    BPTR lock = Lock("ENV:PrintSpool", SHARED_LOCK);

    if (lock == BNULL)
        lock = CreateDir("ENV:PrintSpool");
    if (lock != BNULL)
        UnLock(lock);
    SetVar("PrintSpool/Status", text, -1, GVF_GLOBAL_ONLY);
}

/* The highest job sequence number in the spool directory. */
static ULONG last_sequence(struct PrintSpoolBase *PrintSpoolBase)
{
    TEXT dir[256];
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR lock;
    ULONG last = 0;

    spool_dir(PrintSpoolBase, dir, sizeof(dir));
    lock = Lock(dir, SHARED_LOCK);
    if (lock == BNULL)
        lock = CreateDir(dir);
    if (lock != BNULL && fib != NULL && Examine(lock, fib))
    {
        while (ExNext(lock, fib))
        {
            ULONG n = 0;
            STRPTR c;

            for (c = fib->fib_FileName; *c >= '0' && *c <= '9'; c++)
                n = n * 10 + (*c - '0');
            if (n > last)
                last = n;
        }
    }
    if (lock != BNULL)
        UnLock(lock);
    FreeDosObject(DOS_FIB, fib);
    return last;
}

/* Oldest .job file: its name without the suffix goes to name. */
static BOOL next_job(struct PrintSpoolBase *PrintSpoolBase, STRPTR name, LONG size)
{
    TEXT dir[256];
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR lock;
    BOOL found = FALSE;

    spool_dir(PrintSpoolBase, dir, sizeof(dir));
    lock = Lock(dir, SHARED_LOCK);
    if (lock != BNULL && fib != NULL && Examine(lock, fib))
    {
        while (ExNext(lock, fib))
        {
            LONG len = strlen(fib->fib_FileName);

            if (len <= 4 || len - 4 >= size || strcmp(fib->fib_FileName + len - 4, JOB_SUFFIX) != 0)
                continue;
            /* Names start with a ten-digit sequence, so they sort by age. */
            if (!found || strncmp(fib->fib_FileName, name, len - 4) < 0)
            {
                CopyMem(fib->fib_FileName, name, len - 4);
                name[len - 4] = '\0';
                found = TRUE;
            }
        }
    }
    if (lock != BNULL)
        UnLock(lock);
    FreeDosObject(DOS_FIB, fib);
    return found;
}

static BOOL cancelled(struct PrintSpoolBase *PrintSpoolBase, CONST_STRPTR name)
{
    TEXT path[300];
    BPTR lock;

    job_path(PrintSpoolBase, name, ".cancel", path, sizeof(path));
    lock = Lock(path, SHARED_LOCK);
    if (lock == BNULL)
        return FALSE;
    UnLock(lock);
    return TRUE;
}

/* "<device> <unit>" from PrintSpool/Unit<n>. */
static void unit_target(struct PrintSpoolBase *PrintSpoolBase, LONG unit, STRPTR device,
    LONG size, LONG *devunit)
{
    TEXT var[24] = "PrintSpool/Unit0";
    TEXT value[128];
    STRPTR space;

    var[15] = '0' + unit;
    strcpy(device, "parallel.device");
    *devunit = 0;
    if (GetVar(var, value, sizeof(value), GVF_GLOBAL_ONLY) <= 0)
        return;
    space = strchr(value, ' ');
    if (space != NULL)
    {
        *space++ = '\0';
        StrToLong(space, devunit);
    }
    if ((LONG)strlen(value) < size)
        strcpy(device, value);
}

/* Writers hand over their closed jobs: close them and rename them to .job. */
static void publish_finished(struct PrintSpoolBase *PrintSpoolBase)
{
    struct FinishedJob *fj;
    TEXT from[300], to[300];

    ObtainSemaphore(&PrintSpoolBase->ps_Lock);
    while ((fj = (struct FinishedJob *)REMHEAD((struct List *)&finished)) != NULL)
    {
        ReleaseSemaphore(&PrintSpoolBase->ps_Lock);
        Close(fj->fj_File);
        if (fj->fj_Application[0] != '\0' || fj->fj_Document[0] != '\0')
        {
            BPTR info;

            job_path(PrintSpoolBase, fj->fj_Name, ".info", to, sizeof(to));
            if ((info = Open(to, MODE_NEWFILE)) != BNULL)
            {
                FPuts(info, "Application=");
                FPuts(info, fj->fj_Application);
                FPuts(info, "\nDocument=");
                FPuts(info, fj->fj_Document);
                FPuts(info, "\n");
                Close(info);
            }
        }
        job_path(PrintSpoolBase, fj->fj_Name, TMP_SUFFIX, from, sizeof(from));
        job_path(PrintSpoolBase, fj->fj_Name, JOB_SUFFIX, to, sizeof(to));
        Rename(from, to);
        FreeMem(fj, sizeof(*fj));
        ObtainSemaphore(&PrintSpoolBase->ps_Lock);
    }
    ReleaseSemaphore(&PrintSpoolBase->ps_Lock);
}

/* Takes the jobs closed by their writers, then sends .job files in order. */
static void spooler_main(void)
{
    struct PrintSpoolBase *PrintSpoolBase = FindTask(NULL)->tc_UserData;
    UBYTE *chunk = AllocMem(CHUNK_SIZE, MEMF_ANY);
    struct MsgPort *port = CreateMsgPort();
    struct IOStdReq *io = (struct IOStdReq *)CreateIORequest(port,
        sizeof(struct IOExtPar) > sizeof(struct IOExtSer) ?
        sizeof(struct IOExtPar) : sizeof(struct IOExtSer));
    ULONG sigmask = 1UL << PrintSpoolBase->ps_SpoolerSignal;
    TEXT name[64], path[300], status[200];

    set_status(PrintSpoolBase, "idle");
    for (;;)
    {
        BPTR file;
        TEXT device[64];
        LONG unit = 0, devunit, actual, sent = 0, total = 0;
        BOOL done = FALSE;

        publish_finished(PrintSpoolBase);

        if (chunk == NULL || io == NULL || !next_job(PrintSpoolBase, name, sizeof(name)))
        {
            set_status(PrintSpoolBase, "idle");
            if (Wait(sigmask | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
                break;
            continue;
        }

        {
            STRPTR u = strstr(name, "-u");

            if (u != NULL && u[2] >= '0' && u[2] <= '9')
                unit = u[2] - '0';
        }
        unit_target(PrintSpoolBase, unit, device, sizeof(device), &devunit);
        job_path(PrintSpoolBase, name, JOB_SUFFIX, path, sizeof(path));
        file = Open(path, MODE_OLDFILE);
        if (file == BNULL)
        {
            DeleteFile(path);
            continue;
        }

        while (!done)
        {
            if (cancelled(PrintSpoolBase, name))
                break;
            io->io_Device = NULL;
            if (OpenDevice(device, devunit, (struct IORequest *)io, 0) == 0)
                break;
            __sprintf(status, "%s: waiting, cannot open %s unit %ld", name, device, (long)devunit);
            set_status(PrintSpoolBase, status);
            Delay(RETRY_TICKS);
        }
        if (cancelled(PrintSpoolBase, name))
        {
            done = TRUE;
            __sprintf(status, "%s: cancelled", name);
            set_status(PrintSpoolBase, status);
        }

        {
            struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);

            total = 0;
            if (fib != NULL && ExamineFH(file, fib))
                total = fib->fib_Size;
            FreeDosObject(DOS_FIB, fib);
            sent = 0;
        }
        while (!done && (actual = Read(file, chunk, CHUNK_SIZE)) > 0)
        {
            /* Jobs closed meanwhile are published while this one prints. */
            publish_finished(PrintSpoolBase);
            __sprintf(status, "%s: printing on %s unit %ld, %ld/%ld KB", name, device,
                (long)devunit, (long)(sent / 1024), (long)(total / 1024));
            set_status(PrintSpoolBase, status);
            for (;;)
            {
                io->io_Command = CMD_WRITE;
                io->io_Data = chunk;
                io->io_Length = actual;
                io->io_Flags = 0;
                if (DoIO((struct IORequest *)io) == 0)
                {
                    sent += actual;
                    break;
                }
                /* Out of paper, offline, unplugged: wait, then send the chunk again. */
                __sprintf(status, "%s: waiting, %s error %ld", name, device, (long)io->io_Error);
                set_status(PrintSpoolBase, status);
                Delay(RETRY_TICKS);
                if (cancelled(PrintSpoolBase, name))
                {
                    done = TRUE;
                    break;
                }
            }
        }
        if (io->io_Device != NULL)
        {
            CloseDevice((struct IORequest *)io);
            io->io_Device = NULL;
        }
        Close(file);
        DeleteFile(path);
        job_path(PrintSpoolBase, name, ".cancel", path, sizeof(path));
        DeleteFile(path);
        job_path(PrintSpoolBase, name, ".info", path, sizeof(path));
        DeleteFile(path);
        D(bug("[printspool] %s done\n", name));
    }

    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    if (chunk != NULL)
        FreeMem(chunk, CHUNK_SIZE);
}

/* The spooler process, started on the first job. */
static BOOL start_spooler(struct PrintSpoolBase *PrintSpoolBase)
{
    struct Process *proc;

    if (PrintSpoolBase->ps_Spooler != NULL)
        return TRUE;
    proc = CreateNewProcTags(
        NP_Entry, (IPTR)spooler_main,
        NP_Name, (IPTR)"printspool.device",
        NP_UserData, (IPTR)PrintSpoolBase,
        NP_Priority, 0,
        NP_Input, BNULL,
        NP_Output, BNULL,
        NP_CloseInput, FALSE,
        NP_CloseOutput, FALSE,
        TAG_END);
    if (proc == NULL)
        return FALSE;
    PrintSpoolBase->ps_Spooler = proc;
    /* Kept open while the spooler runs, so the device is never expunged under it. */
    PrintSpoolBase->ps_Device.dd_Library.lib_OpenCnt++;
    return TRUE;
}

static LONG write_job(struct PrintSpoolBase *PrintSpoolBase, struct IOStdReq *io)
{
    struct PrintSpoolUnit *pu = (struct PrintSpoolUnit *)io->io_Unit;
    LONG unit = pu - PrintSpoolBase->ps_Unit;
    LONG written;

    ObtainSemaphore(&PrintSpoolBase->ps_Lock);
    if (!start_spooler(PrintSpoolBase))
    {
        ReleaseSemaphore(&PrintSpoolBase->ps_Lock);
        return IOERR_OPENFAIL;
    }
    if (pu->pu_Job == BNULL)
    {
        TEXT path[300];
        ULONG seq;

        if (PrintSpoolBase->ps_Sequence == 0)
            PrintSpoolBase->ps_Sequence = last_sequence(PrintSpoolBase);
        seq = ++PrintSpoolBase->ps_Sequence;
        __sprintf(pu->pu_Name, "%010lu-u%ld", (unsigned long)seq, (long)unit);
        job_path(PrintSpoolBase, pu->pu_Name, TMP_SUFFIX, path, sizeof(path));
        pu->pu_Job = Open(path, MODE_NEWFILE);
        if (pu->pu_Job == BNULL)
        {
            LONG err = IoErr();

            ReleaseSemaphore(&PrintSpoolBase->ps_Lock);
            return err != 0 ? err : IOERR_OPENFAIL;
        }
    }
    ReleaseSemaphore(&PrintSpoolBase->ps_Lock);

    written = Write(pu->pu_Job, io->io_Data, io->io_Length);
    if (written < 0)
    {
        io->io_Actual = 0;
        return IoErr();
    }
    io->io_Actual = written;
    return 0;
}

AROS_LH1(void, BeginIO,
    AROS_LHA(struct IOStdReq *, io, A1),
    struct PrintSpoolBase *, PrintSpoolBase, 5, PrintSpool)
{
    AROS_LIBFUNC_INIT

    switch (io->io_Command)
    {
    case CMD_WRITE:
        io->io_Error = write_job(PrintSpoolBase, io);
        break;
    case PRD_SETJOBINFO:
    {
        struct PrintSpoolUnit *pu = (struct PrintSpoolUnit *)io->io_Unit;
        struct TagItem *tags = io->io_Data, *tag;

        ObtainSemaphore(&PrintSpoolBase->ps_Lock);
        while ((tag = LibNextTagItem(&tags)) != NULL)
        {
            STRPTR dest = NULL;
            LONG size = 0;

            if (tag->ti_Tag == PJIA_Application)
            {
                dest = pu->pu_Application;
                size = sizeof(pu->pu_Application);
            }
            else if (tag->ti_Tag == PJIA_Document)
            {
                dest = pu->pu_Document;
                size = sizeof(pu->pu_Document);
            }
            if (dest != NULL)
            {
                strncpy(dest, tag->ti_Data ? (const char *)tag->ti_Data : "", size - 1);
                dest[size - 1] = '\0';
            }
        }
        ReleaseSemaphore(&PrintSpoolBase->ps_Lock);
        io->io_Error = 0;
        break;
    }
    case CMD_FLUSH:
    case CMD_UPDATE:
    case CMD_RESET:
        io->io_Error = 0;
        break;
    default:
        io->io_Error = IOERR_NOCMD;
        break;
    }
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);

    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, AbortIO,
    AROS_LHA(struct IORequest *, io, A1),
    struct PrintSpoolBase *, PrintSpoolBase, 6, PrintSpool)
{
    AROS_LIBFUNC_INIT
    (void)io;
    return IOERR_NOCMD;
    AROS_LIBFUNC_EXIT
}

static int GM_UNIQUENAME(Init)(struct PrintSpoolBase *PrintSpoolBase)
{
    InitSemaphore(&PrintSpoolBase->ps_Lock);
    NEWLIST((struct List *)&finished);
    PrintSpoolBase->ps_SpoolerSignal = SIGBREAKB_CTRL_F;
    PrintSpoolBase->ps_DOSBase = OpenLibrary("dos.library", 36);
    return PrintSpoolBase->ps_DOSBase != NULL;
}

static int GM_UNIQUENAME(Expunge)(struct PrintSpoolBase *PrintSpoolBase)
{
    CloseLibrary(PrintSpoolBase->ps_DOSBase);
    return TRUE;
}

static int GM_UNIQUENAME(Open)(struct PrintSpoolBase *PrintSpoolBase, struct IOStdReq *io,
    ULONG unitnum, ULONG flags)
{
    if (unitnum >= PRINTSPOOL_UNITS)
        return FALSE;
    ObtainSemaphore(&PrintSpoolBase->ps_Lock);
    PrintSpoolBase->ps_Unit[unitnum].pu_Openers++;
    ReleaseSemaphore(&PrintSpoolBase->ps_Lock);
    io->io_Unit = (struct Unit *)&PrintSpoolBase->ps_Unit[unitnum];
    return TRUE;
}

/* The last opener closing ends the job; the spooler closes and publishes it,
   since DOS calls do not belong here. */
static int GM_UNIQUENAME(Close)(struct PrintSpoolBase *PrintSpoolBase, struct IOStdReq *io)
{
    struct PrintSpoolUnit *pu = (struct PrintSpoolUnit *)io->io_Unit;
    struct FinishedJob *fj = NULL;

    io->io_Unit = NULL;
    ObtainSemaphore(&PrintSpoolBase->ps_Lock);
    if (pu->pu_Openers > 0)
        pu->pu_Openers--;
    if (pu->pu_Openers == 0 && pu->pu_Job == BNULL)
        pu->pu_Application[0] = pu->pu_Document[0] = '\0';
    if (pu->pu_Openers == 0 && pu->pu_Job != BNULL)
    {
        fj = AllocMem(sizeof(*fj), MEMF_ANY);
        /* Without memory for the hand-over the job stays open and the next
           writer appends to it. */
        if (fj != NULL)
        {
            fj->fj_File = pu->pu_Job;
            strcpy(fj->fj_Name, pu->pu_Name);
            strcpy(fj->fj_Application, pu->pu_Application);
            strcpy(fj->fj_Document, pu->pu_Document);
            ADDTAIL((struct List *)&finished, (struct Node *)fj);
            pu->pu_Job = BNULL;
            pu->pu_Application[0] = pu->pu_Document[0] = '\0';
        }
    }
    ReleaseSemaphore(&PrintSpoolBase->ps_Lock);
    if (fj != NULL && PrintSpoolBase->ps_Spooler != NULL)
        Signal((struct Task *)PrintSpoolBase->ps_Spooler, 1UL << PrintSpoolBase->ps_SpoolerSignal);
    return TRUE;
}

ADD2OPENDEV(GM_UNIQUENAME(Open), 0)
ADD2CLOSEDEV(GM_UNIQUENAME(Close), 0)
ADD2INITLIB(GM_UNIQUENAME(Init), 0)
ADD2EXPUNGELIB(GM_UNIQUENAME(Expunge), 0)
