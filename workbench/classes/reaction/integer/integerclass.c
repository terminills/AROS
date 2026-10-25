/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Reaction integer.gadget - BOOPSI class implementation
*/
#define DEBUG 0

#include <string.h>
#include <limits.h>

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/gadgetclass.h>
#include <intuition/imageclass.h>
#include <graphics/gfxmacros.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <proto/alib.h>

#include <gadgets/integer.h>

#include "integer_intern.h"
#include "../reaction_box.h"
#include <images/bevel.h>

#define IntegerBase ((struct Library *)(cl->cl_UserData))

/* ------------------------------------------------------------------ */

static void integer_set(Class *cl, Object *o, struct opSet *msg)
{
    struct IntegerData *data = INST_DATA(cl, o);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *tag;

    while ((tag = NextTagItem(&tags)) != NULL)
    {
        switch (tag->ti_Tag)
        {
            case INTEGER_Number:
                data->number = (LONG)tag->ti_Data;
                if (data->number < data->minimum)
                    data->number = data->minimum;
                if (data->number > data->maximum)
                    data->number = data->maximum;
                break;

            case INTEGER_Minimum:
                data->minimum = (LONG)tag->ti_Data;
                break;

            case INTEGER_Maximum:
                data->maximum = (LONG)tag->ti_Data;
                break;

            case INTEGER_MaxChars:
                data->maxchars = (ULONG)tag->ti_Data;
                break;

            case INTEGER_Arrows:
                data->arrows = (BOOL)tag->ti_Data;
                break;
        }
    }

    /* Clamp to range after min/max may have changed */
    if (data->number < data->minimum)
        data->number = data->minimum;
    if (data->number > data->maximum)
        data->number = data->maximum;
}

/* ------------------------------------------------------------------ */

/******************************************************************************/

/* The field frame: a BVS_FIELD bevel around the gadget, the text inset
   inside it. Intuition's string code draws and places the cursor from the
   gadget box, so the box is narrowed around those calls. */
#define FIELD_DX 4
#define FIELD_DY 2

static void field_inset(struct Gadget *g, WORD sign)
{
    g->LeftEdge += sign * FIELD_DX;
    g->TopEdge  += sign * FIELD_DY;
    g->Width    -= sign * 2 * FIELD_DX;
    g->Height   -= sign * 2 * FIELD_DY;
}

static void field_draw_frame(Object **bevel, struct Gadget *g, struct gpRender *msg);

static IPTR field_super_input(Class *cl, Object *o, struct gpInput *msg, UBYTE *inset)
{
    IPTR r;
    field_inset(G(o), 1);
    *inset = TRUE;
    msg->gpi_Mouse.X -= FIELD_DX;
    msg->gpi_Mouse.Y -= FIELD_DY;
    r = DoSuperMethodA(cl, o, (Msg)msg);
    msg->gpi_Mouse.X += FIELD_DX;
    msg->gpi_Mouse.Y += FIELD_DY;
    *inset = FALSE;
    field_inset(G(o), -1);
    return r;
}

/* Frame first: the bevel fills its inside. Intuition's string code renders
   from inside GM_HANDLEINPUT, when the box is already inset. */
static IPTR field_render(Class *cl, Object *o, struct gpRender *msg, Object **bevel, UBYTE inset)
{
    IPTR r;
    if (inset)
        field_inset(G(o), -1);
    field_draw_frame(bevel, G(o), msg);
    field_inset(G(o), 1);
    r = DoSuperMethodA(cl, o, (Msg)msg);
    if (!inset)
        field_inset(G(o), -1);
    return r;
}

static void field_draw_frame(Object **bevel, struct Gadget *g, struct gpRender *msg)
{
    struct RastPort *rp = msg->gpr_RPort;
    struct DrawInfo *dri = msg->gpr_GInfo ? msg->gpr_GInfo->gi_DrInfo : NULL;
    WORD x, y, w, h;
    struct impDraw id;

    if (!rp || !dri)
        return;
    if (!*bevel)
        *bevel = NewObject(NULL, "bevel.image", BEVEL_Style, BVS_FIELD, TAG_END);
    if (!*bevel)
        return;
    reaction_gadget_box(g, msg->gpr_GInfo, &x, &y, &w, &h);
    /* the strip between the frame and the text area */
    SetAPen(rp, dri->dri_Pens[BACKGROUNDPEN]);
    RectFill(rp, x, y, x + w - 1, y + FIELD_DY - 1);
    RectFill(rp, x, y + h - FIELD_DY, x + w - 1, y + h - 1);
    RectFill(rp, x, y, x + FIELD_DX - 1, y + h - 1);
    RectFill(rp, x + w - FIELD_DX, y, x + w - 1, y + h - 1);
    id.MethodID = IM_DRAWFRAME;
    id.imp_RPort = rp;
    id.imp_Offset.X = x;
    id.imp_Offset.Y = y;
    id.imp_State = (g->Flags & GFLG_DISABLED) ? IDS_DISABLED : IDS_NORMAL;
    id.imp_DrInfo = dri;
    id.imp_Dimensions.Width = w;
    id.imp_Dimensions.Height = h;
    DoMethodA(*bevel, (Msg)&id);
}

static void field_domain(struct gpDomain *msg, UWORD chars)
{
    struct TextFont *font = NULL;
    UWORD cw = 8, ch = 8;

    if (msg->gpd_GInfo && msg->gpd_GInfo->gi_DrInfo)
        font = msg->gpd_GInfo->gi_DrInfo->dri_Font;
    if (!font && msg->gpd_RPort)
        font = msg->gpd_RPort->Font;
    if (font)
    {
        cw = font->tf_XSize;
        ch = font->tf_YSize;
    }
    msg->gpd_Domain.Left   = 0;
    msg->gpd_Domain.Top    = 0;
    msg->gpd_Domain.Width  = (chars ? chars : 4) * cw + 2 * FIELD_DX + 4;
    msg->gpd_Domain.Height = ch + 2 * FIELD_DY + 4;
}

IPTR Integer__OM_NEW(Class *cl, Object *o, struct opSet *msg)
{
    struct IntegerData *data;

    D(bug("[Integer] OM_NEW: enter\n"));

    o = (Object *)DoSuperMethodA(cl, o, (Msg)msg);
    D(bug("[Integer] OM_NEW: obj=%p\n", (void *)o));
    if (!o)
        return (IPTR)0;

    data = INST_DATA(cl, o);
    memset(data, 0, sizeof(struct IntegerData));

    /* Defaults */
    data->minimum      = (LONG)0x80000000;
    data->maximum      = (LONG)0x7FFFFFFF;
    data->maxchars     = 10;

    integer_set(cl, o, msg);

    return (IPTR)o;
}

/* ------------------------------------------------------------------ */

IPTR Integer__OM_DISPOSE(Class *cl, Object *o, Msg msg)
{
    D(bug("[Integer] OM_DISPOSE: obj=%p\n", (void *)o));
    {
        Object *bevel = ((struct IntegerData *)INST_DATA(cl, o))->id_Bevel;
        if (bevel)
            DisposeObject(bevel);
    }

    return DoSuperMethodA(cl, o, msg);
}

/* ------------------------------------------------------------------ */

IPTR Integer__OM_SET(Class *cl, Object *o, struct opSet *msg)
{
    IPTR retval;

    D(bug("[Integer] OM_SET: obj=%p\n", (void *)o));

    retval = DoSuperMethodA(cl, o, (Msg)msg);
    integer_set(cl, o, msg);

    return retval;
}

/* ------------------------------------------------------------------ */

IPTR Integer__OM_GET(Class *cl, Object *o, struct opGet *msg)
{
    struct IntegerData *data = INST_DATA(cl, o);

    switch (msg->opg_AttrID)
    {
        case INTEGER_Number:
            *msg->opg_Storage = (IPTR)data->number;
            return (IPTR)TRUE;

        case INTEGER_Minimum:
            *msg->opg_Storage = (IPTR)data->minimum;
            return (IPTR)TRUE;

        case INTEGER_Maximum:
            *msg->opg_Storage = (IPTR)data->maximum;
            return (IPTR)TRUE;

        case INTEGER_MaxChars:
            *msg->opg_Storage = (IPTR)data->maxchars;
            return (IPTR)TRUE;

        case INTEGER_Arrows:
            *msg->opg_Storage = (IPTR)data->arrows;
            return (IPTR)TRUE;
    }

    return DoSuperMethodA(cl, o, (Msg)msg);
}

/* ------------------------------------------------------------------ */

IPTR Integer__GM_RENDER(Class *cl, Object *o, struct gpRender *msg)
{
    IPTR retval;

    D(bug("[Integer] GM_RENDER: obj=%p redraw=%ld\n", (void *)o, msg->gpr_Redraw));

    {
        struct IntegerData *data = INST_DATA(cl, o);
        retval = field_render(cl, o, msg, &data->id_Bevel, data->id_Inset);
    }

    return retval;
}

/******************************************************************************/

IPTR Integer__GM_GOACTIVE(Class *cl, Object *o, struct gpInput *msg)
{
    struct IntegerData *data = INST_DATA(cl, o);
    return field_super_input(cl, o, msg, &data->id_Inset);
}

IPTR Integer__GM_HANDLEINPUT(Class *cl, Object *o, struct gpInput *msg)
{
    struct IntegerData *data = INST_DATA(cl, o);
    return field_super_input(cl, o, msg, &data->id_Inset);
}

IPTR Integer__GM_DOMAIN(Class *cl, Object *o, struct gpDomain *msg)
{
    struct IntegerData *data = INST_DATA(cl, o);
    (void)data;
    field_domain(msg, 0);
    return TRUE;
}
