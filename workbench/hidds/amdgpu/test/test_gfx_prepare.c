/* Actual preparation helper with a bounded VRAM fixture. Deliberately exposes
 * NO scanout/viewport/cursor mutators: adding one makes this compile fail. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef uintptr_t IPTR;
typedef void *APTR;
typedef char *STRPTR;
typedef int BOOL;
#define FALSE 0
#define TRUE 1
#define D(x) do { } while (0)
#define AMDGPU_FB_AUX_RESERVE 0x100000UL
#define AMDGPU_VRAM_POOL_SIZE (64UL << 20)
#define MAP_Readable 1
#define MAP_Writable 2
#define MAP_WriteCombine 4
#define CopyMem(src,dst,len) memcpy((dst),(src),(len))
struct Library { int unused; };
struct AmdgpuDevice { void *pciDriver; };
struct AmdgpuDisplayMode { ULONG w, h; };
struct SignalSemaphore { unsigned initialized; };
static void InitSemaphore(struct SignalSemaphore *sem)
{ assert(!sem->initialized); sem->initialized = 1; }
struct HWData {
    APTR framebuffer, vram_pool_cpu;
    UQUAD fb_mc, vram_pool_mc;
    ULONG width, height, bytesperpixel, bytesperline, fbsize;
    ULONG fb_width, fb_height;
    ULONG vram_pool_size, vram_pool_used;
    struct SignalSemaphore pool_lock;
    struct SignalSemaphore display_lock;
    BOOL pool_lock_ready;
    struct AmdgpuDevice *ad;
};
struct AmdgpuGfx_staticdata { struct AmdgpuDevice *device; struct HWData data; };
static unsigned char *arena;
static const ULONG arena_bytes = 96UL << 20;
static const UQUAD original_mc = 0x8000000000ULL;
static ULONG map_calls, mapped_bytes, wc_calls;
static BOOL native, read_ok, master_on, map_fail, no_pool;
static struct Library library;
static void bug(const char *fmt, ...) { (void)fmt; }
static struct Library *OpenResource(STRPTR name)
{ assert(!strcmp(name, "kernel.resource")); return &library; }
static BOOL amdgpu_dcn_get_scanout(struct AmdgpuDevice *ad, UQUAD *mc,
    ULONG *w, ULONG *h, BOOL *master)
{
    assert(ad); *mc = original_mc; *w = 1024; *h = 768;
    *master = master_on; return read_ok;
}
static BOOL amdgpu_modeset_read_timing(struct AmdgpuDevice *ad, ULONG pipe,
    struct AmdgpuDisplayMode *mode)
{
    assert(ad && pipe == 0); mode->w = 1920; mode->h = 1080; return native;
}
static APTR HIDD_PCIDriver_MapPCI(APTR driver, APTR address, ULONG length)
{
    assert(driver && address == arena && length <= arena_bytes);
    map_calls++;
    if (map_fail) return (APTR)-1;
    if (no_pool && length > AMDGPU_VRAM_POOL_SIZE) return NULL;
    mapped_bytes = length;
    return arena;
}
static int KrnMapGlobal(APTR cpu, APTR phys, ULONG length, ULONG flags)
{
    assert(cpu == arena && phys == arena && length == mapped_bytes);
    assert(flags == (MAP_Readable | MAP_Writable | MAP_WriteCombine));
    wc_calls++; return 0;
}
#include "gfx_prepare.inc"
static void reset(struct AmdgpuGfx_staticdata *xsd, struct AmdgpuDevice *ad)
{
    memset(xsd, 0, sizeof(*xsd));
    memset(arena, 0xa5, arena_bytes);
    ad->pciDriver = ad;
    xsd->device = ad;
    xsd->data.framebuffer = arena;
    xsd->data.width = 1024; xsd->data.height = 768;
    xsd->data.fb_width = 1024; xsd->data.fb_height = 768;
    xsd->data.bytesperpixel = 4; xsd->data.bytesperline = 4096;
    xsd->data.fbsize = 1024 * 768 * 4;
    read_ok = master_on = native = TRUE;
    map_fail = no_pool = FALSE;
    map_calls = mapped_bytes = wc_calls = 0;
}
static void original_intact(void)
{
    ULONG i;
    for (i = 0; i < 1024 * 768 * 4; i++) assert(arena[i] == 0xa5);
}
int main(void)
{
    struct AmdgpuDevice ad;
    struct AmdgpuGfx_staticdata xsd;
    struct HWData *hw = &xsd.data;
    ULONG i;
    arena = malloc(arena_bytes); assert(arena);
    reset(&xsd, &ad);
    amdgpuGfxVRAMHandoff(&xsd);
    assert(hw->pool_lock_ready && hw->pool_lock.initialized);
    assert(hw->display_lock.initialized);
    assert(hw->width == 1920 && hw->height == 1080 && hw->bytesperline == 7680);
    assert(hw->fb_width == 1920 && hw->fb_height == 1080);
    assert(hw->framebuffer == arena + (8UL << 20));
    assert(hw->fb_mc == original_mc + (8UL << 20));
    assert(hw->vram_pool_size == (64UL << 20) && hw->vram_pool_used == 0);
    assert(hw->vram_pool_cpu == arena + (17UL << 20));
    assert((IPTR)hw->vram_pool_cpu + hw->vram_pool_size <= (IPTR)arena + mapped_bytes);
    assert(mapped_bytes == (81UL << 20) && map_calls == 1 && wc_calls == 1);
    for (i = 0; i < hw->fbsize; i++) assert(((unsigned char *)hw->framebuffer)[i] == 0);
    original_intact();
    reset(&xsd, &ad); native = FALSE;
    amdgpuGfxVRAMHandoff(&xsd);
    assert(hw->width == 1024 && hw->height == 768);
    assert(hw->fb_width == 1024 && hw->fb_height == 768);
    assert(hw->framebuffer == arena + (3UL << 20));
    assert(!memcmp(arena, hw->framebuffer, hw->fbsize));
    original_intact();
    reset(&xsd, &ad); no_pool = TRUE;
    amdgpuGfxVRAMHandoff(&xsd);
    assert(map_calls == 2 && !hw->vram_pool_size && hw->framebuffer == arena + (8UL << 20));
    original_intact();
    reset(&xsd, &ad); map_fail = TRUE;
    amdgpuGfxVRAMHandoff(&xsd);
    assert(map_calls == 2 && !wc_calls && hw->framebuffer == arena);
    assert(!hw->fb_mc && !hw->vram_pool_size); original_intact();
    reset(&xsd, &ad); read_ok = FALSE;
    amdgpuGfxVRAMHandoff(&xsd); assert(!map_calls && hw->framebuffer == arena);
    reset(&xsd, &ad); master_on = FALSE;
    amdgpuGfxVRAMHandoff(&xsd); assert(!map_calls && hw->framebuffer == arena);
    reset(&xsd, &ad); xsd.device = NULL;
    amdgpuGfxVRAMHandoff(&xsd); assert(!map_calls && hw->framebuffer == arena);
    free(arena);
    puts("PASS: actual framebuffer preparation preserves GOP, defers plane/cursor writes, bounds full pool, seeds/clears correctly, rejects mapping failures");
    return 0;
}
