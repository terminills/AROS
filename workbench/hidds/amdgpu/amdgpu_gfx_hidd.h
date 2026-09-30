#ifndef AMDGPU_GFX_HIDD_H
#define AMDGPU_GFX_HIDD_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: amdgpu boot-mode framebuffer Gfx Hidd data.
*/

#include <exec/interrupts.h>

#include "amdgpu_gfx_bitmap.h"
#include "amdgpu_gfx_support.h"

#define IID_Hidd_Gfx_Amdgpu  "hidd.gfx.amdgpu"
#define CLID_Hidd_Gfx_Amdgpu "hidd.gfx.amdgpu"

struct AmdgpuGfxHiddData
{
    struct Interrupt ResetInterrupt;
};

#endif /* AMDGPU_GFX_HIDD_H */
