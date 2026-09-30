# BO storage-v1: bounded native lifetime acceptance PASS

Cold41 sourcebecb861f80 was installed, byte-compared501864B and physically
rebooted. Corrected public clientaec01e6df3,74968B SHA256
8c0434da9c00499973a5421b500c7b793e5b61ed54778ee94641f16b8c8abfa7,
compared IDENTICAL74968 with native0 before execution. All exercised storage
gates passed; actual native0/error0/ioerr0 at000005ee8e5010db.

Native WIDTH-SAMPLE confirms ULONG4/C-long8: word2 untruncated1996ff320,
expected32=actual32=996ff320. Complete8192B pattern and neighbour checks pass;
two borrowed mappings survive owner release, final unmap alone retires storage,
reused slot has a fresh generation and zeroed storage, stale handles reject.
All64 mapped allocations retain data after owner release, capacity fails
cleanly until borrowers leave, then allocation/mapping recovers. Cleanup passes.
Frozen UART[644134954,644289764),154810B,SHA256
e215a6e380e5346e8e9dffca67952da91f0caead28200498406ad9d051f30e6f,
cold41-bo-probe-v2-pass.uart.log in JOB-0084-native-completion-20260906.

Unchanged same-boot regressions also passed, each actual native0/error0/ioerr0:
drawing18/0 incl4task DMA at000005669c98184c; two guarded512x256 copies and all
four131072-pixel checks at0000069b01accf1d; seven-block bitmap pressure and
16588800B interior reuse at000007133451a89e; seven-stage software-pointer
images/coordinates/sentinel at000007b624984b58. All three standalone clients
were byte-compared natively before use. Fresh HDMI stages490..492 and500..506
were inspected; logs/hashes are in bo-storage-v1-provenance.md in the same set.
This is bounded VRAM/CPU lifetime acceptance with retained baseline, NOT GPU
execution, general VA/GTT, full2D or SMP BO soak. Strict GHCv40 also completed
1850checks with fail0 and the same6missing library operations, actual native5;
no timeout/fallback messages. Details are in ME_DISPATCH_STATUS.md.

This is the first bounded storage tier of ABI-RFC.md, not a new Mesa port or
a claim that Mesa can render. Six appended public vectors leave existing
Device/Query/compute vector positions unchanged. The native client refuses an
old shorter table before calling a new vector. No private driver object or
auto-opening amdgpu link library is linked into that client.

## Contract

- Driver-owned backing uses the SAME paired VRAM allocator/free as bitmaps.
  No separate physical arena, GART mapping, firmware reservation or scanout
  ownership change. Registering the service at the end of module init only
  initializes its lock and binds the existing primary display pool.
- CPU-map returns a borrowed reference. Free drops the owner reference and
  prevents new maps/queries; storage and the slot remain until the last unmap.
  Each successful map requires an unmap. Callers never free the returned CPU
  pointer. No submitted or displayed use is enabled in this tier.
- Handles use 8 index bits (1..64 valid) and 24 generation bits. A saturated
  generation retires its slot permanently, rather than wrapping. Lookup
  compares device identity before touching a slot; stale generations cannot
  act on replacement allocations. Handles are native single-address-domain
  records, NOT transferable capability/security tokens or fds.
- Size rounds to 4 KiB, bounded before arithmetic; alignment 0 means 4 KiB,
  otherwise a power of two with 64 KiB maximum. CPU and MC alignment are both
  checked. The exact original backing range includes owned alignment padding,
  so freeing an aligned subrange cannot strand a bump-allocator prefix.
- Newly exposed storage is cleared. CPU publication/retirement uses the same
  full barrier primitive as the existing SDMA path.
- Lock order is registry semaphore then pool semaphore. Bitmap/plane code
  does not acquire the registry lock; no OOP calls are made under it.
- 64 slots, 8 MiB maximum individual payload, 16 MiB total backing budget including
  alignment padding. Caps report these bounds; ENOMEM is a normal result when
  the shared pool or registry cannot satisfy a request. This leaves most of the
  existing 64 MiB pool available to the desktop; it is not a full-VRAM allocator.
- Unsupported domains/flags fail. Errors do not return a success handle or
  mapping. Valid output structures are cleared, with explicit size checks.

## Native acceptance cycle

Cold41 loaded sourcebecb861f80 after501864-byte installed verification and
physical reboot. Its new storage marker and native1920x1080 selection were
observed. Unchanged ME32/32 and compute600dc0de boot checks passed.
The byte-verified v1 client then returned20 at000004524ba1d0ff on the first
patterned write/read, after identity/caps/rejection/alignment/zero/map checks.
Failed UART[643342259,643584264),242005bytes, SHA256
47b13aa41af794f99e60b29a65ef88c22b9c94575f377586b5e304569d314c34 is retained
as cold41-bo-probe-v1-fail.uart.log in the registered native-completion set.

The v1 oracle compared a32-bit ULONG load against a64-bit unsigned-long
expression (x86_64 cpu.h defines int32,long64). Stores truncate; the comparison
did not. The v2 client explicitly compares32-bit words and logs the first
width-overflow sample plus any actual data mismatch. This is a client-only
correction, not a driver change. The subsequent native v2 evidence above
confirms this diagnosis and passes the bounded lifetime gates; the original
failed run is retained, not relabelled successful.

Build and freeze driver/client; preserve old installed driver; byte-compare
the installed new driver against a fresh reference; physically reboot. Confirm
the new storage-v1 boot marker, native 1080p and existing wake tests. Only then
compare and execute AmdgpuBoProbe on that resident module.

Probe checks: caps/identity, rejected sizes/alignments/domains, 64 KiB alignment,
zeroed rounded storage, two borrowed maps surviving owner release, no alias
against intervening allocations, exact last-unmap retirement, generation reuse
and stale-handle rejection, 64 live retained mappings, clean capacity failure and
recovery after all borrowers leave. No invalid-address memory probing and no
GPU commands. Follow with unchanged native drawing/copy/allocator acceptance.

## Still required before Mesa execution

General BO/GTT and reusable checked VA allocation/mapping; per-client lifetime
and task-death cleanup; explicit submission/fence/scanout references; serialized
submission with individually retired fences and error propagation; wiring the
existing ac_drm adapter and upstream winsys. No import/export, GPU VA or scanout
API is exposed by storage-v1. Client abandonment may retain the bounded quota;
never force-reclaim a mapping that could remain in use. Query/compile/CPU data
checks are not GPU output or an SMP soak. Hardware cursor remains deferred;
pending UpdateRect coordination is untouched.
