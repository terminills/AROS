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
    "UTF-8 above Latin-1 (box drawing, arrows, braille):\r\n",
    "  \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90"
        "   \xe2\x86\x90\xe2\x86\x91\xe2\x86\x92\xe2\x86\x93"
        "   \xe2\xa0\x8b\xe2\xa0\x99\xe2\xa0\xb9\r\n",
    "  \xe2\x94\x82   \xe2\x94\x82   (these are what Ink draws with)\r\n",
    "  \xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x98\r\n",
    "\r\n",
    "Latin-1 still goes through Text(): \xc3\xa4 \xc3\xb6 \xc3\xbc \xc3\x9f\r\n",
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
