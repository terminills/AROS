/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Reaction speedbar.gadget - Internal definitions
*/

#ifndef SPEEDBAR_INTERN_H
#define SPEEDBAR_INTERN_H

#include <exec/types.h>
#include <exec/lists.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/gadgetclass.h>
#include <gadgets/speedbar.h>

#ifdef __AROS__
#include <aros/debug.h>
#endif

#include LC_LIBDEFS_FILE

#include <exec/libraries.h>

/* Module library base with stored class pointer */
struct SpeedBarBase_intern
{
    struct Library lib;
    Class *rc_Class;
};

#define G(obj)  ((struct Gadget *)(obj))

/* A speed button, as made by AllocSpeedButtonNodeA() */
struct SpeedButtonNode
{
    struct Node sbn_Node;
    UWORD       sbn_Number;
    Object     *sbn_Image;
    Object     *sbn_SelImage;
    STRPTR      sbn_Help;
    APTR        sbn_UserData;
    WORD        sbn_Left;
    WORD        sbn_Top;
    WORD        sbn_Width;          /* 0 = from the image */
    WORD        sbn_Height;
    WORD        sbn_Spacing;
    UWORD       sbn_Highlight;      /* SBH_* */
    ULONG       sbn_MXGroup;        /* 0 = none */
    ULONG       sbn_Flags;
};

#define SBNF_DISABLED  (1 << 0)
#define SBNF_TOGGLE    (1 << 1)
#define SBNF_SELECTED  (1 << 2)

void speedbar_node_set(struct SpeedButtonNode *sbn, struct TagItem *tags);

struct SpeedBarData
{
    struct List     *sd_Buttons;        /* List of SpeedButtonNodes */
    ULONG           sd_Orientation;     /* SBORIENT_HORIZ or SBORIENT_VERT */
    ULONG           sd_BevelStyle;      /* Bevel style for buttons */
    struct Window   *sd_Window;         /* Parent window */
    BOOL            sd_EvenSize;        /* Size all buttons equally */
    BOOL            sd_StrumBar;        /* Pressing follows the pointer */
    LONG            sd_Background;      /* Pen, -1 = window background */
    LONG            sd_Selected;        /* Number of the last button released */
    struct SpeedButtonNode *sd_SelectedNode;
    struct SpeedButtonNode *sd_Active;  /* Button held down */
    BOOL            sd_Pressed;         /* Pointer still over sd_Active */
};

#endif /* SPEEDBAR_INTERN_H */
