/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Open consoleng.device on a window and render into it, so that "the
          device works" is something you can look at rather than a build log.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

#include <exec/io.h>
#include <exec/memory.h>
#include <devices/conunit.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>

#include <stdio.h>
#include <string.h>

/* Written straight into the unit, so each line is exactly what the device's
 * own parser sees. Kept deliberately mixed: plain text, an SGR colour run,
 * a cursor-position CSI, and a UTF-8 run above Latin-1 - the last one is the
 * interesting case, because that is the path the device's local TextUTF8
 * stand-in handles. */
static const char *lines[] =
{
    "consoleng.device -- second console, stock console untouched\r\n",
    "\r\n",
    "plain ASCII: The quick brown fox jumps over the lazy dog 0123456789\r\n",
    "\x1b[1mbold\x1b[0m  \x1b[31mred\x1b[0m  \x1b[32mgreen\x1b[0m  "
        "\x1b[34mblue\x1b[0m  \x1b[7mreverse\x1b[0m\r\n",
    "\r\n",
    "Latin-1 still goes through Text(): \xc3\xa4 \xc3\xb6 \xc3\xbc \xc3\x9f\r\n",
    "\r\n",

    /* THE TEST, part 1 - codepoints above U+00FF that Vera Mono DOES have.
     *
     * The face's cmap was parsed directly (it maps 282 codepoints, 91 of them
     * above U+00FF), so this list is evidence-based rather than assumed. Every
     * character on this line must appear as a real glyph. Anything that comes
     * out as a bare letter here is a reason code, and the glyph path is
     * broken - see CONSOLENG_GLYPH_* in console_gcc.h.
     *
     * This replaces an earlier line that used Greek alpha/beta/gamma/delta and
     * Cyrillic A/Be/Ve/Ge. Those all rendered as 'F' (OTERR_UnknownGlyph) and
     * that looked like a failure of this device - but Bitstream Vera is a
     * Latin-only face and simply does not contain them. The engine was right
     * and the TEST was wrong. Note U+03C0 and U+03A9 below: Vera has exactly
     * two Greek letters, and picking the other four was pure bad luck. */
    "IN FACE (must render as glyphs):\r\n",
    "  \xcf\x80 \xce\xa9 \xe2\x82\xac \xe2\x84\xa2 \xe2\x88\x9e \xe2\x88\x91 "
        "\xe2\x88\x9a \xe2\x89\xa4 \xe2\x89\xa5 \xe2\x89\xa0 \xe2\x80\xa2 "
        "\xe2\x80\xa6 \xc5\x82 \xef\xac\x81\r\n",
    "  (pi omega euro tm infinity sigma root le ge ne bullet "
        "ellipsis l-stroke fi)\r\n",
    "\r\n",

    /* THE TEST, part 2 - the control. These are genuinely absent from this
     * face, so they MUST come back as 'F' (CONSOLENG_GLYPH_OBT_NOCP,
     * ObtainInfoA returning OTERR_UnknownGlyph). An 'F' here is the device
     * reporting honestly, not failing; an 'E' would mean the face never
     * opened, which is a different bug entirely. Keeping both halves on one
     * screen makes the screenshot self-checking. */
    "NOT IN FACE (must show 'F' = no such glyph):\r\n",
    "  \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90 \xe2\x86\x90\xe2\x86\x92 "
        "\xe2\xa0\x8b\xe2\xa0\x99 \xd0\x90\xd0\x91 \xce\xb1\xce\xb2\r\n",
    "  (box drawing, arrows, braille, Cyrillic, Greek alpha/beta)\r\n",
    NULL
};

int main(void)
{
    struct MsgPort   *port  = NULL;
    struct IOStdReq  *ioreq = NULL;
    struct Window    *win   = NULL;
    BOOL              opened = FALSE;
    int               i;

    win = OpenWindowTags(NULL,
        WA_Left,        20,
        WA_Top,         20,
        WA_Width,       600,
        WA_Height,      360,
        WA_Title,       (IPTR)"consoleng.device test",
        WA_CloseGadget, TRUE,
        WA_DragBar,     TRUE,
        WA_DepthGadget, TRUE,
        WA_Activate,    TRUE,
        WA_IDCMP,       IDCMP_CLOSEWINDOW,
        TAG_DONE);

    if (!win)
    {
        printf("CONSOLENG-TEST: FAIL could not open a window\n");
        return RETURN_FAIL;
    }

    port = CreateMsgPort();
    if (port)
        ioreq = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));

    if (!ioreq)
    {
        printf("CONSOLENG-TEST: FAIL no IO request\n");
        goto cleanup;
    }

    /* The whole point: open the NEW device by name. Nothing here touches
     * console.device, and this program is the only thing selecting consoleng
     * until a handler does it. */
    ioreq->io_Data = (APTR)win;
    ioreq->io_Length = sizeof(struct Window);

    if (OpenDevice("consoleng.device", CONU_CHARMAP,
                   (struct IORequest *)ioreq, 0) != 0)
    {
        printf("CONSOLENG-TEST: FAIL OpenDevice(consoleng.device) rc!=0\n");
        goto cleanup;
    }
    opened = TRUE;

    printf("CONSOLENG-TEST: opened consoleng.device unit=%p\n",
           (void *)ioreq->io_Unit);

    /* Ask the unit to interpret the stream as UTF-8. The sequence is the
     * device's own private CSI >8h (support.c: str_su8 = 0x3E 0x38 0x68) -
     * NOT >1h, which is what the first version of this test guessed and which
     * left the unit in byte mode, rendering every UTF-8 sequence as two or
     * three Latin-1 characters of mojibake. */
    {
        static const char utf8on[] = "\x1b[>8h";
        ioreq->io_Command = CMD_WRITE;
        ioreq->io_Data    = (APTR)utf8on;
        ioreq->io_Length  = sizeof(utf8on) - 1;
        DoIO((struct IORequest *)ioreq);
    }

    /* Switch to an OUTLINE font via OSC 50. This is not decoration: the
     * default console font is a bitmap font with no .otag, so it has no
     * outline engine and NOTHING above Latin-1 can be rasterised from it -
     * the glyph path correctly falls back to a placeholder. Exercising the
     * engine at all requires a font that has one. "Vera Mono" ships with this
     * tree (Fonts/Vera Mono.otag) and is monospaced, which a terminal wants. */
    {
        static const char setfont[] = "\x1b]50;Vera Mono,16\x07";
        ioreq->io_Command = CMD_WRITE;
        ioreq->io_Data    = (APTR)setfont;
        ioreq->io_Length  = sizeof(setfont) - 1;
        DoIO((struct IORequest *)ioreq);
        printf("CONSOLENG-TEST: requested outline font via OSC 50\n");
    }

    for (i = 0; lines[i]; i++)
    {
        ioreq->io_Command = CMD_WRITE;
        ioreq->io_Data    = (APTR)lines[i];
        ioreq->io_Length  = strlen(lines[i]);
        if (DoIO((struct IORequest *)ioreq) != 0)
            printf("CONSOLENG-TEST: write %d returned error\n", i);
    }

    printf("CONSOLENG-TEST: PASS rendered %d lines into consoleng.device\n", i);

    /* Hold the window so a screenshot can be taken, but do not hang a test
     * harness forever: give up after a bounded wait if nobody closes it. */
    {
        ULONG sigs;
        ULONG waited = 0;

        while (waited < 120)
        {
            sigs = CheckSignal(1L << port->mp_SigBit);
            (void)sigs;
            if (GetMsg(win->UserPort))
                break;
            Delay(50);              /* 1s */
            waited++;
        }
    }

cleanup:
    if (opened)
        CloseDevice((struct IORequest *)ioreq);
    if (ioreq)
        DeleteIORequest((struct IORequest *)ioreq);
    if (port)
        DeleteMsgPort(port);
    if (win)
        CloseWindow(win);

    return RETURN_OK;
}
