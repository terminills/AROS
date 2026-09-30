/* Gfx2DProbe - visible 2D/Intuition conformance surface for amdgpu. */
#define DEBUG 1
#include <aros/debug.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <string.h>

static void label(struct RastPort *rp, WORD x, WORD y, CONST_STRPTR s)
{
    Move(rp, x, y);
    Text(rp, (STRPTR)s, (ULONG)strlen(s));
}

static void box(struct RastPort *rp, WORD x, WORD y, WORD w, WORD h, UBYTE pen)
{
    SetAPen(rp, pen);
    RectFill(rp, x, y, x + w - 1, y + h - 1);
    SetAPen(rp, 1);
    Move(rp, x, y);
    Draw(rp, x + w - 1, y);
    Draw(rp, x + w - 1, y + h - 1);
    Draw(rp, x, y + h - 1);
    Draw(rp, x, y);
}

static void pixel_probe(struct RastPort *rp, WORD x, WORD y, UBYTE pen,
                        CONST_STRPTR name)
{
    ULONG got;

    SetAPen(rp, pen);
    WritePixel(rp, x, y);
    got = ReadPixel(rp, x, y);
    bug("[Gfx2DProbe] pixel %s pen=%lu readback=%lu\n",
        name, (ULONG)pen, got);
}

int main(void)
{
    struct Window *win;
    struct IntuiMessage *imsg;
    struct RastPort *rp;
    BOOL running = TRUE;

    bug("[Gfx2DProbe] opening visible 2D conformance surface\n");
    win = OpenWindowTags(NULL,
        WA_Left, 80, WA_Top, 70, WA_Width, 800, WA_Height, 480,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                  WFLG_ACTIVATE | WFLG_REPORTMOUSE,
        WA_Title, (IPTR)"amdgpu 2D / cursor / pen probe", TAG_DONE);
    if (!win) {
        bug("[Gfx2DProbe] OpenWindow failed\n");
        return 20;
    }

    rp = win->RPort;
    SetAPen(rp, 0);
    RectFill(rp, 0, 0, win->Width - 1, win->Height - 1);
    SetAPen(rp, 1);
    label(rp, 18, 28, "amdgpu 2D conformance probe");
    label(rp, 18, 48, "Close window to finish. Move the mouse across every panel.");

    box(rp, 18, 70, 235, 100, 2);
    SetAPen(rp, 1);
    label(rp, 30, 92, "EXPECTED GREY BACKGROUND");
    label(rp, 30, 112, "RectFill + pen 2");
    label(rp, 30, 132, "Black here = bad clear/pen");

    box(rp, 270, 70, 235, 100, 3);
    SetAPen(rp, 1);
    label(rp, 282, 92, "TITLE/HIGHLIGHT PEN");
    label(rp, 282, 112, "RectFill + pen 3");
    label(rp, 282, 132, "Compare shell title bar");

    box(rp, 522, 70, 235, 100, 4);
    SetAPen(rp, 1);
    label(rp, 534, 92, "SELECTION / MENU PEN");
    label(rp, 534, 112, "RectFill + pen 4");
    label(rp, 534, 132, "Tests highlight repaint");

    SetAPen(rp, 1);
    Move(rp, 18, 205);
    Draw(rp, 757, 205);
    label(rp, 18, 230, "TEXT + LINE TEST: jagged, missing, or wrong-color output identifies Draw/Text path");

    SetAPen(rp, 2);
    RectFill(rp, 18, 250, 360, 180);
    SetAPen(rp, 1);
    label(rp, 34, 276, "MOUSE VISIBILITY TEST");
    label(rp, 34, 298, "Place pointer here; it must remain visible");
    label(rp, 34, 320, "while this window is active");
    Move(rp, 34, 350);
    Draw(rp, 350, 350);
    Move(rp, 34, 370);
    Draw(rp, 350, 410);
    label(rp, 34, 420, "Software pointer and hardware cursor are separate tests");

    SetAPen(rp, 4);
    RectFill(rp, 405, 250, 352, 180);
    SetAPen(rp, 1);
    label(rp, 421, 276, "READBACK / DAMAGE TEST");
    label(rp, 421, 298, "Repeated fills must not leave stale pixels");
    SetAPen(rp, 0);
    RectFill(rp, 421, 320, 720, 400);
    SetAPen(rp, 1);
    label(rp, 437, 350, "BLACK STRIPE");
    SetAPen(rp, 2);
    RectFill(rp, 437, 370, 720, 400);
    SetAPen(rp, 1);
    label(rp, 453, 398, "GREY STRIPE");
    label(rp, 421, 420, "If either stripe is wrong, capture the fill pen and bitmap");

    label(rp, 18, 448, "PIXEL READBACK: WritePixel then ReadPixel for pens 2, 3, 4");
    pixel_probe(rp, 34, 462, 2, "grey");
    pixel_probe(rp, 50, 462, 3, "highlight");
    pixel_probe(rp, 66, 462, 4, "selection");
    label(rp, 90, 466, "single-pixel path (contrasts with accelerated fills)");

    bug("[Gfx2DProbe] window=%p rastport=%p size=%ldx%ld; tests drawn\n",
        win, rp, (long)win->Width, (long)win->Height);
    while (running) {
        WaitPort(win->UserPort);
        while ((imsg = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
            if (imsg->Class == IDCMP_CLOSEWINDOW)
                running = FALSE;
            else if (imsg->Class == IDCMP_MOUSEMOVE)
                bug("[Gfx2DProbe] mouse x=%ld y=%ld\n",
                    (long)imsg->MouseX, (long)imsg->MouseY);
            ReplyMsg((struct Message *)imsg);
        }
    }
    CloseWindow(win);
    bug("[Gfx2DProbe] complete\n");
    return 0;
}
