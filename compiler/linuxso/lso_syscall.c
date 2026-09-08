/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    linuxso Linux x86-64 system call emulation.

    Two paths end up here: the guest's syscall() import, and (native only)
    inline `syscall` instructions.  With IA32_EFER.SCE off those raise #UD;
    a kernel.resource exception handler recognises the 0f 05 opcode,
    parks the return address on the task's stack and redirects RIP to
    lso_syscall_tramp, which calls lso_syscall() in task context.

    Results follow the raw kernel convention: -errno (Linux numbering) on
    failure.  Unknown numbers are logged and answered with -ENOSYS so the
    set the guest actually needs can be grown from the trace.
*/

#include <proto/exec.h>
#include <proto/kernel.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <aros/x86_64/cpucontext.h>
#include <aros/symbolsets.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "linuxso_intern.h"
#include "lso_lx.h"

extern void lso_syscall_tramp(void);

/* ---- #UD trap ------------------------------------------------------------ */

static APTR trap_handle;
static APTR trap_KernelBase;

/*
 * Runs in trap context on every #UD in the system; only a `syscall`
 * opcode is ours.  No logging here: the task's own context does that.
 */
static int ud_handler(struct ExceptionContext *regs, void *d1, void *d2)
{
    const unsigned char *ip = (const unsigned char *)regs->rip;
    UQUAD *slot;

    (void)d1; (void)d2;
    if (!ip || ip[0] != 0x0f || ip[1] != 0x05)
        return 0;

    /* return address below the red zone, then divert to the trampoline */
    slot = (UQUAD *)(regs->rsp - 136);
    *slot = regs->rip + 2;
    regs->rsp = (UQUAD)slot;
    regs->rip = (UQUAD)lso_syscall_tramp;
    return 1;
}

int lso_syscall_trap_install(void)
{
    APTR KernelBase;

    if (trap_handle)
        return 0;
    /* hosted kernels execute syscall natively; only native needs the trap */
    if (OpenResource("hostlib.resource"))
    {
        LSO_DEBUG("hosted kernel: raw syscalls run on the host, no #UD trap");
        return 0;
    }
    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
        return -1;
    trap_handle = KrnAddExceptionHandler(6, ud_handler, NULL, NULL);
    trap_KernelBase = KernelBase;
    LSO_DEBUG("#UD syscall trap %s", trap_handle ? "installed" : "NOT installed");
    return trap_handle ? 0 : -1;
}

void lso_syscall_trap_remove(void)
{
    APTR KernelBase = trap_KernelBase;

    if (trap_handle && KernelBase)
    {
        KrnRemExceptionHandler(trap_handle);
        trap_handle = NULL;
    }
}

/* the handler lives in this program's memory: it must go before we do */
ADD2EXIT(lso_syscall_trap_remove, 0);

/* ---- helpers -------------------------------------------------------------- */

#define LX_ENOTTY 25
#define LX_ECHILD 10

static long neg_errno(void)
{
    int e = *lso_errno_location();
    return -(e ? e : LX_EIO);
}

static int have_rdrand(void)
{
    static int r = -1;

    if (r < 0)
    {
        unsigned int a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        r = (c >> 30) & 1;
    }
    return r;
}

long lso_getrandom(void *buf, size_t len)
{
    unsigned char *p = buf;
    size_t i = 0;

    while (i < len)
    {
        unsigned long v;
        unsigned char ok = 0;

        if (have_rdrand())
        {
            int tries = 10;
            while (tries-- && !ok)
                __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        }
        if (!ok)
        {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            v = ((unsigned long)rand() << 32) ^ (unsigned long)rand() ^ tv.tv_usec ^ (uintptr_t)&v;
        }
        memcpy(p + i, &v, len - i < sizeof(v) ? len - i : sizeof(v));
        i += sizeof(v);
    }
    return len;
}

static long sys_gettid(void)
{
    /* glibc pthread ids are 0 for foreign tasks; fold the Task pointer instead */
    uintptr_t t = (uintptr_t)FindTask(NULL);
    return (long)(((t >> 4) ^ (t >> 32)) & 0x3fffffff) + 2;
}

/* ---- futex ---------------------------------------------------------------- */

#define LX_FUTEX_WAIT        0
#define LX_FUTEX_WAKE        1
#define LX_FUTEX_WAIT_BITSET 9
#define LX_FUTEX_WAKE_BITSET 10
#define LX_FUTEX_PRIVATE     128
#define LX_FUTEX_CLOCK_REALTIME 256
#define LX_FUTEX_CMD_MASK    (~(LX_FUTEX_PRIVATE | LX_FUTEX_CLOCK_REALTIME))

struct futex_waiter {
    struct MinNode node;
    volatile int *addr;
    struct Task *task;
    unsigned int bitset;
    BYTE sigbit;
    int woken;
};

static struct MinList futex_waiters;
static struct SignalSemaphore futex_lock;
static int futex_ready;

static void futex_init(void)
{
    Forbid();
    if (!futex_ready)
    {
        NEWLIST(&futex_waiters);
        InitSemaphore(&futex_lock);
        futex_ready = 1;
    }
    Permit();
}

/* relative timeout in microseconds, or -1 for none */
static long futex_timeout_us(int cmd, int op, const struct lx_timespec *ts)
{
    struct timespec now;
    long us;

    if (!ts)
        return -1;
    if (cmd == LX_FUTEX_WAIT)
        return ts->tv_sec * 1000000L + ts->tv_nsec / 1000;
    /* WAIT_BITSET: absolute on CLOCK_MONOTONIC unless CLOCK_REALTIME is set */
    lso_clock_gettime((op & LX_FUTEX_CLOCK_REALTIME) ? 0 : 1, &now);
    us = (ts->tv_sec - now.tv_sec) * 1000000L + (ts->tv_nsec - now.tv_nsec) / 1000;
    return us < 0 ? 0 : us;
}

static long sys_futex(volatile int *uaddr, int op, int val, const struct lx_timespec *ts,
                      volatile int *uaddr2, unsigned int val3)
{
    int cmd = op & LX_FUTEX_CMD_MASK;
    struct futex_waiter w, *n, *n2;

    (void)uaddr2;
    futex_init();
    switch (cmd)
    {
    case LX_FUTEX_WAIT:
    case LX_FUTEX_WAIT_BITSET:
    {
        long us = futex_timeout_us(cmd, op, ts);
        struct MsgPort *port = NULL;
        struct timerequest *tr = NULL;
        ULONG sigmask, got;
        int timedout = 0;

        w.addr = uaddr;
        w.task = FindTask(NULL);
        w.bitset = cmd == LX_FUTEX_WAIT ? ~0u : val3;
        w.woken = 0;
        w.sigbit = AllocSignal(-1);
        if (w.sigbit < 0)
            return -LX_EAGAIN;
        sigmask = 1UL << w.sigbit;

        ObtainSemaphore(&futex_lock);
        if (*uaddr != val)
        {
            ReleaseSemaphore(&futex_lock);
            FreeSignal(w.sigbit);
            return -LX_EAGAIN;
        }
        AddTail((struct List *)&futex_waiters, (struct Node *)&w.node);
        ReleaseSemaphore(&futex_lock);

        if (us == 0)
        {
            timedout = 1;
        }
        else
        {
            if (us > 0)
            {
                port = CreateMsgPort();
                tr = port ? (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : NULL;
                if (tr && OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)tr, 0) == 0)
                {
                    tr->tr_node.io_Command = TR_ADDREQUEST;
                    tr->tr_time.tv_secs = us / 1000000;
                    tr->tr_time.tv_micro = us % 1000000;
                    SendIO((struct IORequest *)tr);
                    sigmask |= 1UL << port->mp_SigBit;
                }
                else
                {
                    if (tr)
                        DeleteIORequest((struct IORequest *)tr);
                    if (port)
                        DeleteMsgPort(port);
                    tr = NULL;
                    port = NULL;
                }
            }
            got = Wait(sigmask);
            if (tr)
            {
                if (!CheckIO((struct IORequest *)tr))
                    AbortIO((struct IORequest *)tr);
                WaitIO((struct IORequest *)tr);
                if ((got & (1UL << port->mp_SigBit)) && !(got & (1UL << w.sigbit)))
                    timedout = 1;
                CloseDevice((struct IORequest *)tr);
                DeleteIORequest((struct IORequest *)tr);
                DeleteMsgPort(port);
            }
        }

        ObtainSemaphore(&futex_lock);
        if (!w.woken)
            Remove((struct Node *)&w.node);
        ReleaseSemaphore(&futex_lock);
        FreeSignal(w.sigbit);
        if (w.woken)
            return 0;
        return timedout ? -LX_ETIMEDOUT : -LX_EINTR;
    }
    case LX_FUTEX_WAKE:
    case LX_FUTEX_WAKE_BITSET:
    {
        unsigned int mask = cmd == LX_FUTEX_WAKE ? ~0u : val3;
        long woken = 0;

        ObtainSemaphore(&futex_lock);
        ForeachNodeSafe(&futex_waiters, n, n2)
        {
            if (woken >= val)
                break;
            if (n->addr == uaddr && (n->bitset & mask))
            {
                Remove((struct Node *)&n->node);
                n->woken = 1;
                Signal(n->task, 1UL << n->sigbit);
                woken++;
            }
        }
        ReleaseSemaphore(&futex_lock);
        return woken;
    }
    default:
        LSO_DEBUG("futex op %d unsupported", op);
        return -LX_ENOSYS;
    }
}

/* ---- the dispatcher ------------------------------------------------------- */

#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
#define SYS_stat            4
#define SYS_fstat           5
#define SYS_lstat           6
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_mprotect        10
#define SYS_munmap          11
#define SYS_brk             12
#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_ioctl           16
#define SYS_pread64         17
#define SYS_sched_yield     24
#define SYS_madvise         28
#define SYS_nanosleep       35
#define SYS_getpid          39
#define SYS_exit            60
#define SYS_kill            62
#define SYS_uname           63
#define SYS_fcntl           72
#define SYS_getcwd          79
#define SYS_readlink        89
#define SYS_gettimeofday    96
#define SYS_getrlimit       97
#define SYS_sysinfo         99
#define SYS_ptrace          101
#define SYS_getuid          102
#define SYS_getgid          104
#define SYS_geteuid         107
#define SYS_getegid         108
#define SYS_getppid         110
#define SYS_sigaltstack     131
#define SYS_prctl           157
#define SYS_arch_prctl      158
#define SYS_gettid          186
#define SYS_futex           202
#define SYS_sched_getaffinity 204
#define SYS_clock_gettime   228
#define SYS_clock_getres    229
#define SYS_clock_nanosleep 230
#define SYS_exit_group      231
#define SYS_tgkill          234
#define SYS_openat          257
#define SYS_newfstatat      262
#define SYS_set_robust_list 273
#define SYS_prlimit64       302
#define SYS_getrandom       318
#define SYS_membarrier      324
#define SYS_statx           332
#define SYS_rseq            334

#define LX_AT_FDCWD       (-100)
#define LX_AT_EMPTY_PATH  0x1000
#define LX_TIMER_ABSTIME  1
#define LX_SIGABRT        6
#define LX_SIGKILL        9
#define LX_ARCH_GET_FS    0x1003

static long ret_or_errno(long r)
{
    return r < 0 ? neg_errno() : r;
}

long lso_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long r;

    switch (nr)
    {
    case SYS_read:      r = ret_or_errno(lx_read((int)a1, (void *)a2, (size_t)a3)); break;
    case SYS_write:     r = ret_or_errno(lx_write((int)a1, (const void *)a2, (size_t)a3)); break;
    case SYS_open:      r = ret_or_errno(lso_open((const char *)a1, (int)a2, (int)a3)); break;
    case SYS_openat:
        if (a1 != LX_AT_FDCWD && ((const char *)a2)[0] != '/')
        {
            LSO_DEBUG("openat(dirfd %ld, %s) relative dirfd unsupported", a1, (const char *)a2);
            r = -LX_ENOTSUP;
        }
        else
            r = ret_or_errno(lso_open((const char *)a2, (int)a3, (int)a4));
        break;
    case SYS_close:     r = ret_or_errno(lx_close((int)a1)); break;
    case SYS_stat:
    case SYS_lstat:     r = ret_or_errno(lso_stat_to_glibc((const char *)a1, -1, (void *)a2)); break;
    case SYS_fstat:     r = ret_or_errno(lso_stat_to_glibc(NULL, (int)a1, (void *)a2)); break;
    case SYS_newfstatat:
        if ((a4 & LX_AT_EMPTY_PATH) && ((const char *)a2)[0] == 0)
            r = ret_or_errno(lso_stat_to_glibc(NULL, (int)a1, (void *)a3));
        else if (a1 != LX_AT_FDCWD && ((const char *)a2)[0] != '/')
            r = -LX_ENOTSUP;
        else
            r = ret_or_errno(lso_stat_to_glibc((const char *)a2, -1, (void *)a3));
        break;
    case SYS_lseek:     r = ret_or_errno(lx_lseek64((int)a1, a2, (int)a3)); break;
    case SYS_pread64:   r = ret_or_errno(lx_pread64((int)a1, (void *)a2, (size_t)a3, a4)); break;
    case SYS_mmap:
    {
        void *p = lx_mmap64((void *)a1, (size_t)a2, (int)a3, (int)a4, (int)a5, a6);
        r = p == (void *)-1 ? neg_errno() : (long)p;
        break;
    }
    case SYS_munmap:    r = ret_or_errno(lx_munmap((void *)a1, (size_t)a2)); break;
    case SYS_mprotect:  r = ret_or_errno(lx_mprotect((void *)a1, (size_t)a2, (int)a3)); break;
    case SYS_madvise:   r = ret_or_errno(lx_madvise((void *)a1, (size_t)a2, (int)a3)); break;
    case SYS_brk:       r = -LX_ENOMEM; break;
    case SYS_rt_sigaction:
    case SYS_rt_sigprocmask:
    case SYS_sigaltstack:
    case SYS_set_robust_list:
        r = 0; break;
    case SYS_rseq:      r = -LX_ENOSYS; break;
    case SYS_ioctl:     r = -LX_ENOTTY; break;
    case SYS_fcntl:
        switch (a2)
        {
        case 1: /* F_GETFD */ r = 0; break;
        case 2: /* F_SETFD */ r = 0; break;
        case 3: /* F_GETFL */ r = 0; break;
        case 4: /* F_SETFL */ r = 0; break;
        default: r = -LX_EINVAL; break;
        }
        break;
    case SYS_sched_yield: r = 0; sched_yield(); break;
    case SYS_nanosleep:
        r = lx_nanosleep((const struct lx_timespec *)a1, (struct lx_timespec *)a2) < 0 ? -LX_EINTR : 0;
        break;
    case SYS_clock_nanosleep:
    {
        struct lx_timespec req = *(const struct lx_timespec *)a3;
        if (a2 & LX_TIMER_ABSTIME)
        {
            struct timespec now;
            lso_clock_gettime((int)a1, &now);
            req.tv_sec -= now.tv_sec;
            req.tv_nsec -= now.tv_nsec;
            if (req.tv_nsec < 0) { req.tv_nsec += 1000000000L; req.tv_sec--; }
            if (req.tv_sec < 0) { r = 0; break; }
        }
        r = lx_nanosleep(&req, (struct lx_timespec *)a4) < 0 ? -LX_EINTR : 0;
        break;
    }
    case SYS_getpid:    r = getpid(); break;
    case SYS_getppid:   r = 1; break;
    case SYS_gettid:    r = sys_gettid(); break;
    case SYS_getuid:
    case SYS_geteuid:   r = geteuid(); break;
    case SYS_getgid:
    case SYS_getegid:   r = 1000; break;
    case SYS_uname:     r = lx_uname((void *)a1); break;
    case SYS_getcwd:
        if ((size_t)a2 < 2)
            r = -LX_ERANGE;
        else
        {
            strcpy((char *)a1, "/");
            r = 2;
        }
        break;
    case SYS_readlink:
        LSO_DEBUG("readlink(%s) -> ENOENT", (const char *)a1);
        r = -LX_ENOENT;
        break;
    case SYS_gettimeofday:
        r = lx_gettimeofday((struct lx_timeval *)a1, NULL) < 0 ? neg_errno() : 0;
        break;
    case SYS_clock_gettime:
        r = lx_clock_gettime((int)a1, (struct lx_timespec *)a2) < 0 ? neg_errno() : 0;
        break;
    case SYS_clock_getres:
        if (a2)
        {
            ((struct lx_timespec *)a2)->tv_sec = 0;
            ((struct lx_timespec *)a2)->tv_nsec = 1000;
        }
        r = 0;
        break;
    case SYS_getrlimit:  r = lx_getrlimit64((int)a1, (void *)a2); break;
    case SYS_prlimit64:
        if (a4)
            r = lx_getrlimit64((int)a2, (void *)a4);
        else
            r = 0;
        break;
    case SYS_sysinfo:    r = -LX_ENOSYS; break;
    case SYS_ptrace:     r = 0; break;   /* PTRACE_TRACEME succeeds: nobody is attached */
    case SYS_prctl:      r = lx_prctl((int)a1, a2, a3, a4, a5); break;
    case SYS_arch_prctl:
        if (a1 == LX_ARCH_GET_FS && trap_KernelBase)
        {
            /* rdmsr is supervisor-only; tasks run in user mode on pc-x86_64 */
            APTR KernelBase = trap_KernelBase;
            if (KrnIsSuper())
            {
                unsigned int lo, hi;
                __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000100));
                *(unsigned long *)a2 = ((unsigned long)hi << 32) | lo;
                r = 0;
            }
            else
                r = -LX_EINVAL;
        }
        else
        {
            LSO_DEBUG("arch_prctl(%#lx) refused", a1);
            r = -LX_EINVAL;
        }
        break;
    case SYS_futex:
        r = sys_futex((volatile int *)a1, (int)a2, (int)a3, (const struct lx_timespec *)a4,
                      (volatile int *)a5, (unsigned int)a6);
        break;
    case SYS_sched_getaffinity:
        r = lx_sched_getaffinity((int)a1, (size_t)a2, (unsigned long *)a3) < 0 ? -LX_EINVAL
            : (long)(a2 < 8 ? a2 : 8);
        break;
    case SYS_getrandom:  r = lso_getrandom((void *)a1, (size_t)a2); break;
    case SYS_membarrier: r = 0; break;
    case SYS_statx:      r = -LX_ENOSYS; break;
    case SYS_kill:
    case SYS_tgkill:
    {
        long sig = nr == SYS_kill ? a2 : a3;
        lso_log("guest %s(sig %ld)", nr == SYS_kill ? "kill" : "tgkill", sig);
        if (sig == LX_SIGABRT || sig == LX_SIGKILL)
            abort();
        r = 0;
        break;
    }
    case SYS_exit:
        LSO_DEBUG("thread exit(%ld)", a1);
        lx_pthread_exit((void *)a1);
        r = 0;
        break;
    case SYS_exit_group:
        lso_log("guest exit_group(%ld): aborting the process", a1);
        abort();
        break;
    default:
        lso_log("syscall %ld(%#lx, %#lx, %#lx, %#lx, %#lx, %#lx) unhandled -> ENOSYS",
                nr, a1, a2, a3, a4, a5, a6);
        r = -LX_ENOSYS;
        break;
    }
    if (linuxso_debug_enabled() && nr != SYS_futex && nr != SYS_clock_gettime)
        lso_log("syscall %ld(%#lx, %#lx, %#lx) -> %ld", nr, a1, a2, a3, r);
    return r;
}
