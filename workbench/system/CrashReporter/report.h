/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CrashReporter — turn a CrashRecord into a report (DESIGN.md §3,
          PROTOCOL.md §9).  Runs in the daemon task only.

    AI_PHASE: CRASH_REPORTER
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: LOW
*/

#ifndef CRASHREPORTER_REPORT_H
#define CRASHREPORTER_REPORT_H

#include <exec/types.h>
#include "crashrec.h"

#define CR_REPORT_VERSION   1
#define CR_REPORT_EXT       ".report"
#define CR_DEFAULT_DIR      "ENVARC:CrashReports"
#define CR_FALLBACK_DIR     "RAM:CrashReports"
#define CR_ENV_DIR          "CrashReporter/Reports"
#define CR_ENV_URL          "CrashReporter/URL"
#define CR_ENV_AUTOSEND     "CrashReporter/AutoSend"
#define CR_LOG_TAIL         50          /* log.resource entries included    */

struct CrashReport
{
    STRPTR  Text;                       /* full report, NUL-terminated      */
    ULONG   Len;
    TEXT    Path[512];                  /* where report_store() put it      */
    TEXT    Task[CR_NAMELEN];
    TEXT    Summary[160];               /* one line for the popup           */
    TEXT    Location[160];              /* "module!function+0x..".          */
    TEXT    Signature[200];             /* grouping key (PROTOCOL.md §9.3)  */
    ULONG   AlertNum;
    BOOL    DeadEnd;
};

/* format rec into *rep (rep->Text allocated; report_free() releases it) */
LONG report_build(const struct CrashRecord *rec, struct CrashReport *rep);

/* write rep->Text to the reports directory (ENV override, ENVARC: default,
   RAM: fallback); fills rep->Path.  Also publishes a Crit entry to
   log.resource so LogView shows the crash alongside the system log. */
LONG report_store(struct CrashReport *rep);

void report_free(struct CrashReport *rep);

/* append the user's own account of what they were doing as a
   "[Description]" section to the stored report at path.  Control
   characters other than newline are dropped; empty text is a no-op. */
LONG report_annotate(CONST_STRPTR path, CONST_STRPTR text);

/* the directory reports live in (resolved the same way as report_store) */
void report_dir(STRPTR buf, ULONG len);

/* the [Log] tail is collected at build time; report.c opens log.resource
   lazily and keeps a provider handle for the lifetime of the daemon */
void report_init(void);
void report_exit(void);

#endif /* CRASHREPORTER_REPORT_H */
