/* Execute the actual OOP-facing switch hook with attribute-forwarding fakes.
 * No fake class internals, no GPU access. Later register rollback, cursor
 * ownership and software scanout geometry are intentionally separate gates. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../amdgpu_plane.h"
typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef uintptr_t IPTR;
typedef intptr_t SIPTR;
typedef void *APTR;
typedef IPTR HIDDT_ModeID; /* rom/hidds/gfx/include/gfx.h, not ULONG */
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define vHidd_ModeID_Invalid ((HIDDT_ModeID)-1)
enum {
    aHidd_BitMap_Display, aHidd_Display_DMEnumerator, aHidd_BitMap_ModeID,
    aHidd_Sync_HDisp, aHidd_Sync_VDisp, aHidd_PixFmt_BytesPerPixel,
    aHidd_ChunkyBM_Buffer, aHidd_BitMap_Width, aHidd_BitMap_Height,
    aHidd_BitMap_BytesPerRow, aHidd_BitMap_PixFmt,
    aHidd_BitMap_LeftEdge, aHidd_BitMap_TopEdge, ATTRS
};
typedef struct { IPTR attrs[ATTRS]; } OOP_Object;
struct AmdgpuDevice { int unused; };
struct SignalSemaphore { unsigned held; };
struct HWData {
    ULONG width, height, vram_pool_size;
    ULONG fb_width, fb_height, fbsize, bytesperline, bytesperpixel;
    APTR vram_pool_cpu, framebuffer;
    UQUAD vram_pool_mc, fb_mc;
    struct SignalSemaphore pool_lock;
    BOOL pool_lock_ready, plane_faulted;
};
struct AmdgpuGfx_staticdata { struct AmdgpuDevice *device; struct HWData data; };
typedef struct { struct AmdgpuGfx_staticdata *sd; } OOP_Class;
#define XSD(cl) ((cl)->sd)
static OOP_Object bitmap, display, enumerator, sync_object, format, bitmap_format;
static struct AmdgpuDevice device;
static struct AmdgpuGfx_staticdata sd;
static OOP_Class cls = { &sd };
static unsigned software_lock, software_locks, software_unlocks;
static unsigned lock_calls, unlock_calls, apply_calls, retire_calls;
static ULONG actual_pitch;
static BOOL mode_ok;
static enum AmdgpuPlaneResult plane_result;
static void bug(const char *format_string, ...) { (void)format_string; }
static void OOP_GetAttr(OOP_Object *o, unsigned attr, IPTR *out)
{
    assert(o && attr < ATTRS && out);
    *out = o->attrs[attr];
}
static IPTR get(OOP_Object *o, unsigned attr)
{
    IPTR value;
    OOP_GetAttr(o, attr, &value);
    return value;
}
#define OOP_GET(o, attr) get(o, attr)
static BOOL HIDD_DMEnum_GetMode(OOP_Object *o, HIDDT_ModeID mode,
                                OOP_Object **s, OOP_Object **f)
{
    assert(o == &enumerator && mode == bitmap.attrs[aHidd_BitMap_ModeID]);
    *s = &sync_object; *f = &format;
    return mode_ok;
}
static void lock_software(struct AmdgpuGfx_staticdata *state)
{
    assert(state == &sd && !software_lock);
    software_lock = 1; ++software_locks;
}
static void unlock_software(struct AmdgpuGfx_staticdata *state)
{
    assert(state == &sd && software_lock);
    software_lock = 0; ++software_unlocks;
}
#define LOCK_FRAMEBUFFER(state) lock_software(state)
#define UNLOCK_FRAMEBUFFER(state) unlock_software(state)
static void ObtainSemaphore(struct SignalSemaphore *sem)
{
    assert(sem == &sd.data.pool_lock && !sem->held && software_lock);
    sem->held = 1; ++lock_calls;
}
static void ReleaseSemaphore(struct SignalSemaphore *sem)
{
    assert(sem == &sd.data.pool_lock && sem->held && software_lock);
    sem->held = 0; ++unlock_calls;
}
enum AmdgpuPlaneResult amdgpu_plane_apply(struct AmdgpuDevice *ad,
                                         const struct AmdgpuPlane *plane)
{
    assert(ad == &device && software_lock && sd.data.pool_lock.held);
    ++apply_calls;
    assert(plane->width == sync_object.attrs[aHidd_Sync_HDisp]);
    assert(plane->height == sync_object.attrs[aHidd_Sync_VDisp]);
    assert(plane->bytes_per_pixel == format.attrs[aHidd_PixFmt_BytesPerPixel]);
    actual_pitch = plane->pitch_pixels;
    assert(actual_pitch == bitmap.attrs[aHidd_BitMap_BytesPerRow] /
                    format.attrs[aHidd_PixFmt_BytesPerPixel]);
    assert(plane->mc == sd.data.vram_pool_mc +
           bitmap.attrs[aHidd_ChunkyBM_Buffer] - (IPTR)sd.data.vram_pool_cpu);
    assert(plane->raster_width == sd.data.width && plane->raster_height == sd.data.height);
    return plane_result;
}
static void amdgpu_vram_retire_scanout_locked(struct HWData *hw, UQUAD mc, ULONG size)
{
    assert(hw == &sd.data && hw->pool_lock.held && software_lock);
    assert(plane_result == AMDGPU_PLANE_ACTIVE && apply_calls == 1);
    assert(mc == hw->vram_pool_mc + bitmap.attrs[aHidd_ChunkyBM_Buffer] - (IPTR)hw->vram_pool_cpu);
    assert(size == bitmap.attrs[aHidd_BitMap_BytesPerRow] * bitmap.attrs[aHidd_BitMap_Height]);
    ++retire_calls;
}
#include "display_switch.inc"
#define amdgpu_compositor_switch_mode amdgpu_select_plane
static void reset(void)
{
    memset(&bitmap, 0, sizeof(bitmap));
    memset(&sd, 0, sizeof(sd));
    sd.device = &device;
    sd.data.width = 1920; sd.data.height = 1080;
    sd.data.vram_pool_size = 16 * 1024 * 1024;
    sd.data.vram_pool_cpu = (APTR)(IPTR)0x100000;
    sd.data.vram_pool_mc = UINT64_C(0x8000800000);
    sd.data.framebuffer = (APTR)(IPTR)0x2000;
    sd.data.fb_mc = UINT64_C(0x8000000000);
    sd.data.fb_width = 1024; sd.data.fb_height = 768;
    sd.data.bytesperline = 4096; sd.data.bytesperpixel = 4;
    sd.data.fbsize = 4096 * 768;
    sd.data.pool_lock_ready = TRUE;
    bitmap.attrs[aHidd_BitMap_Display] = (IPTR)&display;
    display.attrs[aHidd_Display_DMEnumerator] = (IPTR)&enumerator;
    bitmap.attrs[aHidd_BitMap_ModeID] = 0x110001;
    bitmap.attrs[aHidd_ChunkyBM_Buffer] = 0x101000;
    bitmap.attrs[aHidd_BitMap_Width] = 1366;
    bitmap.attrs[aHidd_BitMap_Height] = 768;
    bitmap.attrs[aHidd_BitMap_BytesPerRow] = 1376 * 4;
    bitmap.attrs[aHidd_BitMap_PixFmt] = (IPTR)&bitmap_format;
    bitmap_format.attrs[aHidd_PixFmt_BytesPerPixel] = 4;
    sync_object.attrs[aHidd_Sync_HDisp] = 1366;
    sync_object.attrs[aHidd_Sync_VDisp] = 768;
    format.attrs[aHidd_PixFmt_BytesPerPixel] = 4;
    software_lock = software_locks = software_unlocks = 0;
    lock_calls = unlock_calls = apply_calls = retire_calls = actual_pitch = 0;
    mode_ok = TRUE; plane_result = AMDGPU_PLANE_ACTIVE;
}
static void unchanged(void)
{
    assert(sd.data.framebuffer == (APTR)(IPTR)0x2000);
    assert(sd.data.fb_mc == UINT64_C(0x8000000000));
    assert(sd.data.fb_width == 1024 && sd.data.fb_height == 768);
    assert(sd.data.bytesperline == 4096 && sd.data.bytesperpixel == 4);
    assert(sd.data.fbsize == 4096 * 768);
    assert(!software_lock && software_locks == software_unlocks);
    assert(!sd.data.pool_lock.held && lock_calls == unlock_calls && !retire_calls);
}
static void rejected(void)
{
    assert(!amdgpu_compositor_switch_mode(&cls, &bitmap));
    assert(apply_calls == 0 && lock_calls == 0 && unlock_calls == 0);
    assert(!software_locks && !software_unlocks);
    unchanged();
}
static void valid(void)
{
    assert(amdgpu_compositor_switch_mode(&cls, &bitmap));
    assert(apply_calls == 1 && retire_calls == 1 && lock_calls == 1 && unlock_calls == 1);
    assert(software_locks == 1 && software_unlocks == 1 && !software_lock);
    assert(sd.data.framebuffer == (APTR)bitmap.attrs[aHidd_ChunkyBM_Buffer]);
    assert(actual_pitch == 1376);
    assert(sd.data.width == 1920 && sd.data.height == 1080);
    assert(sd.data.fb_width == 1366 && sd.data.fb_height == 768);
    assert(sd.data.bytesperline == bitmap.attrs[aHidd_BitMap_BytesPerRow]);
    assert(sd.data.bytesperpixel == format.attrs[aHidd_PixFmt_BytesPerPixel]);
    assert(sd.data.fbsize == bitmap.attrs[aHidd_BitMap_BytesPerRow] *
                            bitmap.attrs[aHidd_BitMap_Height]);
}
int main(void)
{
    unsigned i;
    reset(); valid(); /* padded storage, not visible width */
    reset(); bitmap.attrs[aHidd_BitMap_BytesPerRow] = 1376 * 2;
    bitmap_format.attrs[aHidd_PixFmt_BytesPerPixel] = 2;
    format.attrs[aHidd_PixFmt_BytesPerPixel] = 2; valid();
    reset(); sd.data.vram_pool_size = 0x1000 + 1376 * 4 * 768; valid();
    reset(); sd.data.pool_lock_ready = FALSE;
    assert(!amdgpu_compositor_switch_mode(&cls, &bitmap));
    assert(lock_calls == 0 && apply_calls == 0 && unlock_calls == 0);
    assert(software_locks == 1 && software_unlocks == 1); unchanged();
    reset(); plane_result = AMDGPU_PLANE_PRESERVED;
    assert(!amdgpu_compositor_switch_mode(&cls, &bitmap));
    assert(apply_calls == 1 && !sd.data.plane_faulted); unchanged();
    reset(); plane_result = AMDGPU_PLANE_UNCERTAIN;
    assert(!amdgpu_compositor_switch_mode(&cls, &bitmap));
    assert(apply_calls == 1 && sd.data.plane_faulted); unchanged();
    assert(!amdgpu_compositor_switch_mode(&cls, &bitmap));
    assert(apply_calls == 1 && sd.data.plane_faulted); unchanged();
    reset(); assert(!amdgpu_compositor_switch_mode(&cls, NULL)); unchanged();
    reset(); bitmap.attrs[aHidd_BitMap_Display] = 0; rejected();
    reset(); display.attrs[aHidd_Display_DMEnumerator] = 0; rejected();
    reset(); sd.device = NULL; rejected();
    reset(); mode_ok = FALSE; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_ModeID] = vHidd_ModeID_Invalid; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_ModeID] |= UINT64_C(1) << 32; valid();
    reset(); sync_object.attrs[aHidd_Sync_HDisp] = 0; rejected();
    reset(); sync_object.attrs[aHidd_Sync_VDisp] = 0x4000; rejected();
    reset(); sync_object.attrs[aHidd_Sync_HDisp] |= UINT64_C(1) << 32; rejected();
    reset(); sd.data.width = 1365; rejected();
    reset(); sd.data.height = 0; rejected();
    for (i = 0; i <= 5; ++i) {
        if (i == 2 || i == 4) continue;
        reset(); format.attrs[aHidd_PixFmt_BytesPerPixel] = i; rejected();
    }
    reset(); bitmap.attrs[aHidd_BitMap_PixFmt] = 0; rejected();
    reset(); bitmap_format.attrs[aHidd_PixFmt_BytesPerPixel] = 2; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_Width] = 1365; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_Height] = 767; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_Height] |= UINT64_C(1) << 32; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_Width] = 0x10001; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_BytesPerRow] = 1365 * 4; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_BytesPerRow]++; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_BytesPerRow] = 0; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_BytesPerRow] = 0x10001 * 4; rejected();
    reset(); bitmap.attrs[aHidd_BitMap_BytesPerRow] |= UINT64_C(1) << 32; rejected();
    reset(); sd.data.vram_pool_cpu = NULL; rejected();
    reset(); sd.data.vram_pool_size = 0; rejected();
    reset(); bitmap.attrs[aHidd_ChunkyBM_Buffer] = 0; rejected();
    reset(); bitmap.attrs[aHidd_ChunkyBM_Buffer] = 0xfffff; rejected();
    reset(); bitmap.attrs[aHidd_ChunkyBM_Buffer] = 0x100000 + sd.data.vram_pool_size; rejected();
    reset(); sd.data.vram_pool_size = 0x1000 + 1376 * 4 * 768 - 1; rejected();
    reset(); sd.data.vram_pool_size = 0x1000 + 1376 * 4 * 768;
    bitmap.attrs[aHidd_BitMap_Height]++; rejected(); /* entire bitmap, not just viewport */
    reset(); bitmap.attrs[aHidd_ChunkyBM_Buffer]++; rejected(); /* alignment */
    reset(); sd.data.vram_pool_cpu = (APTR)(UINTPTR_MAX - 0x2000);
    bitmap.attrs[aHidd_ChunkyBM_Buffer] = UINTPTR_MAX - 0x1000; rejected();
    reset(); sd.data.vram_pool_mc = UINT64_MAX - 0xfff; rejected(); /* base overflow */
    reset(); sd.data.vram_pool_mc = UINT64_MAX - 0x1fff; rejected(); /* span overflow */
    reset(); sd.data.vram_pool_mc = UINT64_C(1) << 48; rejected(); /* register width */
    reset(); sd.data.vram_pool_mc = (UINT64_C(1) << 48) - 0x2000; rejected();
    puts("PASS: actual display hook padded stride, full pool span, wide attributes, format/alignment validation and failed-lock no-plane-write behavior");
    return 0;
}
