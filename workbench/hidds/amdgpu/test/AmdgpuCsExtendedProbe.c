/* Native resident-only CS-v2 capacity and GPU execution. No private driver.
 * Copyright (C) 2026 The AROS Development Team. SPDX-License-Identifier: MIT */
#define DEBUG 1
#define __INLINE_AMDGPU_STACKCALL__ 1
#include <aros/debug.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/amdgpu.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct Library *AmdgpuBase;
#define LOG(...) do { printf("[AmdgpuCsExtendedProbe] " __VA_ARGS__); printf("\n"); \
    bug("[AmdgpuCsExtendedProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define NEED(c,s) do { if (!(c)) { LOG("FAIL %s",s); goto out; } LOG("PASS %s",s); } while (0)
#define CHECK(c,s) do { if (!(c)) { LOG("FAIL %s index=%lu round=%lu",s, \
    (unsigned long)i, (unsigned long)round); goto out; } } while (0)
#define COUNT AMDGPU_CS_EXTENDED_MAX_RESOURCES
#define NDW AMDGPU_CS_EXTENDED_MAX_IB_DWORDS
#define ALIGN (2UL << 20)

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad = NULL;
    struct AmdgpuCsCaps oldcaps, caps;
    struct AmdgpuBoCaps bo_caps;
    struct AmdgpuMemoryInfo before, after;
    struct AmdgpuCsRequest legacy = {0};
    static struct AmdgpuCsRequest2 req, bad;
    ULONG bo[COUNT] = {0}, va[COUNT] = {0}, ctx = 0, done = 0;
    UQUAD address[COUNT] = {0}, seq = 0, rejected = 0;
    APTR cpu[COUNT] = {0};
    BOOL owner[COUNT] = {FALSE}, bound[COUNT] = {FALSE};
    BOOL baseline = FALSE, pending = FALSE, clean = TRUE;
    ULONG i, round = 0, poll;
    LONG status, result = RETURN_FAIL;
    volatile ULONG *ib;

    Forbid(); resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd"); Permit();
    if (!resident) { LOG("REFUSED no resident driver; no load attempted"); return result; }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    NEED(AmdgpuBase == resident && AmdgpuBase->lib_NegSize >= 46 * sizeof(APTR) &&
         (AmdgpuBase->lib_Version || AmdgpuBase->lib_Revision >= 8), "resident0.8 and vector46 extent");
    NEED(amdgpu_get_device_count() == 1 && (ad = amdgpu_get_device(0)), "one resident device");
    NEED(!amdgpu_query_cs_caps(ad, &oldcaps, sizeof(oldcaps)) &&
         oldcaps.abi_version == 1 && oldcaps.max_resources == 8 && oldcaps.max_ib_dwords == 1024,
         "legacy64-byte request and queried v1 limits unchanged");
    NEED(!amdgpu_query_cs_caps2(ad, &caps, sizeof(caps)) && caps.abi_version == 2 &&
         caps.max_resources == COUNT && caps.max_ib_dwords == NDW && caps.max_inflight == 1 &&
         caps.flags == AMDGPU_CS_TRUSTED_SHARED_VM, "extended bounded limits, same trusted single queue");
    NEED(!amdgpu_query_bo_caps(ad, &bo_caps, sizeof(bo_caps)) && bo_caps.max_alignment >= ALIGN &&
         !amdgpu_query_memory_info(ad, &before, sizeof(before)), "2MiB alignment and baseline accounting");
    baseline = TRUE;
    NEED(!before.live_handles && before.reusable_handles >= COUNT, "all64 resource slots available");
    NEED(!amdgpu_cs_ctx_create(ad, 0, &ctx), "owned context");
    req.abi_version = AMDGPU_CS_EXTENDED_ABI_VERSION;
    req.ib_offset = 256; req.ib_dwords = NDW; req.resource_count = COUNT;
    for (i = 0; i < COUNT; i++) {
        ULONG bytes = i ? 8192 : NDW * 4 + 512;
        ULONG alignment = i < 2 ? ALIGN : 4096;
        ULONG domain = i == 1 ? AMDGPU_BO_DOMAIN_VRAM : AMDGPU_BO_DOMAIN_GTT;
        struct AmdgpuVaInfo vi;
        CHECK(!amdgpu_bo_alloc(ad, bytes, alignment, domain, 0, &bo[i]), "owned backing for resource");
        owner[i] = TRUE;
        CHECK(!amdgpu_bo_cpu_map(ad, bo[i], &cpu[i]) && !((IPTR)cpu[i] & (alignment - 1)) &&
             !amdgpu_va_range_alloc(ad, bytes, alignment, 0, &va[i], &address[i]) &&
             !(address[i] & (alignment - 1)), "aligned borrowed CPU and independent GPU address");
        status = amdgpu_bo_va_bind(ad, bo[i], va[i]);
        bound[i] = !amdgpu_va_query_info(ad, va[i], &vi, sizeof(vi)) && vi.bo_handle != 0;
        CHECK(!status && bound[i], "checked whole-BO PTE binding");
        req.resources[i] = va[i];
    }
    req.ib_handle = va[0];
    legacy.ib_handle = va[0]; legacy.ib_dwords = 5; legacy.resource_count = 9;
    NEED(amdgpu_cs_submit(ad, ctx, &legacy, &rejected) == -EINVAL && !rejected,
         "v1 rejects resource9 instead of reading extended fields");
    legacy.resource_count = 1; legacy.resources[0] = va[0]; legacy.ib_dwords = 1025;
    NEED(amdgpu_cs_submit(ad, ctx, &legacy, &rejected) == -EINVAL && !rejected,
         "v1 retains its1024-DWORD bound");
    NEED(amdgpu_cs_submit2(ad, ctx, &req, sizeof(req) - 1, &rejected) == -EINVAL && !rejected &&
         amdgpu_cs_submit2(ad, ctx, &req, sizeof(req) + 1, &rejected) == -EINVAL && !rejected,
         "v2 rejects both short and unknown larger request sizes");
    bad = req; bad.abi_version = 1;
    NEED(amdgpu_cs_submit2(ad, ctx, &bad, sizeof(bad), &rejected) == -EINVAL && !rejected,
         "wrong extended ABI rejected");
    bad = req; bad.resources[COUNT - 1] = bad.resources[0];
    NEED(amdgpu_cs_submit2(ad, ctx, &bad, sizeof(bad), &rejected) == -EINVAL && !rejected,
         "duplicate final resource rejected before publication");
    bad = req; bad.resource_count = COUNT + 1;
    NEED(amdgpu_cs_submit2(ad, ctx, &bad, sizeof(bad), &rejected) == -EINVAL && !rejected,
         "resource65 rejected before array access");
    bad = req; bad.ib_dwords = NDW + 1;
    NEED(amdgpu_cs_submit2(ad, ctx, &bad, sizeof(bad), &rejected) == -EINVAL && !rejected,
         "oversized IB rejected");
    bad = req; bad.reserved[1] = 1;
    NEED(amdgpu_cs_submit2(ad, ctx, &bad, sizeof(bad), &rejected) == -EINVAL && !rejected,
         "reserved fields rejected");

    ib = (volatile ULONG *)((UBYTE *)cpu[0] + req.ib_offset);
    for (round = 1; round <= 3; round++) {
        ULONG first_write = NDW - (COUNT - 1) * 5;
        for (i = 0; i < first_write; i++) ib[i] = 0xffff1000; /* one-DWORD PKT3_NOP_PAD */
        ib[-1] = 0x6811cafe; ib[NDW] = 0x6822cafe;
        for (i = 1; i < COUNT; i++) {
            volatile ULONG *target = (volatile ULONG *)((UBYTE *)cpu[i] + 4104);
            ULONG p = first_write + (i - 1) * 5;
            UQUAD dst = address[i] + 4104;
            target[-1] = 0x6833cafe; target[0] = 0; target[1] = 0x6844cafe;
            ib[p] = 0xc0033700; ib[p+1] = 0x00100500;
            ib[p+2] = (ULONG)dst; ib[p+3] = (ULONG)(dst >> 32);
            ib[p+4] = 0xe0080000 | (round << 8) | i;
        }
        __sync_synchronize();
        NEED(!amdgpu_cs_submit2(ad, ctx, &req, sizeof(req), &seq) && seq == round,
             "actual long IB submitted retaining all64 resources");
        pending = TRUE;
        NEED(amdgpu_cs_ctx_free(ad, ctx) == -EBUSY, "pending context cannot be freed");
        for (i = 0; i < COUNT; i++) {
            struct AmdgpuVaInfo vi;
            CHECK(!amdgpu_va_query_info(ad, va[i], &vi, sizeof(vi)) && vi.gpu_refs == 1 &&
                 amdgpu_bo_va_unbind(ad, va[i]) == -EBUSY, "each pending resource remains retained");
            if (round == 3) {
                CHECK(!amdgpu_bo_free(ad, bo[i]), "release allocation owner while GPU retains backing");
                owner[i] = FALSE;
            }
        }
        for (poll = 0, done = 0; poll < 250; poll++) {
            status = amdgpu_cs_query_fence(ad, ctx, seq, &done);
            if (status || done) break;
            Delay(1);
        }
        NEED(!status && done, "actual private EOP retirement");
        pending = FALSE;
        __sync_synchronize();
        NEED(ib[-1] == 0x6811cafe && ib[NDW] == 0x6822cafe, "long IB boundary guards intact");
        for (i = 1; i < COUNT; i++) {
            volatile ULONG *target = (volatile ULONG *)((UBYTE *)cpu[i] + 4104);
            CHECK(target[0] == (0xe0080000 | (round << 8) | i) &&
                 target[-1] == 0x6833cafe && target[1] == 0x6844cafe,
                 "GPU wrote referenced destination and preserved both guards");
        }
        LOG("round=%lu resources=%u IB-dwords=%lu uniqueGPUwrites=%u guards=PASS realEOP=PASS polls=%lu",
            (unsigned long)round, COUNT, (unsigned long)NDW, COUNT-1, (unsigned long)poll);
    }
    result = RETURN_OK;
out:
    if (ad) {
        for (i = 0; i < COUNT; i++) {
            if (cpu[i] && amdgpu_bo_cpu_unmap(ad, bo[i])) clean = FALSE;
            if (bound[i] && amdgpu_bo_va_unbind(ad, va[i])) clean = FALSE;
            if (owner[i] && amdgpu_bo_free(ad, bo[i])) clean = FALSE;
            if (va[i] && amdgpu_va_range_free(ad, va[i])) clean = FALSE;
        }
        if (ctx && amdgpu_cs_ctx_free(ad, ctx)) clean = FALSE;
        if (baseline && (amdgpu_query_memory_info(ad, &after, sizeof(after)) ||
                         memcmp(&before, &after, sizeof(before)))) clean = FALSE;
    }
    if (!clean || pending) result = RETURN_FAIL;
    CloseLibrary(AmdgpuBase);
    LOG("END result=%ld cleanup=%s; no shader or visible3D claim", (long)result,
        clean && !pending ? "PASS" : "FAIL-retain-quarantine");
    return result;
}
