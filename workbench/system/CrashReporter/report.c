/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CrashReporter — report formatting, storage and log publishing.
          Everything here runs in the daemon's own task, from a copy of the
          record, so it may allocate, lock, symbolise and touch the disk.
          The text format is PROTOCOL.md §9: a stanza header (Key: Value
          lines, blank line) the server indexes, followed by free-form
          sections it stores verbatim.

    AI_PHASE: CRASH_REPORTER
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: MEDIUM
    AI_NOTE: Register names and the exception table are x86_64; other
             architectures get the generic header and backtrace only.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/debug.h>
#include <proto/kernel.h>
#include <proto/aros.h>
#include <proto/log.h>

#include <aros/inquire.h>
#include <dos/datetime.h>
#include <dos/dosextens.h>
#include <exec/alerts.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <libraries/debug.h>
#include <resources/log.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pkg.h"
#include "pkg_private.h"
#include "report.h"

/* ------------------------------------------------------------------------ */

struct Library *LogResBase;         /* proto/log.h declares it extern */
static APTR             logHandle;
static struct LogProviderNode logProvider;

void report_init(void)
{
    LogResBase = OpenResource("log.resource");
    if (LogResBase)
    {
        logProvider.lpn_Node.ln_Name = "CrashReporter";
        logHandle = logInitialise(&logProvider);
    }
}

void report_exit(void)
{
    if (LogResBase && logHandle)
        logFinalise(logHandle);
    logHandle = NULL;
}

/* ------------------------------------------------------------------------ */
/* growable text buffer                                                      */

struct TextBuf
{
    STRPTR  s;
    ULONG   len, cap;
    BOOL    failed;
};

static void tb_grow(struct TextBuf *tb, ULONG need)
{
    ULONG ncap = tb->cap ? tb->cap : 8192;
    STRPTR ns;

    while (ncap < tb->len + need + 1) ncap *= 2;
    ns = AllocVec(ncap, MEMF_ANY);
    if (!ns) { tb->failed = TRUE; return; }
    if (tb->s) { memcpy(ns, tb->s, tb->len); FreeVec(tb->s); }
    tb->s = ns; tb->cap = ncap;
    tb->s[tb->len] = 0;
}

static void tb_printf(struct TextBuf *tb, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (tb->failed) return;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (tb->len + n + 1 > tb->cap) tb_grow(tb, n);
    if (tb->failed) return;
    va_start(ap, fmt);
    vsnprintf(tb->s + tb->len, tb->cap - tb->len, fmt, ap);
    va_end(ap);
    tb->len += n;
}

/* ------------------------------------------------------------------------ */
/* names                                                                     */

static const char *exception_name(ULONG vec)
{
#if defined(__x86_64__) || defined(__i386__)
    switch (vec)
    {
    case 0:  return "Divide error";
    case 1:  return "Debug";
    case 3:  return "Breakpoint";
    case 4:  return "Overflow";
    case 5:  return "Bound range";
    case 6:  return "Invalid opcode";
    case 7:  return "Device not available";
    case 10: return "Invalid TSS";
    case 11: return "Segment not present";
    case 12: return "Stack fault";
    case 13: return "General protection";
    case 14: return "Page fault";
    case 16: return "x87 FPU error";
    case 17: return "Alignment check";
    case 18: return "Machine check";
    case 19: return "SIMD exception";
    }
#endif
    return "CPU exception";
}

static const char *alert_subsystem(ULONG num)
{
    switch ((num >> 24) & 0x7f)
    {
    case 0x00: return (num & 0x00ff0000) == 0 ? "CPU" : "general";
    case 0x01: return "exec";
    case 0x02: return "graphics";
    case 0x03: return "layers";
    case 0x04: return "intuition";
    case 0x05: return "math";
    case 0x07: return "dos";
    case 0x08: return "ramlib";
    case 0x09: return "icon";
    case 0x0a: return "expansion";
    case 0x0b: return "diskfont";
    case 0x10: return "audio";
    case 0x11: return "console";
    case 0x12: return "gameport";
    case 0x13: return "keyboard";
    case 0x14: return "trackdisk";
    case 0x15: return "timer";
    case 0x20: return "cia";
    case 0x21: return "disk";
    case 0x22: return "misc";
    case 0x30: return "bootstrap";
    case 0x31: return "workbench";
    case 0x32: return "diskcopy";
    case 0x33: return "gadtools";
    case 0x35: return "unknown";
    }
    return "user";
}

static const char *alert_general(ULONG num)
{
    switch (num & 0x00ff0000)
    {
    case AG_NoMemory:   return "no memory";
    case AG_MakeLib:    return "MakeLibrary failed";
    case AG_OpenLib:    return "OpenLibrary failed";
    case AG_OpenDev:    return "OpenDevice failed";
    case AG_OpenRes:    return "OpenResource failed";
    case AG_IOError:    return "I/O error";
    case AG_NoSignal:   return "no signal";
    case AG_BadParm:    return "bad parameter";
    case AG_CloseLib:   return "CloseLibrary failed";
    case AG_CloseDev:   return "CloseDevice failed";
    case AG_ProcCreate: return "CreateProc failed";
    }
    switch (num)
    {
    case ACPU_BusErr:       return "bus error";
    case ACPU_AddressErr:   return "address error";
    case ACPU_InstErr:      return "illegal instruction";
    case ACPU_DivZero:      return "division by zero";
    case ACPU_PrivErr:      return "privilege violation";
    case AN_MemCorrupt:     return "memory list corrupt";
    case AN_IntrMem:        return "no memory for interrupt";
    case AN_FreeTwice:      return "memory freed twice";
    case AN_BogusExcpt:     return "bogus exception";
    case AN_MemoryInsane:   return "memory insane";
    case AN_StackProbe:     return "stack overflow";
    case AN_BadFreeAddr:    return "bad free address";
    case AN_BadSemaphore:   return "bad semaphore";
    }
    return NULL;
}

static void stamp_now(STRPTR buf, ULONG len)
{
    struct DateTime dt;
    TEXT date[LEN_DATSTRING], time[LEN_DATSTRING];

    DateStamp(&dt.dat_Stamp);
    dt.dat_Format  = FORMAT_DOS;
    dt.dat_Flags   = 0;
    dt.dat_StrDay  = NULL;
    dt.dat_StrDate = date;
    dt.dat_StrTime = time;
    if (DateToStr(&dt))
        snprintf(buf, len, "%s %s", date, time);
    else
        strlcpy(buf, "-", len);
}

/* "module!symbol+0xoff" or "0x..." for one PC; TRUE if symbolised */
static BOOL symbolise(APTR pc, STRPTR out, ULONG len, STRPTR module, ULONG mlen,
                      STRPTR func, ULONG flen, IPTR *offset)
{
    STRPTR modname = NULL, symname = NULL, segname = NULL;
    APTR symaddr = NULL, segaddr = NULL;

    if (module) module[0] = 0;
    if (func)   func[0]   = 0;
    if (offset) *offset   = 0;

    if (DebugBase && DecodeLocation(pc,
                                    DL_ModuleName,   &modname,
                                    DL_SegmentName,  &segname,
                                    DL_SegmentStart, &segaddr,
                                    DL_SymbolName,   &symname,
                                    DL_SymbolStart,  &symaddr,
                                    TAG_DONE))
    {
        const char *m = modname ? (const char *)modname : "?";
        if (symaddr)
        {
            const char *f = symname ? (const char *)symname : "?";
            snprintf(out, len, "%p %s!%s+0x%lx", pc, m, f, (unsigned long)((IPTR)pc - (IPTR)symaddr));
            if (module) strlcpy(module, m, mlen);
            if (func)   strlcpy(func, f, flen);
            if (offset) *offset = (IPTR)pc - (IPTR)symaddr;
            return TRUE;
        }
        if (segaddr)
        {
            snprintf(out, len, "%p %s:%s+0x%lx", pc, m, segname ? (const char *)segname : "?",
                     (unsigned long)((IPTR)pc - (IPTR)segaddr));
            if (module) strlcpy(module, m, mlen);
            if (func)   snprintf(func, flen, "%s", segname ? (const char *)segname : "?");
            if (offset) *offset = (IPTR)pc - (IPTR)segaddr;
            return TRUE;
        }
    }
    snprintf(out, len, "%p", pc);
    return FALSE;
}

/* ------------------------------------------------------------------------ */
/* sections                                                                  */

static void section_registers(struct TextBuf *tb, const struct CrashRecord *rec)
{
#if defined(__x86_64__)
    const struct ExceptionContext *c = &rec->rec_Ctx;
    tb_printf(tb, "[Registers]\n");
    tb_printf(tb, "rip=%016lx rsp=%016lx rbp=%016lx rflags=%08lx\n",
              (unsigned long)c->rip, (unsigned long)c->rsp, (unsigned long)c->rbp, (unsigned long)c->rflags);
    tb_printf(tb, "rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n",
              (unsigned long)c->rax, (unsigned long)c->rbx, (unsigned long)c->rcx, (unsigned long)c->rdx);
    tb_printf(tb, "rsi=%016lx rdi=%016lx r8 =%016lx r9 =%016lx\n",
              (unsigned long)c->rsi, (unsigned long)c->rdi, (unsigned long)c->r8, (unsigned long)c->r9);
    tb_printf(tb, "r10=%016lx r11=%016lx r12=%016lx r13=%016lx\n",
              (unsigned long)c->r10, (unsigned long)c->r11, (unsigned long)c->r12, (unsigned long)c->r13);
    tb_printf(tb, "r14=%016lx r15=%016lx cs=%04lx ss=%04lx\n",
              (unsigned long)c->r14, (unsigned long)c->r15, (unsigned long)c->cs, (unsigned long)c->ss);
    if (rec->rec_Flags & CRF_FAULTADDR)
        tb_printf(tb, "cr2=%016lx\n", (unsigned long)(IPTR)rec->rec_FaultAddress);
    if (rec->rec_Flags & CRF_ERRORCODE)
        tb_printf(tb, "error=%016lx\n", (unsigned long)rec->rec_ErrorCode);
    tb_printf(tb, "\n");
#else
    (void)tb; (void)rec;
#endif
}

static void section_backtrace(struct TextBuf *tb, const struct CrashRecord *rec)
{
    ULONG i, n = rec->rec_Depth;
    TEXT line[256];

    if (n > CR_MAX_FRAMES) n = CR_MAX_FRAMES;
    tb_printf(tb, "[Backtrace]\n");
    for (i = 0; i < n; i++)
    {
        symbolise(rec->rec_PCs[i], line, sizeof line, NULL, 0, NULL, 0, NULL);
        tb_printf(tb, "#%-2lu %s\n", (unsigned long)i, line);
    }
    tb_printf(tb, "\n");
}

static void section_modules(struct TextBuf *tb)
{
    struct Node *n;
    struct Library *lib;

    tb_printf(tb, "[Modules]\n");
    Forbid();
    ForeachNode(&SysBase->LibList, n)
    {
        lib = (struct Library *)n;
        tb_printf(tb, "%s %lu.%lu\n", n->ln_Name ? (const char *)n->ln_Name : "?",
                  (unsigned long)lib->lib_Version, (unsigned long)lib->lib_Revision);
    }
    ForeachNode(&SysBase->DeviceList, n)
    {
        lib = (struct Library *)n;
        tb_printf(tb, "%s %lu.%lu\n", n->ln_Name ? (const char *)n->ln_Name : "?",
                  (unsigned long)lib->lib_Version, (unsigned long)lib->lib_Revision);
    }
    Permit();
    tb_printf(tb, "\n");
}

static const char *log_type_name(ULONG flags)
{
    switch ((flags & LOGM_Flag_TypeMask) >> LOGMS_Flag_Type)
    {
    case LOGF_Type_Error:       return "Error";
    case LOGF_Type_Crit:        return "Critical";
    case LOGF_Type_Warn:        return "Warning";
    case LOGF_Type_Information: return "Info";
    case LOGF_Type_Verbose:     return "Verbose";
    case LOGF_Type_Debug:       return "Debug";
    }
    return "?";
}

/* last CR_LOG_TAIL log.resource entries, oldest first */
static void section_log(struct TextBuf *tb)
{
    APTR entries[CR_LOG_TAIL];
    APTR node = 0;
    ULONG count = 0, i, start;

    if (!LogResBase) return;

    tb_printf(tb, "[Log]\n");
    logLockEntries(LLF_READ);
    while ((node = logNextEntry(&node)) && node != (APTR)-1)
        entries[count++ % CR_LOG_TAIL] = node;
    start = count > CR_LOG_TAIL ? count % CR_LOG_TAIL : 0;
    for (i = 0; i < (count < CR_LOG_TAIL ? count : CR_LOG_TAIL); i++)
    {
        APTR e = entries[(start + i) % CR_LOG_TAIL];
        ULONG flags = 0;
        struct DateStamp *ds = NULL;
        char *comp = NULL, *sub = NULL, *text = NULL;
        struct TagItem attrs[] = {
            { LOGMA_Flags,        (IPTR)&flags },
            { LOGMA_DateStamp,    (IPTR)&ds    },
            { LOGMA_Component,    (IPTR)&comp  },
            { LOGMA_SubComponent, (IPTR)&sub   },
            { LOGMA_Entry,        (IPTR)&text  },
            { TAG_DONE, 0 }
        };
        TEXT when[LEN_DATSTRING * 2 + 2] = "-";

        logGetEntryAttrs(e, attrs);
        if (ds)
        {
            struct DateTime dt;
            TEXT date[LEN_DATSTRING], time[LEN_DATSTRING];
            dt.dat_Stamp   = *ds;
            dt.dat_Format  = FORMAT_DOS;
            dt.dat_Flags   = 0;
            dt.dat_StrDay  = NULL;
            dt.dat_StrDate = date;
            dt.dat_StrTime = time;
            if (DateToStr(&dt)) snprintf(when, sizeof when, "%s %s", date, time);
        }
        tb_printf(tb, "%s %s %s%s%s: %s\n", when, log_type_name(flags),
                  comp ? comp : "?", (sub && *sub) ? ":" : "", (sub && *sub) ? sub : "",
                  text ? text : "");
    }
    logUnlockEntries(LLF_READ);
    tb_printf(tb, "\n");
}

/* ------------------------------------------------------------------------ */

LONG report_build(const struct CrashRecord *rec, struct CrashReport *rep)
{
    struct TextBuf tb = { 0 };
    TEXT when[64], loc[256], module[64], func[96];
    IPTR offset = 0;
    IPTR ver = 0, relmaj = 0, relmin = 0, abi = 0;
    /* AI_ArosReleaseDate is a day count, not a string (aros/inquire.h);
       the printable date is AI_ArosBuildDate */
    STRPTR builddate = NULL, variant = NULL, arch = NULL;
    ULONG cpus = 1;
    APTR pc0;
    const char *general;
    BOOL trap = (rec->rec_Flags & CRF_CONTEXT) != 0;

    memset(rep, 0, sizeof *rep);
    rep->AlertNum = rec->rec_AlertNum;
    rep->DeadEnd  = (rec->rec_Flags & CRF_DEADEND) != 0;
    strlcpy(rep->Task, rec->rec_TaskName[0] ? (const char *)rec->rec_TaskName : "(unnamed)", sizeof rep->Task);

    stamp_now(when, sizeof when);

    /* the crash site: faulting PC for a trap, the Alert() caller otherwise */
    pc0 = rec->rec_Depth ? rec->rec_PCs[0] : rec->rec_Caller;
    symbolise(pc0, loc, sizeof loc, module, sizeof module, func, sizeof func, &offset);
    if (!module[0]) strlcpy(module, "?", sizeof module);
    if (!func[0])   snprintf(func, sizeof func, "%p", pc0);
    snprintf(rep->Location, sizeof rep->Location, "%s!%s+0x%lx", module, func, (unsigned long)offset);
    snprintf(rep->Signature, sizeof rep->Signature, "%s!%s+0x%lx/%08lx",
             module, func, (unsigned long)offset, (unsigned long)rec->rec_AlertNum);

    general = alert_general(rec->rec_AlertNum);
    if (trap)
        snprintf(rep->Summary, sizeof rep->Summary, "%s at %s", exception_name(rec->rec_Exception), rep->Location);
    else
        snprintf(rep->Summary, sizeof rep->Summary, "Alert %08lx%s%s at %s",
                 (unsigned long)rec->rec_AlertNum, general ? ": " : "", general ? general : "", rep->Location);

    ArosInquire(AI_ArosVersion,      (IPTR)&ver,
                AI_ArosReleaseMajor, (IPTR)&relmaj,
                AI_ArosReleaseMinor, (IPTR)&relmin,
                AI_ArosBuildDate,    (IPTR)&builddate,
                AI_ArosVariant,      (IPTR)&variant,
                AI_ArosArchitecture, (IPTR)&arch,
                AI_ArosABIMajor,     (IPTR)&abi,
                TAG_DONE);
    {
        APTR KernelBase = OpenResource("kernel.resource");
        if (KernelBase) cpus = KrnGetCPUCount();
    }

    /* --- header (PROTOCOL.md §9.2) --- */
    tb_printf(&tb, "Report-Version: %d\n", CR_REPORT_VERSION);
    tb_printf(&tb, "Reporter: CrashReporter/1.0\n");
    tb_printf(&tb, "Date: %s\n", when);
    tb_printf(&tb, "Task: %s\n", rep->Task);
    tb_printf(&tb, "Task-Type: %s\n", rec->rec_TaskType == NT_PROCESS ? "Process" : "Task");
    if (rec->rec_Program[0])
        tb_printf(&tb, "Program: %s\n", rec->rec_Program);
    if ((rec->rec_Flags & CRF_ANNOT) && rec->rec_Product[0])
        tb_printf(&tb, "Product: %s\n", rec->rec_Product);
    tb_printf(&tb, "Alert: %08lx\n", (unsigned long)rec->rec_AlertNum);
    tb_printf(&tb, "Alert-Kind: %s\n", rep->DeadEnd ? "DeadEnd" : "Recoverable");
    tb_printf(&tb, "Alert-Subsystem: %s\n", alert_subsystem(rec->rec_AlertNum));
    if (general) tb_printf(&tb, "Alert-Text: %s\n", general);
    if (trap)
    {
        tb_printf(&tb, "Exception: %lu\n", (unsigned long)rec->rec_Exception);
        tb_printf(&tb, "Exception-Name: %s\n", exception_name(rec->rec_Exception));
        if (rec->rec_Flags & CRF_FAULTADDR)
            tb_printf(&tb, "Fault-Address: %p\n", rec->rec_FaultAddress);
        if (rec->rec_Flags & CRF_ERRORCODE)
            tb_printf(&tb, "Error-Code: %lx\n", (unsigned long)rec->rec_ErrorCode);
    }
    tb_printf(&tb, "PC: %p\n", pc0);
    tb_printf(&tb, "Module: %s\n", module);
    tb_printf(&tb, "Function: %s\n", func);
    tb_printf(&tb, "Offset: 0x%lx\n", (unsigned long)offset);
    tb_printf(&tb, "Location: %s\n", rep->Location);
    tb_printf(&tb, "Signature: %s\n", rep->Signature);
    tb_printf(&tb, "Summary: %s\n", rep->Summary);
    tb_printf(&tb, "Arch: %s\n", arch ? (const char *)arch : "?");
    tb_printf(&tb, "AROS-Version: %lu\n", (unsigned long)ver);
    tb_printf(&tb, "AROS-Release: %lu.%lu%s%s\n", (unsigned long)relmaj, (unsigned long)relmin,
              (builddate && *builddate) ? " " : "", (builddate && *builddate) ? (const char *)builddate : "");
    if (variant) tb_printf(&tb, "AROS-Variant: %s\n", variant);
    /* aros.library answers -1 for an unknown ABI; leave the line out then */
    if (abi != (IPTR)-1 && abi != 0)
        tb_printf(&tb, "ABI: %lu\n", (unsigned long)abi);
    tb_printf(&tb, "Exec-Version: %u.%u\n", SysBase->LibNode.lib_Version, SysBase->LibNode.lib_Revision);
    tb_printf(&tb, "CPUs: %lu\n", (unsigned long)cpus);
    tb_printf(&tb, "Memory-Free: %lu\n", (unsigned long)AvailMem(MEMF_ANY));
    tb_printf(&tb, "Memory-Total: %lu\n", (unsigned long)AvailMem(MEMF_ANY | MEMF_TOTAL));
    tb_printf(&tb, "Memory-Largest: %lu\n", (unsigned long)AvailMem(MEMF_ANY | MEMF_LARGEST));
    tb_printf(&tb, "\n");

    /* --- sections --- */
    if (trap) section_registers(&tb, rec);
    section_backtrace(&tb, rec);
    section_modules(&tb);
    section_log(&tb);
    if ((rec->rec_Flags & CRF_ANNOT) && rec->rec_Annot[0])
    {
        /* the program's own state (crashreporter/annotations.h), already
           rendered as "key: value" lines by the Alert() hook */
        tb_printf(&tb, "[Annotations]\n%s\n", rec->rec_Annot);
    }

    if (tb.failed || !tb.s)
    {
        if (tb.s) FreeVec(tb.s);
        return PKGERR_NOMEM;
    }
    rep->Text = tb.s;
    rep->Len  = tb.len;
    return PKGERR_OK;
}

void report_free(struct CrashReport *rep)
{
    if (rep->Text) FreeVec(rep->Text);
    rep->Text = NULL;
    rep->Len  = 0;
}

/* ------------------------------------------------------------------------ */

void report_dir(STRPTR buf, ULONG len)
{
    if (GetVar(CR_ENV_DIR, buf, len, GVF_GLOBAL_ONLY) > 0 && buf[0])
        return;
    strlcpy(buf, CR_DEFAULT_DIR, len);
}

static void safe_name(STRPTR out, ULONG len, CONST_STRPTR in)
{
    ULONG i;
    for (i = 0; i + 1 < len && in[i]; i++)
    {
        TEXT c = in[i];
        out[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                  || c == '-' || c == '_' || c == '.') ? c : '_';
    }
    out[i] = 0;
}

LONG report_store(struct CrashReport *rep)
{
    TEXT dir[PKG_PATHLEN], name[160], task[CR_NAMELEN];
    struct DateStamp ds;
    LONG rc;

    if (!rep->Text) return PKGERR_FORMAT;

    DateStamp(&ds);
    safe_name(task, sizeof task, rep->Task);
    snprintf(name, sizeof name, "%lu-%lu-%lu-%s%s", (unsigned long)ds.ds_Days,
             (unsigned long)ds.ds_Minute, (unsigned long)ds.ds_Tick, task, CR_REPORT_EXT);

    report_dir(dir, sizeof dir);
    pkg_mkdir_p(dir);
    pkg_join(rep->Path, sizeof rep->Path, dir, name);
    rc = pkg_file_write(rep->Path, rep->Text, rep->Len);
    if (rc != PKGERR_OK)
    {
        pkg_mkdir_p(CR_FALLBACK_DIR);
        pkg_join(rep->Path, sizeof rep->Path, CR_FALLBACK_DIR, name);
        rc = pkg_file_write(rep->Path, rep->Text, rep->Len);
    }
    if (rc != PKGERR_OK) rep->Path[0] = 0;

    if (LogResBase && logHandle)
    {
        logAddEntry(LOGF_Flag_Type_Crit, logHandle, "", "report", rep->AlertNum,
                    "%s: %s%s%s", rep->Task, rep->Summary,
                    rep->Path[0] ? " -> " : "", rep->Path[0] ? (char *)rep->Path : "");
    }
    return rc;
}

LONG report_annotate(CONST_STRPTR path, CONST_STRPTR text)
{
    struct TextBuf tb = { 0 };
    CONST_STRPTR p;
    BOOL any = FALSE;
    BPTR fh;
    LONG rc = PKGERR_OK;

    if (!path || !path[0] || !text) return PKGERR_OK;
    for (p = text; *p; p++)
        if (*p > ' ') { any = TRUE; break; }
    if (!any) return PKGERR_OK;

    tb_printf(&tb, "[Description]\n");
    for (p = text; *p; p++)
    {
        if (*p == '\n' || *p == '\r') tb_printf(&tb, "\n");
        else if ((UBYTE)*p >= ' ')   tb_printf(&tb, "%c", *p);
    }
    tb_printf(&tb, "\n\n");
    if (tb.failed || !tb.s) { if (tb.s) FreeVec(tb.s); return PKGERR_NOMEM; }

    /* the report on disk ends with "\n\n"; the section simply follows */
    fh = Open(path, MODE_READWRITE);
    if (fh)
    {
        if (Seek(fh, 0, OFFSET_END) < 0 || Write(fh, tb.s, tb.len) != (LONG)tb.len)
            rc = PKGERR_IO;
        Close(fh);
    }
    else
        rc = PKGERR_IO;
    FreeVec(tb.s);
    return rc;
}
