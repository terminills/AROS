/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Reaction string.gadget - BOOPSI class implementation
*/
#define DEBUG 0

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/graphics.h>
#include <proto/alib.h>

#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/cghooks.h>
#include <intuition/gadgetclass.h>
#include <intuition/imageclass.h>
#include <gadgets/string.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>

#include <string.h>

#include "string_intern.h"
#include "../reaction_box.h"
#include <images/bevel.h>

#define StringBase ((struct Library *)(cl->cl_UserData))

/******************************************************************************/

static void string_set(Class *cl, Object *o, struct opSet *msg)
{
    struct StringGadData *data = INST_DATA(cl, o);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *tag;

    while ((tag = NextTagItem(&tags)))
    {
        switch (tag->ti_Tag)
        {
            case STRINGA_MinVisible:
                data->sd_MinVisible = (UWORD)tag->ti_Data;
                break;
            case STRINGA_HookType:
                data->sd_HookType = (UWORD)tag->ti_Data;
                break;
        }
    }
}

/******************************************************************************/

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
    D(bug("[String] input method %lx obj=%p mouse %d,%d box %d,%d %dx%d -> %lx\n",
        msg->MethodID, (void *)o, msg->gpi_Mouse.X, msg->gpi_Mouse.Y,
        G(o)->LeftEdge, G(o)->TopEdge, G(o)->Width, G(o)->Height, r));
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

IPTR String__OM_NEW(Class *cl, Object *o, struct opSet *msg)
{
    IPTR retval;

    D(bug("[String] OM_NEW: enter\n"));

    retval = DoSuperMethodA(cl, o, (Msg)msg);
    D(bug("[String] OM_NEW: obj=%p\n", (void *)retval));
    if (retval)
    {
        struct StringGadData *data = INST_DATA(cl, (Object *)retval);

        memset(data, 0, sizeof(struct StringGadData));

        string_set(cl, (Object *)retval, msg);
    }

    return retval;
}

/******************************************************************************/

IPTR String__OM_DISPOSE(Class *cl, Object *o, Msg msg)
{
    D(bug("[String] OM_DISPOSE: obj=%p\n", (void *)o));
    {
        Object *bevel = ((struct StringGadData *)INST_DATA(cl, o))->sd_Bevel;
        if (bevel)
            DisposeObject(bevel);
    }

    return DoSuperMethodA(cl, o, msg);
}

/******************************************************************************/

IPTR String__OM_SET(Class *cl, Object *o, struct opSet *msg)
{
    D(bug("[String] OM_SET: obj=%p\n", (void *)o));

    IPTR retval = DoSuperMethodA(cl, o, (Msg)msg);
    string_set(cl, o, msg);
    return retval;
}

/******************************************************************************/

IPTR String__OM_GET(Class *cl, Object *o, struct opGet *msg)
{
    struct StringGadData *data = INST_DATA(cl, o);

    switch (msg->opg_AttrID)
    {
        case STRINGA_MinVisible:
            *msg->opg_Storage = data->sd_MinVisible;
            return TRUE;

        case STRINGA_HookType:
            *msg->opg_Storage = data->sd_HookType;
            return TRUE;
    }

    return DoSuperMethodA(cl, o, (Msg)msg);
}

/******************************************************************************/

IPTR String__GM_RENDER(Class *cl, Object *o, struct gpRender *msg)
{
    D(bug("[String] GM_RENDER: obj=%p redraw=%ld\n", (void *)o, msg->gpr_Redraw));

    {
        struct StringGadData *data = INST_DATA(cl, o);
        return field_render(cl, o, msg, &data->sd_Bevel, data->sd_Inset);
    }
}

/******************************************************************************/

IPTR String__GM_GOACTIVE(Class *cl, Object *o, struct gpInput *msg)
{
    struct StringGadData *data = INST_DATA(cl, o);
    return field_super_input(cl, o, msg, &data->sd_Inset);
}

IPTR String__GM_HANDLEINPUT(Class *cl, Object *o, struct gpInput *msg)
{
    struct StringGadData *data = INST_DATA(cl, o);
    return field_super_input(cl, o, msg, &data->sd_Inset);
}

IPTR String__GM_DOMAIN(Class *cl, Object *o, struct gpDomain *msg)
{
    struct StringGadData *data = INST_DATA(cl, o);
    (void)data;
    field_domain(msg, data->sd_MinVisible);
    return TRUE;
}
