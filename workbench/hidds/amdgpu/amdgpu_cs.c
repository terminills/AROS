/* Managed graphics IB/context/fence ownership, native bounded first tier.
 * Resource lifetime is base-owned, never inferred from client exit or timeout.
 * No scheduler, VM isolation, reset recovery or Mesa capability advertisement.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <proto/exec.h>
#include <hidd/amdgpu.h>
#include <errno.h>
#include <string.h>
#include "amdgpu_intern.h"
#include "amdgpu_gfx_support.h"
#include "amdgpu_bo.h"
#include "amdgpu_cs.h"

#define CS_CONTEXTS 16
#define CS_GENERATION_MAX 0xffffffUL
struct CsContext {
    ULONG generation;
    BOOL used;
    UQUAD submitted, completed;
};
static struct SignalSemaphore cs_lock;
static struct HWData *cs_hw;
static struct CsContext contexts[CS_CONTEXTS];
static ULONG pending_context, pending_count, pending_refs[AMDGPU_CS_EXTENDED_MAX_RESOURCES];
static UQUAD token_counter, pending_token;
static BOOL cs_ready, cs_poisoned;
static BOOL cs_submit_reported, cs_retire_reported;
_Static_assert(sizeof(struct AmdgpuCsRequest) == 64, "v1 request ABI remains64 bytes");
_Static_assert(sizeof(struct AmdgpuCsRequest2) == 288, "v2 request fixed size");

void amdgpu_cs_service_init(struct HWData *hw)
{
    if (cs_ready) return;
    InitSemaphore(&cs_lock);
    cs_hw = hw;
    cs_ready = TRUE;
    bug("[amdgpu-cs] managed-gfx-v2: contexts=%u resources=%u single-inflight max-boot=%lu ready=%lu; retired ring-storage reuse, no pointer rollover/reset/full-Mesa advertisement\n",
        CS_CONTEXTS, AMDGPU_CS_MAX_RESOURCES, (unsigned long)AMDGPU_CS_BOOT_SUBMITS,
        (unsigned long)(hw && gfx_v12_managed_ready(hw->ad)));
}

static BOOL valid(struct AmdgpuDevice *ad)
{
    return cs_ready && cs_hw && ad && ad == cs_hw->ad && gfx_v12_managed_ready(ad);
}
/* Lock order: cs_lock -> bo_lock -> GART/pool. No reverse entry exists. */
static struct CsContext *lookup(ULONG handle)
{
    ULONG i = handle & 255;
    if (!i || i > CS_CONTEXTS) return NULL;
    return contexts[i - 1].used && contexts[i - 1].generation == (handle >> 8)
        ? &contexts[i - 1] : NULL;
}

LONG amdgpu_query_cs_caps(struct AmdgpuDevice *ad, struct AmdgpuCsCaps *out, ULONG size)
{
    if (!out || size != sizeof(*out)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!valid(ad)) return -ENODEV;
    out->abi_version = AMDGPU_CS_ABI_VERSION;
    out->flags = AMDGPU_CS_TRUSTED_SHARED_VM;
    out->max_contexts = CS_CONTEXTS;
    out->max_resources = AMDGPU_CS_MAX_RESOURCES;
    out->max_ib_dwords = 1024;
    out->max_inflight = 1;
    out->max_submits_per_boot = AMDGPU_CS_BOOT_SUBMITS;
    return 0;
}

LONG amdgpu_query_cs_caps2(struct AmdgpuDevice *ad, struct AmdgpuCsCaps *out, ULONG size)
{
    LONG result = amdgpu_query_cs_caps(ad, out, size);
    if (!result) {
        out->abi_version = AMDGPU_CS_EXTENDED_ABI_VERSION;
        out->max_resources = AMDGPU_CS_EXTENDED_MAX_RESOURCES;
        out->max_ib_dwords = AMDGPU_CS_EXTENDED_MAX_IB_DWORDS;
    }
    return result;
}

LONG amdgpu_cs_ctx_create(struct AmdgpuDevice *ad, ULONG flags, ULONG *handle)
{
    ULONG i;
    LONG result = -ENOMEM;
    if (!handle) return -EINVAL;
    *handle = 0;
    if (flags) return -EINVAL;
    if (!valid(ad)) return -ENODEV;
    ObtainSemaphore(&cs_lock);
    for (i = 0; i < CS_CONTEXTS; i++) {
        struct CsContext *c = &contexts[i];
        if (c->used || c->generation == CS_GENERATION_MAX) continue;
        c->generation++; c->used = TRUE; c->submitted = c->completed = 0;
        *handle = (c->generation << 8) | (i + 1);
        result = 0; break;
    }
    ReleaseSemaphore(&cs_lock);
    return result;
}

LONG amdgpu_cs_ctx_free(struct AmdgpuDevice *ad, ULONG handle)
{
    struct CsContext *c;
    LONG result = -ENOENT;
    if (!valid(ad)) return -ENODEV;
    ObtainSemaphore(&cs_lock);
    c = lookup(handle);
    if (c) {
        result = -EBUSY;
        if (handle != pending_context) { c->used = FALSE; result = 0; }
    }
    ReleaseSemaphore(&cs_lock);
    return result;
}

/* Only called after confirmed retirement or before ANY hardware publication.
 * A failed release poisons further submits; affected storage remains pinned. */
static BOOL release_refs(struct AmdgpuDevice *ad)
{
    ULONG i;
    BOOL ok = TRUE;
    for (i = 0; i < pending_count; i++) if (pending_refs[i]) {
        if (amdgpu_va_gpu_release(ad, pending_refs[i])) ok = FALSE;
        else pending_refs[i] = 0;
    }
    if (!ok) cs_poisoned = TRUE;
    else pending_count = 0;
    return ok;
}

static LONG submit_checked(struct AmdgpuDevice *ad, ULONG handle,
                           const struct AmdgpuCsRequest2 *r, UQUAD *sequence)
{
    struct CsContext *c;
    UQUAD ib = 0;
    ULONG i, j;
    LONG result;
    if (r->abi_version != AMDGPU_CS_EXTENDED_ABI_VERSION || r->flags ||
        r->reserved[0] || r->reserved[1] || !r->resource_count ||
        r->resource_count > AMDGPU_CS_EXTENDED_MAX_RESOURCES ||
        !r->ib_dwords || r->ib_dwords > AMDGPU_CS_EXTENDED_MAX_IB_DWORDS ||
        (r->ib_offset & 3)) return -EINVAL;
    for (i = 0; i < r->resource_count; i++) {
        if (!r->resources[i]) return -EINVAL;
        for (j = 0; j < i; j++) if (r->resources[i] == r->resources[j]) return -EINVAL;
    }
    if (!valid(ad)) return -ENODEV;
    ObtainSemaphore(&cs_lock);
    c = lookup(handle);
    result = -ENOENT;
    if (!c) goto out;
    result = -EIO;
    if (cs_poisoned) goto out;
    result = -EBUSY;
    if (pending_context) goto out;
    result = -EOVERFLOW;
    if (token_counter == ~(UQUAD)0 || c->submitted == ~(UQUAD)0) goto out;
    for (i = 0; i < r->resource_count; i++) {
        UQUAD address = 0;
        ULONG size = 0;
        result = amdgpu_va_gpu_acquire(ad, r->resources[i], &address, &size);
        if (result) goto unwind;
        pending_refs[pending_count++] = r->resources[i];
        if (r->resources[i] == r->ib_handle) {
            result = -EINVAL;
            if (r->ib_offset > size || r->ib_dwords > (size - r->ib_offset) / 4)
                goto unwind;
            ib = address + r->ib_offset;
        }
    }
    result = -EINVAL;
    if (!ib) goto unwind;
    result = gfx_v12_managed_submit(ad, ib, r->ib_dwords, token_counter + 1);
    if (result) goto unwind;
    pending_token = ++token_counter;
    pending_context = handle;
    *sequence = ++c->submitted;
    /* One success proves the public CS -> managed-ring handoff. Per-submit
     * logging serialized every frame on the UART and reduced Gears to ~7 FPS;
     * faults, timeouts and ring-wrap evidence remain reported elsewhere. */
    if (!cs_submit_reported) {
        cs_submit_reported = TRUE;
        bug("[amdgpu-cs] first queued ctx=%08lx sequence=%08lx:%08lx "
            "token=%08lx:%08lx refs=%lu; further successes quiet\n",
            (unsigned long)handle, (unsigned long)(*sequence >> 32),
            (unsigned long)*sequence, (unsigned long)(pending_token >> 32),
            (unsigned long)pending_token, (unsigned long)pending_count);
    }
    goto out;
unwind:
    if (!release_refs(ad)) {
        pending_context = handle; /* Prevent context teardown of quarantined refs. */
        result = -EIO;
    }
out:
    ReleaseSemaphore(&cs_lock);
    return result;
}

LONG amdgpu_cs_submit(struct AmdgpuDevice *ad, ULONG handle,
                      const struct AmdgpuCsRequest *request, UQUAD *sequence)
{
    struct AmdgpuCsRequest old;
    struct AmdgpuCsRequest2 r = {0};
    if (!sequence) return -EINVAL;
    *sequence = 0;
    if (!request) return -EINVAL;
    /* Never read a v2-sized record from a legacy caller's64-byte request. */
    memcpy(&old, request, sizeof(old));
    if (old.flags || old.reserved[0] || old.reserved[1] || old.reserved[2] ||
        !old.resource_count || old.resource_count > AMDGPU_CS_MAX_RESOURCES ||
        !old.ib_dwords || old.ib_dwords > 1024) return -EINVAL;
    r.abi_version = AMDGPU_CS_EXTENDED_ABI_VERSION;
    r.ib_handle = old.ib_handle; r.ib_offset = old.ib_offset;
    r.ib_dwords = old.ib_dwords; r.resource_count = old.resource_count;
    memcpy(r.resources, old.resources, sizeof(old.resources));
    return submit_checked(ad, handle, &r, sequence);
}

LONG amdgpu_cs_submit2(struct AmdgpuDevice *ad, ULONG handle,
                       const struct AmdgpuCsRequest2 *request, ULONG size,
                       UQUAD *sequence)
{
    struct AmdgpuCsRequest2 r;
    if (!sequence) return -EINVAL;
    *sequence = 0;
    if (!request || size != sizeof(r)) return -EINVAL;
    memcpy(&r, request, sizeof(r));
    return submit_checked(ad, handle, &r, sequence);
}

LONG amdgpu_cs_query_fence(struct AmdgpuDevice *ad, ULONG handle, UQUAD sequence,
                           ULONG *complete)
{
    struct CsContext *c;
    LONG result = -ENOENT;
    if (!complete) return -EINVAL;
    *complete = 0;
    if (!sequence) return -EINVAL;
    if (!valid(ad)) return -ENODEV;
    ObtainSemaphore(&cs_lock);
    c = lookup(handle);
    if (!c || sequence > c->submitted) goto out;
    result = 0;
    if (sequence <= c->completed) { *complete = 1; goto out; }
    result = -EIO;
    if (cs_poisoned || pending_context != handle || !pending_token) goto out;
    result = gfx_v12_managed_retired(ad, pending_token);
    if (result != 1) goto out; /* zero pending and negative error both retain refs */
    if (!release_refs(ad)) { result = -EIO; goto out; }
    c->completed = c->submitted;
    pending_context = 0; pending_token = 0;
    *complete = 1; result = 0;
    if (!cs_retire_reported) {
        cs_retire_reported = TRUE;
        bug("[amdgpu-cs] first retired ctx=%08lx sequence=%08lx:%08lx; "
            "GPU fence confirmed and refs released; further successes quiet\n",
            (unsigned long)handle, (unsigned long)(sequence >> 32),
            (unsigned long)sequence);
    }
out:
    ReleaseSemaphore(&cs_lock);
    return result;
}
