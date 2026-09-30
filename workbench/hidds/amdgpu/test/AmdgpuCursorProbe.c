/* Native public-API pointer acceptance. Install and reboot before running.
 * No driver initialization/register access is linked into this executable.
 * Coordinate/memory success is NOT cursor visibility: pair every HOLD with HDMI.
 */
#define DEBUG 1
#include <aros/debug.h>
#include <exec/types.h>
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

#define LOG(...) do { bug("[AmdgpuCursorProbe] " __VA_ARGS__); bug("\n"); } while (0)

static BOOL hold(struct IOStdReq *io, struct Screen *screen, const char *stage,
                 WORD x, WORD y)
{
    struct InputEvent event = {0};
    event.ie_Class = IECLASS_POINTERPOS;
    event.ie_X = x; event.ie_Y = y;
    io->io_Command = IND_WRITEEVENT;
    io->io_Data = &event; io->io_Length = sizeof(event);
    if (DoIO((struct IORequest *)io)) return FALSE;
    Delay(10);
    LOG("HOLD %s requested=%d,%d actual=%d,%d seconds=10; HDMI required",
        stage, x, y, screen->MouseX, screen->MouseY);
    Delay(500);
    return screen->MouseX == x && screen->MouseY == y;
}

int main(void)
{
    struct Screen *prior = LockPubScreen(NULL), *screen = NULL;
    struct Window *window = NULL;
    struct MsgPort *port = NULL;
    struct IOStdReq *io = NULL;
    struct DisplayInfo info = {0};
    UWORD pointer[36] = {0};
    UBYTE pixel[4];
    BOOL input_open = FALSE, ok = TRUE;
    LONG result = RETURN_FAIL;
    unsigned row;
    if (!prior) return result;
    LOG("BEGIN v1 prior mode=%08lx size=%dx%d", (unsigned long)
        GetVPModeID(&prior->ViewPort), prior->Width, prior->Height);
    if (GetVPModeID(&prior->ViewPort) != 0x00110000 ||
        prior->Width != 1920 || prior->Height != 1080) goto out;
    if (GetDisplayInfoData(NULL, (UBYTE *)&info, sizeof(info), DTAG_DISP, 0x00110000))
        LOG("advertised_hw_sprite=%lu; advertisement is NOT on-screen proof",
            (unsigned long)((info.PropertyFlags & DIPF_IS_SPRITES) != 0));
    port = CreateMsgPort();
    if (port) io = (struct IOStdReq *)CreateIORequest(port, sizeof(*io));
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
    FillPixelArray(window->RPort, 0, 0, 1920, 1080, 0x00383838);
    FillPixelArray(window->RPort, 384, 256, 256, 192, 0x0044cc66);
    if (!hold(io, screen, "DEFAULT", 480, 320)) ok = FALSE;

    /* Asymmetric L outline with transparent interior. Hotspot4,4 must clip
     * exactly four top/left rows at0,0, never slide the whole image inwards. */
    for (row = 0; row < 16; ++row) {
        UWORD bits = row < 2 ? 0xff00 : row < 12 ? 0xc000 : 0xffff;
        pointer[2 + row * 2] = pointer[3 + row * 2] = bits;
    }
    SetPointer(window, pointer, 16, 16, -4, -4);
    if (!hold(io, screen, "CUSTOM", 480, 320)) ok = FALSE;
    if (!hold(io, screen, "TOPLEFT", 0, 0)) ok = FALSE;
    if (!hold(io, screen, "BOTTOMRIGHT", 1919, 1079)) ok = FALSE;
    SetWindowPointer(window, WA_BusyPointer, TRUE, TAG_DONE);
    if (!hold(io, screen, "BUSY", 480, 320)) ok = FALSE;
    ClearPointer(window);
    ScreenToFront(prior);
    if (!hold(io, prior, "PRIOR-SCREEN", 480, 320)) ok = FALSE;
    ScreenToFront(screen);
    if (!hold(io, screen, "RETURNED-DEFAULT", 480, 320)) ok = FALSE;
    if (ReadPixelArray(pixel, 0, 0, 4, window->RPort, 64, 64, 1, 1,
                       RECTFMT_ARGB) != 1 ||
        pixel[1] != 0x38 || pixel[2] != 0x38 || pixel[3] != 0x38) ok = FALSE;
    LOG("background sentinel %s", ok ? "PASS" : "FAIL (includes earlier API checks)");
    result = ok ? RETURN_OK : RETURN_ERROR;
out:
    ScreenToFront(prior);
    if (window) { ClearPointer(window); CloseWindow(window); }
    if (screen && !CloseScreen(screen)) result = RETURN_FAIL;
    if (input_open) CloseDevice((struct IORequest *)io);
    if (io) DeleteIORequest((struct IORequest *)io);
    if (port) DeleteMsgPort(port);
    UnlockPubScreen(NULL, prior);
    LOG("END API/coordinate result=%ld; seven matching HDMI stages required", (long)result);
    return result;
}
