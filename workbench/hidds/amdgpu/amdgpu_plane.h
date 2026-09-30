/* DCN4.1 pipe0 transaction. Private driver interface; no ME dependency. */
#ifndef AMDGPU_PLANE_H
#define AMDGPU_PLANE_H
#include <stdint.h>

struct AmdgpuPlane {
    uint64_t mc;
    uint32_t width, height, pitch_pixels, bytes_per_pixel;
    uint32_t raster_width, raster_height;
    uint32_t source_x, source_y, backing_height;
};

/* Offsets are relative to DMU segment2, not CPU physical addresses.
 * The adapter validates the device/BAR and supplies a monotonic clock.
 * Caller must serialize the entire transaction and keep old/new storage alive. */
struct AmdgpuPlaneIO {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    uint64_t (*ticks)(void *);
    uint64_t timeout_ticks;
};

enum AmdgpuPlaneResult {
    AMDGPU_PLANE_PRESERVED, /* No change, or verified rollback to old plane. */
    AMDGPU_PLANE_ACTIVE,   /* New address in use, no pending flip/owned lock. */
    AMDGPU_PLANE_UNCERTAIN /* Retain ALL possibly scanned storage; recover. */
};

enum AmdgpuPlaneResult amdgpu_plane_update(const struct AmdgpuPlaneIO *,
                                          const struct AmdgpuPlane *);
struct AmdgpuDevice;
enum AmdgpuPlaneResult amdgpu_plane_apply(struct AmdgpuDevice *,
                                         const struct AmdgpuPlane *);
#endif
