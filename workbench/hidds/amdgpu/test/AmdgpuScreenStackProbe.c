/* Native two-screen composition discriminator, JOB-0084. Public APIs only;
 * no driver initialization, MMIO, replacement renderer or repair writes.
 * Run after the installed driver has booted and passed its regression gates.
 * Backing-store pixel checks and accepted offsets are NOT scanout proof: pair
 * all six HOLD stages with fresh HDMI. The locked public screen is restored.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>

#define LOG(...) do { bug("[AmdgpuScreenStackProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define RW 128
#define RH 32
#define BOTTOM 0x002244ccUL
#define TOP 0x0033bb66UL
#define BOTTOM_MARK 0x00dd33bbUL
#define TOP_MARK 0x00eebb33UL

#ifdef AMDGPU_STACK_SCANOUT_OBSERVER
#include "NativeScanoutSample.h"
#endif

static BOOL check(struct Screen *screen, UBYTE *pixels, LONG x, ULONG expected,
                  const char *name)
{
    ULONG i, bad = 0;
    if (ReadPixelArray(pixels, 0, 0, RW * 4, &screen->RastPort,
                       x, 16, RW, RH, RECTFMT_ARGB) != RW * RH) {
        LOG("FAIL read %s", name);
        return FALSE;
    }
    for (i = 0; i < RW * RH; ++i) {
        const UBYTE *p = pixels + i * 4;
        ULONG got = ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
        if (got != expected) ++bad;
    }
    LOG("%s backing pixels=%u bad=%lu want=%08lx", name, RW * RH,
        (unsigned long)bad, (unsigned long)expected);
    return bad == 0;
}

static struct Screen *make_screen(ULONG color)
{
    struct Screen *screen = OpenScreenTags(NULL, SA_DisplayID, 0x00110000,
        SA_Width, 1920, SA_Height, 1080, SA_Depth, 24, SA_Quiet, TRUE,
        SA_ShowTitle, FALSE, SA_AutoScroll, FALSE, SA_Draggable, FALSE, TAG_DONE);
    if (screen) FillPixelArray(&screen->RastPort, 0, 0, 1920, 1080, color);
    return screen;
}

static BOOL hold(struct Screen *screen, const char *stage, LONG y)
{
    BOOL ok;
    Delay(10);
    ok = screen->LeftEdge == 0 && screen->TopEdge == y &&
         screen->ViewPort.DxOffset == 0 && screen->ViewPort.DyOffset == y;
#ifdef AMDGPU_STACK_SCANOUT_OBSERVER
    if (!native_scanout_observe(stage, "before-hold")) ok = FALSE;
#endif
    LOG("HOLD %s expected_y=%ld actual=%d,%d viewport=%d,%d hold10s; HDMI required",
        stage, (long)y, screen->LeftEdge, screen->TopEdge,
        screen->ViewPort.DxOffset, screen->ViewPort.DyOffset);
    Delay(500);
#ifdef AMDGPU_STACK_SCANOUT_OBSERVER
    if (!native_scanout_observe(stage, "after-hold")) ok = FALSE;
#endif
    return ok;
}

int main(void)
{
    struct Screen *prior = LockPubScreen(NULL), *bottom = NULL, *top = NULL;
    UBYTE *pixels = NULL;
    BOOL ok = TRUE;
    LONG result = RETURN_FAIL;
    if (!prior) return result;
#ifdef AMDGPU_STACK_SCANOUT_OBSERVER
    LOG("OBSERVER v2 fixed read-only Navi44 scanout; rendering sequence remains v1");
    if (!native_scanout_open()) {
        LOG("FAIL observer PCI/BAR prerequisites; no screens opened");
        goto out;
    }
#endif
    LOG("BEGIN v1 mode=%08lx size=%dx%d depth=%lu", (unsigned long)
        GetVPModeID(&prior->ViewPort), prior->Width, prior->Height,
        (unsigned long)GetBitMapAttr(prior->RastPort.BitMap, BMA_DEPTH));
    if (GetVPModeID(&prior->ViewPort) != 0x00110000 || prior->Width != 1920 ||
        prior->Height != 1080 ||
        GetBitMapAttr(prior->RastPort.BitMap, BMA_DEPTH) != 24) goto out;
    pixels = AllocVec(RW * RH * 4, MEMF_ANY);
    if (!pixels) goto out;
    bottom = make_screen(BOTTOM);
    if (!bottom) goto out;
    top = make_screen(TOP);
    if (!top) goto out;
    if (!check(bottom, pixels, 512, BOTTOM, "BOTTOM-BEFORE")) ok = FALSE;
    if (!check(top, pixels, 512, TOP, "TOP-BEFORE")) ok = FALSE;
    if (!hold(top, "TOP-COVERS", 0)) ok = FALSE;

    /* These screens are ours. FORCEDRAG is limited to our own top screen;
     * no public screen coordinates or preferences are changed. */
    ScreenPosition(top, SPOS_ABSOLUTE | SPOS_FORCEDRAG, 0, 192, 0, 0);
    if (!hold(top, "DRAG192", 192)) ok = FALSE;
    if (top->TopEdge != 192 || top->ViewPort.DyOffset != 192) goto out;

    /* Exposed lower content must update at y16; top content at y208. No
     * explicit refresh/repaint call repairs the compositor's presentation. */
    FillPixelArray(&bottom->RastPort, 256, 16, RW, RH, BOTTOM_MARK);
    FillPixelArray(&top->RastPort, 256, 16, RW, RH, TOP_MARK);
    if (!check(bottom, pixels, 256, BOTTOM_MARK, "BOTTOM-UPDATED")) ok = FALSE;
    if (!check(top, pixels, 256, TOP_MARK, "TOP-UPDATED")) ok = FALSE;
    if (!hold(top, "UPDATED", 192)) ok = FALSE;

    ScreenToFront(bottom);
    if (!hold(bottom, "BOTTOM-FRONT", 0)) ok = FALSE;
    ScreenToFront(top);
    if (!hold(top, "TOP-RETURNED", 192)) ok = FALSE;
    ScreenPosition(top, SPOS_ABSOLUTE | SPOS_FORCEDRAG, 0, 0, 0, 0);
    if (!hold(top, "RESTORED0", 0)) ok = FALSE;
    if (!check(bottom, pixels, 512, BOTTOM, "BOTTOM-AFTER")) ok = FALSE;
    if (!check(top, pixels, 512, TOP, "TOP-AFTER")) ok = FALSE;
    if (!check(bottom, pixels, 256, BOTTOM_MARK, "BOTTOM-MARK-AFTER")) ok = FALSE;
    if (!check(top, pixels, 256, TOP_MARK, "TOP-MARK-AFTER")) ok = FALSE;
    result = ok ? RETURN_OK : RETURN_ERROR;
out:
    ScreenToFront(prior);
    if (top && !CloseScreen(top)) result = RETURN_FAIL;
    if (bottom && !CloseScreen(bottom)) result = RETURN_FAIL;
    if (pixels) FreeVec(pixels);
    UnlockPubScreen(NULL, prior);
#ifdef AMDGPU_STACK_SCANOUT_OBSERVER
    native_scanout_close();
#endif
    LOG("END memory/coordinate result=%ld; six fresh HDMI stages decide presentation",
        (long)result);
    return result;
}
