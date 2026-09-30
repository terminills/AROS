/*
 * Copyright (C) 2020  Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * AROS VENDORED EXCERPT (acd-project: keep-current recipe)
 *
 * Source: torvalds/linux drivers/gpu/drm/amd/include/asic_reg/nbio/
 *         nbio_2_3_offset.h  (register offsets, MIT-licensed by AMD)
 *       + drivers/gpu/drm/amd/include/navi10_ip_offset.h (IP base table)
 * Fetched: 2026-07-18 from master.
 *
 * Only the registers this driver consumes are excerpted; refresh by
 * re-fetching the upstream files and re-extracting. NBIO 2.3 covers
 * Navi 1x/2x — including the Radeon Pro V620 (Navi 21).
 */

#ifndef AROS_NBIO_2_3_EXCERPT_H
#define AROS_NBIO_2_3_EXCERPT_H

/* navi10_ip_offset.h: NBIO_BASE instance 0 segment table */
#define NBIO_2_3_BASE__SEG0     0x00000000
#define NBIO_2_3_BASE__SEG1     0x00000014
#define NBIO_2_3_BASE__SEG2     0x00000D20
#define NBIO_2_3_BASE__SEG3     0x00010400

/* nbio_2_3_offset.h */
#define mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE              0x00c3
#define mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE_BASE_IDX     2

/* Resolved absolute dword offset, as the kernel's
   nbio_v2_3_get_memsize() computes via RREG32_SOC15(NBIO, 0, ...):
   base segment 2 + register offset. Value read back is VRAM size in
   megabytes. */
#define NBIO_2_3_RCC_CONFIG_MEMSIZE_DW \
    (NBIO_2_3_BASE__SEG2 + mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE)

/* The same register expressed RELATIVE to an NBIO base resolved from
   the IP discovery table (GFX11/12, where the base is not fixed).
   base + this == the absolute dword. */
#define AMDGPU_mmRCC_CONFIG_MEMSIZE_REL     mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE

#endif /* AROS_NBIO_2_3_EXCERPT_H */
