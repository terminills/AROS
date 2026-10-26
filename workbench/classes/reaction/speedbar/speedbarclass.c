/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Reaction speedbar.gadget - BOOPSI class implementation
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
#include <exec/lists.h>
#include <gadgets/speedbar.h>
#include <utility/tagitem.h>

#include <string.h>

#include "speedbar_intern.h"
#include "../reaction_box.h"

#define SpeedBarBase ((struct Library *)(cl->cl_UserData))

/******************************************************************************/

#define SB_BORDER   1   /* bevel around each button */
#define SB_INSET    2   /* bevel around the whole bar */

static void sb_image_size(Object *img, WORD *w, WORD *h)
{
    struct Image *im = (struct Image *)img;
    IPTR iw = 0, ih = 0;

    if (!im)
    {
        *w = *h = 0;
        return;
    }
    if (im->Depth == CUSTOMIMAGEDEPTH)
    {
        GetAttr(IA_Width, img, &iw);
        GetAttr(IA_Height, img, &ih);
    }
    else
    {
        iw = im->Width;
        ih = im->Height;
    }
    *w = (WORD)iw;
    *h = (WORD)ih;
}

/* Natural size of one button: its image plus offsets and bevel */
static void sb_button_size(struct SpeedButtonNode *sbn, WORD *w, WORD *h)
{
    WORD iw, ih, sw, sh;

    sb_image_size(sbn->sbn_Image, &iw, &ih);
    sb_image_size(sbn->sbn_SelImage, &sw, &sh);
    if (sw > iw) iw = sw;
    if (sh > ih) ih = sh;
    *w = sbn->sbn_Width  ? sbn->sbn_Width  : iw + 2 * sbn->sbn_Left + 2 * SB_BORDER + 2;
    *h = sbn->sbn_Height ? sbn->sbn_Height : ih + 2 * sbn->sbn_Top  + 2 * SB_BORDER;
}

static void sb_max_size(struct SpeedBarData *data, WORD *mw, WORD *mh)
{
    struct Node *n;
    WORD w, h;

    *mw = *mh = 0;
    if (!data->sd_Buttons)
        return;
    for (n = data->sd_Buttons->lh_Head; n->ln_Succ; n = n->ln_Succ)
    {
        sb_button_size((struct SpeedButtonNode *)n, &w, &h);
        if (w > *mw) *mw = w;
        if (h > *mh) *mh = h;
    }
}

/* Place every button in turn; returns the button under (px,py) when asked.
 * Coordinates are relative to the gadget. */
struct sb_place
{
    struct SpeedButtonNode *sbn;
    WORD x, y, w, h;
};

static BOOL sb_next(struct SpeedBarData *data, struct sb_place *p, WORD maxw, WORD maxh)
{
    struct Node *n;
    WORD w, h;

    if (!data->sd_Buttons)
        return FALSE;
    if (!p->sbn)
    {
        n = data->sd_Buttons->lh_Head;
        if (!n->ln_Succ)
            return FALSE;
        p->x = p->y = SB_INSET;
        p->sbn = (struct SpeedButtonNode *)n;
        p->x += (data->sd_Orientation == SBORIENT_VERT) ? 0 : p->sbn->sbn_Spacing;
        p->y += (data->sd_Orientation == SBORIENT_VERT) ? p->sbn->sbn_Spacing : 0;
    }
    else
    {
        n = p->sbn->sbn_Node.ln_Succ;
        if (!n->ln_Succ)
            return FALSE;
        if (data->sd_Orientation == SBORIENT_VERT)
            p->y += p->h + ((struct SpeedButtonNode *)n)->sbn_Spacing;
        else
            p->x += p->w + ((struct SpeedButtonNode *)n)->sbn_Spacing;
        p->sbn = (struct SpeedButtonNode *)n;
    }
    sb_button_size(p->sbn, &w, &h);
    if (data->sd_EvenSize)
    {
        WORD mw, mh;
        sb_max_size(data, &mw, &mh);
        w = mw;
        h = mh;
    }
    /* Buttons fill the bar across its short side */
    if (data->sd_Orientation == SBORIENT_VERT)
        w = maxw - 2 * SB_INSET;
    else
        h = maxh - 2 * SB_INSET;
    p->w = w;
    p->h = h;
    return TRUE;
}

static struct SpeedButtonNode *sb_hit(struct SpeedBarData *data, WORD mx, WORD my,
    WORD gw, WORD gh)
{
    struct sb_place p = { NULL };

    while (sb_next(data, &p, gw, gh))
    {
        if (mx >= p.x && mx < p.x + p.w && my >= p.y && my < p.y + p.h)
            return p.sbn;
    }
    return NULL;
}

/******************************************************************************/

static void speedbar_set(Class *cl, Object *o, struct opSet *msg)
{
    struct SpeedBarData *data = INST_DATA(cl, o);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *tag;

    while ((tag = NextTagItem(&tags)))
    {
        switch (tag->ti_Tag)
        {
            case SPEEDBAR_Buttons:
                data->sd_Buttons = (struct List *)tag->ti_Data;
                data->sd_Active = NULL;
                data->sd_SelectedNode = NULL;
                break;
            case SPEEDBAR_Orientation:
                data->sd_Orientation = tag->ti_Data;
                break;
            case SPEEDBAR_BevelStyle:
                data->sd_BevelStyle = tag->ti_Data;
                break;
            case SPEEDBAR_Window:
                data->sd_Window = (struct Window *)tag->ti_Data;
                break;
            case SPEEDBAR_EvenSize:
                data->sd_EvenSize = (BOOL)tag->ti_Data;
                break;
            case SPEEDBAR_StrumBar:
                data->sd_StrumBar = (BOOL)tag->ti_Data;
                break;
            case SPEEDBAR_Background:
                data->sd_Background = (LONG)tag->ti_Data;
                break;
            case SPEEDBAR_OnButton:
            case SPEEDBAR_OffButton:
                if (data->sd_Buttons)
                {
                    struct Node *n;
                    for (n = data->sd_Buttons->lh_Head; n->ln_Succ; n = n->ln_Succ)
                    {
                        struct SpeedButtonNode *sbn = (struct SpeedButtonNode *)n;
                        if (sbn->sbn_Number != (UWORD)tag->ti_Data)
                            continue;
                        if (tag->ti_Tag == SPEEDBAR_OnButton)
                            sbn->sbn_Flags |= SBNF_SELECTED;
                        else
                            sbn->sbn_Flags &= ~SBNF_SELECTED;
                    }
                }
                break;
        }
    }
}

/******************************************************************************/

IPTR SpeedBar__OM_NEW(Class *cl, Object *o, struct opSet *msg)
{
    IPTR retval;

    retval = DoSuperMethodA(cl, o, (Msg)msg);
    if (retval)
    {
        struct SpeedBarData *data = INST_DATA(cl, (Object *)retval);

        memset(data, 0, sizeof(struct SpeedBarData));
        data->sd_Background = -1;
        data->sd_Selected = -1;

        speedbar_set(cl, (Object *)retval, msg);
    }

    return retval;
}

/******************************************************************************/

IPTR SpeedBar__OM_DISPOSE(Class *cl, Object *o, Msg msg)
{
    /* Buttons list is owned by the caller - do not free it here */
    return DoSuperMethodA(cl, o, msg);
}

/******************************************************************************/

IPTR SpeedBar__OM_SET(Class *cl, Object *o, struct opSet *msg)
{
    IPTR retval = DoSuperMethodA(cl, o, (Msg)msg);
    speedbar_set(cl, o, msg);
    return retval;
}

/******************************************************************************/

IPTR SpeedBar__OM_GET(Class *cl, Object *o, struct opGet *msg)
{
    struct SpeedBarData *data = INST_DATA(cl, o);

    switch (msg->opg_AttrID)
    {
        case SPEEDBAR_Buttons:
            *msg->opg_Storage = (IPTR)data->sd_Buttons;
            return TRUE;

        case SPEEDBAR_Orientation:
            *msg->opg_Storage = data->sd_Orientation;
            return TRUE;

        case SPEEDBAR_BevelStyle:
            *msg->opg_Storage = data->sd_BevelStyle;
            return TRUE;

        case SPEEDBAR_Window:
            *msg->opg_Storage = (IPTR)data->sd_Window;
            return TRUE;

        case SPEEDBAR_EvenSize:
            *msg->opg_Storage = data->sd_EvenSize;
            return TRUE;

        case SPEEDBAR_StrumBar:
            *msg->opg_Storage = data->sd_StrumBar;
            return TRUE;

        case SPEEDBAR_Background:
            *msg->opg_Storage = data->sd_Background;
            return TRUE;

        case SPEEDBAR_Selected:
            *msg->opg_Storage = data->sd_Selected;
            return TRUE;

        case SPEEDBAR_SelectedNode:
            *msg->opg_Storage = (IPTR)data->sd_SelectedNode;
            return TRUE;

        case SPEEDBAR_Total:
        case SPEEDBAR_Visible:
        {
            struct Node *n;
            IPTR count = 0;

            if (data->sd_Buttons)
                for (n = data->sd_Buttons->lh_Head; n->ln_Succ; n = n->ln_Succ)
                    count++;
            *msg->opg_Storage = count;
            return TRUE;
        }

        case SPEEDBAR_Top:
            *msg->opg_Storage = 0;
            return TRUE;
    }

    return DoSuperMethodA(cl, o, (Msg)msg);
}

/******************************************************************************/

IPTR SpeedBar__GM_DOMAIN(Class *cl, Object *o, struct gpDomain *msg)
{
    struct SpeedBarData *data = INST_DATA(cl, o);
    struct Node *n;
    WORD len = 0, across = 0, w, h, mw, mh;

    sb_max_size(data, &mw, &mh);
    if (data->sd_Buttons)
    {
        for (n = data->sd_Buttons->lh_Head; n->ln_Succ; n = n->ln_Succ)
        {
            struct SpeedButtonNode *sbn = (struct SpeedButtonNode *)n;

            sb_button_size(sbn, &w, &h);
            if (data->sd_EvenSize)
            {
                w = mw;
                h = mh;
            }
            if (data->sd_Orientation == SBORIENT_VERT)
            {
                len += h + sbn->sbn_Spacing;
                if (w > across) across = w;
            }
            else
            {
                len += w + sbn->sbn_Spacing;
                if (h > across) across = h;
            }
        }
    }

    msg->gpd_Domain.Left = 0;
    msg->gpd_Domain.Top = 0;
    if (data->sd_Orientation == SBORIENT_VERT)
    {
        msg->gpd_Domain.Width  = across + 2 * SB_INSET;
        msg->gpd_Domain.Height = len + 2 * SB_INSET;
    }
    else
    {
        msg->gpd_Domain.Width  = len + 2 * SB_INSET;
        msg->gpd_Domain.Height = across + 2 * SB_INSET;
    }
    return TRUE;
}

/******************************************************************************/

static void sb_bevel(struct RastPort *rp, WORD x, WORD y, WORD w, WORD h,
    UWORD tl, UWORD br)
{
    SetAPen(rp, tl);
    Move(rp, x, y + h - 1);
    Draw(rp, x, y);
    Draw(rp, x + w - 1, y);
    SetAPen(rp, br);
    Move(rp, x + 1, y + h - 1);
    Draw(rp, x + w - 1, y + h - 1);
    Draw(rp, x + w - 1, y + 1);
}

static void sb_draw_button(struct SpeedBarData *data, struct RastPort *rp,
    struct DrawInfo *dri, struct sb_place *p, WORD ox, WORD oy, UWORD bg)
{
    struct SpeedButtonNode *sbn = p->sbn;
    UWORD *pens = dri->dri_Pens;
    BOOL down = (sbn->sbn_Flags & SBNF_SELECTED) != 0;
    WORD x = ox + p->x, y = oy + p->y;
    Object *img = sbn->sbn_Image;
    WORD iw, ih;
    UWORD fill = bg;

    if (data->sd_Active == sbn && data->sd_Pressed)
        down = !((sbn->sbn_Flags & SBNF_TOGGLE) && down && !sbn->sbn_MXGroup);

    if (down && sbn->sbn_Highlight == SBH_BACKFILL)
        fill = pens[FILLPEN];
    if (down && sbn->sbn_Highlight == SBH_IMAGE && sbn->sbn_SelImage)
        img = sbn->sbn_SelImage;

    SetAPen(rp, fill);
    RectFill(rp, x, y, x + p->w - 1, y + p->h - 1);

    if (sbn->sbn_Highlight != SBH_NONE)
    {
        BOOL recess = down && sbn->sbn_Highlight != SBH_IMAGE;
        sb_bevel(rp, x, y, p->w, p->h,
            recess ? pens[SHADOWPEN] : pens[SHINEPEN],
            recess ? pens[SHINEPEN] : pens[SHADOWPEN]);
    }

    if (img)
    {
        sb_image_size(img, &iw, &ih);
        DrawImageState(rp, (struct Image *)img,
            x + (p->w - iw) / 2 + (down && sbn->sbn_Highlight == SBH_RECESS ? 1 : 0),
            y + (p->h - ih) / 2 + (down && sbn->sbn_Highlight == SBH_RECESS ? 1 : 0),
            down ? IDS_SELECTED : IDS_NORMAL, dri);
    }
    else if (sbn->sbn_Node.ln_Name)
    {
        SetAPen(rp, pens[TEXTPEN]);
        SetDrMd(rp, JAM1);
        Move(rp, x + SB_BORDER + 2, y + (p->h - rp->TxHeight) / 2 + rp->TxBaseline);
        Text(rp, sbn->sbn_Node.ln_Name, strlen(sbn->sbn_Node.ln_Name));
    }

    if (sbn->sbn_Flags & SBNF_DISABLED)
    {
        static UWORD ghost[] = { 0x2222, 0x8888 };

        SetAPen(rp, pens[BLOCKPEN]);
        SetAfPt(rp, ghost, 1);
        RectFill(rp, x, y, x + p->w - 1, y + p->h - 1);
        SetAfPt(rp, NULL, 0);
    }
}

static void sb_render(Class *cl, Object *o, struct GadgetInfo *gi,
    struct RastPort *rp, struct SpeedButtonNode *only)
{
    struct SpeedBarData *data = INST_DATA(cl, o);
    struct DrawInfo *dri = gi ? gi->gi_DrInfo : NULL;
    struct Gadget *gad = G(o);
    struct sb_place p = { NULL };
    WORD x, y, w, h;
    UWORD bg;

    if (!rp || !dri)
        return;

    reaction_gadget_box(gad, gi, &x, &y, &w, &h);
    bg = data->sd_Background >= 0 ? (UWORD)data->sd_Background
                                  : dri->dri_Pens[BACKGROUNDPEN];

    if (!only)
    {
        SetAPen(rp, bg);
        RectFill(rp, x, y, x + w - 1, y + h - 1);
        sb_bevel(rp, x, y, w, h, dri->dri_Pens[SHADOWPEN], dri->dri_Pens[SHINEPEN]);
    }

    while (sb_next(data, &p, w, h))
    {
        if (data->sd_Orientation == SBORIENT_VERT ? p.y + p.h > h - SB_INSET
                                                  : p.x + p.w > w - SB_INSET)
            break;
        if (!only || only == p.sbn)
            sb_draw_button(data, rp, dri, &p, x, y, bg);
    }

    if (!only && (gad->Flags & GFLG_DISABLED))
    {
        static UWORD ghost[] = { 0x2222, 0x8888 };

        SetAPen(rp, dri->dri_Pens[BLOCKPEN]);
        SetAfPt(rp, ghost, 1);
        RectFill(rp, x, y, x + w - 1, y + h - 1);
        SetAfPt(rp, NULL, 0);
    }
}

static void sb_redraw(Class *cl, Object *o, struct GadgetInfo *gi,
    struct SpeedButtonNode *only)
{
    struct RastPort *rp;

    if (gi && (rp = ObtainGIRPort(gi)))
    {
        sb_render(cl, o, gi, rp, only);
        ReleaseGIRPort(rp);
    }
}

IPTR SpeedBar__GM_RENDER(Class *cl, Object *o, struct gpRender *msg)
{
    struct RastPort *rp = msg->gpr_RPort;

    if (!rp && msg->gpr_GInfo)
    {
        sb_redraw(cl, o, msg->gpr_GInfo, NULL);
        return TRUE;
    }
    sb_render(cl, o, msg->gpr_GInfo, rp, NULL);
    return TRUE;
}

/******************************************************************************/

IPTR SpeedBar__GM_HITTEST(Class *cl, Object *o, struct gpHitTest *msg)
{
    struct SpeedBarData *data = INST_DATA(cl, o);
    WORD x, y, w, h;

    reaction_gadget_box(G(o), msg->gpht_GInfo, &x, &y, &w, &h);
    return sb_hit(data, msg->gpht_Mouse.X, msg->gpht_Mouse.Y, w, h)
        ? GMR_GADGETHIT : 0;
}

IPTR SpeedBar__GM_GOACTIVE(Class *cl, Object *o, struct gpInput *msg)
{
    struct SpeedBarData *data = INST_DATA(cl, o);
    struct SpeedButtonNode *sbn;
    WORD x, y, w, h;

    if (!msg->gpi_IEvent || (G(o)->Flags & GFLG_DISABLED))
        return GMR_NOREUSE;

    reaction_gadget_box(G(o), msg->gpi_GInfo, &x, &y, &w, &h);
    sbn = sb_hit(data, msg->gpi_Mouse.X, msg->gpi_Mouse.Y, w, h);
    if (!sbn || (sbn->sbn_Flags & SBNF_DISABLED))
        return GMR_NOREUSE;

    data->sd_Active = sbn;
    data->sd_Pressed = TRUE;
    sb_redraw(cl, o, msg->gpi_GInfo, sbn);
    return GMR_MEACTIVE;
}

static void sb_release(struct SpeedBarData *data, struct SpeedButtonNode *sbn)
{
    if (sbn->sbn_MXGroup)
    {
        struct Node *n;

        for (n = data->sd_Buttons->lh_Head; n->ln_Succ; n = n->ln_Succ)
        {
            struct SpeedButtonNode *o = (struct SpeedButtonNode *)n;
            if (o->sbn_MXGroup == sbn->sbn_MXGroup)
                o->sbn_Flags &= ~SBNF_SELECTED;
        }
        sbn->sbn_Flags |= SBNF_SELECTED;
    }
    else if (sbn->sbn_Flags & SBNF_TOGGLE)
        sbn->sbn_Flags ^= SBNF_SELECTED;

    data->sd_Selected = sbn->sbn_Number;
    data->sd_SelectedNode = sbn;
}

IPTR SpeedBar__GM_HANDLEINPUT(Class *cl, Object *o, struct gpInput *msg)
{
    struct SpeedBarData *data = INST_DATA(cl, o);
    struct InputEvent *ie = msg->gpi_IEvent;
    struct SpeedButtonNode *sbn, *active = data->sd_Active;
    WORD x, y, w, h;

    if (!active)
        return GMR_NOREUSE;
    if (!ie || ie->ie_Class != IECLASS_RAWMOUSE)
        return GMR_MEACTIVE;

    reaction_gadget_box(G(o), msg->gpi_GInfo, &x, &y, &w, &h);
    sbn = sb_hit(data, msg->gpi_Mouse.X, msg->gpi_Mouse.Y, w, h);

    switch (ie->ie_Code)
    {
        case SELECTUP:
            if (data->sd_Pressed && sbn == active)
            {
                struct Node *n;

                sb_release(data, active);
                data->sd_Active = NULL;
                data->sd_Pressed = FALSE;
                if (active->sbn_MXGroup)
                {
                    for (n = data->sd_Buttons->lh_Head; n->ln_Succ; n = n->ln_Succ)
                        if (((struct SpeedButtonNode *)n)->sbn_MXGroup == active->sbn_MXGroup)
                            sb_redraw(cl, o, msg->gpi_GInfo, (struct SpeedButtonNode *)n);
                }
                else
                    sb_redraw(cl, o, msg->gpi_GInfo, active);
                *msg->gpi_Termination = active->sbn_Number;
                return GMR_NOREUSE | GMR_VERIFY;
            }
            return GMR_NOREUSE;

        case MENUDOWN:
            return GMR_NOREUSE;

        case IECODE_NOBUTTON:
            if (data->sd_StrumBar && sbn && sbn != active &&
                !(sbn->sbn_Flags & SBNF_DISABLED))
            {
                data->sd_Active = sbn;
                data->sd_Pressed = TRUE;
                sb_redraw(cl, o, msg->gpi_GInfo, active);
                sb_redraw(cl, o, msg->gpi_GInfo, sbn);
            }
            else if ((sbn == active) != data->sd_Pressed)
            {
                data->sd_Pressed = (sbn == active);
                sb_redraw(cl, o, msg->gpi_GInfo, active);
            }
            break;
    }
    return GMR_MEACTIVE;
}

IPTR SpeedBar__GM_GOINACTIVE(Class *cl, Object *o, struct gpGoInactive *msg)
{
    struct SpeedBarData *data = INST_DATA(cl, o);
    struct SpeedButtonNode *active = data->sd_Active;

    data->sd_Active = NULL;
    data->sd_Pressed = FALSE;
    if (active)
        sb_redraw(cl, o, msg->gpgi_GInfo, active);
    return 0;
}
