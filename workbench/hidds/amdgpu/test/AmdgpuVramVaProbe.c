/* Resident-only GPU execution through driver-owned VRAM/GTT bindings.
 * No private driver, raw queue or reinitialization. Install/compare/reboot first.
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
#define LOG(...) do { printf("[AmdgpuVramVaProbe] " __VA_ARGS__); printf("\n"); \
    bug("[AmdgpuVramVaProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define NEED(c, s) do { if (!(c)) { LOG("FAIL %s", s); goto out; } \
    LOG("PASS %s", s); } while (0)

static BOOL execute_pair(struct AmdgpuDevice *ad, ULONG ib_domain, ULONG dst_domain, ULONG round)
{
    ULONG bo[2] = {0}, va[2] = {0}, domains[2] = {ib_domain, dst_domain};
    ULONG ctx = 0, complete = 0, i, poll, want = 0x76000000UL | round;
    UQUAD address[2] = {0}, sequence = 0;
    APTR cpu[2] = {0};
    BOOL owner[2] = {FALSE}, bound[2] = {FALSE}, clean = TRUE, passed = FALSE;
    struct AmdgpuCsRequest req = {0};
    struct AmdgpuVaInfo vi = {0};
    LONG status = 0;
    volatile ULONG *ib, *target;

    NEED(amdgpu_cs_ctx_create(ad, 0, &ctx) == 0, "managed context");
    for (i = 0; i < 2; i++) {
        NEED(amdgpu_bo_alloc(ad, 8192, 65536, domains[i], 0, &bo[i]) == 0,
             "driver-owned aligned storage"); owner[i] = TRUE;
        NEED(amdgpu_bo_cpu_map(ad, bo[i], &cpu[i]) == 0 && !((IPTR)cpu[i] & 65535) &&
             amdgpu_va_range_alloc(ad, 8192, 65536, 0, &va[i], &address[i]) == 0 &&
             !(address[i] & 65535), "64KiB-aligned CPU payload and GPU reservation");
        status = amdgpu_bo_va_bind(ad, bo[i], va[i]);
        /* A failed partial bind still belongs to the driver. Cleanup queries
         * the record, never assumes a nonzero return released its backing. */
        bound[i] = !amdgpu_va_query_info(ad, va[i], &vi, sizeof(vi)) && vi.bo_handle != 0;
        NEED(status == 0 && bound[i], "whole BO checked PTE binding");
    }
    ib = (volatile ULONG *)((UBYTE *)cpu[0] + 256);
    target = (volatile ULONG *)((UBYTE *)cpu[1] + 4104);
    ib[-1] = 0x61c0ffee; ib[5] = 0x62c0ffee;
    target[-1] = 0x71c0ffee; target[0] = ~want; target[1] = 0x72c0ffee;
    ib[0] = 0xc0033700; ib[1] = 0x00100500; /* WRITE_DATA memory, WR_CONFIRM */
    ib[2] = (ULONG)(address[1] + 4104); ib[3] = (ULONG)((address[1] + 4104) >> 32);
    ib[4] = want;
    __sync_synchronize();
    req.ib_handle = va[0]; req.ib_offset = 256; req.ib_dwords = 5;
    req.resource_count = 2; req.resources[0] = va[0]; req.resources[1] = va[1];
    NEED(amdgpu_cs_submit(ad, ctx, &req, &sequence) == 0 && sequence == 1,
         "real graphics IB submitted with driver-owned resources");
    for (i = 0; i < 2; i++) {
        NEED(amdgpu_bo_free(ad, bo[i]) == 0, "owner released while GPU retains backing");
        owner[i] = FALSE;
        NEED(amdgpu_va_query_info(ad, va[i], &vi, sizeof(vi)) == 0 && vi.gpu_refs == 1 &&
             amdgpu_bo_va_unbind(ad, va[i]) == -EBUSY, "pending GPU binding cannot be reclaimed");
    }
    NEED(amdgpu_cs_ctx_free(ad, ctx) == -EBUSY, "pending context cannot be freed");
    for (poll = 0; poll < 250; poll++) {
        status = amdgpu_cs_query_fence(ad, ctx, sequence, &complete);
        if (status || complete) break;
        Delay(1);
    }
    __sync_synchronize();
    LOG("round=%lu IB-domain=%lu dst-domain=%lu status=%ld done=%lu value=%08lx/%08lx polls=%lu",
        (unsigned long)round, (unsigned long)ib_domain, (unsigned long)dst_domain,
        (long)status, (unsigned long)complete, (unsigned long)*target,
        (unsigned long)want, (unsigned long)poll);
    NEED(status == 0 && complete && *target == want && target[-1] == 0x71c0ffee &&
         target[1] == 0x72c0ffee && ib[-1] == 0x61c0ffee && ib[5] == 0x62c0ffee,
         "GPU consumed offset IB, wrote second-page payload and preserved both guard pairs");
    for (i = 0; i < 2; i++)
        NEED(amdgpu_va_query_info(ad, va[i], &vi, sizeof(vi)) == 0 && !vi.gpu_refs &&
             vi.state == AMDGPU_VA_STATE_BOUND, "real EOP retirement released only GPU references");
    passed = TRUE;
out:
    for (i = 0; i < 2; i++) {
        if (cpu[i] && amdgpu_bo_cpu_unmap(ad, bo[i])) clean = FALSE;
        if (bound[i] && amdgpu_bo_va_unbind(ad, va[i])) clean = FALSE;
        if (owner[i] && amdgpu_bo_free(ad, bo[i])) clean = FALSE;
        if (va[i] && amdgpu_va_range_free(ad, va[i])) clean = FALSE;
    }
    if (ctx && amdgpu_cs_ctx_free(ad, ctx)) clean = FALSE;
    if (!clean) LOG("FAIL cleanup; pending/quarantined backing remains driver-owned");
    return passed && clean;
}

int main(void)
{
    struct Library *resident;
    struct AmdgpuDevice *ad = NULL;
    struct AmdgpuVaCaps caps = {0};
    struct AmdgpuMemoryInfo before = {0}, after = {0};
    LONG result = RETURN_FAIL;
    ULONG round = 0, i, j;
    const ULONG domains[] = {AMDGPU_BO_DOMAIN_GTT, AMDGPU_BO_DOMAIN_VRAM};
    Forbid(); resident = (struct Library *)FindName(&SysBase->LibList, "amdgpu.hidd"); Permit();
    if (!resident) { LOG("REFUSED absent resident driver; no load attempted"); return result; }
    AmdgpuBase = OpenLibrary("amdgpu.hidd", 0);
    if (!AmdgpuBase) return result;
    NEED(AmdgpuBase == resident && AmdgpuBase->lib_NegSize >= 44 * sizeof(APTR) &&
         (AmdgpuBase->lib_Version > 0 || AmdgpuBase->lib_Revision >= 6), "resident0.6 identity/vector extent");
    NEED(amdgpu_get_device_count() == 1 && (ad = amdgpu_get_device(0)) &&
         amdgpu_query_va_caps(ad, &caps, sizeof(caps)) == 0 && caps.abi_version == 1 &&
         caps.domains == (AMDGPU_BO_DOMAIN_GTT | AMDGPU_BO_DOMAIN_VRAM), "advertised native VRAM/GTT bindings");
    LOG("BEGIN v1 resident=%p revision=%u.%u", AmdgpuBase,
        AmdgpuBase->lib_Version, AmdgpuBase->lib_Revision);
    NEED(amdgpu_query_memory_info(ad, &before, sizeof(before)) == 0, "initial managed memory snapshot");
    for (i = 0; i < 2; i++) for (j = 0; j < 2; j++) {
        NEED(execute_pair(ad, domains[i], domains[j], ++round), "domain pair execution and cleanup");
        NEED(amdgpu_query_memory_info(ad, &after, sizeof(after)) == 0 &&
             !memcmp(&before, &after, sizeof(before)), "exact backing/reference accounting recovered");
    }
    result = RETURN_OK;
out:
    LOG("END result=%ld; no renderer or3D acceptance claim", (long)result);
    CloseLibrary(AmdgpuBase);
    return result;
}
