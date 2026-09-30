/* Native public-API panning discriminator. Run only after installing/rebooting
 * the test package. No hardware initialization or direct MMIO writes. The
 * inherited public screen stays locked so it can be restored on every exit.
 * Memory preservation is an independent oracle; accepted viewport coordinates
 * alone do NOT prove that DCN fetched the requested origin.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>

#define LOG(...) do { bug("[AmdgpuPanProbe] " __VA_ARGS__); bug("\n"); } while (0)

static const ULONG colors[4] = {0x00ee4444, 0x0044ee88, 0x004488ee, 0x00eedd44};

static BOOL pixels_intact(struct Screen *screen, const char *stage)
{
    unsigned i;
    BOOL ok = TRUE;
    for (i = 0; i < 4; ++i) {
        ULONG got = ReadRGBPixel(&screen->RastPort,
            8 + (i & 1) * 64, 8 + (i >> 1) * 32) & 0x00ffffff;
        if (got != colors[i]) ok = FALSE;
        LOG("%s pixel%u %s got=%08lx want=%08lx", stage, i,
            got == colors[i] ? "PASS" : "FAIL",
            (unsigned long)got, (unsigned long)colors[i]);
    }
    return ok;
}

int main(void)
{
    struct Screen *prior = LockPubScreen(NULL), *screen = NULL;
    struct BitMap *bitmap = NULL;
    ULONG mode, depth;
    unsigned i;
    BOOL ok = TRUE;
    LONG result = RETURN_FAIL;
    if (!prior) { LOG("FAIL no public screen"); return result; }
    mode = GetVPModeID(&prior->ViewPort);
    depth = GetBitMapAttr(prior->RastPort.BitMap, BMA_DEPTH);
    LOG("BEGIN mode=%08lx prior=%dx%d depth=%lu", (unsigned long)mode,
        prior->Width, prior->Height, (unsigned long)depth);
    /* This first discriminator is intentionally bounded to the verified DUT
     * mode; do not change a firmware/VESA or unknown public display. */
    if (mode != 0x00110000 || prior->Width != 1920 ||
        prior->Height != 1080 || depth != 24) {
        LOG("FAIL public-screen precondition");
        goto out;
    }
    /* Let Intuition own the screen's backing bitmap. SA_BitMap may legally
     * be replaced when the custom bitmap is not compositable with the chosen
     * monitor; v1 rejected that replacement before testing any pan. Do not
     * patch Intuition or assume pointer identity is a public API guarantee.
     */
    screen = OpenScreenTags(NULL,
        SA_DisplayID, mode, SA_Depth, depth,
        SA_Width, 1984, SA_Height, 1112, SA_Quiet, TRUE,
        SA_ShowTitle, FALSE, SA_AutoScroll, FALSE, SA_Draggable, FALSE,
        TAG_DONE);
    if (!screen) { LOG("FAIL OpenScreenTags"); goto out; }
    bitmap = screen->RastPort.BitMap; /* borrowed until CloseScreen */
    LOG("v2 backing screen=%dx%d bitmap=%lux%lu depth=%lu mode=%08lx",
        screen->Width, screen->Height,
        (unsigned long)GetBitMapAttr(bitmap, BMA_WIDTH),
        (unsigned long)GetBitMapAttr(bitmap, BMA_HEIGHT),
        (unsigned long)GetBitMapAttr(bitmap, BMA_DEPTH),
        (unsigned long)GetVPModeID(&screen->ViewPort));
    if (screen->Width != 1984 || screen->Height != 1112 ||
        GetVPModeID(&screen->ViewPort) != mode ||
        GetBitMapAttr(bitmap, BMA_WIDTH) < 1984 ||
        GetBitMapAttr(bitmap, BMA_HEIGHT) < 1112 ||
        GetBitMapAttr(bitmap, BMA_DEPTH) != depth) {
        LOG("FAIL screen/backing geometry");
        goto out;
    }

    FillPixelArray(&screen->RastPort, 0, 0, 1984, 1112, 0x00202020);
    for (i = 0; i < 4; ++i)
        FillPixelArray(&screen->RastPort, (i & 1) * 64, (i >> 1) * 32,
            64, 32, colors[i]);
    ok = pixels_intact(screen, "before");
    LOG("BEFORE visible origin0,0; red/green above blue/yellow; hold20s");
    Delay(1000);

    screen->ViewPort.DxOffset = -64;
    screen->ViewPort.DyOffset = -32;
    ScrollVPort(&screen->ViewPort);
    Delay(5);
    LOG("PAN requested=-64,-32 accepted=%d,%d; expected visible yellow at origin",
        screen->ViewPort.DxOffset, screen->ViewPort.DyOffset);
    if (screen->ViewPort.DxOffset != -64 || screen->ViewPort.DyOffset != -32)
        ok = FALSE;
    if (!pixels_intact(screen, "panned")) ok = FALSE;
    LOG("PANNED hold20s; capture plus plane readback required for presentation");
    Delay(1000);

    screen->ViewPort.DxOffset = 0;
    screen->ViewPort.DyOffset = 0;
    ScrollVPort(&screen->ViewPort);
    Delay(5);
    if (screen->ViewPort.DxOffset || screen->ViewPort.DyOffset) ok = FALSE;
    if (!pixels_intact(screen, "restored")) ok = FALSE;
    result = ok ? RETURN_OK : RETURN_ERROR;
out:
    ScreenToFront(prior);
    if (screen && !CloseScreen(screen)) {
        LOG("FAIL CloseScreen: screen retains its own backing bitmap");
        result = RETURN_FAIL;
    }
    UnlockPubScreen(NULL, prior);
    LOG("END memory/coordinate result=%ld; physical fetch still needs separate evidence",
        (long)result);
    return result;
}
