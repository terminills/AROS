#ifndef AMDGPU_GFX_DISPLAY_H
#define AMDGPU_GFX_DISPLAY_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: amdgpu boot-mode framebuffer Gfx display class data.
*/

#include <exec/types.h>

#define CLID_Hidd_Display_Amdgpu	"hidd.display.amdgpu"
#define IID_Hidd_Display_Amdgpu	"hidd.display.amdgpu"

struct AmdgpuGfxDisplayData
{
    void *pad;
};

#endif /* AMDGPU_GFX_DISPLAY_H */
