#ifndef AMDGPU_CS_PRIVATE_H
#define AMDGPU_CS_PRIVATE_H
#include <exec/types.h>
struct AmdgpuDevice;
struct HWData;
void amdgpu_cs_service_init(struct HWData *hw);
/* Called only while the CS service owns its semaphore. Publication returns
 * zero iff queued. Errors are strictly before publication, safe to unwind.
 * Retirement: 1 = matching GPU EOP, pointers and idle; 0 = still pending;
 * negative = unknown/error, resources MUST remain pinned. */
BOOL gfx_v12_managed_ready(struct AmdgpuDevice *ad);
LONG gfx_v12_managed_submit(struct AmdgpuDevice *ad, UQUAD ib, ULONG dwords, UQUAD token);
LONG gfx_v12_managed_retired(struct AmdgpuDevice *ad, UQUAD token);
/* Keep the monotonic hardware pointer below32-bit rollover. Ring STORAGE
 * wraps, but the producer counter never wraps during this bounded tier. */
#define AMDGPU_CS_BOOT_SUBMITS ((0xffffffffUL - 330UL) / 12UL)
#endif
