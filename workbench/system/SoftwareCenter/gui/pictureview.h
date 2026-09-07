/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: PictureView — Zune Area subclass that shows a datatypes picture
          (PNG screenshot from the repository cache) scaled to fit a box
          whose size is a fraction of the *screen*, so it is resolution
          independent.  Clicking it sets MUIA_PictureView_Pressed so the
          owner can open the large view.  See DESIGN.md §1.2.

    AI_PHASE: SOFTWARE_CENTER_GUI
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: MEDIUM
*/

#ifndef SC_PICTUREVIEW_H
#define SC_PICTUREVIEW_H

#include <exec/types.h>
#include <intuition/classusr.h>
#include <libraries/mui.h>

#define MUIB_PictureView            (TAG_USER | 0x5C700000)

/* [IS.] STRPTR  — file to show; NULL/"" = show placeholder text.
   The string is copied. */
#define MUIA_PictureView_File       (MUIB_PictureView | 0x01)
/* [I..] ULONG   — box width  = screen width  * Num / 100 */
#define MUIA_PictureView_PercentW   (MUIB_PictureView | 0x02)
/* [I..] ULONG   — box height = screen height * Num / 100 */
#define MUIA_PictureView_PercentH   (MUIB_PictureView | 0x03)
/* [..G] BOOL    — set TRUE on every mouse click (for MUIM_Notify) */
#define MUIA_PictureView_Pressed    (MUIB_PictureView | 0x04)
/* [IS.] STRPTR  — text drawn when no picture is loaded (not copied) */
#define MUIA_PictureView_Placeholder (MUIB_PictureView | 0x05)
/* [..G] BOOL    — TRUE when a picture is currently loaded */
#define MUIA_PictureView_Loaded     (MUIB_PictureView | 0x06)

/* Created by ZUNE_CUSTOMCLASS autoinit (customclasses.h); datatypes.library
   is auto-opened by the startup code. */
extern struct MUI_CustomClass *PictureView_CLASS;

#define PictureViewObject NewObject(PictureView_CLASS->mcc_Class, NULL

#endif /* SC_PICTUREVIEW_H */
