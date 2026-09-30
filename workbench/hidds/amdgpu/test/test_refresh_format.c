/* Actual refresh routine, normal bounded RAM buffers, no physical GPU access.
 * Deliberately provide no plane-programming service: copying pixels cannot
 * depend on or perform a modeset. Geometry/lifetime tests are separate gates.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef int64_t QUAD;
typedef int32_t LONG;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
struct AmdgpuDevice { unsigned unused; };
struct HWData {
    void *framebuffer;
    struct AmdgpuDevice *ad;
    UQUAD fb_mc;
    ULONG width, height, bytesperline, bytesperpixel;
    ULONG fb_width, fb_height, fbsize;
    BOOL plane_faulted;
};
struct AmdgpuGfxBitMapData {
    UBYTE *VideoData;
    LONG xoffset, yoffset, disp_width, disp_height;
    ULONG bytesperpix, bytesperline, height;
    UQUAD gart_va;
    ULONG gart_size;
    ULONG width;
};
static unsigned dma_calls;
static void bug(const char *format, ...) { (void)format; }
static void CopyMem(const void *src, void *dst, ULONG n) { memmove(dst, src, n); }
static void SetMem(void *dst, unsigned char value, ULONG n) { memset(dst, value, n); }
static ULONG filled_rows, filled_bytes;
static BOOL amdgpu_sdma_fill_rect(struct AmdgpuDevice *ad, UQUAD mc,
    ULONG pitch, ULONG offset, ULONG width, ULONG rows, ULONG value)
{
    (void)ad; (void)mc; (void)offset; (void)value;
    assert(width <= pitch);
    filled_rows += rows; filled_bytes += width * rows;
    return FALSE; /* test bounded CPU fallback as well */
}
static BOOL amdgpu_sdma_blit_rect(struct AmdgpuDevice *ad,
    UQUAD *va, ULONG *size, UBYTE *buffer, ULONG total,
    ULONG srcmod, ULONG srcoff, UQUAD dest, ULONG dstmod,
    ULONG dstoff, ULONG width, ULONG rows)
{
    (void)ad; (void)va; (void)size; (void)buffer; (void)total;
    (void)srcmod; (void)srcoff; (void)dest; (void)dstmod;
    (void)dstoff; (void)width; (void)rows;
    dma_calls++;
    return FALSE;
}
#include "extent.inc"
#include "refresh.inc"
#include "clear.inc"

int main(void)
{
    struct AmdgpuDevice device = {0};
    UWORD src16[8] = {0xf800, 0x07e0, 0x001f, 0xaaaa,
                     0xffff, 0x0000, 0x8410, 0xbbbb};
    ULONG dst32[10], expected32[10];
    struct HWData hw = {dst32, &device, 1, 1920, 1080, 20, 4, 3, 2, 40, FALSE}, before;
    struct AmdgpuGfxBitMapData bm = {(UBYTE *)src16, 0, 0, 1920, 1080, 2, 8, 2, 0, 0, 3};
    for (unsigned i = 0; i < 10; i++) dst32[i] = expected32[i] = 0x5a5a5a5a;
    expected32[0] = 0xff0000; expected32[1] = 0x00ff00;
    expected32[2] = 0x0000ff; expected32[5] = 0xffffff;
    expected32[6] = 0; expected32[7] = 0x848284;
    before = hw;
    amdgpuGfxDoRefreshArea(&hw, &bm, 0, 0, 1920, 1080);
    assert(memcmp(dst32, expected32, sizeof(dst32)) == 0);
    assert(memcmp(&hw, &before, sizeof(hw)) == 0 && dma_calls == 0);

    /* Incomplete extents and invalid source pitch cannot write anything. */
    hw.fbsize = 39;
    amdgpuGfxDoRefreshArea(&hw, &bm, 0, 0, 3, 2);
    assert(memcmp(dst32, expected32, sizeof(dst32)) == 0);
    hw.fbsize = 40;
    bm.bytesperline = 5;
    amdgpuGfxDoRefreshArea(&hw, &bm, 0, 0, 3, 2);
    assert(memcmp(dst32, expected32, sizeof(dst32)) == 0);
    bm.bytesperline = 8;

    /* An uncertain plane cannot be refreshed or cleared through a stale tuple. */
    hw.plane_faulted = TRUE;
    amdgpuGfxDoRefreshArea(&hw, &bm, 0, 0, 3, 2);
    ClearBuffer(&hw);
    assert(!dma_calls && !filled_rows && !filled_bytes);
    assert(memcmp(dst32, expected32, sizeof(dst32)) == 0);
    hw.plane_faulted = FALSE;

    /* Offset clipping consumes only the remaining source pixels. */
    for (unsigned i = 0; i < 10; i++) dst32[i] = expected32[i] = 0x5a5a5a5a;
    bm.xoffset = -1; bm.yoffset = -1;
    expected32[0] = 0; expected32[1] = 0x848284;
    amdgpuGfxDoRefreshArea(&hw, &bm, -10, -10, 1920, 1080);
    assert(memcmp(dst32, expected32, sizeof(dst32)) == 0);
    bm.xoffset = 0; bm.yoffset = 0;

    /* Same-format raw copies keep row padding and hardware metadata intact. */
    UWORD dst16[10], expected16[10];
    for (unsigned i = 0; i < 10; i++) dst16[i] = expected16[i] = 0x5a5a;
    memcpy(expected16, src16, 6); memcpy(expected16 + 5, src16 + 4, 6);
    hw.framebuffer = dst16; hw.bytesperline = 10; hw.bytesperpixel = 2; hw.fbsize = 20;
    before = hw;
    amdgpuGfxDoRefreshArea(&hw, &bm, 0, 0, 3, 2);
    assert(memcmp(dst16, expected16, sizeof(dst16)) == 0);
    assert(memcmp(&hw, &before, sizeof(hw)) == 0);

    /* Unsupported conversions retain the current plane and destination. */
    bm.VideoData = (UBYTE *)dst32; bm.bytesperpix = 4; bm.bytesperline = 20;
    amdgpuGfxDoRefreshArea(&hw, &bm, 0, 0, 3, 2);
    assert(memcmp(dst16, expected16, sizeof(dst16)) == 0);
    assert(memcmp(&hw, &before, sizeof(hw)) == 0 && dma_calls == 0);

    /* Direct-scanout identity updates must not launch a self-DMA. */
    bm.VideoData = (UBYTE *)dst16; bm.bytesperpix = 2; bm.bytesperline = 10;
    amdgpuGfxDoRefreshArea(&hw, &bm, 0, 0, 3, 2);
    assert(memcmp(dst16, expected16, sizeof(dst16)) == 0 && dma_calls == 0);

    /* Clear a small framebuffer, never the advertised1920x1080 raster. */
    ULONG guarded[14];
    for (unsigned i = 0; i < 14; i++) guarded[i] = 0x5a5a5a5a;
    hw.framebuffer = guarded + 1; hw.bytesperpixel = 4;
    hw.bytesperline = 20; hw.fbsize = 40;
    ClearBuffer(&hw);
    assert(filled_rows == 2 && filled_bytes == 24);
    for (unsigned i = 1; i <= 10; i++) assert(guarded[i] == 0);
    assert(guarded[0] == 0x5a5a5a5a && guarded[11] == 0x5a5a5a5a);
    assert(guarded[12] == 0x5a5a5a5a && guarded[13] == 0x5a5a5a5a);
    hw.fbsize = 39;
    ClearBuffer(&hw);
    assert(filled_rows == 2); /* malformed extent does not touch DMA */
    puts("PASS: framebuffer extents, source/offset clipping, bounded clear, self-copy refusal, format isolation");
    return 0;
}
