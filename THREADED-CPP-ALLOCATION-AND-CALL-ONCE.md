# Multithreaded C++ on AROS: `std::call_once` and `malloc`

Two problems in AROS's C and C++ runtime stop a multithreaded C++ program
from using more than one core, and one of them can deadlock it outright.
Both were found while compiling GTA V's PPU code with RPCS3's LLVM
recompiler on 8 cores, but neither is specific to RPCS3. Anything that
runs LLVM, or allocates heavily from several threads, is exposed:
llvmpipe (through gallivm) and Chromium are the obvious candidates.

| | `std::call_once` | `malloc` / `free` / `realloc` |
|---|---|---|
| Where | libstdc++ built without TLS; `pthread_once` in `compiler/pthread/pthread.c` | `compiler/crt/stdc/malloc.c`: one `stdc_pool_sem` around the pool |
| Effect | Deadlock when `call_once` nests across threads; global serialisation otherwise | Every allocating thread queues on one semaphore |
| Symptom | All workers asleep, one thread spinning, no progress | All cores mostly idle while the program is "busy" |
| Fixed for | LLVM (websub `ab1df518`) | RPCS3 (aros-emulators `cbf7a31`) |
| Still exposed | Any other C++ code using `std::call_once` across threads | Any program that does not route `malloc` elsewhere |

Measured on the GTA V compile (8 vCPUs, QEMU/KVM):

| Build | Compile jobs done | vCPU load (host) |
|---|---|---|
| Before either fix | 38 at 300 s, 64 at 600 s, then deadlock in one run | about 20% each |
| `call_once` fix only | 38 at 300 s, 64 at 600 s | about 20% each |
| Both fixes | 103 at 90 s, 167 at about 300 s | 99% each |

The `call_once` fix removes the deadlock but does not change the speed.
The speed comes from the `malloc` fix.

---

## 1. `std::call_once` deadlocks

### Mechanism

libstdc++ implements `std::call_once` on top of `__gthread_once`, which is
`pthread_once`. To pass the callable to `pthread_once`'s argument-less init
routine it needs per-thread storage. When libstdc++ is built without TLS,
as AROS's is, it falls back to a **single global mutex**
(`__get_once_mutex()`) plus a global functor pointer, and holds that mutex
**across the `pthread_once` call** until the init routine has taken the
functor. In a backtrace the fallback shows up as
`std::__set_once_functor_lock_ptr` and `__once_proxy`.

AROS's `pthread_once` (`compiler/pthread/pthread.c`) takes a spinlock on
the `pthread_once_t` and keeps it for the whole time the init routine
runs. A second thread arriving at the same once-control spins until the
first finishes.

Put together, nested `call_once` from two threads deadlocks:

1. Thread A calls `call_once(X)`: takes the global mutex, enters
   `pthread_once(X)`, takes X's spinlock, `__once_proxy` releases the
   global mutex and runs X's initializer.
2. Thread B calls `call_once(X)`: takes the global mutex, enters
   `pthread_once(X)` and spins on X's spinlock, **still holding the global
   mutex**.
3. X's initializer, on thread A, calls `call_once(Y)` and blocks on the
   global mutex, which B holds while waiting for A.

LLVM does exactly this. Every pass constructor calls
`initialize<Pass>Pass()` through `llvm::call_once`, and pass
initializers initialize the passes they depend on, so the calls nest. Any
program that builds codegen pipelines on several threads at once
(`TargetPassConfig`, `addPassesToEmitMC` per JIT worker) will hit it
sooner or later. It is timing dependent: several GTA V runs got through
before one deadlocked with all 8 compile workers in `call_once`.

Even without the deadlock, every `call_once` (also an already completed
one) takes the global mutex, so all threads serialise on it.

### What was fixed

LLVM already refuses libstdc++'s `call_once` on NetBSD, OpenBSD and
PowerPC for this reason and uses its own lock-free implementation there.
`include/llvm/Support/Threading.h` now adds AROS to that list
(`tools/crosstools/llvm/llvm-20.1.0.src-aros.diff`, websub `ab1df518`):

```c
#elif defined(LLVM_ON_UNIX) &&                                                 \
    (defined(_LIBCPP_VERSION) ||                                               \
     !(defined(__NetBSD__) || defined(__OpenBSD__) || defined(__powerpc__) ||  \
       defined(__AROS__)))
#define LLVM_THREADING_USE_STD_CALL_ONCE 1
```

With `LLVM_THREADING_USE_STD_CALL_ONCE` at 0, `llvm::call_once` uses an
atomic state word. Waiters spin on that word, and different flags never
share a lock, so nesting is safe.

Two LLVM internals still call `std::call_once` directly
(`CodeGenData::getInstance`, `TimerGlobals::initDeferred`). Both are
one-shot and do not nest.

### Picking it up in another program

`llvm::once_flag` changes type with this setting, so **every object that
includes LLVM headers must be rebuilt**, not just the LLVM libraries.
Mixing old and new objects is an ODR violation.

1. Rebuild and reinstall LLVM. The `workbench-libs-llvm` mmake target
   keeps a stamp file and does not notice a changed header, so build in
   the CMake tree directly:

   ```sh
   cd bin/pc-x86_64-smp/gen/workbench/libs/llvm/target
   make -j32 && make install        # installs into AROS/Developer
   ```

   An already extracted `Ports/llvm/llvm-20.1.0.src` is not re-patched
   from the diff. Apply the change there as well, or extract it again.
2. Rebuild the program from clean, or at least every source that
   includes `llvm/`.
3. Check the result. No pass-registry `call_once` should be left:

   ```sh
   nm -C <binary or lib> | grep 'std::call_once<.*PassRegistry'   # expect nothing
   ```

**llvmpipe:** `libgalliumvm` links the SDK's static LLVM, so Mesa must be
rebuilt after the LLVM rebuild. llvmpipe compiles shaders on the calling
thread. A program with several GL contexts on several threads (Chromium's
GPU process, for example) can therefore reach LLVM's pass setup
concurrently.

**Chromium:** the AROS builds set `use_custom_libcxx = false`, so they use
AROS's libstdc++ and its TLS-less `std::call_once`. Chromium's own code
mostly uses its own once primitives (V8's `base::CallOnce`, Skia's
`SkOnce`, function-local statics), but any third-party code in the tree
that calls `std::call_once` from several threads has the global-mutex
behaviour described above. If a Chromium hang shows threads in
`std::call_once` / `__once_proxy` / `pthread_once`, this is the cause.

### Still open

- **libstdc++ with TLS.** The systemic fix is building libstdc++ in the
  crosstools with TLS enabled (AROS has emulated TLS), so `std::call_once`
  uses its per-thread path and no global mutex. That fixes every program
  at once.
- **`pthread_once`** should not hold a spinlock while the init routine
  runs: it needs a done fast path and should let waiters sleep. This would
  not fix the libstdc++ deadlock by itself, because thread B still holds
  the global mutex while it waits, but it stops waiters burning a CPU.

---

## 2. `malloc` serialises every thread

### Mechanism

`compiler/crt/stdc/malloc.c` serves `malloc`, `realloc` and `free` from
one memory pool, guarded by one `SignalSemaphore` (`stdc_pool_sem`). Every
allocation and every free from every thread obtains it. With several
threads allocating, they spend their time queued on that semaphore,
asleep in `Wait()` on `SIGF_SINGLE` (0x10).

C++ code is affected even if `operator new` is replaced: LLVM grows
`SmallVector` (`grow_pod`), `IntEqClasses`, `LivePhysRegs` and others with
`malloc` / `realloc` directly, and so do most C libraries.

### What was fixed (RPCS3)

RPCS3 on AROS serves both `operator new`/`delete` and the C allocation
functions from rpmalloc, a thread-caching allocator
(`rpcs3/engine/aros-platform/src/aros_rpmalloc.cpp` and
`aros_rpmalloc_malloc.cpp`, options `RPCS3_AROS_RPMALLOC` and
`RPCS3_AROS_RPMALLOC_MALLOC`):

- The executable is linked with `-Wl,--wrap=` for `malloc`, `calloc`,
  `realloc`, `free`, `aligned_alloc`, `memalign` and `posix_memalign`.
  `--wrap` rewrites every reference in the program's own objects and in
  static archives (the LLVM `.a` files). AROS shared libraries keep
  calling their own stdc `malloc`.
- **The two heaps must never mix.** AROS libraries hand back `malloc`ed
  memory for the caller to `free` (and the reverse happens too). rpmalloc
  therefore records every span it maps in an ownership map with one bit
  per 64 KiB granule, and `free`/`realloc` send any pointer it does not own
  to stdc (`__real_free`, `__real_realloc`).
- rpmalloc's spans come from `AllocMem()` and are rounded up to whole
  64 KiB spans. A granule never holds both rpmalloc memory and someone
  else's `AllocMem()` block, so the ownership test cannot misfire.
- **No TLS on the allocation path.** AROS `__thread` is emulated, and the
  emulation calls `malloc`. An earlier attempt that used TLS for rpmalloc's
  per-thread heap deadlocked before `main()`. The heap is looked up in a
  table keyed by `FindTask(NULL)` instead.
- rpmalloc 1.4.x is used, not 2.x: 2.x reserves 256 MiB spans, and AROS has
  no overcommit to back that.

Check that the wrap took effect. Nearly all references should be to the
`__wrap_` names; the few direct ones are the `__real_` forwards:

```sh
readelf -rW <binary> | awk '{print $5}' | grep -E '^(__wrap_)?(malloc|free|realloc|calloc)$' | sort | uniq -c
```

### Applying it to llvmpipe and Chromium

- **llvmpipe:** llvmpipe's rasterizer threads (`LP_NUM_THREADS`) and the
  gallivm compile path allocate through stdc `malloc`. Mesa is a shared
  library on AROS, so `--wrap` at the program's link does not reach it. It
  would need the same wrapping at Mesa's own link, with the same ownership
  map so frees of memory from other libraries still reach stdc. Measure
  first: run a multithreaded GL load and look for threads waiting on
  `SIGF_SINGLE` under `stdc.library malloc`.
- **Chromium:** check which allocator serves `malloc` on AROS before
  changing anything. Where PartitionAlloc serves `malloc` through the
  allocator shim, stdc's semaphore is already out of the hot path. Where
  plain `malloc` reaches stdc (third-party code, any code not built with
  the shim), the same queueing applies. The same `--wrap` plus ownership
  map approach works for any statically linked part.

### Still open

`stdc.library` itself should not serialise all threads on one semaphore:
per-task or per-CPU pools, or a thread-caching front end, would fix every
program. Until then, programs that allocate heavily from several threads
have to bring their own allocator, as above.

---

## Diagnosing either problem

Both look like "the program is busy but slow" or "the program is stuck".
Under QEMU/KVM:

1. **Measure the cores from the host**, not the guest:
   `top -H -p <qemu pid>` shows one line per vCPU.
   - Mostly-idle vCPUs during a CPU-bound job point at a lock.
   - Two counters are wrong on AROS today. RPCS3's PERF line always
     reports 12.5% total with every core at 0%. SysMon's Processors tab
     shows a flat 99.9%.
   - SysMon's per-task CPU time is charged to the last task a CPU ran,
     even while that CPU is idle, so an idle CPU's last task looks like it
     is spinning.
2. **Pause the guest before reading kernel lists**
   (`stop` / `cont` in the QEMU monitor). Walking `TaskReady`, `TaskWait`
   and `TaskRunning` link by link on a running guest mixes states and
   produces lists that look corrupt but are not.
3. **Read what each thread waits on.** For every task: `tc_SigWait` (struct
   Task +40) and the saved stack pointer `tc_SPReg` (+96). Scan the stack
   for return addresses and resolve them against the loaded segments
   (debug.library module list) and the program's symbols.
   - `tc_SigWait` 0x10 (`SIGF_SINGLE`) with `stdc.library malloc` or
     `free` near the top of the stack: the `malloc` problem.
   - Threads in `pthread_mutex_lock` from `std::call_once`, one thread in
     `pthread_once` / `__once_proxy` that never finishes: the `call_once`
     problem.
4. `SysBase` can be found by searching guest memory for a pointer to the
   string `"exec.library"` at a node's `ln_Name` (offset 24; the node type
   is 9, `NT_LIBRARY`).
