/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Reaction label.image - BOOPSI class implementation
*/
#define DEBUG 0

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/graphics.h>
#include <proto/alib.h>
#include <proto/diskfont.h>

#include <graphics/gfxmacros.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/imageclass.h>
#include <images/label.h>
#include <utility/tagitem.h>
#include <reaction/reaction_prefs.h>
#include <exec/semaphores.h>

#include <string.h>

#include "label_intern.h"

#define LabelBase ((struct Library *)(cl->cl_UserData))

/******************************************************************************/

static void label_free_segs(struct LabelData *data)
{
    struct LabelSeg *seg = data->ld_Segs, *next;

    while (seg)
    {
        next = seg->ls_Next;
        if (data->ld_DisposeImage && seg->ls_Image)
            DisposeObject(seg->ls_Image);
        FreeVec(seg);
        seg = next;
    }
    data->ld_Segs = NULL;
    data->ld_Text = NULL;
    data->ld_Image = NULL;
}

static struct LabelSeg *label_add_seg(struct LabelSeg ***tail, UBYTE type,
    CONST_STRPTR text, ULONG len)
{
    struct LabelSeg *seg = AllocVec(sizeof(struct LabelSeg) + len + 1,
        MEMF_ANY | MEMF_CLEAR);

    if (seg)
    {
        seg->ls_Type = type;
        seg->ls_Len = len;
        if (len)
            CopyMem(text, seg->ls_Text, len);
        **tail = seg;
        *tail = &seg->ls_Next;
    }
    return seg;
}

static struct TextFont *label_open_font(struct LabelData *data,
    struct TextAttr *ta)
{
    struct TextFont *font = NULL;
    struct Library *DiskfontBase;
    int i;

    if (!ta)
        return NULL;
    if ((DiskfontBase = OpenLibrary("diskfont.library", 0)))
    {
        font = OpenDiskFont(ta);
        CloseLibrary(DiskfontBase);
    }
    if (!font && !(font = OpenFont(ta)))
        return NULL;
    for (i = 0; i < 4; i++)
    {
        if (!data->ld_Fonts[i])
        {
            data->ld_Fonts[i] = font;
            return font;
        }
    }
    CloseFont(font);
    return NULL;
}

static BOOL label_has_contents(struct TagItem *tags)
{
    return FindTagItem(LABEL_Text, tags) || FindTagItem(LABEL_Image, tags);
}

static void label_set(Class *cl, Object *o, struct opSet *msg)
{
    struct LabelData *data = INST_DATA(cl, o);
    struct TagItem *tags = msg->ops_AttrList;
    struct TagItem *tag;
    struct LabelSeg **tail, *seg;
    struct TextFont *font = NULL;
    UWORD pen = 0;

    if (label_has_contents(tags))
        label_free_segs(data);
    for (tail = &data->ld_Segs; *tail; tail = &(*tail)->ls_Next)
        ;

    while ((tag = NextTagItem(&tags)))
    {
        switch (tag->ti_Tag)
        {
            case LABEL_Text:
            {
                CONST_STRPTR text = (CONST_STRPTR)tag->ti_Data;

                if (!text)
                    break;
                if (!data->ld_Text)
                    data->ld_Text = (STRPTR)text;
                for (;;)
                {
                    CONST_STRPTR nl = strchr(text, '\n');
                    ULONG len = nl ? (ULONG)(nl - text) : strlen(text);

                    if (len && (seg = label_add_seg(&tail, LSEG_TEXT, text, len)))
                    {
                        seg->ls_SoftStyle = data->ld_SoftStyle;
                        seg->ls_Pen = pen;
                        seg->ls_Font = font;
                    }
                    if (!nl)
                        break;
                    label_add_seg(&tail, LSEG_NEWLINE, NULL, 0);
                    text = nl + 1;
                }
                break;
            }
            case LABEL_Image:
                if (!tag->ti_Data)
                    break;
                if (!data->ld_Image)
                    data->ld_Image = (Object *)tag->ti_Data;
                if ((seg = label_add_seg(&tail, LSEG_IMAGE, NULL, 0)))
                {
                    seg->ls_Image = (Object *)tag->ti_Data;
                    seg->ls_Mapping = data->ld_Mapping;
                }
                break;
            case IA_Font:
                font = label_open_font(data, (struct TextAttr *)tag->ti_Data);
                break;
            case IA_FGPen:
                pen = (UWORD)tag->ti_Data;
                break;
            case LABEL_Justification:
                data->ld_Justification = tag->ti_Data;
                break;
            case LABEL_SoftStyle:
                data->ld_SoftStyle = tag->ti_Data;
                break;
            case LABEL_DisposeImage:
                data->ld_DisposeImage = (BOOL)tag->ti_Data;
                break;
            case LABEL_Mapping:
                data->ld_Mapping = (UWORD *)tag->ti_Data;
                break;
            case LABEL_DrawInfo:
                data->ld_DrawInfo = (struct DrawInfo *)tag->ti_Data;
                break;
            case LABEL_Underscore:
                data->ld_Underscore = (UBYTE)tag->ti_Data;
                break;
            case LABEL_VerticalSpacing:
                data->ld_VSpacing = (UWORD)tag->ti_Data;
                break;
        }
    }
}

/******************************************************************************/

static void label_draw_text(struct RastPort *rp, STRPTR text, ULONG len,
    WORD x, WORD y, UBYTE underscore)
{
    if (!text || len == 0)
        return;

    if (underscore)
    {
        ULONG i;
        WORD cx = x;

        for (i = 0; i < len; i++)
        {
            if (text[i] == underscore && (i + 1) < len)
            {
                /* Draw text before underscore char */
                if (i > 0)
                {
                    /* Partial text already drawn, advance from cx */
                }

                /* Skip the underscore character itself */
                i++;

                /* Draw the underlined character */
                Move(rp, cx, y);
                Text(rp, &text[i], 1);

                {
                    WORD charW = TextLength(rp, &text[i], 1);
                    /* Draw underline beneath the character */
                    Move(rp, cx, y + 1);
                    Draw(rp, cx + charW - 1, y + 1);
                    cx += charW;
                }
            }
            else
            {
                Move(rp, cx, y);
                Text(rp, &text[i], 1);
                cx += TextLength(rp, &text[i], 1);
            }
        }
    }
    else
    {
        Move(rp, x, y);
        Text(rp, text, len);
    }
}

/* Calculate text length excluding the underscore prefix character */
static ULONG label_text_pixel_width(struct RastPort *rp, STRPTR text,
    ULONG len, UBYTE underscore)
{
    ULONG pixelWidth;

    if (!underscore)
        return TextLength(rp, text, len);

    pixelWidth = 0;
    {
        ULONG i;
        for (i = 0; i < len; i++)
        {
            if (text[i] == underscore && (i + 1) < len)
            {
                /* Skip the underscore prefix character itself */
                continue;
            }
            pixelWidth += TextLength(rp, &text[i], 1);
        }
    }

    return pixelWidth;
}

/******************************************************************************/

static void label_image_size(Object *img, WORD *w, WORD *h)
{
    struct Image *im = (struct Image *)img;
    IPTR iw = 0, ih = 0;

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

static void label_seg_font(struct RastPort *rp, struct LabelSeg *seg,
    struct TextFont *deffont)
{
    struct TextFont *font = seg->ls_Font ? seg->ls_Font : deffont;

    if (font && rp->Font != font)
        SetFont(rp, font);
    SetSoftStyle(rp, seg->ls_SoftStyle, AskSoftStyle(rp));
}

/* Size of the line starting at seg; returns the first segment of the next line */
static struct LabelSeg *label_line(struct LabelData *data, struct LabelSeg *seg,
    struct RastPort *rp, struct TextFont *deffont, WORD *lw, WORD *lh)
{
    WORD w = 0, h = 0, sw, sh;

    for (; seg && seg->ls_Type != LSEG_NEWLINE; seg = seg->ls_Next)
    {
        if (seg->ls_Type == LSEG_TEXT)
        {
            label_seg_font(rp, seg, deffont);
            sw = label_text_pixel_width(rp, seg->ls_Text, seg->ls_Len,
                data->ld_Underscore);
            sh = rp->TxHeight ? rp->TxHeight : 8;
        }
        else
            label_image_size(seg->ls_Image, &sw, &sh);
        w += sw;
        if (sh > h)
            h = sh;
    }
    if (h == 0)
        h = deffont ? deffont->tf_YSize : 8;
    *lw = w;
    *lh = h;
    return seg ? seg->ls_Next : NULL;
}

static void label_extent(struct LabelData *data, struct RastPort *rp,
    struct TextFont *deffont, WORD *tw, WORD *th)
{
    struct LabelSeg *seg = data->ld_Segs;
    WORD w = 0, h = 0, lw, lh;
    BOOL first = TRUE;

    while (seg)
    {
        BOOL last;

        seg = label_line(data, seg, rp, deffont, &lw, &lh);
        last = (seg == NULL);
        if (!first)
            h += data->ld_VSpacing;
        if (lw > w)
            w = lw;
        h += lh;
        first = FALSE;
        if (last)
            break;
    }
    *tw = w;
    *th = h;
}

static struct TextFont *label_default_font(struct LabelData *data)
{
    struct TextFont *font = NULL;
    struct Screen *scr;

    if (data->ld_DrawInfo)
        font = data->ld_DrawInfo->dri_Font;
    if (!font && (scr = LockPubScreen(NULL)))
    {
        font = scr->RastPort.Font;
        UnlockPubScreen(NULL, scr);
    }
    return font;
}

/* A plain image drawn through a LABEL_Mapping pen table */
static void label_draw_mapped(struct RastPort *rp, struct Image *im,
    UWORD *map, WORD x, WORD y)
{
    UWORD wpr = (im->Width + 15) >> 4;
    ULONG plane = (ULONG)wpr * im->Height;
    UBYTE *buf;
    WORD px, py;

    if (im->Width <= 0 || im->Height <= 0 || !im->ImageData)
        return;
    buf = AllocVec((ULONG)im->Width * im->Height, MEMF_ANY);
    if (!buf)
        return;

    for (py = 0; py < im->Height; py++)
    {
        for (px = 0; px < im->Width; px++)
        {
            ULONG off = (ULONG)py * wpr + (px >> 4);
            UWORD bit = 0x8000 >> (px & 15);
            UBYTE color = 0, pick = 0;
            int p;

            for (p = 0; p < 8; p++)
            {
                if (im->PlanePick & (1 << p))
                {
                    if (pick < im->Depth && (im->ImageData[pick * plane + off] & bit))
                        color |= 1 << p;
                    pick++;
                }
                else if (im->PlaneOnOff & (1 << p))
                    color |= 1 << p;
            }
            buf[(ULONG)py * im->Width + px] = (UBYTE)map[color];
        }
    }
    WriteChunkyPixels(rp, x, y, x + im->Width - 1, y + im->Height - 1,
        buf, im->Width);
    FreeVec(buf);
}

/* ClassAct programs read the natural size straight from the Image */
static void label_size_image(struct LabelData *data, struct Image *im)
{
    struct RastPort rp;
    WORD w, h;

    if (!data->ld_Segs)
        return;
    InitRastPort(&rp);
    label_extent(data, &rp, label_default_font(data), &w, &h);
    im->Width = w;
    im->Height = h;
}

/******************************************************************************/

IPTR Label__OM_NEW(Class *cl, Object *o, struct opSet *msg)
{
    IPTR retval;

    retval = DoSuperMethodA(cl, o, (Msg)msg);
    if (retval)
    {
        D(bug("[Label] OM_NEW: obj 0x%p\n", (APTR)retval));
        struct LabelData *data = INST_DATA(cl, (Object *)retval);

        memset(data, 0, sizeof(struct LabelData));
        data->ld_Justification = LJ_CENTER;
        data->ld_Underscore = '_';

        /* Snapshot label-related prefs */
        {
            struct UIPrefs *prefs;
            prefs = (struct UIPrefs *)FindSemaphore((STRPTR)RAPREFSSEMAPHORE);
            if (prefs)
            {
                ObtainSemaphoreShared(&prefs->cap_Semaphore);
                data->ld_PrefsLabelPen = prefs->cap_LabelPen;
                data->ld_3DLabel       = prefs->cap_3DLabel ? TRUE : FALSE;
                ReleaseSemaphore(&prefs->cap_Semaphore);
            }
        }

        label_set(cl, (Object *)retval, msg);
        label_size_image(data, (struct Image *)retval);
    }

    return retval;
}

/******************************************************************************/

IPTR Label__OM_DISPOSE(Class *cl, Object *o, Msg msg)
{
    D(bug("[Label] OM_DISPOSE: obj 0x%p\n", o));
    struct LabelData *data = INST_DATA(cl, o);

    int i;

    label_free_segs(data);
    for (i = 0; i < 4; i++)
    {
        if (data->ld_Fonts[i])
            CloseFont(data->ld_Fonts[i]);
    }

    return DoSuperMethodA(cl, o, msg);
}

/******************************************************************************/

IPTR Label__OM_SET(Class *cl, Object *o, struct opSet *msg)
{
    D(bug("[Label] OM_SET: obj 0x%p\n", o));
    IPTR retval = DoSuperMethodA(cl, o, (Msg)msg);
    label_set(cl, o, msg);
    if (label_has_contents(msg->ops_AttrList))
        label_size_image(INST_DATA(cl, o), (struct Image *)o);
    return retval;
}

/******************************************************************************/

IPTR Label__OM_GET(Class *cl, Object *o, struct opGet *msg)
{
    struct LabelData *data = INST_DATA(cl, o);
    struct Image *im = (struct Image *)o;

    switch (msg->opg_AttrID)
    {
        /* imageclass defaults Width/Height to 80x40, so the natural size
         * is always computed from the contents */
        case IA_Width:
        case IA_Height:
        {
            struct RastPort rp;
            WORD w = 0, h = 0;

            InitRastPort(&rp);
            if (data->ld_Segs)
                label_extent(data, &rp, label_default_font(data), &w, &h);
            else
            {
                w = im->Width;
                h = im->Height;
            }
            *msg->opg_Storage = (msg->opg_AttrID == IA_Width) ? w : h;
            return TRUE;
        }

        case LABEL_Text:
            *msg->opg_Storage = (IPTR)data->ld_Text;
            return TRUE;

        case LABEL_Image:
            *msg->opg_Storage = (IPTR)data->ld_Image;
            return TRUE;

        case LABEL_Justification:
            *msg->opg_Storage = data->ld_Justification;
            return TRUE;

        case LABEL_SoftStyle:
            *msg->opg_Storage = data->ld_SoftStyle;
            return TRUE;

        case LABEL_DisposeImage:
            *msg->opg_Storage = data->ld_DisposeImage;
            return TRUE;

        case LABEL_Mapping:
            *msg->opg_Storage = (IPTR)data->ld_Mapping;
            return TRUE;

        case LABEL_DrawInfo:
            *msg->opg_Storage = (IPTR)data->ld_DrawInfo;
            return TRUE;

        case LABEL_Underscore:
            *msg->opg_Storage = data->ld_Underscore;
            return TRUE;
    }

    return DoSuperMethodA(cl, o, (Msg)msg);
}

/******************************************************************************/

IPTR Label__IM_DRAW(Class *cl, Object *o, struct impDraw *msg)
{
    struct LabelData *data = INST_DATA(cl, o);
    struct Image *im = (struct Image *)o;
    struct RastPort *rp = msg->imp_RPort;
    struct DrawInfo *dri = msg->imp_DrInfo;
    UWORD *pens;
    WORD x, y, w, h;
    UWORD textPen;

    D(bug("[Label] IM_DRAW: state %d, dimensions %dx%d, text '%s'\n", msg->imp_State, im->Width, im->Height, data->ld_Text ? data->ld_Text : (STRPTR)"(null)"));

    if (!rp)
        return FALSE;

    /* Use embedded DrawInfo if msg doesn't provide one */
    if (!dri)
        dri = data->ld_DrawInfo;
    if (!dri)
        return FALSE;

    pens = dri->dri_Pens;
    x = im->LeftEdge + msg->imp_Offset.X;
    y = im->TopEdge + msg->imp_Offset.Y;
    w = im->Width;
    h = im->Height;

    /* Select text pen. Order: explicit override (none in public API), then
     * UIPrefs cap_LabelPen, then DrawInfo state-derived. Disabled state
     * always uses SHADOWPEN regardless of overrides. */
    switch (msg->imp_State)
    {
        case IDS_SELECTED:
        case IDS_INACTIVESELECTED:
            textPen = pens[FILLTEXTPEN];
            break;

        case IDS_DISABLED:
        case IDS_INACTIVEDISABLED:
            textPen = pens[SHADOWPEN];
            break;

        default:
            if (data->ld_PrefsLabelPen)
                textPen = data->ld_PrefsLabelPen;
            else
                textPen = pens[TEXTPEN];
            break;
    }

    SetDrMd(rp, JAM1);

    if (data->ld_Segs)
    {
        struct TextFont *deffont = rp->Font, *oldFont = rp->Font;
        ULONG oldStyle = AskSoftStyle(rp) & rp->AlgoStyle;
        struct LabelSeg *seg = data->ld_Segs, *line;
        WORD tw, th, lw, lh, cx, cy;

        if (!deffont)
            deffont = label_default_font(data);
        label_extent(data, rp, deffont, &tw, &th);
        cy = y + (h > th ? (h - th) / 2 : 0);

        while (seg)
        {
            line = seg;
            seg = label_line(data, line, rp, deffont, &lw, &lh);

            switch (data->ld_Justification)
            {
                case LJ_RIGHT:
                    cx = x + w - lw;
                    break;
                case LJ_CENTER:
                    cx = x + (w > lw ? (w - lw) / 2 : 0);
                    break;
                default:
                    cx = x;
                    break;
            }

            for (; line && line->ls_Type != LSEG_NEWLINE; line = line->ls_Next)
            {
                WORD sw, sh;

                if (line->ls_Type == LSEG_IMAGE)
                {
                    struct Image *lim = (struct Image *)line->ls_Image;

                    label_image_size(line->ls_Image, &sw, &sh);
                    if (line->ls_Mapping && lim->Depth != CUSTOMIMAGEDEPTH)
                        label_draw_mapped(rp, lim, line->ls_Mapping,
                            cx + lim->LeftEdge, cy + (lh - sh) / 2 + lim->TopEdge);
                    else
                        DrawImageState(rp, lim, cx, cy + (lh - sh) / 2,
                            msg->imp_State, dri);
                }
                else
                {
                    WORD ty;
                    UWORD pen = line->ls_Pen ? line->ls_Pen : textPen;

                    label_seg_font(rp, line, deffont);
                    sw = label_text_pixel_width(rp, line->ls_Text, line->ls_Len,
                        data->ld_Underscore);
                    sh = rp->TxHeight ? rp->TxHeight : 8;
                    ty = cy + (lh - sh) / 2 + (rp->TxBaseline ? rp->TxBaseline : sh - 1);

                    if (data->ld_3DLabel
                        && msg->imp_State != IDS_DISABLED
                        && msg->imp_State != IDS_INACTIVEDISABLED)
                    {
                        SetAPen(rp, pens[SHINEPEN]);
                        label_draw_text(rp, line->ls_Text, line->ls_Len,
                            cx + 1, ty + 1, data->ld_Underscore);
                    }
                    SetAPen(rp, pen);
                    label_draw_text(rp, line->ls_Text, line->ls_Len, cx, ty,
                        data->ld_Underscore);
                }
                cx += sw;
            }
            cy += lh + data->ld_VSpacing;
        }

        SetSoftStyle(rp, oldStyle, AskSoftStyle(rp));
        if (oldFont && rp->Font != oldFont)
            SetFont(rp, oldFont);
    }

    /* Draw disabled ghosting pattern */
    if (msg->imp_State == IDS_DISABLED || msg->imp_State == IDS_INACTIVEDISABLED)
    {
        UWORD ghostPat[] = { 0x2222, 0x8888 };

        SetAPen(rp, pens[BLOCKPEN]);
        SetAfPt(rp, ghostPat, 1);
        RectFill(rp, x, y, x + w - 1, y + h - 1);
        SetAfPt(rp, NULL, 0);
    }

    return TRUE;
}
