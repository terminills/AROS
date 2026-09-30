/* Native window-exposure discriminator, JOB-0084. Public APIs only: no driver
 * initialization, MMIO, renderer replacement, or repair of a failed result.
 * Run on the installed/rebooted AMDGPU-owned screen. Compare visible screen
 * storage before opening a window and after closing it; arbitrary backing
 * pixels are saved only as an oracle, never written back to conceal a failure.
 * HDMI must independently show the same stages. This tests one quiet region,
 * not every layer/refresh contract and not the cause of a failed restoration.
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

#define LOG(...) do { bug("[AmdgpuExposeProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define WIDTH 512
#define HEIGHT 256
#define LEFT 600
#define TOP 100
#define INNER_X 8
#define INNER_Y 60
#define COLOR 0x0044cc88UL
#define BYTES (WIDTH * HEIGHT * 4)

static ULONG read_screen(struct Screen *screen, UBYTE *pixels)
{
    return ReadPixelArray(pixels, 0, 0, WIDTH * 4, &screen->RastPort,
        LEFT + INNER_X, TOP + INNER_Y, WIDTH, HEIGHT, RECTFMT_ARGB);
}

static ULONG rgb(const UBYTE *p)
{
    return ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static BOOL compare(const UBYTE *before, const UBYTE *after, const char *stage)
{
    ULONG n, bad = 0, first = 0xffffffffUL, want = 0, got = 0;
    ULONG before_hash = 2166136261UL, after_hash = 2166136261UL;
    for (n = 0; n < WIDTH * HEIGHT; ++n) {
        ULONG a = rgb(before + n * 4), b = rgb(after + n * 4);
        before_hash = (before_hash ^ a) * 16777619UL;
        after_hash = (after_hash ^ b) * 16777619UL;
        if (a != b) {
            if (!bad) { first = n; want = a; got = b; }
            ++bad;
        }
    }
    LOG("%s pixels=%lu bad=%lu first=%lu got=%08lx want=%08lx hashes=%08lx/%08lx",
        stage, (unsigned long)(WIDTH * HEIGHT), (unsigned long)bad,
        (unsigned long)first, (unsigned long)got, (unsigned long)want,
        (unsigned long)after_hash, (unsigned long)before_hash);
    return !bad;
}

int main(void)
{
    struct Screen *screen = LockPubScreen(NULL);
    struct Window *window = NULL;
    UBYTE *before = NULL, *after = NULL;
    ULONG n, bad = 0;
    BOOL early_ok, late_ok;
    LONG result = RETURN_FAIL;
    if (!screen) return result;
    LOG("BEGIN v1 mode=%08lx size=%dx%d depth=%lu",
        (unsigned long)GetVPModeID(&screen->ViewPort), screen->Width,
        screen->Height, (unsigned long)GetBitMapAttr(screen->RastPort.BitMap, BMA_DEPTH));
    if (GetVPModeID(&screen->ViewPort) != 0x00110000 ||
        screen->Width != 1920 || screen->Height != 1080 ||
        GetBitMapAttr(screen->RastPort.BitMap, BMA_DEPTH) != 24) goto out;
    before = AllocVec(BYTES, MEMF_ANY);
    after = AllocVec(BYTES, MEMF_ANY);
    if (!before || !after || read_screen(screen, before) != WIDTH * HEIGHT) goto out;
    LOG("BEFORE region=%d,%d %dx%d hold10s; saved oracle will not be repainted",
        LEFT + INNER_X, TOP + INNER_Y, WIDTH, HEIGHT);
    Delay(500);
    window = OpenWindowTags(NULL, WA_CustomScreen, screen,
        WA_Left, LEFT, WA_Top, TOP, WA_Width, 640, WA_Height, 480,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE,
        WA_Title, (IPTR)"Native AMDGPU exposure check - automatic close", TAG_DONE);
    if (!window || window->LeftEdge != LEFT || window->TopEdge != TOP) goto out;
    FillPixelArray(window->RPort, INNER_X, INNER_Y, WIDTH, HEIGHT, COLOR);
    Delay(50);
    if (read_screen(screen, after) != WIDTH * HEIGHT) goto out;
    for (n = 0; n < WIDTH * HEIGHT; ++n)
        if (rgb(after + n * 4) != COLOR) ++bad;
    LOG("COVER pixels=%lu bad=%lu want=%08lx hold10s",
        (unsigned long)(WIDTH * HEIGHT), (unsigned long)bad, (unsigned long)COLOR);
    if (bad) goto out;
    Delay(500);
    CloseWindow(window);
    window = NULL;
    Delay(50);
    if (read_screen(screen, after) != WIDTH * HEIGHT) goto out;
    early_ok = compare(before, after, "RESTORE-1s");
    LOG("AFTER hold10s; no repair writes");
    Delay(500);
    if (read_screen(screen, after) != WIDTH * HEIGHT) goto out;
    late_ok = compare(before, after, "RESTORE-11s");
    /* A delayed restoration is not silently promoted to an immediate pass. */
    if (early_ok && late_ok) result = RETURN_OK;
out:
    if (window) CloseWindow(window);
    if (after) FreeVec(after);
    if (before) FreeVec(before);
    UnlockPubScreen(NULL, screen);
    LOG("END result=%ld; software pixel oracle, require matching HDMI", (long)result);
    return result;
}
