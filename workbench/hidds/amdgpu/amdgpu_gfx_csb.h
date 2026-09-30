/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Adapted from Linux f377d0025eb0 clearstate_gfx12.h and
 * gfx_v12_0_get_csb_buffer. Six context extents, all 62 defaults zero.
 * Format is cluster count, then {register count, index, defaults...}; NOT PM4.
 * Host regression compares every output dword against the upstream table.
 */
#ifndef AMDGPU_GFX_CSB_H
#define AMDGPU_GFX_CSB_H
#define GFX12_CSB_DWORDS 75UL
#define GFX12_CSB_OFFSET 32768UL
#define GFX12_CSB_PAGE_BYTES 4096UL

static inline ULONG gfx12_build_csb(ULONG *buffer, ULONG capacity_dwords)
{
    static const struct { ULONG index, count; } extents[] = {
        { 0xa03e, 34 }, { 0xa0cc, 2 }, { 0xa0d8, 1 },
        { 0xa0db, 6 }, { 0xa2e5, 11 }, { 0xa3c0, 8 }
    };
    ULONG i, pos = 1;
    if (!buffer || capacity_dwords < GFX12_CSB_DWORDS) return 0;
    for (i = 0; i < GFX12_CSB_DWORDS; i++) buffer[i] = 0;
    buffer[0] = 6;
    for (i = 0; i < 6; i++) {
        buffer[pos++] = extents[i].count;
        buffer[pos++] = extents[i].index;
        pos += extents[i].count;
    }
    return pos;
}
#endif
