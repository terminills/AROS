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
 * NOT ENABLED YET - enabling this as-is HANGS THE BOOT. Measured, not feared:
 * a fresh ISO with the define on stops dead just after the last AP calibrates
 * and never reaches Wanderer; the identical tree with only this line commented
 * out boots and runs SMP-SigRace normally (3 PASS, 1 FAIL at -smp 4). One
 * variable, both directions.
 *
 * The cause is a lock-order inversion that the four functions below cannot fix
 * on their own, because it lives in the existing drain:
 *
 *   sender   rom/exec/signal.c holds target->tc_SpinLock across the commit,
 *            and core_CommitCallIPI() then takes kb_BusyIPIHooksLock.
 *   receiver core_IPICallHookHandle() holds kb_BusyIPIHooksLock ACROSS the
 *            CALLHOOKPKT(), and on pc signal_hook runs the full Signal()
 *            (the tc_SpinLock-free restricted path is gated on
 *            __AROSEXEC_IPI_RESTRICTED_CTX__, which only arm-native defines),
 *            so the hook then wants target->tc_SpinLock.
 *
 * A holds tc_SpinLock wanting the queue lock, B holds the queue lock wanting
 * tc_SpinLock. ABBA. arm-native does not hit it because its drain releases the
 * queue lock BEFORE running the hook.
 *
 * Note the irony worth remembering: holding the queue lock across the hook is
 * exactly what lets core_CancelCallIPIs() below skip arm-native's "wait for the
 * executing call" spin - acquiring the lock already waits a mid-hook core out.
 * The same property that makes cancellation cheap makes commit deadlock.
 *
 * TO FINISH THIS: rework core_IPICallHookHandle() to dequeue under the lock,
 * release it, run the hook, then re-acquire for the CPUDone/free bookkeeping -
 * i.e. adopt arm-native's shape - and then core_CancelCallIPIs() must grow
 * per-CPU executing-call tracking (see arch/arm-native/kernel/kernel_ipi.c:186
 * and its :288 note about a call that is invisible to the queue scan). That
 * touches core_DoCallIPI()'s broadcast semantics, where one entry targets many
 * CPUs and is freed when the last one finishes, so it is not a small change and
 * it is not safe to do blind.
 *
 * Until then exec falls back to plain core_DoCallIPI and Exec_CancelSignalIPIs()
 * stays a no-op, which is the behaviour we have always shipped on x86_64 - the
 * stale-Signal-IPI use-after-free (JOB-0103) remains OPEN.
 */
/* #define KERNEL_IPI_CALL_CANCELABLE */

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
    int             ih_Async;
    spinlock_t      ih_Lock;
    spinlock_t      ih_SyncLock;
};

#endif /* __KERNEL_IPI_H_ */
