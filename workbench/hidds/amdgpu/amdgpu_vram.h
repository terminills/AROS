#ifndef AMDGPU_VRAM_PRIVATE_H
#define AMDGPU_VRAM_PRIVATE_H

#include <exec/types.h>

struct AmdgpuDevice;
struct HWData;

/* Immutable boot map. All ranges are byte offsets, with exclusive ends.
 * MC addresses and CPU BAR addresses are different address spaces. */
#define AMDGPU_VRAM_RANGES 32
struct AmdgpuVramRange { UQUAD start, end; };
struct AmdgpuVramMap {
    struct AmdgpuDevice *ad;
    UQUAD mc_base, mc_bytes, total, visible, usable;
    struct AmdgpuVramRange reserved[AMDGPU_VRAM_RANGES];
    ULONG count, psp_seen;
    BOOL valid, sealed;
};

/* Init ladder only: record the existing reads, never reprogram hardware. */
void amdgpu_vram_record_gmc(struct AmdgpuDevice *ad, ULONG base, ULONG top);
void amdgpu_vram_record_psp(struct AmdgpuDevice *ad, ULONG index,
    BOOL completed, ULONG status, UQUAD offset, ULONG size);
const struct AmdgpuVramMap *amdgpu_vram_seal(struct HWData *hw);

/* Find an aligned free interval after considering immutable reservations
 * and every retained BO. The caller serializes BO ownership. */
BOOL amdgpu_vram_find(const struct AmdgpuVramMap *map, UQUAD size,
    UQUAD alignment, const struct AmdgpuVramRange *busy, ULONG count,
    UQUAD *offset);

#endif
