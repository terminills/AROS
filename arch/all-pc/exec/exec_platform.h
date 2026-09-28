/*
    Copyright (C) 2017-2026, The AROS Development Team. All rights reserved.
*/
#ifndef __EXEC_PLATFORM_H
#define __EXEC_PLATFORM_H

// needed to determine if this is an smp build
#include <aros/config.h>
#include <aros/atomic.h>

#ifndef __KERNEL_NOLIBBASE__
#define __KERNEL_NOLIBBASE__
#endif /* !__KERNEL_NOLIBBASE__ */
#include <proto/kernel.h>

#if (__WORDSIZE==64)
#define EXEC_REMTASK_NEEDSSWITCH
#endif
#if defined (__AROSEXEC_SMP__)
#define SCHEDQUANTUM_VALUE      10
#define SCHEDGRAN_VALUE         1
#else
#define SCHEDQUANTUM_VALUE      4
#define SCHEDGRAN_VALUE         1
#endif

#include "kernel_base.h"

#if defined(__AROSEXEC_SMP__)
#include "kernel_intern.h"
#include <aros/types/spinlock_s.h>
#include <utility/hooks.h>

#include "tls.h"
#include "etask.h"

/* special flag to get the scheduling code to unspin tasks */
#define TS_UNSPIN 0x10

extern struct Hook Exec_TaskSpinLockFailHook;
extern struct Hook Exec_TaskSpinLockForbidHook;
extern struct Hook Exec_TaskSpinLockDisableHook;
extern void Exec_TaskSpinUnlock(spinlock_t *);

struct ExecSpinSCData
{
    spinlock_t *lock_ptr;
    struct Hook *lock_obtainhook;
    struct Hook *lock_failhook;
    ULONG lock_mode;
};

struct Exec_PlatformData
{
    spinlock_t *(*SpinLockCall)(spinlock_t *, struct Hook *, struct Hook *, ULONG);
};

#ifndef __KERNEL_NO_SPINLOCK_PROTOS__
extern void Kernel_49_KrnSpinInit(spinlock_t *, void *);
extern spinlock_t *Kernel_52_KrnSpinLock(spinlock_t *, struct Hook *, ULONG, void *);
extern void Kernel_53_KrnSpinUnLock(spinlock_t *, void *);
#endif

#define EXEC_SPINLOCK_INIT(a) Kernel_49_KrnSpinInit((a), NULL)
#define EXEC_SPINLOCK_LOCK(a,b,c) Kernel_52_KrnSpinLock((a), (b), (c), NULL)
#define EXEC_SPINLOCK_UNLOCK(a) Kernel_53_KrnSpinUnLock((a), NULL)

/*
 * Store-store barrier for publishing a freshly built structure to readers
 * that walk it without taking a lock. x86 does not reorder stores, so
 * keeping the compiler from doing it is enough.
 */
#define EXEC_MEMORY_BARRIER()   asm volatile("" ::: "memory")

/*
 * A task allowed on any CPU can be preempted between loading this CPU's
 * ScheduleData and using it, and resume on another CPU: it then reads or
 * changes the first CPU's state - its running task, its Forbid() count.
 * Make each such access with interrupts off. Tasks run with IOPL 3 (KrnCli
 * is a plain cli), so this works outside supervisor mode too.
 */
#define EXEC_PERCPU_ENTER() \
    ({ unsigned long __f; \
       /* step over the caller's red zone: the push is unknown to the compiler */ \
       asm volatile("lea -128(%%rsp),%%rsp; pushf; pop %0; lea 128(%%rsp),%%rsp; cli" \
                    : "=r"(__f) :: "memory"); \
       __f; })
#define EXEC_PERCPU_EXIT(f) \
    do { if ((f) & (1 << 9)) asm volatile("sti" ::: "memory"); } while (0)

#if defined(AROS_NO_ATOMIC_OPERATIONS)
#define IDNESTCOUNT_INC \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->IDNestCnt++; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define IDNESTCOUNT_DEC \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->IDNestCnt--; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define TDNESTCOUNT_INC \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->TDNestCnt++; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define TDNESTCOUNT_DEC \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->TDNestCnt--; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDQUANTUM_CLEAR \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->ScheduleFlags &= ~TLSSF_Quantum; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDQUANTUM_SET \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->ScheduleFlags |= TLSSF_Quantum; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDSWITCH_CLEAR \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->ScheduleFlags &= ~TLSSF_Switch; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDSWITCH_SET \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->ScheduleFlags |= TLSSF_Switch; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDDISPATCH_CLEAR \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->ScheduleFlags &= ~TLSSF_Dispatch; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDDISPATCH_SET \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->ScheduleFlags |= TLSSF_Dispatch; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#else /* !AROS_NO_ATOMIC_OPERATIONS */
#define IDNESTCOUNT_INC \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __AROS_ATOMIC_INC_B(__schd->IDNestCnt); \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define IDNESTCOUNT_DEC \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __AROS_ATOMIC_DEC_B(__schd->IDNestCnt); \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define TDNESTCOUNT_INC \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __AROS_ATOMIC_INC_B(__schd->TDNestCnt); \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define TDNESTCOUNT_DEC \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __AROS_ATOMIC_DEC_B(__schd->TDNestCnt); \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDQUANTUM_CLEAR \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __AROS_ATOMIC_AND_L(__schd->ScheduleFlags, ~TLSSF_Quantum); \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDQUANTUM_SET \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __AROS_ATOMIC_OR_L(__schd->ScheduleFlags, TLSSF_Quantum); \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDSWITCH_CLEAR \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __AROS_ATOMIC_AND_L(__schd->ScheduleFlags, ~TLSSF_Switch); \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDSWITCH_SET \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __AROS_ATOMIC_OR_L(__schd->ScheduleFlags, TLSSF_Switch); \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDDISPATCH_CLEAR \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __AROS_ATOMIC_AND_L(__schd->ScheduleFlags, ~TLSSF_Dispatch); \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDDISPATCH_SET \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __AROS_ATOMIC_OR_L(__schd->ScheduleFlags, TLSSF_Dispatch); \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#endif /* !AROS_NO_ATOMIC_OPERATIONS */
/*
 * Block IRQ-exit dispatch without going through Forbid()/Permit() (Permit's
 * decrement can trigger Reschedule() which yields - exactly what we don't
 * want while holding a scheduler list spinlock). Used by the scheduler-list
 * helpers in rom/exec/exec_intern.h. Maps to the per-CPU task-dispatch nest
 * count, mirroring arm-native's EXEC_BLOCK_DISPATCH_INC/DEC.
 */
#define EXEC_BLOCK_DISPATCH_INC         TDNESTCOUNT_INC
#define EXEC_BLOCK_DISPATCH_DEC         TDNESTCOUNT_DEC
#define SCHEDQUANTUM_SET(val) \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->Quantum = val; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define SCHEDQUANTUM_GET \
    ({ \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        UWORD __ret = 0; \
        if (__schd) \
            __ret = (__schd->Quantum); \
        EXEC_PERCPU_EXIT(__pcf); \
         __ret;  \
   })
#define SCHEDELAPSED_SET(val) \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->Elapsed = val; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define SCHEDELAPSED_GET \
    ({ \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        UWORD __ret = 0; \
        if (__schd) \
            __ret = (__schd->Elapsed); \
        EXEC_PERCPU_EXIT(__pcf); \
         __ret;  \
   })
#define IDNESTCOUNT_GET \
    ({ \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        LONG __ret = 0; \
        if (__schd) \
            __ret = (__schd->IDNestCnt); \
        EXEC_PERCPU_EXIT(__pcf); \
         __ret;  \
   })
#define IDNESTCOUNT_SET(val) \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->IDNestCnt = val; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define TDNESTCOUNT_GET \
    ({ \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        LONG __ret = 0; \
        if (__schd) \
            __ret = (__schd->TDNestCnt); \
        EXEC_PERCPU_EXIT(__pcf); \
        __ret;  \
    })
#define TDNESTCOUNT_SET(val) \
    do { \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
            __schd->TDNestCnt = val; \
        EXEC_PERCPU_EXIT(__pcf); \
    } while(0)
#define FLAG_SCHEDQUANTUM_ISSET \
    ({ \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        BOOL __ret = FALSE; \
        if (__schd) \
            __ret = (__schd->ScheduleFlags & TLSSF_Quantum); \
        EXEC_PERCPU_EXIT(__pcf); \
        __ret;  \
    })
#define FLAG_SCHEDSWITCH_ISSET \
    ({ \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        BOOL __ret = FALSE; \
        if (__schd) \
            __ret = (__schd->ScheduleFlags & TLSSF_Switch); \
        EXEC_PERCPU_EXIT(__pcf); \
        __ret;  \
    })
#define FLAG_SCHEDDISPATCH_ISSET \
    ({ \
        tls_t *__tls = TLS_PTR_GET(); \
        BOOL __ret = FALSE; \
        if (__schd) \
            __ret = (__schd->ScheduleFlags & TLSSF_Dispatch); \
        __ret;  \
    })
#define GET_THIS_TASK \
    ({ \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        struct Task *__ret = NULL; \
        if (__schd) \
            __ret = __schd->RunningTask; \
        EXEC_PERCPU_EXIT(__pcf); \
        __ret;  \
    })
#define SET_THIS_TASK(x)         \
    ({ \
        unsigned long __pcf = EXEC_PERCPU_ENTER(); \
        struct X86SchedulerPrivate  *__schd = TLS_GET(ScheduleData); \
        if (__schd) \
        { \
            EXEC_SPINLOCK_LOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock, NULL, SPINLOCK_MODE_WRITE); \
            __schd->RunningTask = (x); \
            AddHead(&PrivExecBase(SysBase)->TaskRunning, (struct Node *)(x)); \
            EXEC_SPINLOCK_UNLOCK(&PrivExecBase(SysBase)->TaskRunningSpinLock);  \
        } \
        EXEC_PERCPU_EXIT(__pcf); \
    })

#else /* !__AROSEXEC_SMP__ */

struct Exec_PlatformData
{
    /* No platform-specific data on plain x86 builds */
};

#ifdef AROS_NO_ATOMIC_OPERATIONS
#define IDNESTCOUNT_INC                 SysBase->IDNestCnt++
#define IDNESTCOUNT_DEC                 SysBase->IDNestCnt--
#define TDNESTCOUNT_INC                 SysBase->TDNestCnt++
#define TDNESTCOUNT_DEC                 SysBase->TDNestCnt--
#define FLAG_SCHEDQUANTUM_CLEAR         SysBase->SysFlags &= ~SFF_QuantumOver
#define FLAG_SCHEDQUANTUM_SET           SysBase->SysFlags |= SFF_QuantumOver
#define FLAG_SCHEDSWITCH_CLEAR          SysBase->AttnResched &= ~ARF_AttnSwitch
#define FLAG_SCHEDSWITCH_SET            SysBase->AttnResched |= ARF_AttnSwitch
#define FLAG_SCHEDDISPATCH_CLEAR        SysBase->AttnResched &= ~ARF_AttnDispatch
#define FLAG_SCHEDDISPATCH_SET          SysBase->AttnResched |= ARF_AttnDispatch
#else
#define IDNESTCOUNT_INC                 AROS_ATOMIC_INC(SysBase->IDNestCnt)
#define IDNESTCOUNT_DEC                 AROS_ATOMIC_DEC(SysBase->IDNestCnt)
#define TDNESTCOUNT_INC                 AROS_ATOMIC_INC(SysBase->TDNestCnt)
#define TDNESTCOUNT_DEC                 AROS_ATOMIC_DEC(SysBase->TDNestCnt)
#define FLAG_SCHEDQUANTUM_CLEAR         AROS_ATOMIC_AND(SysBase->SysFlags, ~SFF_QuantumOver)
#define FLAG_SCHEDQUANTUM_SET           AROS_ATOMIC_OR(SysBase->SysFlags, SFF_QuantumOver)
#define FLAG_SCHEDSWITCH_CLEAR          AROS_ATOMIC_AND(SysBase->AttnResched, ~ARF_AttnSwitch)
#define FLAG_SCHEDSWITCH_SET            AROS_ATOMIC_OR(SysBase->AttnResched, ARF_AttnSwitch)
#define FLAG_SCHEDDISPATCH_CLEAR        AROS_ATOMIC_AND(SysBase->AttnResched, ~ARF_AttnDispatch)
#define FLAG_SCHEDDISPATCH_SET          AROS_ATOMIC_OR(SysBase->AttnResched, ARF_AttnDispatch)
#endif
#define SCHEDQUANTUM_SET(val)           (SysBase->Quantum=(val))
#define SCHEDQUANTUM_GET                (SysBase->Quantum)
#define SCHEDELAPSED_SET(val)           (SysBase->Elapsed=(val))
#define SCHEDELAPSED_GET                (SysBase->Elapsed)
#define IDNESTCOUNT_GET                 (SysBase->IDNestCnt)
#define IDNESTCOUNT_SET(val)            (SysBase->IDNestCnt=(val))
#define TDNESTCOUNT_GET                 (SysBase->TDNestCnt)
#define TDNESTCOUNT_SET(val)            (SysBase->TDNestCnt=(val))
#define FLAG_SCHEDQUANTUM_ISSET         (SysBase->SysFlags & SFF_QuantumOver)
#define FLAG_SCHEDSWITCH_ISSET          (SysBase->AttnResched & ARF_AttnSwitch)
#define FLAG_SCHEDDISPATCH_ISSET        (SysBase->AttnResched & ARF_AttnDispatch)

#define GET_THIS_TASK                   (SysBase->ThisTask)
#define SET_THIS_TASK(x)                (SysBase->ThisTask=(x))

#endif /* !__AROSEXEC_SMP__ */

struct Task *Exec_X86CreateIdleTask(APTR);

#include "kernel_intr.h"

#include "x86_syscalls.h"

#endif /* __EXEC_PLATFORM_H */
