/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Reaction palette.gadget - BOOPSI class implementation
*/
#define DEBUG 0

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/graphics.h>
#include <proto/alib.h>

#include <graphics/gfxmacros.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/cghooks.h>
#include <intuition/gadgetclass.h>
#include <intuition/imageclass.h>
#include <devices/inputevent.h>
#include <gadgets/palette.h>
#include <utility/tagitem.h>

#include <string.h>

#include "palette_intern.h"
#include "../reaction_box.h"

#define PaletteBase ((struct Library *)(cl->cl_UserData))

/* As many as a GadTools palette shows; a deep screen asks for 1 << 24 */
#define PALETTE_MAXCOLORS   256

static UWORD palette_pen(struct PaletteGadData *data, ULONG i)
{
    return data->pd_ColorTable ? data->pd_ColorTable[i]
                               : (UWORD)(data->pd_ColorOffset + i);
}

/* Grid of cells closest to square that fills w x h */
static void palette_grid(ULONG n, WORD w, WORD h, ULONG *cols, ULONG *rows)
{
    ULONG r, best_r = 1;
    WORD best = -1;

    for (r = 1; r <= n; r++)
    {
        ULONG c = (n + r - 1) / r;
        WORD cw = w / (WORD)c, ch = h / (WORD)r;
        WORD side = cw < ch ? cw : ch;

        if (side > best)
        {
            best = side;
            best_r = r;
        }
    }
    *rows = best_r;
    *cols = (n + best_r - 1) / best_r;
}

/* Inner box of the cells, inside the recessed frame */
static void palette_inner(struct Gadget *gad, struct GadgetInfo *gi,
    WORD *x, WORD *y, WORD *w, WORD *h)
{
    reaction_gadget_box(gad, gi, x, y, w, h);
    *x += 2;
    *y += 1;
    *w -= 4;
    *h -= 2;
}

static LONG palette_cell_at(struct PaletteGadData *data, struct Gadget *gad,
    struct GadgetInfo *gi, WORD mx, WORD my)
{
    WORD x, y, w, h, bx, by, bw, bh;
    ULONG cols, rows;
    LONG col, row, i;

    reaction_gadget_box(gad, gi, &bx, &by, &bw, &bh);
    palette_inner(gad, gi, &x, &y, &w, &h);
    mx += bx - x;
    my += by - y;
    if (mx < 0 || my < 0 || mx >= w || my >= h)
        return -1;
    palette_grid(data->pd_NumColors, w, h, &cols, &rows);
    col = mx / (w / (WORD)cols ? w / (WORD)cols : 1);
    row = my / (h / (WORD)rows ? h / (WORD)rows : 1);
    if (col >= (LONG)cols || row >= (LONG)rows)
        return -1;
    i = row * cols + col;
    return i < (LONG)data->pd_NumColors ? i : -1;
}

/******************************************************************************/

static void palette_set(Class *cl, Object *o, struct opSet *msg)
{
    struct PaletteGadData *data = INST_DATA(cl, o);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *tag;

    while ((tag = NextTagItem(&tags)))
    {
        switch (tag->ti_Tag)
        {
            case PALETTE_Color:
                data->pd_Color = tag->ti_Data;
                break;
            case PALETTE_ColorOffset:
                data->pd_ColorOffset = tag->ti_Data;
                break;
            case PALETTE_NumColors:
                data->pd_NumColors = tag->ti_Data;
                if (data->pd_NumColors < 1)
                    data->pd_NumColors = 1;
                if (data->pd_NumColors > PALETTE_MAXCOLORS)
                    data->pd_NumColors = PALETTE_MAXCOLORS;
                break;
            case PALETTE_ColorTable:
                data->pd_ColorTable = (UWORD *)tag->ti_Data;
                break;
        }
    }
}

/******************************************************************************/

IPTR Palette__OM_NEW(Class *cl, Object *o, struct opSet *msg)
{
    IPTR retval;

    D(bug("[Palette] OM_NEW: entry\n"));
    retval = DoSuperMethodA(cl, o, (Msg)msg);
    if (retval)
    {
        D(bug("[Palette] OM_NEW: obj=%p\n", (Object *)retval));
        struct PaletteGadData *data = INST_DATA(cl, (Object *)retval);

        memset(data, 0, sizeof(struct PaletteGadData));

        /* Set defaults */
        data->pd_NumColors    = 8;
        data->pd_ColorOffset  = 0;
        data->pd_Color        = 0;
        data->pd_Hover        = -1;

        palette_set(cl, (Object *)retval, msg);
    }

    return retval;
}

/******************************************************************************/

IPTR Palette__OM_DISPOSE(Class *cl, Object *o, Msg msg)
{
    D(bug("[Palette] OM_DISPOSE: entry\n"));
    return DoSuperMethodA(cl, o, msg);
}

/******************************************************************************/

IPTR Palette__OM_SET(Class *cl, Object *o, struct opSet *msg)
{
    D(bug("[Palette] OM_SET: entry\n"));
    IPTR retval = DoSuperMethodA(cl, o, (Msg)msg);
    palette_set(cl, o, msg);
    return retval;
}

/******************************************************************************/

IPTR Palette__OM_GET(Class *cl, Object *o, struct opGet *msg)
{
    struct PaletteGadData *data = INST_DATA(cl, o);

    switch (msg->opg_AttrID)
    {
        case PALETTE_Color:
            *msg->opg_Storage = data->pd_Color;
            return TRUE;

        case PALETTE_ColorOffset:
            *msg->opg_Storage = data->pd_ColorOffset;
            return TRUE;

        case PALETTE_NumColors:
            *msg->opg_Storage = data->pd_NumColors;
            return TRUE;

        case PALETTE_ColorTable:
            *msg->opg_Storage = (IPTR)data->pd_ColorTable;
            return TRUE;
    }

    return DoSuperMethodA(cl, o, (Msg)msg);
}

/******************************************************************************/

static void palette_draw(Class *cl, Object *o, struct GadgetInfo *gi,
    struct RastPort *rp)
{
    struct PaletteGadData *data = INST_DATA(cl, o);
    struct DrawInfo *dri = gi ? gi->gi_DrInfo : NULL;
    struct Gadget *gad = G(o);
    UWORD *pens;
    WORD bx, by, bw, bh, x, y, w, h, cellw, cellh;
    ULONG cols, rows, i;

    if (!rp || !dri)
        return;
    pens = dri->dri_Pens;

    reaction_gadget_box(gad, gi, &bx, &by, &bw, &bh);
    palette_inner(gad, gi, &x, &y, &w, &h);
    if (w < 1 || h < 1)
        return;

    SetDrMd(rp, JAM1);
    SetAPen(rp, pens[BACKGROUNDPEN]);
    RectFill(rp, x, y, x + w - 1, y + h - 1);

    /* Recessed frame */
    SetAPen(rp, pens[SHADOWPEN]);
    Move(rp, bx, by + bh - 1); Draw(rp, bx, by); Draw(rp, bx + bw - 1, by);
    Move(rp, bx + 1, by + bh - 2); Draw(rp, bx + 1, by + 1);
    SetAPen(rp, pens[SHINEPEN]);
    Move(rp, bx + 1, by + bh - 1); Draw(rp, bx + bw - 1, by + bh - 1);
    Draw(rp, bx + bw - 1, by + 1);
    Move(rp, bx + bw - 2, by + 1); Draw(rp, bx + bw - 2, by + bh - 2);

    palette_grid(data->pd_NumColors, w, h, &cols, &rows);
    cellw = w / (WORD)cols;
    cellh = h / (WORD)rows;
    if (cellw < 1) cellw = 1;
    if (cellh < 1) cellh = 1;

    for (i = 0; i < data->pd_NumColors; i++)
    {
        WORD cx = x + (WORD)(i % cols) * cellw;
        WORD cy = y + (WORD)(i / cols) * cellh;
        UWORD pen = palette_pen(data, i);
        BOOL sel = (pen == data->pd_Color) || ((LONG)i == data->pd_Hover);
        WORD in = (cellw > 4 && cellh > 4) ? 1 : 0;

        if (sel && cellw > 6 && cellh > 6)
        {
            /* Selected: a highlighted box around a smaller swatch */
            SetAPen(rp, pens[TEXTPEN]);
            RectFill(rp, cx, cy, cx + cellw - 1, cy + cellh - 1);
            SetAPen(rp, pens[BACKGROUNDPEN]);
            RectFill(rp, cx + 1, cy + 1, cx + cellw - 2, cy + cellh - 2);
            in = 3;
        }
        SetAPen(rp, pen);
        RectFill(rp, cx + in, cy + in, cx + cellw - 1 - in, cy + cellh - 1 - in);
    }

    if (gad->Flags & GFLG_DISABLED)
    {
        static UWORD ghost[] = { 0x2222, 0x8888 };

        SetAPen(rp, pens[BLOCKPEN]);
        SetAfPt(rp, ghost, 1);
        RectFill(rp, bx, by, bx + bw - 1, by + bh - 1);
        SetAfPt(rp, NULL, 0);
    }
}

static void palette_redraw(Class *cl, Object *o, struct GadgetInfo *gi)
{
    struct RastPort *rp;

    if (gi && (rp = ObtainGIRPort(gi)))
    {
        palette_draw(cl, o, gi, rp);
        ReleaseGIRPort(rp);
    }
}

IPTR Palette__GM_RENDER(Class *cl, Object *o, struct gpRender *msg)
{
    if (!msg->gpr_RPort)
        palette_redraw(cl, o, msg->gpr_GInfo);
    else
        palette_draw(cl, o, msg->gpr_GInfo, msg->gpr_RPort);
    return TRUE;
}

/******************************************************************************/

IPTR Palette__GM_DOMAIN(Class *cl, Object *o, struct gpDomain *msg)
{
    struct PaletteGadData *data = INST_DATA(cl, o);
    ULONG cols = data->pd_NumColors > 32 ? 32 : data->pd_NumColors;
    ULONG rows = (data->pd_NumColors + cols - 1) / cols;
    WORD cell = (msg->gpd_Which == GDOMAIN_MINIMUM) ? 2 : 4;

    msg->gpd_Domain.Left   = 0;
    msg->gpd_Domain.Top    = 0;
    msg->gpd_Domain.Width  = cols * cell + 4;
    msg->gpd_Domain.Height = rows * cell + 2;
    return TRUE;
}

IPTR Palette__GM_HITTEST(Class *cl, Object *o, struct gpHitTest *msg)
{
    return GMR_GADGETHIT;
}

IPTR Palette__GM_GOACTIVE(Class *cl, Object *o, struct gpInput *msg)
{
    struct PaletteGadData *data = INST_DATA(cl, o);

    if (!msg->gpi_IEvent || (G(o)->Flags & GFLG_DISABLED))
        return GMR_NOREUSE;
    data->pd_Hover = palette_cell_at(data, G(o), msg->gpi_GInfo,
        msg->gpi_Mouse.X, msg->gpi_Mouse.Y);
    if (data->pd_Hover < 0)
        return GMR_NOREUSE;
    palette_redraw(cl, o, msg->gpi_GInfo);
    return GMR_MEACTIVE;
}

IPTR Palette__GM_HANDLEINPUT(Class *cl, Object *o, struct gpInput *msg)
{
    struct PaletteGadData *data = INST_DATA(cl, o);
    struct InputEvent *ie = msg->gpi_IEvent;
    LONG cell;

    if (!ie || ie->ie_Class != IECLASS_RAWMOUSE)
        return GMR_MEACTIVE;

    cell = palette_cell_at(data, G(o), msg->gpi_GInfo,
        msg->gpi_Mouse.X, msg->gpi_Mouse.Y);
    switch (ie->ie_Code)
    {
        case SELECTUP:
            data->pd_Hover = -1;
            if (cell >= 0)
            {
                data->pd_Color = palette_pen(data, cell);
                palette_redraw(cl, o, msg->gpi_GInfo);
                *msg->gpi_Termination = data->pd_Color;
                return GMR_NOREUSE | GMR_VERIFY;
            }
            palette_redraw(cl, o, msg->gpi_GInfo);
            return GMR_NOREUSE;

        case MENUDOWN:
            data->pd_Hover = -1;
            palette_redraw(cl, o, msg->gpi_GInfo);
            return GMR_NOREUSE;

        case IECODE_NOBUTTON:
            if (cell != data->pd_Hover)
            {
                data->pd_Hover = cell;
                palette_redraw(cl, o, msg->gpi_GInfo);
            }
            break;
    }
    return GMR_MEACTIVE;
}

IPTR Palette__GM_GOINACTIVE(Class *cl, Object *o, struct gpGoInactive *msg)
{
    struct PaletteGadData *data = INST_DATA(cl, o);

    if (data->pd_Hover >= 0)
    {
        data->pd_Hover = -1;
        palette_redraw(cl, o, msg->gpgi_GInfo);
    }
    return 0;
}
