/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

#ifndef __KERNEL_IPI_H_
#define __KERNEL_IPI_H_

#include <aros/types/spinlock_s.h>
#include <utility/hooks.h>
#include "kernel_base.h"
#include "apic_ia32.h"

/*
    Private KERNEL IPI messages
*/
#define IPI_NOP         (APIC_EXCEPT_IPI_NOP - X86_CPU_EXCEPT_COUNT)
#define IPI_STOP        (APIC_EXCEPT_IPI_STOP - X86_CPU_EXCEPT_COUNT)
#define IPI_RESUME      (APIC_EXCEPT_IPI_RESUME - X86_CPU_EXCEPT_COUNT)
#define IPI_RESCHEDULE  (APIC_EXCEPT_IPI_RESCHEDULE - X86_CPU_EXCEPT_COUNT)
#define IPI_CALL_HOOK   (APIC_EXCEPT_IPI_CALL_HOOK - X86_CPU_EXCEPT_COUNT)
#define IPI_CAUSE       (APIC_EXCEPT_IPI_CAUSE - X86_CPU_EXCEPT_COUNT)

int core_IPIHandle(struct ExceptionContext *regs, void *data1, struct KernelBase *KernelBase);
void core_DoIPI(uint8_t ipi_number, void *cpu_mask, struct KernelBase *KernelBase);
int core_DoCallIPI(struct Hook *hook, void *cpu_mask, int async, int nargs, IPTR *args, APTR _KB);

/*
 * Split claim/commit API plus cancellation, so exec can validate the target
 * task's lifetime atomically with the enqueue (rom/exec/signal.c) and flush
 * stale calls at task teardown (rom/exec/remtask.c).
 *
 * KERNEL_IPI_CALL_CANCELABLE is the per-architecture opt-in: exec's
 * lifetime-safe path is keyed off this define and platforms that do not
 * provide these four entry points fall back to plain core_DoCallIPI. Nothing
 * outside arch/ needs to change to adopt it, and defining it here cannot
 * affect any other architecture.
 *
 * Without it, Exec_CancelSignalIPIs() compiles to an empty function: RemTask()
 * then frees a task while a Signal-IPI naming that task may still be queued
 * for another core, which drains it against freed memory. That is the failure
 * developer/debug/test/smp/smpsigrace.c reports as "CPU wedged draining a
 * stale Signal-IPI after the task was freed".
 *
 * struct CallIPIEntry is deliberately opaque to exec; on pc it is a
 * struct IPIHook taken from the platform's free list.
 */
struct CallIPIEntry;

extern struct CallIPIEntry *core_ClaimCallIPI(int cpu);
extern void core_CommitCallIPI(struct CallIPIEntry *cie, int cpu,
                               struct Hook *hook, int nargs, IPTR *args);
extern void core_AbortCallIPI(struct CallIPIEntry *cie, int cpu);
extern void core_CancelCallIPIs(APTR hookEntry, IPTR matchArg);

/*
 * Cancelable Signal-IPIs: RemTask() sweeps queued Signal() calls for a task
 * before its memory is freed, so no core drains a signal_hook that names a
 * freed task (SMP-SigRace phase 1; a stale entry used to wedge the core that
 * drained it, spinning on the freed task's tc_SpinLock).
 *
 * This once hung the boot on pc, as a lock-order inversion that lived in the
 * drain: Signal() holds target->tc_SpinLock across core_CommitCallIPI(),
 * which takes kb_BusyIPIHooksLock, while core_IPICallHookHandle() held
 * kb_BusyIPIHooksLock across the hook, and signal_hook's full Signal() wants
 * target->tc_SpinLock - ABBA. The drain now runs hooks with no list lock held
 * (claim, call, mark done), as arm-native's does, and core_CancelCallIPIs()
 * waits out a call that is executing (claimed, not done) instead of relying
 * on the lock as a barrier.
 */
#define KERNEL_IPI_CALL_CANCELABLE

#define IPI_CALL_HOOK_MAX_ARGS  5

/*
    IPI Call hook
*/
struct IPIHook
{
    struct Hook     ih_Hook;
    IPTR            ih_Args[IPI_CALL_HOOK_MAX_ARGS];
    uint32_t *      ih_CPUDone;
    uint32_t *      ih_CPURequested;
    uint32_t *      ih_CPUClaimed;      /* CPUs that have started the call (see core_IPICallHookHandle) */
    int             ih_Async;
    spinlock_t      ih_Lock;
    spinlock_t      ih_SyncLock;
};

#endif /* __KERNEL_IPI_H_ */
