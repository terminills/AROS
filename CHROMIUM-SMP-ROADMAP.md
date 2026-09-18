# Chromium on multiple cores: where we actually are, and the order to fix it

Written 2026-09-18 for terminills ("we need to document and roadmap chromium
using multiple cores"). Every claim below is cited to `file:line` and was read
out of the tree, not recalled. Where something could not be established by
reading code it is marked **UNVERIFIED** and says what would settle it.

Trees referenced:
- `aros-adt-src` (branch `chromium-clean`) — the OS
- `chromium-aros/src` — the browser port
- `chromium-aros-build` — build ledger and docs

---

## 1. The one-sentence answer

Chromium already creates the right number of threads, and AROS already runs a
real SMP scheduler on all 32 CPUs — but **every thread Chromium creates is
pinned to the CPU that created it**, which is CPU 0, so the other 31 cores run
nothing but their own idle task. Chromium never opts in to the mechanism AROS
provides for spreading work, because that mechanism is explicit placement, not
automatic balancing.

## 1a. AROS SMP is master/child. Read this before proposing anything.

**AROS does not automatically balance tasks across cores, and this is by
design, not an omission.** There is no balancer and no work stealing; a task
runs where it was placed. `rom/exec/signal.c:191` says so in the source:
*"find first cpu suitable to run this task. Use CPU balancing some day..."*

The intended pattern is implemented twice in-tree, identically, and those two
programs are the specification:
`developer/debug/test/smp/smp-test/` and
`developer/debug/test/smp/smp-smallpt/`.

1. **Take a core budget as an argument, clamped to the real count.**
   `ARG_TEMPLATE "MAXCPU/N,MAXITER/N,OVERSAMPLE/N,BUDDHA/S"`
   (`smp-test/smpt_main.c:32`), defaulted to `coreCount` (`:78`) and clamped
   with `if (max_cpus > coreCount) max_cpus = coreCount;` (`:110-111`).
   smallpt has the same `MAXCPU/N` (`smp-smallpt/main.c:85-86`).
2. **One master, floating, slightly lower priority.**
   `NewCreateTask(TASKTAG_NAME, "SMP-Test Master", TASKTAG_AFFINITY,
   TASKAFFINITY_ANY, TASKTAG_PRI, -1, ...)` (`smpt_main.c:287-290`;
   `smp-smallpt/main.c:242-243`).
3. **One worker per core, each PINNED to exactly that core.**
   ```c
   for (core = 0; core < max_cpus; core++) {
       void *coreAffinity = KrnAllocCPUMask();
       KrnGetCPUMask(core, coreAffinity);
       ...
       NewCreateTask(TASKTAG_NAME, name,
                     TASKTAG_AFFINITY, coreAffinity,
                     TASKTAG_PRI, 0, TASKTAG_PC, SMPTestWorker, ...);
   }
   ```
   (`smpt_main.c:143-183`; `smp-smallpt/renderer.c:149-156`)
4. **Work flows master → worker over MsgPorts**, one unit at a time:
   `PutMsg(coreWorker->smpw_MsgPort, workMsg)` (`smp-test/master.c:67`)
   against `WaitPort` / `GetMsg` in the worker (`smp-test/worker.c:194-196`).
   Shared state is held under a Semaphore, not Forbid/Disable
   (`smpt_main.c:141`).

**This reframes `KrnGetCPUMask` setting a single bit
(`arch/all-pc/kernel/apic.c:131`).** It is not a wart to be fixed — it is the
primitive "give me the mask for core N", and the per-core loop above is its
intended use. Likewise the per-task default in `Exec_InitETask` is the
conservative, locality-preserving choice for a system with no balancer.

The in-tree production consumer follows the same shape: NVMe allocates one
queue per CPU and asks for one MSI-X vector per CPU
(`rom/devs/nvme/nvme_controllerclass.c:282`,
`rom/devs/nvme/nvme_busclass.c:275`).

**Therefore: the goal is not to make AROS balance Chromium's threads. It is to
make Chromium place its workers the way smp-test does.**

---

## 2. What is already right (do not "fix" these)

**Chromium sizes its pools from the real core count.** `NumberOfProcessors()`
on AROS calls `KrnGetCPUCount()`, not a hardcoded 1
(`base/system/sys_info_posix.cc:176-181`). The sizing math is unpatched:
`max(3, NumberOfProcessors() - 1)` foreground workers
(`base/task/thread_pool/thread_pool_instance.cc:187-189`). Same
`KrnGetCPUCount` approach in V8's monolith (`v8/src/base/sys-info.cc:50-54`),
WebRTC (`third_party/webrtc/system_wrappers/source/cpu_info.cc:55`), gemmlowp
and perfetto. So on the DUT Chromium asks for ~31 foreground workers and gets
them.

**The threads are real Exec processes.** `PlatformThread` uses pthreads
(`base/threading/platform_thread_aros.cc:7,96`), and `pthread_create` creates a
process via `CreateNewProcTags()`
(`compiler/pthread/pthread_create.c:264-277`).

**Blink's compositor and worker threads are created, not disabled.** The AROS
patches in `third_party/blink/renderer/platform/scheduler/common/thread.cc:76-89`
and `third_party/blink/renderer/core/workers/worker_thread.cc:811-817` are a
re-entrancy relaxation and a shutdown-ordering fix respectively. The `[AROS_BMF]`
blocks in `cc/trees/proxy_main.cc` and `proxy_impl.cc` are `VLOG` probes only.

**AROS SMP is genuinely compiled in for this target.** `__AROSEXEC_SMP__` is
gated on `--enable-target-variant=smp` (`configure.in:1667-1670`), and that is
how the shipping SMP build is configured (`scripts/gimmearos-ext.sh:465-467`).
APs really do enter the scheduler: after `RemTask()` of its bootstrap task
(`arch/all-pc/kernel/smp.c:143`) each AP lands in `cpu_Dispatch`'s
`while (!(task = core_Dispatch())) { sti; hlt; cli; }`
(`arch/x86_64-pc/kernel/kernel_cpu.c:90-97`) and walks the same shared ready
list CPU 0 walks.

---

## 3. The actual blocker, in three lines

Every new task's ETask gets an affinity mask containing exactly one CPU — the
one its creator happened to be running on:

```c
/* rom/exec/exec_util.c:126-136, Exec_InitETask() */
cpunum = KrnGetCPUNumber();
if (PrivExecBase(SysBase)->IntFlags & EXECF_CPUAffinity)
{
    IntETask(et)->iet_CpuAffinity = KrnAllocCPUMask();
    KrnGetCPUMask(cpunum, IntETask(et)->iet_CpuAffinity);
}
```

`KrnGetCPUMask` **assigns one bit, it does not OR** —
`apicMask[idlong] = (1 << idbit);` (`arch/all-pc/kernel/apic.c:131`). So the
mask is precisely one CPU.

The dispatcher honours it. `core_Dispatch()` walks the single global
`SysBase->TaskReady` list and skips any task whose mask excludes this CPU
(`arch/all-pc/kernel/kernel_scheduler.c:308-319`, the filter at `:311`).

The same CPU-0-only default is applied retroactively to every task that already
exists when SMP comes up: `cpu_PrepareExec()` sets the current task's and every
`TaskReady`/`TaskWait` node's affinity with a hardcoded `KrnGetCPUMask(0, ...)`
and only then sets `EXECF_CPUAffinity`
(`arch/all-pc/kernel/smp_exec.c:37-60`).

**Nothing in the Chromium path ever widens the mask.** `pthread_create` passes
no affinity tag (`compiler/pthread/pthread_create.c:264-277`);
`CreateNewProc` finishes with `AddTask()` and a `NULL` taglist
(`rom/dos/createnewproc.c:507-516`, `rom/exec/addtask.c:86`), so
`PrepareContext`'s `TASKTAG_AFFINITY` case
(`arch/x86_64-all/exec/preparecontext.c:46-52`) is never reached. The opt-out
exists — `TASKAFFINITY_ANY` (`compiler/include/exec/tasks.h:229-232`) — and is
used by exactly **two** tasks in the whole OS: the Exec housekeeper
(`rom/exec/exec_init.c:313`) and the ACPI button class
(`rom/hidds/acpi/button/acpibutton_class.c:218`).

There is also **no way to re-pin a running task**: no `KrnSetCPUAffinity`, no
`setcpuaffinity*` anywhere in the tree. Affinity is creation-time only.

**Consequence:** the AP scheduler loop works perfectly and finds nothing it is
allowed to run except its own priority -128 idle task, which was created with
that AP's own mask (`arch/all-pc/exec/platform_init.c:363-396`). The cores are
idle *by consequence of the default*, not because anything parks them.

The scheduler source says as much itself, at `rom/exec/signal.c:191`:

> `/* Task is not running now, find first cpu suitable to run this task. Use CPU balancing some day... */`

---

## 4. The sequencing trap — read this before widening anything

**Today's CPU-0 pinning is accidentally acting as a global lock.** Widening
affinity makes AROS genuinely concurrent for the first time under a real
workload, and it does so for *all* of Chromium's ~31 threads at once. Two
documented hazard classes stop being theoretical the moment that happens:

1. **`Forbid()`/`Disable()` do not serialise across CPUs.** The port states it
   plainly in `base/allocator/partition_allocator/src/partition_alloc/page_allocator_internals_aros.h:219-220`:
   *"Forbid() does not stop the other CPUs on an SMP kernel, so the whole block
   is briefly free to every other task."* That comment is a post-mortem: the
   `Forbid(); FreeMem(); AllocAbs()` idiom produced a corrupted free-list link
   on the native `pc-x86_64-smp` guest (`free_node.next = 0x20`, fault at
   `tlsf_allocabs+0x99`, alert `0x80000003`) and the machine froze with no
   output because the alert path deadlocked against the MemHeader spinlock it
   was already holding (TRIG-1005/1006). **PartitionAlloc no longer uses that
   idiom** — the slack now stays with the block — so this specific instance is
   closed. The *class* is not: any AROS code that reaches for Forbid/Disable as
   mutual exclusion is wrong on SMP, and has been getting away with it.
2. **A boot-time SMP hang already exists.** `chromium-aros-build/docs/KNOWN-SITE-ISSUES.md:82`
   records boots stalling roughly 1 in 3 inside `KrnSpinLock`, explicitly
   tagged *"Kernel/SMP lane, not a Chromium defect — needs an owner outside the
   browser work."*

So the order below deliberately puts **measurement** before **widening**, and
widening in a **narrow, reversible** form before anything global.

---

## 5. Roadmap

### Phase 0 — Measure. Prove the claim on hardware before changing behaviour.

Nothing here changes runtime behaviour.

- `maxcpus=N` boot argument — **DONE**, `aros-adt-src 8cf530ee`. Caps
  `apicData->apic_count` in `arch/all-pc/kernel/acpi.c` right after MADT
  enumeration. Deliberately not the `smp_Wake()` loop bound: `apic_count` is
  what `KrnGetCPUCount()` reports, what `alloccpumask.c` sizes masks by, and
  what `kernel_ipi.c` broadcasts over before spinning for every CPU's
  acknowledgement — capping only the wake loop would hang the first IPI on a
  core still parked in the trampoline.
- `--aros-cpu-limit=N` browser argument — **WRITTEN, NOT YET BUILT**. Caps what
  `SysInfo::NumberOfProcessors()` reports
  (`base/system/sys_info_posix.cc`, switch in `base/base_switches.{h,cc}`).
  That one function is the chokepoint every pool size derives from, so capping
  it is equivalent to capping the thread pool, V8, WebRTC, gemmlowp and
  perfetto at once, and cannot drift out of sync with them. Clamps downward
  only — a limit at or above the real count, or an unparseable one, is ignored,
  so it can never invent cores.

  The two limits answer different questions and are both worth having:
  `maxcpus=N` changes what the **machine** has (and so what
  `KrnGetCPUCount()` returns), while `--aros-cpu-limit=N` changes what the
  **browser thinks** it has on an unchanged machine. Running 32 real cores with
  `--aros-cpu-limit=4` isolates Chromium's own scaling from the kernel's.
- Add a per-CPU dispatch counter and a way to dump it (`core_Dispatch()`,
  `arch/all-pc/kernel/kernel_scheduler.c:308`). This is the instrument that
  turns "all threads are on CPU 0" from an inference into a measurement, and
  it is the same instrument that proves any later phase worked.
- **Run the in-tree SMP tests in QEMU first, before touching Chromium.**
  `developer/debug/test/smp/` builds SMP-Stress, SMP-Trace, SMP-Test,
  SMP-Smallpt, SMP-Preempt, SMP-SigRace, SMP-Sem, SMP-Sched, SMP-DoIO and
  SMP-Lists (`mmakefile.src`, targets `test-smp` / `test-smp-quick`). These are
  Nick's own programs written to this design, they already take `MAXCPU/N`, and
  they exercise the exact primitives Phase 1 depends on — per-core task
  placement, MsgPort handover, semaphores under contention. They are the
  acceptance harness: if SMP-Test does not scale in QEMU at `-smp 4`, nothing
  we do to Chromium will either, and we would be debugging the browser for an
  OS defect. Cheap, reproducible, no hardware needed.
- Exit criterion: (a) SMP-Test and SMP-Smallpt run clean in QEMU at `-smp 1`,
  `2` and `4` with wall-clock falling as cores rise; (b) a dispatch-counter run
  showing ~100% of dispatches on CPU 0 while Chromium loads a page; (c)
  `maxcpus=4` visibly changing `KrnGetCPUCount()` and hence Chromium's worker
  count.

Context: Deadwood asked for SMP to be prioritised, and the iteration target is
Alpha 10, in QEMU. The hardware findings that produced `maxcpus=N` are JOB-0101;
they are not a prerequisite for any of this.

### Phase 1 — Give Chromium's pool workers explicit per-core placement.

**Corrected 2026-09-18.** An earlier draft of this document proposed giving
Chromium's threads `TASKAFFINITY_ANY` and letting the scheduler sort it out.
That is wrong and it is recorded here so nobody re-proposes it: AROS has no
balancer (§1a), so "floating" threads do not get distributed — they get handed
to whichever CPU wins the race for the single global `TaskReady` spinlock, with
no locality, maximum contention on that one lock, and no reproducibility
between runs. `TASKAFFINITY_ANY` is for the *master*, which is one mostly-idle
task, not for N workers.

The design-following change is to make Chromium's thread pool do what
`smp-test` does — pin worker *i* to core *i*:

- Chromium already creates the right number of workers, sized from
  `NumberOfProcessors()`
  (`base/task/thread_pool/thread_pool_instance.cc:187-189`). So the count is
  right and only the *placement* is missing. This is a smaller change than the
  rejected one, not a larger one.
- The seam is `pthread_create`, which currently passes no affinity tag
  (`compiler/pthread/pthread_create.c:264-277`). Add the affinity to
  `pthread_attr_t` behind the glibc-compatible spelling
  (`pthread_attr_setaffinity_np`), so the AROS-specific part stays inside the
  pthread implementation and Chromium's side is ordinary POSIX.
- That still needs the DOS gap closed: `CreateNewProc` has no `NP_` tag for
  affinity and `AddTask` passes a `NULL` taglist
  (`rom/dos/createnewproc.c:507-516`, `rom/exec/addtask.c:86`), so
  `PrepareContext`'s `TASKTAG_AFFINITY` case
  (`arch/x86_64-all/exec/preparecontext.c:46-52`) is unreachable from there.
  Either add an `NP_Affinity` passthrough, or have pthread_create use
  `NewCreateTask` directly as the SMP tests do.
- Assign cores round-robin over the worker index, skipping core 0 where
  possible: CPU 0 is where Intuition, input and the handlers live, and
  `base/threading/platform_thread_aros.cc:37-44` records compositing being
  deliberately left at nice 0 to avoid starving `graphics.library`/Intuition.
- Leave the browser main thread, the IO pump and Blink's main thread alone.
  They are latency-sensitive, not throughput-bound, and the IO pump already
  carries a priority workaround (`kIoPumpMinimumPriority = 1`,
  `platform_thread_aros.cc:59,71-75`).
- **Expect this phase to surface bugs rather than speed.** Real concurrency
  will find the Forbid/Disable assumptions in §4. Which is why it goes in
  behind two independent switches: `maxcpus=1` (kernel) and
  `--aros-cpu-limit=1` (browser) are both instant A/Bs.

### Phase 2 — Make the exposed paths actually SMP-safe.

Driven by what Phase 1 breaks. Known candidates, in likelihood order:

- Audit every `Forbid()`/`Disable()` used as mutual exclusion on data another
  CPU can touch. Memory note `aros-smp-disable-forbid-per-cpu` and the
  PartitionAlloc post-mortem above are the map.
- The `KrnSpinLock` boot stall (`KNOWN-SITE-ISSUES.md:82`) — it is already a
  1-in-3 event at 32 CPUs with *nothing* running concurrently. It will get
  worse, and `maxcpus=N` is now the tool to bisect it.
- `PrivExecBase->CPUMask` is clobbered rather than accumulated: each AP calls
  `KrnGetCPUMask(cpuNo, PrivExecBase(SysBase)->CPUMask)`
  (`arch/all-pc/kernel/smp_exec.c:221`) and `core_APIC_GetMask` **assigns**
  (`apic.c:131`), so the field ends up holding only the last AP to boot rather
  than the set of online CPUs. **UNVERIFIED:** no consumer of that field was
  found in the scheduling path, so the downstream effect is unknown — but the
  value is wrong either way and anything added later that reads it will be
  wrong too.

### Phase 3 — Stop lying about the core count, and re-enable Node/V8 parallelism.

These are independent of Phases 1-2 and can proceed in parallel, but they are
pointless before Phase 1 — extra threads with a CPU-0 mask only add contention.

- `uv_cpu_info()` hardcodes `*count = 1`
  (`workbench/libs/uv/src-aros/aros-stubs.c:334-351`). This is what feeds
  Node's `os.cpus()` and `availableParallelism()`, so every JS-side
  parallelism decision — including VS Code's — currently sees a uniprocessor.
- `AROSPlatform::GetNumberOfProcessors()` returns a hardcoded 1 with a
  `// TODO: Check AROS SMP capabilities`
  (`workbench/libs/v8/engine/platform-aros.cc:255-260`). It is compiled in
  unconditionally (`workbench/libs/v8/mmakefile.src:125-131`) while the V8
  monolith carries its own `KrnGetCPUCount` version.
  **UNVERIFIED — settle this first:** which implementation actually services
  V8's queries at runtime. A one-line probe print in each, on one boot,
  answers it. Do not "fix" the shim before knowing whether it is live.
- `NodePlatform` on AROS is foreground-only: the constructor returns *before*
  constructing `WorkerThreadsTaskRunner`, so there are **zero** Node worker
  threads (`workbench/libs/node/node-v22.23.1-aros.diff:3620-3628`), and V8 is
  started with `--single-threaded` (`:2646-2648`). The port's stated reason
  (`:2633-2645`) is that every background job V8 posts — wasm tier-up,
  concurrent recompilation, concurrent marking/sweeping — otherwise executes
  synchronously at the post point. Re-enabling this is the single largest
  available win for JS-heavy workloads (VS Code, Electron), and it is
  meaningless until threads can leave CPU 0.

### Phase 4 — Multi-process. Later, and not required for multicore.

Chromium is forced single-process on AROS:
`ApplyArosProcessModelDefaults()` appends `--single-process`, `--no-sandbox`
and (absent `--use-gl`) `--disable-gpu`, called unconditionally from
`content/app/content_main.cc:316-318`
(definition `:143-156`). It is a default, not a lock — it only appends when the
switch is absent, and children of an explicit `--type=` launch are left alone.

A child launcher **does** exist and is AmigaDOS-native, not fork/exec:
`LoadSeg()` + `CreateNewProc()` with IPC endpoints passed by command-line
rendezvous (`base/process/launch_aros.cc:204,233-253`;
`content/browser/child_process_launcher_helper_aros.cc:34-38,74`).

The structural blocker is that AROS processes share one address space
(`content/app/content_main.cc:100-103`), so per-process singletons have to be
re-keyed by process identity — which the port already does with
`CommandLine*`-keyed registries for the thread pool
(`base/task/thread_pool/thread_pool_instance.cc:33-60`) and content
initialisation (`content/app/content_main.cc:104-135`). The practical blocker
is memory: each child reserves its own PartitionAlloc pools and the attempt
dies with ENOMEM (`tests/aros-hosted-run.sh:77-81`), against a recorded 6 GB
minimum for even single-process 4-core runs
(`chromium-aros-build/BUILD-LEDGER.md:23`).

**UNVERIFIED:** whether the `CreateNewProc` child path has ever succeeded. The
code is complete; no passing evidence set was found.

**Phase 4 is not on the critical path for using multiple cores.** Threads in
one process are enough, and are what Phases 1-3 deliver.

---

## 6. What would change the plan

- If Phase 0 measurement shows dispatches already spread across CPUs, §3 is
  wrong and this whole document needs rewriting. That is precisely why Phase 0
  exists and comes first.
- If Phase 1 immediately reproduces the `KrnSpinLock` boot stall at a much
  higher rate, Phase 2 moves ahead of Phase 1 and the widening gets reverted
  until the lock is fixed.
- The scheduler has **one global ready list under one spinlock**
  (`compiler/include/exec/execbase.inc:75`,
  `rom/exec/exec_intern.h:121`). Once 32 CPUs are genuinely pulling from it,
  that lock becomes the next bottleneck, and per-CPU run queues become a real
  piece of work rather than a micro-optimisation. Do not start there — measure
  first — but expect it.

## 7. Related records

Memory: `aros-smp-disable-forbid-per-cpu`, `aros-smp-32core-stall`,
`aros-smp-idle-gp-under-load`, `aros-tls-on-workers-gap`,
`adt-is-ahead-of-opal-on-smp`, `aros-optimization-roadmap`.
Relay: JOB-0101 (DUT bring-up, where `maxcpus=N` came from), TRIG-1005/1006
(the PartitionAlloc SMP freeze).
