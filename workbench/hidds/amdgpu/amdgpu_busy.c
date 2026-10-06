/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Per-engine busy time, from the driver's own submissions: an engine is
    busy from the first submission until the driver sees its last fence.
*/

#include <proto/exec.h>

#include "amdgpu_intern.h"

static UQUAD busy_now(void)
{
    ULONG lo, hi;

    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((UQUAD)hi << 32) | lo;
}

void amdgpu_busy_init(struct AmdgpuDevice *ad)
{
    ULONG i;

    InitSemaphore(&ad->busy_sem);
    for (i = 0; i < AMDGPU_ENGINE_COUNT; i++) {
        ad->busy[i].busy = ad->busy[i].inflight = 0;
        ad->busy[i].last_busy = 0;
        ad->busy[i].last_now = busy_now();
    }
    ad->busy_ready = TRUE;
}

void amdgpu_busy_begin(struct AmdgpuDevice *ad, ULONG engine)
{
    if (!ad->busy_ready || engine >= AMDGPU_ENGINE_COUNT)
        return;
    ObtainSemaphore(&ad->busy_sem);
    if (!ad->busy[engine].inflight++)
        ad->busy[engine].start = busy_now();
    ReleaseSemaphore(&ad->busy_sem);
}

void amdgpu_busy_end(struct AmdgpuDevice *ad, ULONG engine)
{
    if (!ad->busy_ready || engine >= AMDGPU_ENGINE_COUNT)
        return;
    ObtainSemaphore(&ad->busy_sem);
    if (ad->busy[engine].inflight && !--ad->busy[engine].inflight)
        ad->busy[engine].busy += busy_now() - ad->busy[engine].start;
    ReleaseSemaphore(&ad->busy_sem);
}

ULONG amdgpu_busy_permille(struct AmdgpuDevice *ad, ULONG engine)
{
    struct AmdgpuEngineBusy *e;
    UQUAD now, busy, d_busy, d_now;

    if (!ad->busy_ready || engine >= AMDGPU_ENGINE_COUNT)
        return 0;
    e = &ad->busy[engine];
    ObtainSemaphore(&ad->busy_sem);
    now = busy_now();
    busy = e->busy;
    if (e->inflight) {
        busy += now - e->start;
        e->busy = busy;
        e->start = now;
    }
    d_busy = busy - e->last_busy;
    d_now = now - e->last_now;
    e->last_busy = busy;
    e->last_now = now;
    ReleaseSemaphore(&ad->busy_sem);
    if (!d_now)
        return 0;
    if (d_busy >= d_now)
        return 1000;
    return (ULONG)((d_busy * 1000) / d_now);
}
