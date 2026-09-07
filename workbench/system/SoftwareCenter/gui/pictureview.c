/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: PictureView — datatypes picture scaled into a screen-relative box.
          Load recipe follows muimaster/classes/dtpic.c (NewDTObject with
          PDTA_Screen/PMODE_V43/UseFriendBitMap, DTM_FRAMEBOX,
          DTM_PROCLAYOUT, PDTA_DestBitMap); the scaled copy is made once
          with BitMapScale() and blitted in MUIM_Draw.

    AI_PHASE: SOFTWARE_CENTER_GUI
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: MEDIUM
    AI_NOTE: Dtpic itself is not used (blank-render bug still open on
             hosted, see DESIGN.md).  Everything here runs on the GUI task;
             the file is already local (libpkg downloads it on the worker).
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/datatypes.h>
#include <proto/muimaster.h>

#include <datatypes/pictureclass.h>
#include <graphics/scale.h>
#include <libraries/mui.h>
#include <zune/customclasses.h>

#include <string.h>

#include "pictureview.h"

struct PictureView_DATA
{
    STRPTR                 file;        /* our copy                        */
    CONST_STRPTR           placeholder;
    ULONG                  pw, ph;      /* box as % of screen              */
    ULONG                  boxw, boxh;  /* box in pixels (from Setup)      */
    Object                *dto;
    struct BitMap         *scaled;      /* what MUIM_Draw blits            */
    ULONG                  sw, sh;      /* size of scaled                  */
    struct MUI_EventHandlerNode ehn;
    BOOL setup, shown;            /* MADF_SETUP / MADF_CANDRAW are private */
    BOOL                   pressed;
};

/* ---------------------------------------------------------------------- */

static void free_picture(struct PictureView_DATA *data)
{
    if (data->scaled)
    {
        FreeBitMap(data->scaled);
        data->scaled = NULL;
    }
    if (data->dto)
    {
        DisposeDTObject(data->dto);
        data->dto = NULL;
    }
}

/* Load data->file and produce data->scaled fitting boxw x boxh (aspect
   preserved, never upscaled beyond 1:1 — screenshots look wrong blown up).
   Requires the object to be set up (needs _screen). */
static BOOL load_picture(Object *obj, struct PictureView_DATA *data)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwinptr;
    struct FrameInfo fri;
    struct BitMapHeader *bmhd = NULL;
    struct BitMap *src = NULL;
    ULONG w, h, sw, sh, depth;

    free_picture(data);
    if (!data->file || !*data->file) return FALSE;
    if (!data->boxw || !data->boxh) return FALSE;

    /* no "please insert volume" requesters for a missing cache file */
    oldwinptr = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    data->dto = NewDTObject(data->file,
                            DTA_GroupID,          GID_PICTURE,
                            OBP_Precision,        PRECISION_IMAGE,
                            PDTA_Screen,          (IPTR)_screen(obj),
                            PDTA_DestMode,        PMODE_V43,
                            PDTA_UseFriendBitMap, TRUE,
                            TAG_DONE);
    me->pr_WindowPtr = oldwinptr;
    if (!data->dto) return FALSE;

    memset(&fri, 0, sizeof fri);
    DoMethod(data->dto, DTM_FRAMEBOX, NULL, (IPTR)&fri, (IPTR)&fri, sizeof fri, 0);
    if (fri.fri_Dimensions.Depth == 0 || !DoMethod(data->dto, DTM_PROCLAYOUT, NULL, 1))
        goto fail;

    GetDTAttrs(data->dto, PDTA_BitMapHeader, (IPTR)&bmhd, TAG_DONE);
    GetDTAttrs(data->dto, PDTA_DestBitMap, (IPTR)&src, TAG_DONE);
    if (!src) GetDTAttrs(data->dto, PDTA_BitMap, (IPTR)&src, TAG_DONE);
    if (!bmhd || !src) goto fail;

    w = bmhd->bmh_Width;
    h = bmhd->bmh_Height;
    if (!w || !h) goto fail;

    /* fit into the box, keep aspect, never enlarge */
    sw = w; sh = h;
    if (sw > data->boxw) { sh = sh * data->boxw / sw; sw = data->boxw; }
    if (sh > data->boxh) { sw = sw * data->boxh / sh; sh = data->boxh; }
    if (!sw) sw = 1;
    if (!sh) sh = 1;

    depth = GetBitMapAttr(_screen(obj)->RastPort.BitMap, BMA_DEPTH);
    data->scaled = AllocBitMap(sw, sh, depth, BMF_MINPLANES, _screen(obj)->RastPort.BitMap);
    if (!data->scaled) goto fail;

    if (sw == w && sh == h)
    {
        BltBitMap(src, 0, 0, data->scaled, 0, 0, w, h, 0xC0, 0xFF, NULL);
    }
    else
    {
        struct BitScaleArgs bsa;
        memset(&bsa, 0, sizeof bsa);
        bsa.bsa_SrcX = 0;        bsa.bsa_SrcY = 0;
        bsa.bsa_SrcWidth = w;    bsa.bsa_SrcHeight = h;
        bsa.bsa_DestX = 0;       bsa.bsa_DestY = 0;
        bsa.bsa_XSrcFactor = w;  bsa.bsa_XDestFactor = sw;
        bsa.bsa_YSrcFactor = h;  bsa.bsa_YDestFactor = sh;
        bsa.bsa_SrcBitMap = src;
        bsa.bsa_DestBitMap = data->scaled;
        BitMapScale(&bsa);
    }
    data->sw = sw;
    data->sh = sh;

    /* the datatype object is only needed for its bitmap; drop it now */
    DisposeDTObject(data->dto);
    data->dto = NULL;
    return TRUE;

fail:
    free_picture(data);
    return FALSE;
}

/* ---------------------------------------------------------------------- */

static IPTR PictureView__OM_NEW(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct PictureView_DATA *data;
    struct TagItem *tags, *tag;

    obj = (Object *)DoSuperNewTags(cl, obj, NULL,
                                   MUIA_Frame,     MUIV_Frame_ImageButton,
                                   MUIA_InputMode, MUIV_InputMode_None,
                                   MUIA_FillArea,  TRUE,
                                   TAG_MORE, (IPTR)msg->ops_AttrList);
    if (!obj) return 0;

    data = INST_DATA(cl, obj);
    data->pw = 20;
    data->ph = 20;
    data->placeholder = (CONST_STRPTR)"No screenshot";

    tags = msg->ops_AttrList;
    while ((tag = NextTagItem(&tags)))
    {
        switch (tag->ti_Tag)
        {
        case MUIA_PictureView_File:
            data->file = tag->ti_Data ? StrDup((CONST_STRPTR)tag->ti_Data) : NULL;
            break;
        case MUIA_PictureView_PercentW:  data->pw = tag->ti_Data; break;
        case MUIA_PictureView_PercentH:  data->ph = tag->ti_Data; break;
        case MUIA_PictureView_Placeholder:
            data->placeholder = (CONST_STRPTR)tag->ti_Data;
            break;
        }
    }

    data->ehn.ehn_Events   = IDCMP_MOUSEBUTTONS;
    data->ehn.ehn_Priority = 0;
    data->ehn.ehn_Flags    = 0;
    data->ehn.ehn_Object   = obj;
    data->ehn.ehn_Class    = cl;
    return (IPTR)obj;
}

static IPTR PictureView__OM_DISPOSE(struct IClass *cl, Object *obj, Msg msg)
{
    struct PictureView_DATA *data = INST_DATA(cl, obj);

    free_picture(data);
    if (data->file) FreeVec(data->file);
    return DoSuperMethodA(cl, obj, msg);
}

static IPTR PictureView__OM_SET(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct PictureView_DATA *data = INST_DATA(cl, obj);
    struct TagItem *tags = msg->ops_AttrList, *tag;
    BOOL redraw = FALSE;

    while ((tag = NextTagItem(&tags)))
    {
        switch (tag->ti_Tag)
        {
        case MUIA_PictureView_File:
        {
            CONST_STRPTR nf = (CONST_STRPTR)tag->ti_Data;
            if ((nf == NULL && data->file == NULL) ||
                (nf && data->file && strcmp((const char *)nf, (const char *)data->file) == 0))
                break;                                   /* unchanged */
            if (data->file) FreeVec(data->file);
            data->file = nf ? StrDup(nf) : NULL;
            if (data->setup)
                load_picture(obj, data);
            else
                free_picture(data);
            redraw = TRUE;
            break;
        }
        case MUIA_PictureView_Placeholder:
            data->placeholder = (CONST_STRPTR)tag->ti_Data;
            redraw = TRUE;
            break;
        case MUIA_PictureView_Pressed:
            data->pressed = tag->ti_Data ? TRUE : FALSE;
            break;
        }
    }
    if (redraw && data->shown)
        MUI_Redraw(obj, MADF_DRAWOBJECT);
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static IPTR PictureView__OM_GET(struct IClass *cl, Object *obj, struct opGet *msg)
{
    struct PictureView_DATA *data = INST_DATA(cl, obj);

    switch (msg->opg_AttrID)
    {
    case MUIA_PictureView_File:    *msg->opg_Storage = (IPTR)data->file; return TRUE;
    case MUIA_PictureView_Pressed: *msg->opg_Storage = data->pressed; return TRUE;
    case MUIA_PictureView_Loaded:  *msg->opg_Storage = data->scaled != NULL; return TRUE;
    }
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static IPTR PictureView__MUIM_Setup(struct IClass *cl, Object *obj, struct MUIP_Setup *msg)
{
    struct PictureView_DATA *data = INST_DATA(cl, obj);

    if (!DoSuperMethodA(cl, obj, (Msg)msg)) return FALSE;

    /* box = fraction of the screen we are shown on: same *look* at 800x600
       and 4K, and the large view always fits the screen */
    data->boxw = _screen(obj)->Width  * data->pw / 100;
    data->boxh = _screen(obj)->Height * data->ph / 100;
    if (data->boxw < 32) data->boxw = 32;
    if (data->boxh < 32) data->boxh = 32;

    load_picture(obj, data);
    DoMethod(_win(obj), MUIM_Window_AddEventHandler, (IPTR)&data->ehn);
    data->setup = TRUE;
    return TRUE;
}

static IPTR PictureView__MUIM_Cleanup(struct IClass *cl, Object *obj, struct MUIP_Cleanup *msg)
{
    struct PictureView_DATA *data = INST_DATA(cl, obj);

    data->setup = FALSE;
    DoMethod(_win(obj), MUIM_Window_RemEventHandler, (IPTR)&data->ehn);
    free_picture(data);
    return DoSuperMethodA(cl, obj, (Msg)msg);
}

static IPTR PictureView__MUIM_Show(struct IClass *cl, Object *obj, Msg msg)
{
    struct PictureView_DATA *data = INST_DATA(cl, obj);
    IPTR rc = DoSuperMethodA(cl, obj, msg);
    data->shown = TRUE;
    return rc;
}

static IPTR PictureView__MUIM_Hide(struct IClass *cl, Object *obj, Msg msg)
{
    struct PictureView_DATA *data = INST_DATA(cl, obj);
    data->shown = FALSE;
    return DoSuperMethodA(cl, obj, msg);
}

static IPTR PictureView__MUIM_AskMinMax(struct IClass *cl, Object *obj, struct MUIP_AskMinMax *msg)
{
    struct PictureView_DATA *data = INST_DATA(cl, obj);

    DoSuperMethodA(cl, obj, (Msg)msg);
    msg->MinMaxInfo->MinWidth  += data->boxw;
    msg->MinMaxInfo->MinHeight += data->boxh;
    msg->MinMaxInfo->DefWidth  += data->boxw;
    msg->MinMaxInfo->DefHeight += data->boxh;
    msg->MinMaxInfo->MaxWidth  += data->boxw;
    msg->MinMaxInfo->MaxHeight += data->boxh;
    return 0;
}

static IPTR PictureView__MUIM_Draw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    struct PictureView_DATA *data = INST_DATA(cl, obj);

    DoSuperMethodA(cl, obj, (Msg)msg);
    if (!(msg->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))) return 0;

    if (data->scaled)
    {
        LONG x = _mleft(obj) + ((LONG)_mwidth(obj)  - (LONG)data->sw) / 2;
        LONG y = _mtop(obj)  + ((LONG)_mheight(obj) - (LONG)data->sh) / 2;
        BltBitMapRastPort(data->scaled, 0, 0, _rp(obj), x, y, data->sw, data->sh, 0xC0);
    }
    else
    {
        /* A file that did not decode is worth saying out loud: the usual
           cause is a missing picture datatype (AddDataTypes not run), not a
           bad screenshot, and a silently blank box hides that. */
        CONST_STRPTR t = (data->file && data->setup) ? (CONST_STRPTR)"Cannot display image"
                                                     : data->placeholder;
        if (t && *t)
        {
            struct RastPort *rp = _rp(obj);
            LONG len = strlen((const char *)t);
            LONG tw;
            SetFont(rp, _font(obj));
            tw = TextLength(rp, t, len);
            SetABPenDrMd(rp, _pens(obj)[MPEN_TEXT], 0, JAM1);
            Move(rp, _mleft(obj) + ((LONG)_mwidth(obj) - tw) / 2,
                     _mtop(obj) + (_mheight(obj) + _font(obj)->tf_Baseline) / 2);
            Text(rp, t, len);
        }
    }
    return 0;
}

static IPTR PictureView__MUIM_HandleEvent(struct IClass *cl, Object *obj, struct MUIP_HandleEvent *msg)
{
    if (msg->imsg && msg->imsg->Class == IDCMP_MOUSEBUTTONS &&
        msg->imsg->Code == SELECTDOWN &&
        msg->imsg->MouseX >= _mleft(obj) && msg->imsg->MouseX <= _mright(obj) &&
        msg->imsg->MouseY >= _mtop(obj)  && msg->imsg->MouseY <= _mbottom(obj))
    {
        set(obj, MUIA_PictureView_Pressed, TRUE);
        return MUI_EventHandlerRC_Eat;
    }
    return 0;
}

ZUNE_CUSTOMCLASS_11
(
    PictureView, NULL, MUIC_Area, NULL,
    OM_NEW,           struct opSet *,
    OM_DISPOSE,       Msg,
    OM_SET,           struct opSet *,
    OM_GET,           struct opGet *,
    MUIM_Setup,       struct MUIP_Setup *,
    MUIM_Cleanup,     struct MUIP_Cleanup *,
    MUIM_Show,        Msg,
    MUIM_Hide,        Msg,
    MUIM_AskMinMax,   struct MUIP_AskMinMax *,
    MUIM_Draw,        struct MUIP_Draw *,
    MUIM_HandleEvent, struct MUIP_HandleEvent *
);
