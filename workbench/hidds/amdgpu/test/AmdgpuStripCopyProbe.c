/* Native nonuniform 1920-pixel-stride copy oracle, JOB-0084.
 * Distinguish cold37's missing lower composite marker from bitmap notification:
 * this uses only two owned, undragged native screens and public ClipBlit.
 * Never initializes/replaces the driver or explicitly repairs the display.
 * The already installed/rebooted driver remains under test. Pair HOLD with HDMI.
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

#define LOG(...) do { bug("[AmdgpuStripCopyProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define WIDTH 1920
#define HEIGHT 192
#define ROWS 32
#define BLUE 0x002244ccUL
#define MAGENTA 0x00dd33bbUL
#define AMBER 0x00eebb33UL
#define GRAY 0x00202020UL

static struct Screen *make_screen(ULONG color)
{
    struct Screen *s = OpenScreenTags(NULL, SA_DisplayID, 0x00110000,
        SA_Width, WIDTH, SA_Height, 1080, SA_Depth, 24, SA_Quiet, TRUE,
        SA_ShowTitle, FALSE, SA_AutoScroll, FALSE, SA_Draggable, FALSE, TAG_DONE);
    if (s) FillPixelArray(&s->RastPort, 0, 0, WIDTH, 1080, color);
    return s;
}

static BOOL verify(struct Screen *s, UBYTE *pixels, WORD origin,
                    ULONG marker, const char *name)
{
    ULONG row, y, x, bad = 0, first = (ULONG)~(ULONG)0, actual = 0, count = 0;
    for (row = 0; row < HEIGHT; row += ROWS) {
        ULONG got = ReadPixelArray(pixels, 0, 0, WIDTH * 4, &s->RastPort,
            0, origin + row, WIDTH, ROWS, RECTFMT_ARGB);
        if (got != WIDTH * ROWS) {
            LOG("FAIL %s read row=%lu got=%lu", name,
                (unsigned long)row, (unsigned long)got);
            return FALSE;
        }
        count += got;
        for (y = 0; y < ROWS; ++y)
            for (x = 0; x < WIDTH; ++x) {
                UBYTE *p = pixels + (y * WIDTH + x) * 4;
                ULONG rgb = ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
                ULONG want = x >= 256 && x < 384 && row + y >= 16 &&
                             row + y < 48 ? marker : BLUE;
                if (rgb != want) {
                    if (!bad) { first = (row + y) * WIDTH + x; actual = rgb; }
                    ++bad;
                }
            }
    }
    LOG("%s pixels=%lu bad=%lu first=%lu actual=%08lx marker=%08lx",
        name, (unsigned long)count, (unsigned long)bad,
        (unsigned long)first, (unsigned long)actual, (unsigned long)marker);
    return bad == 0;
}

int main(void)
{
    struct Screen *prior = LockPubScreen(NULL), *source = NULL, *dest = NULL;
    UBYTE *pixels = NULL;
    BOOL ok = TRUE;
    LONG result = RETURN_FAIL;
    if (!prior) return result;
    LOG("BEGIN v1 mode=%08lx size=%dx%d depth=%lu",
        (unsigned long)GetVPModeID(&prior->ViewPort), prior->Width, prior->Height,
        (unsigned long)GetBitMapAttr(prior->RastPort.BitMap, BMA_DEPTH));
    if (GetVPModeID(&prior->ViewPort) != 0x00110000 ||
        prior->Width != WIDTH || prior->Height != 1080 ||
        GetBitMapAttr(prior->RastPort.BitMap, BMA_DEPTH) != 24) goto out;
    pixels = AllocVec(WIDTH * ROWS * 4, MEMF_ANY);
    if (!pixels) goto out;
    source = make_screen(BLUE);
    if (!source) goto out;
    dest = make_screen(GRAY);
    if (!dest) goto out;
    FillPixelArray(&source->RastPort, 256, 16, 128, 32, MAGENTA);
    if (!verify(source, pixels, 0, MAGENTA, "SOURCE-FIRST")) ok = FALSE;
    ClipBlit(&source->RastPort, 0, 0, &dest->RastPort, 0, 0,
             WIDTH, HEIGHT, 0xc0);
    if (!verify(dest, pixels, 0, MAGENTA, "DEST-FIRST")) ok = FALSE;
    LOG("HOLD FIRST top192 blue/magenta; rest gray; HDMI required");
    Delay(500);

    FillPixelArray(&source->RastPort, 256, 16, 128, 32, AMBER);
    if (!verify(source, pixels, 0, AMBER, "SOURCE-UPDATED")) ok = FALSE;
    ClipBlit(&source->RastPort, 0, 0, &dest->RastPort, 0, 256,
             WIDTH, HEIGHT, 0xc0);
    if (!verify(dest, pixels, 256, AMBER, "DEST-UPDATED")) ok = FALSE;
    if (!verify(dest, pixels, 0, MAGENTA, "DEST-FIRST-PRESERVED")) ok = FALSE;
    LOG("HOLD UPDATED top192 blue/magenta; y256..447 blue/amber; HDMI required");
    Delay(500);

    ScreenToFront(source);
    Delay(10);
    ScreenToFront(dest);
    Delay(10);
    if (!verify(dest, pixels, 0, MAGENTA, "RETURNED-FIRST")) ok = FALSE;
    if (!verify(dest, pixels, 256, AMBER, "RETURNED-UPDATED")) ok = FALSE;
    LOG("HOLD RETURNED same two marked blue strips; HDMI required");
    Delay(500);
    result = ok ? RETURN_OK : RETURN_ERROR;
out:
    ScreenToFront(prior);
    if (dest && !CloseScreen(dest)) result = RETURN_FAIL;
    if (source && !CloseScreen(source)) result = RETURN_FAIL;
    if (pixels) FreeVec(pixels);
    UnlockPubScreen(NULL, prior);
    LOG("END memory result=%ld; three HDMI stages required", (long)result);
    return result;
}
