/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Reaction label.image - Internal definitions
*/

#ifndef LABEL_INTERN_H
#define LABEL_INTERN_H

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/imageclass.h>
#include <images/label.h>

#ifdef __AROS__
#include <aros/debug.h>
#endif

#include LC_LIBDEFS_FILE

#include <exec/libraries.h>

/* Module library base with stored class pointer */
struct LabelBase_intern
{
    struct Library lib;
    Class *rc_Class;
};

#define LSEG_TEXT    0
#define LSEG_IMAGE   1
#define LSEG_NEWLINE 2

/* One piece of the label: a run of text, an image or a line break */
struct LabelSeg
{
    struct LabelSeg *ls_Next;
    UBYTE            ls_Type;
    UBYTE            ls_SoftStyle;
    UWORD            ls_Pen;            /* text pen, 0 = default */
    struct TextFont *ls_Font;           /* text font, NULL = default */
    Object          *ls_Image;
    UWORD           *ls_Mapping;        /* pen map for a plain image */
    ULONG            ls_Len;
    char             ls_Text[0];
};

struct LabelData
{
    struct LabelSeg *ld_Segs;           /* label contents, in tag order */
    STRPTR          ld_Text;            /* first text, for OM_GET */
    Object         *ld_Image;           /* first image, for OM_GET */
    ULONG           ld_Justification;   /* LJ_LEFT, LJ_CENTER, LJ_RIGHT */
    ULONG           ld_SoftStyle;       /* Text style flags */
    BOOL            ld_DisposeImage;    /* Dispose image on OM_DISPOSE */
    UWORD          *ld_Mapping;         /* Pen mapping */
    struct DrawInfo *ld_DrawInfo;       /* DrawInfo */
    UBYTE           ld_Underscore;      /* Underscore character */
    UWORD           ld_VSpacing;        /* LABEL_VerticalSpacing */
    UWORD           ld_TextPen;         /* Override text pen (0 = use prefs/dri) */
    BOOL            ld_TextPenSet;
    BOOL            ld_3DLabel;         /* cap_3DLabel snapshot */
    UWORD           ld_PrefsLabelPen;   /* cap_LabelPen snapshot */
    struct TextFont *ld_Fonts[4];       /* fonts opened for IA_Font */
};

#endif /* LABEL_INTERN_H */
