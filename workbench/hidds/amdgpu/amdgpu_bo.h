#ifndef AMDGPU_BO_PRIVATE_H
#define AMDGPU_BO_PRIVATE_H

struct HWData;

/* Called once during module initialization, after the display pool is ready.
 * No allocation, register write or display transaction occurs here. */
void amdgpu_bo_service_init(struct HWData *hw);

struct AmdgpuDevice;
/* Driver-only CS resource references. Acquire BEFORE publishing any
 * packet; release ONLY after confirmed hardware retirement (never on timeout).
 * A binding pins backing; the GPU count additionally prevents its unbind.
 * No public acquire/release vector lets a client assert its own retirement. */
LONG amdgpu_va_gpu_acquire(struct AmdgpuDevice *ad, ULONG handle, UQUAD *address, ULONG *size);
LONG amdgpu_va_gpu_release(struct AmdgpuDevice *ad, ULONG handle);

#endif
