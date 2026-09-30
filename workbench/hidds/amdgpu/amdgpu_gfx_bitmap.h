/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef AMDGPU_GFX_BITMAP_H
#define AMDGPU_GFX_BITMAP_H

#include <hidd/gfx.h>

#define CLID_Hidd_BitMap_Amdgpu     "hidd.bitmap.amdgpu"
#define IID_Hidd_BitMap_Amdgpu      "hidd.bitmap.amdgpu"

#define IS_BM_ATTR(attr, idx) ( ( (idx) = (attr) - HiddBitMapAttrBase) < num_Hidd_BitMap_Attrs)

/*
   This structure is used as instance data for the bitmap class.
*/
struct AmdgpuGfxBitMapData
{
    UBYTE   	    	*VideoData;	/* Pointing to video data */
    LONG		width;		/* Bitmap size */
    LONG   	    	height;
    UBYTE   	    	bytesperpix;
    ULONG   	    	bytesperline;
    UBYTE *   	    	DAC;   		/* Hardware palette registers */
    BYTE    	    	bpp;         	/* Cached bits per pixel */
    BYTE    	    	disp;        	/* !=0 - displayable */
    OOP_Object	    	*pixfmtobj;	/* Cached pixelformat object */
    OOP_Object	    	*displayhidd;	/* display object */
    LONG		disp_width;	/* Display size */
    LONG		disp_height;
    /* amdgpu_sdma.c — GART VA of VideoData, mapped once on first accelerated
       refresh and reused. 0 = not mapped / accel unavailable. Mapping per
       refresh would cost a TLB invalidate on both hubs every frame. */
    UQUAD		gart_va;
    ULONG		gart_size;
    /* MC address of VideoData when this bitmap lives in VRAM (allocated from
       the pool by amdgpu_vram_alloc). 0 = system-RAM bitmap, which still needs
       the copy-forward refresh path. A VRAM-resident bitmap can instead be put
       on display by pointing the scanout straight at this address. */
    UQUAD		vram_mc;
    ULONG		vram_size;	/* bytes allocated from the VRAM pool (for free on Dispose); 0 = none */
    LONG		xoffset;	/* Bitmap offset */
    LONG		yoffset;
};

#endif /* AMDGPU_GFX_BITMAP_H */
