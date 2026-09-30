/* Native cursor/SDMA CopyBox discriminator. Install, reboot, then run. Uses
 * only public graphics/intuition/input APIs; never initializes the driver.
 * Pixel results require matching cursor-guard-v1 completion logs and HDMI
 * evidence of a visible pointer to establish accelerated cursor coverage.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>

#define LOG(...) do { bug("[AmdgpuCopyProbe] " __VA_ARGS__); bug("\n"); } while (0)
#define W 512
#define H 256
#define SX 64
#define SY 128
#define DX 768
#define DY 128
#define GREEN 0x0044cc66
#define BLUE  0x004488ee

static BOOL move_pointer(struct IOStdReq *io, struct Screen *screen, WORD x, WORD y)
{
    struct InputEvent event = {0};
    event.ie_Class = IECLASS_POINTERPOS;
    event.ie_X = x;
    event.ie_Y = y;
    io->io_Command = IND_WRITEEVENT;
    io->io_Data = &event;
    io->io_Length = sizeof(event);
    if (DoIO((struct IORequest *)io)) return FALSE;
    Delay(10);
    LOG("pointer requested=%d,%d actual=%d,%d", x, y,
        screen->MouseX, screen->MouseY);
    return screen->MouseX == x && screen->MouseY == y;
}

static BOOL verify(struct RastPort *rp, UBYTE *pixels, WORD x, const char *stage)
{
    ULONG n, bad = 0, first = 0, actual = 0;
    ULONG count = ReadPixelArray(pixels, 0, 0, W * 4, rp, x, SY,
        W, H, RECTFMT_ARGB);
    for (n = 0; n < W * H; ++n) {
        UBYTE *p = pixels + n * 4;
        ULONG rgb = ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
        if (rgb != GREEN) {
            if (!bad) { first = n; actual = rgb; }
            ++bad;
        }
    }
    LOG("%s %s pixels=%lu bad=%lu first=%lu actual=%08lx want=%08lx",
        stage, count == W * H && !bad ? "PASS" : "FAIL",
        (unsigned long)count, (unsigned long)bad, (unsigned long)first,
        (unsigned long)actual, (unsigned long)GREEN);
    return count == W * H && !bad;
}

int main(void)
{
    struct Screen *prior = LockPubScreen(NULL), *screen = NULL;
    struct Window *window = NULL;
    struct MsgPort *port = NULL;
    struct IOStdReq *io = NULL;
    UBYTE *pixels = NULL;
    UWORD pointer[36] = {0}; /* old-style 16x16 two-plane sprite, pen 3 */
    BOOL input_open = FALSE, ok = TRUE;
    LONG result = RETURN_FAIL;
    unsigned row;
    if (!prior) return result;
    LOG("BEGIN v1 prior mode=%08lx size=%dx%d", (unsigned long)
        GetVPModeID(&prior->ViewPort), prior->Width, prior->Height);
    if (GetVPModeID(&prior->ViewPort) != 0x00110000 ||
        prior->Width != 1920 || prior->Height != 1080) goto out;
    pixels = AllocVec(W * H * 4, MEMF_ANY | MEMF_CLEAR);
    port = CreateMsgPort();
    if (!pixels || !port) goto out;
    io = (struct IOStdReq *)CreateIORequest(port, sizeof(*io));
    if (!io || OpenDevice("input.device", 0, (struct IORequest *)io, 0)) goto out;
    input_open = TRUE;
    screen = OpenScreenTags(NULL, SA_DisplayID, 0x00110000,
        SA_Width, 1920, SA_Height, 1080, SA_Depth, 24, SA_Quiet, TRUE,
        SA_ShowTitle, FALSE, SA_AutoScroll, FALSE, SA_Draggable, FALSE, TAG_DONE);
    if (!screen) goto out;
    window = OpenWindowTags(NULL, WA_CustomScreen, screen,
        WA_Left, 0, WA_Top, 0, WA_Width, 1920, WA_Height, 1080,
        WA_Borderless, TRUE, WA_Backdrop, TRUE, WA_Activate, TRUE,
        WA_RMBTrap, TRUE, TAG_DONE);
    if (!window) goto out;
    for (row = 0; row < 16; ++row)
        pointer[2 + row * 2] = pointer[3 + row * 2] = 0xffff;
    SetPointer(window, pointer, 16, 16, 0, 0);
    FillPixelArray(window->RPort, 0, 0, 1920, 1080, 0x00202020);
    FillPixelArray(window->RPort, SX, SY, W, H, GREEN);
    FillPixelArray(window->RPort, DX, DY, W, H, BLUE);
    if (!verify(window->RPort, pixels, SX, "source-before")) ok = FALSE;

    if (!move_pointer(io, screen, SX + 32, SY + 32)) ok = FALSE;
    LOG("SOURCE pointer hold10s; green source left, blue destination right");
    Delay(500);
    ClipBlit(window->RPort, SX, SY, window->RPort, DX, DY, W, H, 0xc0);
    if (!move_pointer(io, screen, 1600, 800)) ok = FALSE;
    if (!verify(window->RPort, pixels, DX, "source-cursor-copy")) ok = FALSE;
    if (!verify(window->RPort, pixels, SX, "source-preserved")) ok = FALSE;

    FillPixelArray(window->RPort, DX, DY, W, H, BLUE);
    if (!move_pointer(io, screen, DX + 32, DY + 32)) ok = FALSE;
    LOG("DESTINATION pointer hold10s; copy must replace save-under too");
    Delay(500);
    ClipBlit(window->RPort, SX, SY, window->RPort, DX, DY, W, H, 0xc0);
    if (!move_pointer(io, screen, 1600, 800)) ok = FALSE;
    if (!verify(window->RPort, pixels, DX, "destination-cursor-restore")) ok = FALSE;
    LOG("AFTER hold10s; both rectangles must be solid green, no pointer stamp");
    Delay(500);
    result = ok ? RETURN_OK : RETURN_ERROR;
out:
    ScreenToFront(prior);
    if (window) { ClearPointer(window); CloseWindow(window); }
    if (screen && !CloseScreen(screen)) result = RETURN_FAIL;
    if (input_open) CloseDevice((struct IORequest *)io);
    if (io) DeleteIORequest((struct IORequest *)io);
    if (port) DeleteMsgPort(port);
    if (pixels) FreeVec(pixels);
    UnlockPubScreen(NULL, prior);
    LOG("END result=%ld; require matching SDMA guard completion and visible pointer",
        (long)result);
    return result;
}
