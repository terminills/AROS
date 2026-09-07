/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CrashReporter — the crash record shared between the hooks that run
          in the failing task (or in trap context) and the daemon that turns
          it into a report.  See DESIGN.md §2.

    AI_PHASE: CRASH_REPORTER
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: LOW
*/

#ifndef CRASHREPORTER_CRASHREC_H
#define CRASHREPORTER_CRASHREC_H

#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/interrupts.h>      /* pulls in the per-CPU cpucontext.h */

#define CR_MAX_FRAMES   32
#define CR_RING         8           /* concurrent crashes kept before reuse */
#define CR_NAMELEN      64
#define CR_PROGLEN      128
#define CR_ANNOTLEN     4096        /* rendered "key: value\n" lines          */

/* record states (rec_State) */
#define CRS_FREE        0
#define CRS_TRAP        1           /* CPU trap observed, no Alert() yet     */
#define CRS_ALERT       2           /* Alert() seen: complete, daemon's turn */
#define CRS_TAKEN       3           /* daemon is formatting it               */

/* rec_Flags */
#define CRF_CONTEXT     (1 << 0)    /* rec_Ctx is valid (came from a trap)   */
#define CRF_FAULTADDR   (1 << 1)    /* rec_FaultAddress is valid (#PF/CR2)   */
#define CRF_ERRORCODE   (1 << 2)
#define CRF_DEADEND     (1 << 3)    /* alert had AT_DeadEnd set              */
#define CRF_SUPERVISOR  (1 << 4)    /* Alert() called from supervisor mode   */
#define CRF_ANNOT       (1 << 5)    /* rec_Product/rec_Annot were filled from
                                       the program's CrashAnnotationBlock    */

struct CrashRecord
{
    ULONG   rec_Seq;                /* claim order, 0 = never used           */
    ULONG   rec_State;              /* CRS_#?                                */
    ULONG   rec_Flags;              /* CRF_#?                                */
    ULONG   rec_Exception;          /* CPU vector, ~0 when not a trap        */
    struct Task *rec_Task;          /* failing task (may be gone later)      */
    ULONG   rec_AlertNum;
    ULONG   rec_Depth;              /* frames in rec_PCs                     */
    APTR    rec_Caller;             /* Alert() return address                */
    APTR    rec_FaultAddress;
    UQUAD   rec_ErrorCode;
    APTR    rec_PCs[CR_MAX_FRAMES]; /* [0] = faulting PC when CRF_CONTEXT    */
    struct ExceptionContext rec_Ctx;/* GPRs only; FPU pointers not valid     */
    TEXT    rec_TaskName[CR_NAMELEN];
    TEXT    rec_Program[CR_PROGLEN];/* CLI command name, "" for WB/tasks     */
    TEXT    rec_TaskType;           /* NT_TASK / NT_PROCESS                  */
    UBYTE   rec_Pad[7];
    TEXT    rec_Product[96];        /* cab_Product (CRA_PRODUCTLEN)          */
    TEXT    rec_Annot[CR_ANNOTLEN]; /* [Annotations] body, "" when none      */
};

struct CrashRing
{
    ULONG   ring_Cursor;            /* claim counter (atomic add)            */
    ULONG   ring_Pending;           /* records in CRS_ALERT                  */
    struct Task *ring_Daemon;       /* who to Signal()                       */
    ULONG   ring_SigMask;
    struct CrashRecord ring_Rec[CR_RING];
};

/* crashhook.c */
struct CrashRing *crashhook_install(struct Task *daemon, ULONG sigmask);
BOOL              crashhook_remove(struct CrashRing *ring);   /* FALSE = still needed */
/* oldest CRS_ALERT record copied into *out and its slot freed; FALSE = none */
BOOL              crashhook_next(struct CrashRing *ring, struct CrashRecord *out);

#endif /* CRASHREPORTER_CRASHREC_H */
