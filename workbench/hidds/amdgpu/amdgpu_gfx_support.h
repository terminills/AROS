#ifndef AMDGPU_GFX_SUPPORT_H
#define AMDGPU_GFX_SUPPORT_H

#include <exec/types.h>
#include <exec/semaphores.h>
#include <oop/oop.h>

/* Navi44 fixed VRAM MC base. CPU offsets inside the mapped BAR aperture and MC
   offsets from this base stay in lockstep, which is what lets one bump cursor
   yield both addresses (amdgpu_vram_alloc). */
#define AMDGPU_VRAM_MC_BASE  0x8000000000ULL

struct HWData
{
    APTR	 framebuffer;
    /* Prepared/selected software destination and bound device. Preparation
       alone does not establish that the hardware is scanning this address. */
    UQUAD	 fb_mc;
    struct AmdgpuDevice *ad;
    ULONG	 fbsize;
    /* Software framebuffer extent, separate from the published/native raster.
       Update with framebuffer/fb_mc/pitch/format under LOCK_FRAMEBUFFER. */
    ULONG        fb_width;
    ULONG        fb_height;
    ULONG	 width;
    ULONG	 height;
    ULONG	 depth;
    ULONG	 bytesperpixel;
    ULONG	 bitsperpixel;
    ULONG	 redmask;
    ULONG	 greenmask;
    ULONG	 bluemask;
    ULONG	 redshift;
    ULONG	 greenshift;
    ULONG	 blueshift;
    ULONG	 bytesperline;
    UBYTE	 palettewidth;

    /*
     * VRAM pool for bitmap storage, carved out of the same BAR aperture the
     * scanout lives in. CPU and MC offsets stay in lockstep across it (the
     * aperture maps one contiguous VRAM range), so one bump cursor yields both
     * addresses for an allocation. Zero size means the pool is unavailable and
     * callers must fall back -- bitmaps then keep ChunkyBM's system-RAM buffer.
     */
    APTR	 vram_pool_cpu;
    UQUAD	 vram_pool_mc;
    ULONG	 vram_pool_size;
    ULONG	 vram_pool_used;

    /* Pool/plane lifetime lock: acquire after framebufferlock for a switch.
       Alloc/free use only this lock. Never reuse an active scanout allocation;
       after an uncertain transaction retain the entire pool until recovery. */
    struct SignalSemaphore pool_lock;
    BOOL pool_lock_ready;
    /* Serializes plane selection plus OOP publication. Never acquire this
       from a framebuffer/cursor drawing callback: publication calls OOP only
       after framebufferlock and pool_lock have both been released. */
    struct SignalSemaphore display_lock;
    BOOL cursor_requested_visible;
    BOOL plane_faulted;
    UQUAD scanout_active_mc;
    ULONG scanout_active_size;
    UQUAD scanout_release_mc;
    ULONG scanout_release_size;

    /* AUDIT P1-2: a free list so create/destroy churn reclaims pool space
       instead of exhausting it. amdgpu_vram_free() returns a block here (or
       shrinks the bump cursor when the block is at the tail); amdgpu_vram_alloc()
       satisfies from here first-fit before bumping. Bounded -- a full list just
       forgoes reuse of that block, it is never a correctness failure. */
#define AMDGPU_VRAM_FREE_MAX 64
    struct AmdgpuVramFreeBlock { ULONG off; ULONG size; } vram_free[AMDGPU_VRAM_FREE_MAX];
    UWORD	 vram_free_count;
};

/* Allocate from the VRAM pool: reuses a freed block (first-fit) before bumping.
   Returns FALSE (and leaves *cpu/*mc alone) when the pool is absent or exhausted,
   a normal recoverable case (caller keeps ChunkyBM's system-RAM buffer). */
BOOL amdgpu_vram_alloc(struct HWData *hw, ULONG size, ULONG align,
                       APTR *cpu, UQUAD *mc);

/* Return a pool allocation (by its MC address and the size it was allocated
   with) so it can be reused. Shrinks the bump cursor if the block is at the tail,
   otherwise records it in the free list. */
void amdgpu_vram_free(struct HWData *hw, UQUAD mc, ULONG size);

/* aHidd_Gfx_MemoryAttribs / aHidd_Gfx_SensorAttribs tag fillers. */
struct TagItem;
void amdgpu_gfx_memory_tags(struct HWData *hw, struct TagItem *tags);
void amdgpu_gfx_sensor_tags(struct AmdgpuDevice *ad, struct TagItem *tags);
void amdgpu_gfx_info_tags(struct AmdgpuDevice *ad, struct TagItem *tags, ULONG version,
                          ULONG revision);

/* Caller owns pool_lock, and has verified new scanout latch/old retirement. */
void amdgpu_vram_retire_scanout_locked(struct HWData *hw, UQUAD mc, ULONG size);

struct AmdgpuGfx_staticdata;
struct AmdgpuGfxBitMapData;

BOOL initAmdgpuGfxHW(struct HWData *);
void amdgpuGfxVRAMHandoff(struct AmdgpuGfx_staticdata *xsd);
void DACLoad(struct AmdgpuGfx_staticdata *, UBYTE *, unsigned char, int);
void ClearBuffer(struct HWData *data);
void amdgpuGfxDoRefreshArea(struct HWData *hwdata, struct AmdgpuGfxBitMapData *data,
			      LONG x1, LONG y1, LONG x2, LONG y2);

#endif /* AMDGPU_GFX_SUPPORT_H */
