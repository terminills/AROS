/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: consoleng.device's local stand-in for graphics.library/TextUTF8().
*/

#include <proto/graphics.h>
#include <graphics/rastport.h>
#include <graphics/text.h>

#include "console_gcc.h"

/*
 * WHY THIS EXISTS, AND WHAT IT DELIBERATELY DOES NOT DO.
 *
 * The OPAL tree renders codepoints above U+00FF through a new
 * graphics.library LVO, TextUTF8(), which in turn needs a UTF-8 glyph-map API
 * in diskfont.library (UTF8GlyphAdvance / UTF8RenderCP / ColorGlyphMap).
 * Neither exists in this tree, and adding them means changing two ROM
 * libraries that the whole system sits on. terminills' call: keep it out of
 * graphics.library - if it belongs there, that is an upstream merge decision,
 * not a side effect of porting a console.
 *
 * So this device carries its own stand-in instead, and the cost is bounded
 * because of how the callers are written: stdconclass.c and charmapconclass.c
 * already SPLIT each same-pen run, sending ASCII/Latin-1 cells through the
 * ordinary Text() path as bytes and calling here only for codepoints ABOVE
 * U+00FF. Everything a terminal needs structurally - escape-sequence
 * reassembly across writes, CSI semantics through UTF-8 input, deferred
 * autowrap, the redraw controls, UCS-4 charmap cells - is in the device and is
 * unaffected by this file.
 *
 * What IS lost: box-drawing, arrows, Braille spinners and emoji draw as a
 * placeholder rather than their real glyph. A TUI stays laid out and usable;
 * it does not look right. That is a visible, honest degradation and not a
 * silent one - do not describe this device as matching OPAL's rendering until
 * the glyph path exists.
 *
 * The callers have already filled the cell background and advance the cursor
 * by CELLS, so the only job here is to put something of the right width where
 * the glyph would have gone. One placeholder per codepoint keeps the column
 * arithmetic honest; drawing the raw UTF-8 bytes through Text() would emit two
 * to four mojibake characters per cell and shear the whole line.
 */

/* Decode one UTF-8 sequence, advancing *cursor. Malformed input is tolerated
   the same way the device's own decoder does: a stray continuation byte
   consumes one cell rather than desynchronising the whole run. */
static ULONG ConsoleNGUTF8Next(const UBYTE **cursor, const UBYTE *end)
{
    const UBYTE *p = *cursor;
    UBYTE        b = *p;
    ULONG        cp;

    if (b < 0x80)
    {
        *cursor = p + 1;
        return b;
    }
    if (b < 0xC0)
    {
        *cursor = p + 1;         /* unexpected continuation byte */
        return '?';
    }
    if (b < 0xE0)
    {
        if (p + 2 > end) { *cursor = end; return '?'; }
        cp = ((ULONG)(b & 0x1F) << 6) | (p[1] & 0x3F);
        *cursor = p + 2;
        return cp;
    }
    if (b < 0xF0)
    {
        if (p + 3 > end) { *cursor = end; return '?'; }
        cp = ((ULONG)(b & 0x0F) << 12) | ((ULONG)(p[1] & 0x3F) << 6) |
             (p[2] & 0x3F);
        *cursor = p + 3;
        return cp;
    }
    if (p + 4 > end) { *cursor = end; return '?'; }
    cp = ((ULONG)(b & 0x07) << 18) | ((ULONG)(p[1] & 0x3F) << 12) |
         ((ULONG)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    *cursor = p + 4;
    return cp;
}

void ConsoleNGTextUTF8(struct Library *GfxBase, struct RastPort *rp,
                       CONST_STRPTR string, ULONG byteLen)
{
    const UBYTE *p   = (const UBYTE *)string;
    const UBYTE *end = p + byteLen;

    if (!rp || !string || !byteLen)
        return;

    while (p < end)
    {
        const UBYTE *prev = p;
        ULONG        cp   = ConsoleNGUTF8Next(&p, end);

        if (cp < 0x100)
        {
            /* Latin-1 and below: draw it properly as a single byte. This is
               the SAME rule charmapconclass.c:1113 uses on the refresh path
               ("if (str[idx] < 0x100) ... Text()"), and matching it here
               matters for two reasons.

               First, it is the factory ceiling. diskfont pre-renders glyphs
               0-255 only (bullet.c: {OT_GlyphCode, (i < 256) ? i : 0x25A1}),
               so Latin-1 is exactly what the stock console can draw and
               exactly what this device should still draw without a live
               glyph engine. Throwing it away would make this device WORSE
               than the one it sits beside.

               Second, the write path disagrees with the refresh path:
               stdconclass.c:427 sends everything >= 0x80 here, while the
               refresh keeps < 0x100 on Text(). That asymmetry is harmless in
               OPAL, whose TextUTF8 renders Latin-1 through the engine, but
               with a placeholder it made a freshly-typed 'a-umlaut' show as
               '?' and then correct itself on the next redraw. Handling it
               here removes the inconsistency. */
            UBYTE byte = (UBYTE)cp;

            Text(rp, (CONST_STRPTR)&byte, 1);
        }
        else
        {
            /* Above Latin-1 there is genuinely no glyph to be had without a
               live GlyphEngine on the font, which diskfont closes at the end
               of OTAG_ReadOutlineFont (bullet.c:1048). Placeholder keeps the
               column arithmetic honest until that changes. */
            Text(rp, (CONST_STRPTR)"?", 1);
        }

        if (p <= prev)
            break;               /* never loop on a zero-width step */
    }
}
