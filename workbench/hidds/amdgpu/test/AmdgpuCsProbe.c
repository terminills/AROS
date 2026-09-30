/* Native resident-only managed graphics submission and lifetime acceptance.
 * No private driver or hardware initialization. Every binary change requires
 * installed-byte comparison and a physical reboot before running this client.
 */
#define DEBUG 1
#define __INLINE_AMDGPU_STACKCALL__ 1
#include <aros/debug.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/amdgpu.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct Library *AmdgpuBase;
#define LOG(...) do { printf("[AmdgpuCsProbe] " __VA_ARGS__); printf("\n"); \
                       bug("[AmdgpuCsProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define NEED(c, s) do { if (!(c)) { LOG("FAIL %s", s); goto out; } \
                         LOG("PASS %s", s); } while (0)

/* Keep two owned bindings live while changing their contents ONLY between
 * confirmed retirements.256 writes cross the1024-dword ring at least three
 * times even from the bootstrap pointer330. The original eight lifetime
 * rounds below remain independent. No VA recycling is assumed here. */
static BOOL ring_reuse_probe(struct AmdgpuDevice *ad)
{
    struct AmdgpuCsRequest req = {0};
    struct AmdgpuVaInfo vi = {0};
    ULONG bo[2] = {0}, va[2] = {0}, ctx = 0, complete, i, round, poll;
    UQUAD address[2] = {0}, sequence = 0, rejected = 0;
    APTR cpu[2] = {0};
    BOOL clean = TRUE, passed = FALSE;
    LONG status;
    volatile ULONG *ib, *target;

    NEED(amdgpu_cs_ctx_create(ad, 0, &ctx) == 0, "ring reuse context created");
    for (i = 0; i < 2; i++) {
        NEED(amdgpu_bo_alloc(ad, 8192, 4096, AMDGPU_BO_DOMAIN_GTT, 0, &bo[i]) == 0 &&
             amdgpu_bo_cpu_map(ad, bo[i], &cpu[i]) == 0 &&
             amdgpu_va_range_alloc(ad, 8192, 4096, 0, &va[i], &address[i]) == 0 &&
             amdgpu_bo_va_bind(ad, bo[i], va[i]) == 0,
             "ring reuse stable owned backing and binding");
    }
    ib = cpu[0]; target = (volatile ULONG *)((UBYTE *)cpu[1] + 4104);
    req.ib_handle = va[0]; req.ib_dwords = 5; req.resource_count = 2;
    req.resources[0] = va[0]; req.resources[1] = va[1];
    for (round = 1; round <= 256; round++) {
        ULONG want = 0x90610000UL | round;
        target[-1] = 0x31c0ffee; *target = ~want; target[1] = 0x42c0ffee;
        ib[0] = 0xc0033700; ib[1] = 0x00100500;
        ib[2] = (ULONG)(address[1] + 4104);
        ib[3] = (ULONG)((address[1] + 4104) >> 32); ib[4] = want;
        __sync_synchronize();
        status = amdgpu_cs_submit(ad, ctx, &req, &sequence);
        if (status || sequence != round) {
            LOG("FAIL ring reuse publish round=%lu status=%ld sequence=%lu",
                (unsigned long)round, (long)status, (unsigned long)sequence);
            goto out;
        }
        if (round == 1 || round == 128 || round == 256) {
            rejected = 123;
            NEED(amdgpu_cs_submit(ad, ctx, &req, &rejected) == -EBUSY && !rejected &&
                 amdgpu_cs_ctx_free(ad, ctx) == -EBUSY &&
                 amdgpu_va_query_info(ad, va[1], &vi, sizeof(vi)) == 0 &&
                 vi.gpu_refs == 1 && amdgpu_bo_va_unbind(ad, va[1]) == -EBUSY,
                 "ring reuse cannot overwrite or release unretired submission");
        }
        complete = 0;
        for (poll = 0; poll < 250; poll++) {
            status = amdgpu_cs_query_fence(ad, ctx, sequence, &complete);
            if (status || complete) break;
            Delay(1);
        }
        if (status || !complete || *target != want || target[-1] != 0x31c0ffee ||
            target[1] != 0x42c0ffee) {
            LOG("FAIL ring reuse round=%lu status=%ld done=%lu payload=%08lx/%08lx guards=%08lx/%08lx",
                (unsigned long)round, (long)status, (unsigned long)complete,
                (unsigned long)*target, (unsigned long)want,
                (unsigned long)target[-1], (unsigned long)target[1]);
            goto out;
        }
        for (i = 0; i < 2; i++) {
            if (amdgpu_va_query_info(ad, va[i], &vi, sizeof(vi)) || vi.gpu_refs ||
                vi.state != AMDGPU_VA_STATE_BOUND) {
                LOG("FAIL ring reuse retirement reference round=%lu binding=%lu",
                    (unsigned long)round, (unsigned long)i);
                goto out;
            }
        }
        if (round == 1 || !(round & 63))
            LOG("ring reuse round=%lu/256 sequence=%lu payload=%08lx guards=PASS real-EOP=PASS polls=%lu",
                (unsigned long)round, (unsigned long)sequence,
                (unsigned long)*target, (unsigned long)poll);
    }
    passed = TRUE;
out:
    for (i = 0; i < 2; i++) {
        if (cpu[i] && amdgpu_bo_cpu_unmap(ad, bo[i])) clean = FALSE;
        if (bo[i] && amdgpu_bo_free(ad, bo[i])) clean = FALSE;
        if (va[i]) {
            if (amdgpu_va_query_info(ad, va[i], &vi, sizeof(vi))) clean = FALSE;
            else if (vi.bo_handle && amdgpu_bo_va_unbind(ad, va[i])) clean = FALSE;
            else if (amdgpu_va_range_free(ad, va[i])) clean = FALSE;
        }
    }
    if (ctx && amdgpu_cs_ctx_free(ad, ctx)) clean = FALSE;
    LOG("ring reuse256 result=%s cleanup=%s; require matching private EOP and ring-boundary logs",
        passed ? "PASS" : "FAIL", clean ? "PASS" : "FAIL-retain-quarantine");
    return passed && clean;
}

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad = NULL;
    struct AmdgpuGpuInfo gpu = {0};
    struct AmdgpuCsCaps caps = {0};
    struct AmdgpuCsRequest req = {0}, bad;
    struct AmdgpuVaInfo vi = {0};
    ULONG ctx = 0, stale = 0, pressure[16] = {0}, bo[2] = {0}, va[2] = {0};
    ULONG i, round, complete, other = 0, poll;
    UQUAD addr[2] = {0}, sequence = 0, rejected = 0;
    APTR cpu[2] = {0};
    BOOL owner[2] = {FALSE, FALSE}, clean = TRUE;
    LONG status, result = RETURN_FAIL;

    Forbid(); resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd"); Permit();
    if (!resident) { LOG("REFUSED no resident driver; no load attempted"); return result; }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    LOG("resident=%p opened=%p negative-bytes=%lu required=%lu",
        resident, AmdgpuBase, (unsigned long)AmdgpuBase->lib_NegSize,
        (unsigned long)(40 * sizeof(APTR)));
    /* Generated stack-call vectors are one-based: the final CS vector is
     * base[-40], so 40 pointer slots cover it. There is no negative slot0. */
    NEED(AmdgpuBase == resident && AmdgpuBase->lib_NegSize >= 40 * sizeof(APTR),
         "resident identity and CS vector extent");
    NEED(amdgpu_get_device_count() == 1 && (ad = amdgpu_get_device(0)), "one installed device");
    amdgpu_query_gpu_info(ad, &gpu);
    NEED(gpu.asic_id == 0x7590 && gpu.pci_rev == 0xc0 && gpu.gc_major == 12,
         "RX9060XT GC12 identity");
    NEED(amdgpu_query_cs_caps(ad, &caps, sizeof(caps)) == 0 && caps.abi_version == 1 &&
         caps.flags == AMDGPU_CS_TRUSTED_SHARED_VM && caps.max_inflight == 1 &&
         caps.max_contexts == 16 && caps.max_resources == 8,
         "bounded managed graphics capability; trusted shared VM only");
    NEED((AmdgpuBase->lib_Version > 0 || AmdgpuBase->lib_Revision >= 5) &&
         caps.max_submits_per_boot >= 264,
         "resident ring reuse revision and capacity; no append-only driver stress");
    LOG("BEGIN v2; eight lifetime rounds then256 retired ring reuse writes");
    ctx = 123;
    NEED(amdgpu_cs_ctx_create(ad, 1, &ctx) == -EINVAL && !ctx, "unsupported context flags rejected");
    NEED(amdgpu_cs_ctx_create(ad, 0, &ctx) == 0, "context created");
    complete = 123;
    NEED(amdgpu_cs_query_fence(ad, ctx, 1, &complete) == -ENOENT && !complete,
         "never-submitted fence cannot appear complete");

    for (round = 1; round <= 8; round++) {
        volatile ULONG *ib, *target;
        ULONG want = 0x90600000UL | round;
        for (i = 0; i < 2; i++) {
            NEED(amdgpu_bo_alloc(ad, 8192, 4096, AMDGPU_BO_DOMAIN_GTT, 0, &bo[i]) == 0,
                 "owned GTT BO allocated"); owner[i] = TRUE;
            NEED(amdgpu_bo_cpu_map(ad, bo[i], &cpu[i]) == 0 &&
                 amdgpu_va_range_alloc(ad, 8192, 4096, 0, &va[i], &addr[i]) == 0 &&
                 amdgpu_bo_va_bind(ad, bo[i], va[i]) == 0, "CPU borrow and public GTT binding");
        }
        ib = cpu[0]; target = (volatile ULONG *)((UBYTE *)cpu[1] + 4104);
        target[-1] = 0x11c0ffee; *target = ~want; target[1] = 0x22c0ffee;
        ib[0] = 0xc0033700; ib[1] = 0x00100500; /* WRITE_DATA memory, WR_CONFIRM */
        ib[2] = (ULONG)(addr[1] + 4104); ib[3] = (ULONG)((addr[1] + 4104) >> 32); ib[4] = want;
        __sync_synchronize();
        memset(&req, 0, sizeof(req));
        req.ib_handle = va[0]; req.ib_dwords = 5; req.resource_count = 2;
        req.resources[0] = va[0]; req.resources[1] = va[1];
        bad = req; bad.ib_offset = 8192; rejected = 123;
        NEED(amdgpu_cs_submit(ad, ctx, &bad, &rejected) == -EINVAL && !rejected &&
             amdgpu_va_query_info(ad, va[0], &vi, sizeof(vi)) == 0 && !vi.gpu_refs,
             "out-of-range IB rejected and acquired refs unwound");
        bad = req; bad.resources[1] = va[0];
        NEED(amdgpu_cs_submit(ad, ctx, &bad, &rejected) == -EINVAL && !rejected,
             "duplicate resources rejected before publication");
        bad = req; bad.ib_handle = 0;
        NEED(amdgpu_cs_submit(ad, ctx, &bad, &rejected) == -EINVAL && !rejected &&
             amdgpu_va_query_info(ad, va[1], &vi, sizeof(vi)) == 0 && !vi.gpu_refs,
             "unlisted IB rejected and all references unwound");
        NEED(amdgpu_cs_submit(ad, ctx, &req, &sequence) == 0 && sequence == round,
             "managed graphics IB published with unique context sequence");
        for (i = 0; i < 2; i++) {
            NEED(amdgpu_bo_free(ad, bo[i]) == 0, "owner release while submitted"); owner[i] = FALSE;
            NEED(amdgpu_va_query_info(ad, va[i], &vi, sizeof(vi)) == 0 && vi.gpu_refs == 1 &&
                 amdgpu_bo_va_unbind(ad, va[i]) == -EBUSY,
                 "driver retains GPU reference and rejects premature unbind");
        }
        NEED(amdgpu_bo_cpu_unmap(ad, bo[0]) == 0, "last IB CPU borrow dropped while GPU retains BO");
        cpu[0] = NULL;
        NEED(amdgpu_cs_ctx_free(ad, ctx) == -EBUSY &&
             amdgpu_cs_submit(ad, ctx, &req, &rejected) == -EBUSY && !rejected,
             "pending context and queue cannot be reclaimed or overwritten");
        NEED(amdgpu_bo_alloc(ad, 8192, 4096, AMDGPU_BO_DOMAIN_GTT, 0, &other) == 0 &&
             (other & 255) != (bo[0] & 255) && (other & 255) != (bo[1] & 255),
             "owner-free in-flight BO slots not recycled");
        NEED(amdgpu_bo_free(ad, other) == 0, "unrelated temporary released"); other = 0;
        complete = 0; status = 0;
        for (poll = 0; poll < 250; poll++) {
            status = amdgpu_cs_query_fence(ad, ctx, sequence, &complete);
            if (status || complete) break;
            Delay(1);
        }
        LOG("round=%lu sequence=%lu query=%ld complete=%lu polls=%lu payload=%08lx/%08lx guards=%08lx/%08lx",
            (unsigned long)round, (unsigned long)sequence, (long)status,
            (unsigned long)complete, (unsigned long)poll, (unsigned long)*target,
            (unsigned long)want, (unsigned long)target[-1], (unsigned long)target[1]);
        NEED(!status && complete && *target == want && target[-1] == 0x11c0ffee &&
             target[1] == 0x22c0ffee, "GPU wrote public second-page VA and private retirement fence");
        NEED(amdgpu_cs_query_fence(ad, ctx, sequence, &complete) == 0 && complete,
             "completed fence remains queryable without double release");
        NEED(amdgpu_bo_cpu_unmap(ad, bo[1]) == 0, "last output CPU borrow released"); cpu[1] = NULL;
        for (i = 0; i < 2; i++) {
            NEED(amdgpu_va_query_info(ad, va[i], &vi, sizeof(vi)) == 0 && !vi.gpu_refs &&
                 amdgpu_bo_va_unbind(ad, va[i]) == 0 && amdgpu_va_range_free(ad, va[i]) == 0,
                 "retirement permits checked unbind and owner-free backing destruction");
            va[i] = bo[i] = 0;
        }
    }
    stale = ctx;
    NEED(amdgpu_cs_ctx_free(ad, ctx) == 0, "retired context released"); ctx = 0;
    for (i = 0; i < 16; i++)
        NEED(amdgpu_cs_ctx_create(ad, 0, &pressure[i]) == 0, "context registry allocation");
    NEED(amdgpu_cs_ctx_create(ad, 0, &ctx) == -ENOMEM && !ctx, "17th context refused");
    NEED(amdgpu_cs_ctx_free(ad, stale) == -ENOENT &&
         amdgpu_cs_query_fence(ad, stale, 1, &complete) == -ENOENT && !complete,
         "stale context generation cannot access replacement");
    for (i = 0; i < 16; i++) {
        NEED(amdgpu_cs_ctx_free(ad, pressure[i]) == 0, "context pressure released before ring reuse");
        pressure[i] = 0;
    }
    NEED(ring_reuse_probe(ad), "256 real GPU writes across retired ring-storage boundaries");
    result = RETURN_OK;
out:
    if (ad) {
        for (i = 0; i < 2; i++) {
            if (cpu[i] && amdgpu_bo_cpu_unmap(ad, bo[i])) clean = FALSE;
            if (owner[i] && amdgpu_bo_free(ad, bo[i])) clean = FALSE;
            if (va[i]) {
                if (amdgpu_bo_va_unbind(ad, va[i]) || amdgpu_va_range_free(ad, va[i])) clean = FALSE;
            }
        }
        if (other && amdgpu_bo_free(ad, other)) clean = FALSE;
        if (ctx && amdgpu_cs_ctx_free(ad, ctx)) clean = FALSE;
        for (i = 0; i < 16; i++) if (pressure[i] && amdgpu_cs_ctx_free(ad, pressure[i])) clean = FALSE;
    }
    if (!clean) result = RETURN_FAIL;
    LOG("END result=%ld cleanup=%s; no full3D/reset/isolation/SMP claim", (long)result,
        clean ? "PASS" : "FAIL-retain-quarantine");
    CloseLibrary(AmdgpuBase);
    return result;
}
