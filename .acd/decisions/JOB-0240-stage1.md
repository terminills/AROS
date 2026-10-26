# JOB-0240: isolate and test single-instruction lowering first

Status: hypothesis

## Rationale

Claude owns expanding compatibility work in JOB-0238. A minimal MOVEQ native
payload exercises allocation, code emission, condition-code lowering and a
per-instance code cache without changing memory-writing or exception instructions.
The independently pinned Moira core remains the differential oracle. Production
integration is gated on the committed baseline, corpus ownership and published SDK.

## Boundaries

The host adapter has nonthrowing ordinary RAM and is a test adapter only. It cannot
establish AROS memory-fault, device-access, task or OS compatibility. Synthetic
throughput cannot establish application speedup. Compilation is not runtime proof.
The first prototype has a bounded 2048-entry cache (one host page per entry) and
requires a task-local owner. All unsupported operations use the interpreter.

## Validation

Exhaustive MOVEQ/CCR differential comparison, unsupported opcode coverage, slow
path fallback, full bounded host step state, per-instance ownership and prefetch
mutation checks are run in the private build. Results and hashes belong to the
JOB-0240 evidence manifest. The full AROS corpus and target integration remain open.

## Native capability probe

Import committed compatibility changes with upstream provenance, never the
owner's remaining dirty file. Keep the first native allocator test in standalone
C so the missing published C++ header cannot be hidden by a private overlay.
Use KrnAllocPages/KrnFreePages for dedicated pages. Do not equate a void
KrnSetProtection call with verified protection or enable the production JIT on
that assumption. The prepared probe is compile/link evidence until its serial
completion is captured in an owned runtime slot.

## Iteration 2 rationale

User explicitly requested continued independent iteration and rebasing onto
Claude's current committed work. The native page allocator returned NULL, but
the platform's executable heap passed direct runtime tests. Qualify that backend
without pretending it enforces W^X or changing permissions of shared heap pages.
The guarded step is shared with the host differential harness and restricted to
040/LC040, ordinary nonthrowing prefetch, no state flags and no instruction hooks.
Keep the complete outer integration loop intact. MOVBE is CPUID-gated; its tests
compare against memcpy/byte-swap and do not assume the instruction is free.

The published C++/DOS header gaps are explicit prerequisites. Source-header builds
may qualify the copied integration image but cannot satisfy pristine SDK delivery.
Use identical binaries for paired interpreter/JIT runs, with runtime opt-in only.
