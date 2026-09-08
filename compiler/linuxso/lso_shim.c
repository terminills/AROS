/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    linuxso glibc ABI shim: the symbol table the mapper resolves a Linux
    x86-64 shared object's imports against, plus the small functions that
    have no direct AROS CRT counterpart (auxv, ctype tables, locale
    handles, uname, sysconf, timegm, ...).  Functions whose ABI is the same
    on both sides are passed straight to the AROS CRT.
*/

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <proto/exec.h>
#include <proto/kernel.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <aros/debug.h>

#include <errno.h>
#include <math.h>
#include <sched.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "linuxso_intern.h"
#include "lso_lx.h"

/* the program's libgcc unwinder */
extern void _Unwind_DeleteException(void *);
extern unsigned long _Unwind_GetIP(void *);
extern void *_Unwind_GetLanguageSpecificData(void *);
extern unsigned long _Unwind_GetRegionStart(void *);
extern int _Unwind_RaiseException(void *);
extern void _Unwind_Resume(void *);
extern void _Unwind_SetGR(void *, int, unsigned long);
extern void _Unwind_SetIP(void *, unsigned long);

/* ---- data symbols --------------------------------------------------------- */

/* glibc exports these as variables the guest reads through GLOB_DAT */
static FILE *lx_stdin, *lx_stdout, *lx_stderr;
static void *lx_libc_stack_end;

/* ---- misc ----------------------------------------------------------------- */

static void lx_abort(void)
{
    lso_log("abort() called by guest");
    abort();
}

static void lx_stack_chk_fail(void)
{
    lso_log("__stack_chk_fail: guest stack smashing detected");
    abort();
}

#define LX_AT_PAGESZ  6
#define LX_AT_HWCAP   16
#define LX_AT_SECURE  23
#define LX_AT_RANDOM  25
#define LX_AT_HWCAP2  26

static unsigned long lx_getauxval(unsigned long type)
{
    static unsigned char at_random[16];
    static int at_random_ready;

    switch (type)
    {
    case LX_AT_PAGESZ:
        return LSO_PAGE;
    case LX_AT_RANDOM:
        if (!at_random_ready)
        {
            unsigned int i;
            for (i = 0; i < sizeof(at_random); i++)
                at_random[i] = (unsigned char)(rand() ^ (uintptr_t)&i ^ i);
            at_random_ready = 1;
        }
        return (unsigned long)at_random;
    case LX_AT_HWCAP:
    {
        unsigned int a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        return d;
    }
    case LX_AT_SECURE:
    case LX_AT_HWCAP2:
        return 0;
    default:
        LSO_DEBUG("getauxval(%lu) unknown", type);
        lso_set_errno(LX_ENOENT);
        return 0;
    }
}

/* glibc's ctype tables are indexed -128..255 through a pointer to entry 128 */
static int32_t ctype_tolower_tab[384];
static int32_t ctype_toupper_tab[384];
static const int32_t *ctype_tolower_ptr, *ctype_toupper_ptr;

static void ctype_init(void)
{
    int i;

    for (i = 0; i < 384; i++)
    {
        int c = i - 128;
        int lo = c, up = c;
        if (c >= 'A' && c <= 'Z')
            lo = c - 'A' + 'a';
        if (c >= 'a' && c <= 'z')
            up = c - 'a' + 'A';
        ctype_tolower_tab[i] = lo;
        ctype_toupper_tab[i] = up;
    }
    ctype_tolower_ptr = ctype_tolower_tab + 128;
    ctype_toupper_ptr = ctype_toupper_tab + 128;
}

static const int32_t **lx_ctype_tolower_loc(void) { return &ctype_tolower_ptr; }
static const int32_t **lx_ctype_toupper_loc(void) { return &ctype_toupper_ptr; }
static size_t lx_ctype_get_mb_cur_max(void)       { return 1; }

static int lx_cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    lso_atexit_push(fn, arg, dso);
    return 0;
}

static void lx_cxa_finalize(void *dso)
{
    lso_atexit_run(dso);
}

static void *lx_dlsym(void *handle, const char *name)
{
    void *p = lso_shim_lookup(name);

    (void)handle;
    if (!p)
        p = lso_objects_sym(name);
    LSO_DEBUG("dlsym(%p, %s) -> %p", handle, name, p);
    return p;
}

static int lx_dlclose(void *handle)
{
    LSO_DEBUG("dlclose(%p)", handle);
    return 0;
}

static void *lx_dlopen(const char *name, int flags)
{
    LSO_DEBUG("dlopen(%s, %x) -> NULL", name ? name : "(self)", flags);
    if (!name)
        return (void *)1;   /* the program itself: dlsym then searches everything */
    return NULL;
}

static char *lx_dlerror(void)
{
    return "linuxso: dlopen is not supported";
}

/* Linux resource numbers */
#define LX_RLIMIT_STACK  3
#define LX_RLIMIT_NOFILE 7
#define LX_RLIMIT_AS     9
#define LX_RLIM_INFINITY (~0UL)

struct lx_rlimit { unsigned long cur, max; };

int lx_getrlimit64(int resource, void *out)
{
    struct lx_rlimit *r = out;

    switch (resource)
    {
    case LX_RLIMIT_STACK:
    {
        struct Task *t = FindTask(NULL);
        r->cur = r->max = (char *)t->tc_SPUpper - (char *)t->tc_SPLower;
        break;
    }
    case LX_RLIMIT_NOFILE:
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
            r->cur = r->max = rl.rlim_cur;
        else
            r->cur = r->max = 1024;
        break;
    }
    default:
        r->cur = r->max = LX_RLIM_INFINITY;
        break;
    }
    return 0;
}

int lx_gettimeofday(struct lx_timeval *tv, void *tz)
{
    struct timeval a;

    (void)tz;
    if (gettimeofday(&a, NULL) < 0)
    {
        lso_set_errno_from_aros();
        return -1;
    }
    tv->tv_sec = a.tv_sec;
    tv->tv_usec = a.tv_usec;
    return 0;
}

int lso_clock_gettime(int lx_clock, struct timespec *ts)
{
    int id;

    switch (lx_clock)
    {
    case 0:  /* REALTIME */
    case 5:  /* REALTIME_COARSE */
        id = CLOCK_REALTIME;
        break;
    case 2:  /* PROCESS_CPUTIME_ID: no per-process accounting, use uptime */
    case 3:  /* THREAD_CPUTIME_ID */
    default: /* MONOTONIC, MONOTONIC_RAW, MONOTONIC_COARSE, BOOTTIME */
        id = CLOCK_MONOTONIC;
        break;
    }
    if (clock_gettime(id, ts) < 0)
    {
        lso_set_errno_from_aros();
        return -1;
    }
    return 0;
}

int lx_clock_gettime(int lx_clock, struct lx_timespec *ts)
{
    struct timespec a;

    if (lso_clock_gettime(lx_clock, &a) < 0)
        return -1;
    ts->tv_sec = a.tv_sec;
    ts->tv_nsec = a.tv_nsec;
    return 0;
}

int lx_nanosleep(const struct lx_timespec *req, struct lx_timespec *rem)
{
    struct timespec a, r = { 0, 0 };

    a.tv_sec = (time_t)req->tv_sec;
    a.tv_nsec = (long)req->tv_nsec;
    if (nanosleep(&a, &r) < 0)
    {
        if (rem)
        {
            rem->tv_sec = r.tv_sec;
            rem->tv_nsec = r.tv_nsec;
        }
        lso_set_errno_from_aros();
        return -1;
    }
    return 0;
}

static int lx_usleep(unsigned int us)
{
    return usleep(us);
}

static int lx_sched_yield(void)
{
    return sched_yield();
}

static int lx_sched_setscheduler(int pid, int policy, const void *param)
{
    (void)pid; (void)policy; (void)param;
    return 0;
}

static int lx_setpriority(int which, unsigned int who, int prio)
{
    (void)which; (void)who; (void)prio;
    return 0;
}

#define LX_PR_SET_NAME 15
#define LX_PR_GET_NAME 16

int lx_prctl(int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    (void)a3; (void)a4; (void)a5;
    if (option == LX_PR_SET_NAME)
        return lx_pthread_setname_np(lx_pthread_self(), (const char *)a2);
    if (option == LX_PR_GET_NAME)
    {
        struct Task *t = FindTask(NULL);
        strncpy((char *)a2, t->tc_Node.ln_Name ? t->tc_Node.ln_Name : "", 15);
        ((char *)a2)[15] = 0;
        return 0;
    }
    LSO_DEBUG("prctl(%d) ignored", option);
    return 0;
}

static int lx_register_atfork(void *prepare, void *parent, void *child, void *dso)
{
    (void)prepare; (void)parent; (void)child; (void)dso;
    return 0;
}

unsigned int lso_ncpus(void)
{
    static unsigned int n;

    if (!n)
    {
        APTR KernelBase = OpenResource("kernel.resource");
        if (KernelBase)
            n = KrnGetCPUCount();
        if (!n)
            n = 1;
    }
    return n;
}

/* glibc's numeric sysconf names */
static long lx_sysconf(int name)
{
    switch (name)
    {
    case 0:   return 2097152;               /* _SC_ARG_MAX */
    case 2:   return 100;                   /* _SC_CLK_TCK */
    case 4:   return 1024;                  /* _SC_OPEN_MAX */
    case 30:  return LSO_PAGE;              /* _SC_PAGESIZE */
    case 70:  return 1024;                  /* _SC_GETPW_R_SIZE_MAX */
    case 75:  return 16384;                 /* _SC_THREAD_STACK_MIN */
    case 83:                                /* _SC_NPROCESSORS_CONF */
    case 84:  return lso_ncpus();           /* _SC_NPROCESSORS_ONLN */
    case 85:  return AvailMem(MEMF_TOTAL) / LSO_PAGE;  /* _SC_PHYS_PAGES */
    case 86:  return AvailMem(MEMF_ANY) / LSO_PAGE;    /* _SC_AVPHYS_PAGES */
    case 149: return 200809L;               /* _SC_MONOTONIC_CLOCK */
    case 190: return 64;                    /* _SC_LEVEL1_DCACHE_LINESIZE */
    default:
        LSO_DEBUG("sysconf(%d) unknown", name);
        lso_set_errno(LX_EINVAL);
        return -1;
    }
}

struct lx_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };

int lx_uname(void *out)
{
    struct lx_utsname *u = out;
    struct utsname a;

    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "Linux");
    if (uname(&a) == 0)
        strncpy(u->nodename, a.nodename, sizeof(u->nodename) - 1);
    else
        strcpy(u->nodename, "aros");
    strcpy(u->release, "6.8.0");
    strcpy(u->version, "#1 SMP");
    strcpy(u->machine, "x86_64");
    strcpy(u->domainname, "(none)");
    return 0;
}

static long lx_syscall(long nr, ...)
{
    va_list ap;
    long a[6], i, r;

    va_start(ap, nr);
    for (i = 0; i < 6; i++)
        a[i] = va_arg(ap, long);
    va_end(ap);
    r = lso_syscall(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
    if (r < 0 && r > -4096)
    {
        lso_set_errno((int)-r);
        return -1;
    }
    return r;
}

static time_t lx_timegm(struct tm *tm)
{
    /* days from civil, proleptic Gregorian (Howard Hinnant's algorithm) */
    long y = tm->tm_year + 1900;
    long m = tm->tm_mon + 1;
    long d = tm->tm_mday;
    long era, yoe, doy, doe, days;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return (time_t)days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

static int lx_bcmp(const void *a, const void *b, size_t n)
{
    return memcmp(a, b, n);
}

static const char *lx_strerror_text(int lx)
{
    switch (lx)
    {
    case 0:            return "Success";
    case LX_EPERM:     return "Operation not permitted";
    case LX_ENOENT:    return "No such file or directory";
    case LX_EINTR:     return "Interrupted system call";
    case LX_EIO:       return "Input/output error";
    case LX_EBADF:     return "Bad file descriptor";
    case LX_EAGAIN:    return "Resource temporarily unavailable";
    case LX_ENOMEM:    return "Cannot allocate memory";
    case LX_EACCES:    return "Permission denied";
    case LX_EBUSY:     return "Device or resource busy";
    case LX_EEXIST:    return "File exists";
    case LX_EINVAL:    return "Invalid argument";
    case LX_ENOSYS:    return "Function not implemented";
    case LX_ENOTSUP:   return "Operation not supported";
    case LX_ETIMEDOUT: return "Connection timed out";
    default:           return NULL;
    }
}

/* GNU strerror_r: returns a string, not necessarily buf */
static char *lx_strerror_r(int lx, char *buf, size_t len)
{
    const char *t = lx_strerror_text(lx);

    if (t)
        return (char *)t;
    snprintf(buf, len, "Unknown error %d", lx);
    return buf;
}

static void lx_perror(const char *s)
{
    char buf[64];
    int e = *lso_errno_location();

    fprintf(stderr, "%s%s%s\n", s ? s : "", s && *s ? ": " : "", lx_strerror_r(e, buf, sizeof(buf)));
}

static char *lx_getenv(const char *name)
{
    return getenv(name);
}

static unsigned int lx_geteuid(void) { return geteuid(); }
static int lx_getpid(void)           { return getpid(); }

/* ---- locale --------------------------------------------------------------- */

#define LX_LC_GLOBAL_LOCALE ((void *)-1L)
static int lx_fake_locale;

static void *lx_newlocale(int mask, const char *name, void *base)
{
    (void)mask; (void)name; (void)base;
    return &lx_fake_locale;
}

static void lx_freelocale(void *loc) { (void)loc; }

static void *lx_uselocale(void *loc)
{
    (void)loc;
    return LX_LC_GLOBAL_LOCALE;
}

static double lx_strtod_l(const char *s, char **end, void *loc)       { (void)loc; return strtod(s, end); }
static float lx_strtof_l(const char *s, char **end, void *loc)        { (void)loc; return strtof(s, end); }
static long double lx_strtold_l(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
static long long lx_strtoll_l(const char *s, char **end, int base, void *loc)
{
    (void)loc;
    return strtoll(s, end, base);
}
static unsigned long long lx_strtoull_l(const char *s, char **end, int base, void *loc)
{
    (void)loc;
    return strtoull(s, end, base);
}
static size_t lx_strftime_l(char *s, size_t max, const char *fmt, const struct tm *tm, void *loc)
{
    (void)loc;
    return strftime(s, max, fmt, tm);
}

/* ---- fortified and renamed stdio/string ---------------------------------- */

static int lx_vsnprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list ap)
{
    (void)flag; (void)slen;
    return vsnprintf(s, maxlen, fmt, ap);
}

static void *lx_memcpy_chk(void *d, const void *s, size_t n, size_t dlen)
{
    if (n > dlen)
        lx_stack_chk_fail();
    return memcpy(d, s, n);
}

static char *lx_strncat_chk(char *d, const char *s, size_t n, size_t dlen)
{
    (void)dlen;
    return strncat(d, s, n);
}

static int lx_isoc99_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

static int lx_isoc99_vsscanf(const char *s, const char *fmt, va_list ap)
{
    return vsscanf(s, fmt, ap);
}

static size_t lx_mbrlen(const char *s, size_t n, void *ps)
{
    return mbrlen(s, n, (mbstate_t *)ps);
}

/* glibc mbstate_t is 8 bytes, AROS's is 12: give the CRT its own state */
static mbstate_t shim_mbstate;

static size_t lx_mbrtowc(wchar_t *pwc, const char *s, size_t n, void *ps)
{
    (void)ps;
    return mbrtowc(pwc, s, n, &shim_mbstate);
}

static size_t lx_wcrtomb(char *s, wchar_t wc, void *ps)
{
    (void)ps;
    return wcrtomb(s, wc, &shim_mbstate);
}

static size_t lx_mbsrtowcs(wchar_t *dst, const char **src, size_t len, void *ps)
{
    (void)ps;
    return mbsrtowcs(dst, src, len, &shim_mbstate);
}

static size_t lx_mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps)
{
    (void)ps;
    return mbsnrtowcs(dst, src, nms, len, &shim_mbstate);
}

static size_t lx_wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps)
{
    (void)ps;
    return wcsnrtombs(dst, src, nwc, len, &shim_mbstate);
}

/* ---- message catalogs ----------------------------------------------------- */

static void *lx_catopen(const char *name, int flag)
{
    (void)name; (void)flag;
    return (void *)-1;
}

static char *lx_catgets(void *catd, int set, int msg, const char *dflt)
{
    (void)catd; (void)set; (void)msg;
    return (char *)dflt;
}

static int lx_catclose(void *catd)
{
    (void)catd;
    return 0;
}

/* ---- the table ------------------------------------------------------------ */

/* Keep sorted by strcmp order: lso_shim_lookup() binary-searches. */
static const struct lso_symbol shim_table[] = {
    { "OPENSSL_memory_alloc",   NULL },
    { "OPENSSL_memory_free",    NULL },
    { "OPENSSL_memory_get_size", NULL },
    { "_ITM_deregisterTMCloneTable", NULL },
    { "_ITM_registerTMCloneTable", NULL },
    { "_Unwind_DeleteException", (void *)_Unwind_DeleteException },
    { "_Unwind_GetIP",          (void *)_Unwind_GetIP },
    { "_Unwind_GetLanguageSpecificData", (void *)_Unwind_GetLanguageSpecificData },
    { "_Unwind_GetRegionStart", (void *)_Unwind_GetRegionStart },
    { "_Unwind_RaiseException", (void *)_Unwind_RaiseException },
    { "_Unwind_Resume",         (void *)_Unwind_Resume },
    { "_Unwind_SetGR",          (void *)_Unwind_SetGR },
    { "_Unwind_SetIP",          (void *)_Unwind_SetIP },
    { "__ctype_get_mb_cur_max", (void *)lx_ctype_get_mb_cur_max },
    { "__ctype_tolower_loc",    (void *)lx_ctype_tolower_loc },
    { "__ctype_toupper_loc",    (void *)lx_ctype_toupper_loc },
    { "__cxa_atexit",           (void *)lx_cxa_atexit },
    { "__cxa_finalize",         (void *)lx_cxa_finalize },
    { "__errno_location",       (void *)lso_errno_location },
    { "__fxstat64",             (void *)lx_fxstat64 },
    { "__gmon_start__",         NULL },
    { "__isoc99_sscanf",        (void *)lx_isoc99_sscanf },
    { "__isoc99_vsscanf",       (void *)lx_isoc99_vsscanf },
    { "__libc_stack_end",       &lx_libc_stack_end },
    { "__lxstat64",             (void *)lx_lxstat64 },
    { "__mbrlen",               (void *)lx_mbrlen },
    { "__memcpy_chk",           (void *)lx_memcpy_chk },
    { "__register_atfork",      (void *)lx_register_atfork },
    { "__sched_cpucount",       (void *)lx_sched_cpucount },
    { "__stack_chk_fail",       (void *)lx_stack_chk_fail },
    { "__strncat_chk",          (void *)lx_strncat_chk },
    { "__tls_get_addr",         (void *)lso_tls_get_addr },
    { "__vsnprintf_chk",        (void *)lx_vsnprintf_chk },
    { "__xstat64",              (void *)lx_xstat64 },
    { "_setjmp",                (void *)setjmp },
    { "abort",                  (void *)lx_abort },
    { "bcmp",                   (void *)lx_bcmp },
    { "catclose",               (void *)lx_catclose },
    { "catgets",                (void *)lx_catgets },
    { "catopen",                (void *)lx_catopen },
    { "ceil",                   (void *)ceil },
    { "ceilf",                  (void *)ceilf },
    { "clock_gettime",          (void *)lx_clock_gettime },
    { "close",                  (void *)lx_close },
    { "creat64",                (void *)lx_creat64 },
    { "dlclose",                (void *)lx_dlclose },
    { "dlerror",                (void *)lx_dlerror },
    { "dlopen",                 (void *)lx_dlopen },
    { "dlsym",                  (void *)lx_dlsym },
    { "exp",                    (void *)exp },
    { "fclose",                 (void *)fclose },
    { "fflush",                 (void *)fflush },
    { "fgets",                  (void *)fgets },
    { "floor",                  (void *)floor },
    { "fopen64",                (void *)lso_fopen },
    { "fprintf",                (void *)fprintf },
    { "fputc",                  (void *)fputc },
    { "fputwc",                 (void *)fputwc },
    { "freelocale",             (void *)lx_freelocale },
    { "frexp",                  (void *)frexp },
    { "fstat64",                (void *)lx_fstat64 },
    { "fwrite",                 (void *)fwrite },
    { "getauxval",              (void *)lx_getauxval },
    { "getc",                   (void *)getc },
    { "getenv",                 (void *)lx_getenv },
    { "geteuid",                (void *)lx_geteuid },
    { "getpid",                 (void *)lx_getpid },
    { "getrlimit64",            (void *)lx_getrlimit64 },
    { "gettimeofday",           (void *)lx_gettimeofday },
    { "getwc",                  (void *)getwc },
    { "gmtime_r",               (void *)gmtime_r },
    { "ldexp",                  (void *)ldexp },
    { "localtime_r",            (void *)localtime_r },
    { "log",                    (void *)log },
    { "log2",                   (void *)log2 },
    { "longjmp",                (void *)longjmp },
    { "lseek64",                (void *)lx_lseek64 },
    { "madvise",                (void *)lx_madvise },
    { "mbrtowc",                (void *)lx_mbrtowc },
    { "mbsnrtowcs",             (void *)lx_mbsnrtowcs },
    { "mbsrtowcs",              (void *)lx_mbsrtowcs },
    { "mbtowc",                 (void *)mbtowc },
    { "memchr",                 (void *)memchr },
    { "memcmp",                 (void *)memcmp },
    { "memcpy",                 (void *)memcpy },
    { "memmove",                (void *)memmove },
    { "memset",                 (void *)memset },
    { "mktime",                 (void *)mktime },
    { "mmap64",                 (void *)lx_mmap64 },
    { "mprotect",               (void *)lx_mprotect },
    { "munmap",                 (void *)lx_munmap },
    { "nanosleep",              (void *)lx_nanosleep },
    { "newlocale",              (void *)lx_newlocale },
    { "open64",                 (void *)lx_open64 },
    { "perror",                 (void *)lx_perror },
    { "prctl",                  (void *)lx_prctl },
    { "pread64",                (void *)lx_pread64 },
    { "pthread_attr_destroy",   (void *)lx_pthread_attr_destroy },
    { "pthread_attr_getdetachstate", (void *)lx_pthread_attr_getdetachstate },
    { "pthread_attr_getstacksize", (void *)lx_pthread_attr_getstacksize },
    { "pthread_attr_init",      (void *)lx_pthread_attr_init },
    { "pthread_attr_setdetachstate", (void *)lx_pthread_attr_setdetachstate },
    { "pthread_attr_setstacksize", (void *)lx_pthread_attr_setstacksize },
    { "pthread_cond_broadcast", (void *)lx_pthread_cond_broadcast },
    { "pthread_cond_destroy",   (void *)lx_pthread_cond_destroy },
    { "pthread_cond_init",      (void *)lx_pthread_cond_init },
    { "pthread_cond_signal",    (void *)lx_pthread_cond_signal },
    { "pthread_cond_timedwait", (void *)lx_pthread_cond_timedwait },
    { "pthread_cond_wait",      (void *)lx_pthread_cond_wait },
    { "pthread_condattr_destroy", (void *)lx_pthread_condattr_destroy },
    { "pthread_condattr_init",  (void *)lx_pthread_condattr_init },
    { "pthread_condattr_setclock", (void *)lx_pthread_condattr_setclock },
    { "pthread_create",         (void *)lx_pthread_create },
    { "pthread_detach",         (void *)lx_pthread_detach },
    { "pthread_equal",          (void *)lx_pthread_equal },
    { "pthread_exit",           (void *)lx_pthread_exit },
    { "pthread_getaffinity_np", (void *)lx_pthread_getaffinity_np },
    { "pthread_getspecific",    (void *)lx_pthread_getspecific },
    { "pthread_join",           (void *)lx_pthread_join },
    { "pthread_key_create",     (void *)lx_pthread_key_create },
    { "pthread_key_delete",     (void *)lx_pthread_key_delete },
    { "pthread_mutex_destroy",  (void *)lx_pthread_mutex_destroy },
    { "pthread_mutex_init",     (void *)lx_pthread_mutex_init },
    { "pthread_mutex_lock",     (void *)lx_pthread_mutex_lock },
    { "pthread_mutex_trylock",  (void *)lx_pthread_mutex_trylock },
    { "pthread_mutex_unlock",   (void *)lx_pthread_mutex_unlock },
    { "pthread_mutexattr_destroy", (void *)lx_pthread_mutexattr_destroy },
    { "pthread_mutexattr_init", (void *)lx_pthread_mutexattr_init },
    { "pthread_mutexattr_settype", (void *)lx_pthread_mutexattr_settype },
    { "pthread_once",           (void *)lx_pthread_once },
    { "pthread_rwlock_destroy", (void *)lx_pthread_rwlock_destroy },
    { "pthread_rwlock_init",    (void *)lx_pthread_rwlock_init },
    { "pthread_rwlock_rdlock",  (void *)lx_pthread_rwlock_rdlock },
    { "pthread_rwlock_tryrdlock", (void *)lx_pthread_rwlock_tryrdlock },
    { "pthread_rwlock_trywrlock", (void *)lx_pthread_rwlock_trywrlock },
    { "pthread_rwlock_unlock",  (void *)lx_pthread_rwlock_unlock },
    { "pthread_rwlock_wrlock",  (void *)lx_pthread_rwlock_wrlock },
    { "pthread_self",           (void *)lx_pthread_self },
    { "pthread_setname_np",     (void *)lx_pthread_setname_np },
    { "pthread_setspecific",    (void *)lx_pthread_setspecific },
    { "qsort",                  (void *)qsort },
    { "rand",                   (void *)rand },
    { "read",                   (void *)lx_read },
    { "round",                  (void *)round },
    { "sched_getaffinity",      (void *)lx_sched_getaffinity },
    { "sched_setscheduler",     (void *)lx_sched_setscheduler },
    { "sched_yield",            (void *)lx_sched_yield },
    { "sem_destroy",            (void *)lx_sem_destroy },
    { "sem_init",               (void *)lx_sem_init },
    { "sem_post",               (void *)lx_sem_post },
    { "sem_trywait",            (void *)lx_sem_trywait },
    { "sem_wait",               (void *)lx_sem_wait },
    { "setpriority",            (void *)lx_setpriority },
    { "snprintf",               (void *)snprintf },
    { "stat64",                 (void *)lx_stat64 },
    { "stderr",                 &lx_stderr },
    { "stdin",                  &lx_stdin },
    { "stdout",                 &lx_stdout },
    { "strchr",                 (void *)strchr },
    { "strcmp",                 (void *)strcmp },
    { "strerror_r",             (void *)lx_strerror_r },
    { "strftime_l",             (void *)lx_strftime_l },
    { "strlen",                 (void *)strlen },
    { "strncpy",                (void *)strncpy },
    { "strrchr",                (void *)strrchr },
    { "strstr",                 (void *)strstr },
    { "strtod_l",               (void *)lx_strtod_l },
    { "strtof_l",               (void *)lx_strtof_l },
    { "strtol",                 (void *)strtol },
    { "strtold_l",              (void *)lx_strtold_l },
    { "strtoll_l",              (void *)lx_strtoll_l },
    { "strtoul",                (void *)strtoul },
    { "strtoull_l",             (void *)lx_strtoull_l },
    { "syscall",                (void *)lx_syscall },
    { "sysconf",                (void *)lx_sysconf },
    { "timegm",                 (void *)lx_timegm },
    { "uname",                  (void *)lx_uname },
    { "ungetc",                 (void *)ungetc },
    { "ungetwc",                (void *)ungetwc },
    { "uselocale",              (void *)lx_uselocale },
    { "usleep",                 (void *)lx_usleep },
    { "vasprintf",              (void *)vasprintf },
    { "vfprintf",               (void *)vfprintf },
    { "vsnprintf",              (void *)vsnprintf },
    { "wcrtomb",                (void *)lx_wcrtomb },
    { "wcslen",                 (void *)wcslen },
    { "wcsnrtombs",             (void *)lx_wcsnrtombs },
    { "wmemchr",                (void *)wmemchr },
    { "write",                  (void *)lx_write },
};

#define SHIM_COUNT (sizeof(shim_table) / sizeof(shim_table[0]))

void *lso_shim_lookup(const char *name)
{
    size_t lo = 0, hi = SHIM_COUNT;

    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(shim_table[mid].name, name);
        if (c == 0)
            return shim_table[mid].addr;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

void lso_shim_init(void)
{
    struct Task *t = FindTask(NULL);
    size_t i;

    lx_stdin = stdin;
    lx_stdout = stdout;
    lx_stderr = stderr;
    lx_libc_stack_end = t->tc_SPUpper;
    ctype_init();

    /* the binary search silently misses entries out of order: check once */
    for (i = 1; i < SHIM_COUNT; i++)
    {
        if (strcmp(shim_table[i - 1].name, shim_table[i].name) >= 0)
            lso_log("shim table out of order at '%s' / '%s'", shim_table[i - 1].name, shim_table[i].name);
    }
}
