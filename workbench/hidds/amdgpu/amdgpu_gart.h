/* Private GART service initialization; not a public GPU-VA ABI. */
#ifndef AMDGPU_GART_H
#define AMDGPU_GART_H

void amdgpu_gart_service_init(void);

struct AmdgpuDevice;
struct AmdgpuGartRange;
BOOL amdgpu_gart_capacity(struct AmdgpuDevice *ad, UQUAD *total, UQUAD *remaining);

/* Private, task-context VA transactions. Every range is fresh and can be
 * bound only once. The BO service serializes ownership and GPU retirement;
 * this layer never takes the BO lock. Failed binds retain their range until
 * a successful checked unbind, so backing cannot be freed under stale PTEs. */
struct AmdgpuGartRange *amdgpu_gart_reserve(struct AmdgpuDevice *ad,
    ULONG size, ULONG alignment, UQUAD requested, UQUAD *address);
BOOL amdgpu_gart_bind_range(struct AmdgpuDevice *ad,
    struct AmdgpuGartRange *range, APTR cpu);
BOOL amdgpu_gart_bind_vram_range(struct AmdgpuDevice *ad,
    struct AmdgpuGartRange *range, UQUAD payload_mc);
BOOL amdgpu_gart_unbind_range(struct AmdgpuDevice *ad,
    struct AmdgpuGartRange *range);
void amdgpu_gart_release_range(struct AmdgpuGartRange *range);

#endif
