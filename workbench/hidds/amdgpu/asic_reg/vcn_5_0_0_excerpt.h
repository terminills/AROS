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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */
/* Excerpt of vcn_5_0_0_offset.h, vcn_5_0_0_sh_mask.h and amdgpu_vcn.h. */

#ifndef AMDGPU_VCN_5_0_0_EXCERPT_H
#define AMDGPU_VCN_5_0_0_EXCERPT_H

#define regUVD_STATUS                               0x0183
#define regUVD_STATUS_BASE_IDX                      1
#define regUVD_VCPU_CNTL                            0x01d6
#define regUVD_VCPU_CNTL_BASE_IDX                   1
#define regUVD_MASTINT_EN                           0x0121
#define regUVD_MASTINT_EN_BASE_IDX                  1
#define regUVD_LMI_CTRL2                            0x032e
#define regUVD_LMI_CTRL2_BASE_IDX                   1
#define regUVD_LMI_CTRL                             0x0330
#define regUVD_LMI_CTRL_BASE_IDX                    1
#define regUVD_GFX10_ADDR_CONFIG                    0x0042
#define regUVD_GFX10_ADDR_CONFIG_BASE_IDX           1
#define regVCN_RB1_DB_CTRL                          0x0072
#define regVCN_RB1_DB_CTRL_BASE_IDX                 1
#define regUVD_RB_BASE_LO                           0x012a
#define regUVD_RB_BASE_LO_BASE_IDX                  1
#define regUVD_RB_BASE_HI                           0x012b
#define regUVD_RB_BASE_HI_BASE_IDX                  1
#define regUVD_RB_SIZE                              0x012c
#define regUVD_RB_SIZE_BASE_IDX                     1
#define regVCN_RB_ENABLE                            0x0085
#define regVCN_RB_ENABLE_BASE_IDX                   1
#define regUVD_RB_RPTR                              0x00ac
#define regUVD_RB_RPTR_BASE_IDX                     1
#define regUVD_RB_WPTR                              0x00ad
#define regUVD_RB_WPTR_BASE_IDX                     1
#define regUVD_POWER_STATUS                         0x0002
#define regUVD_POWER_STATUS_BASE_IDX                1
#define regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW         0x02cf
#define regUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW_BASE_IDX 1
#define regUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH        0x02d0
#define regUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH_BASE_IDX 1
#define regUVD_VCPU_CACHE_OFFSET0                   0x01c0
#define regUVD_VCPU_CACHE_OFFSET0_BASE_IDX          1
#define regUVD_VCPU_CACHE_SIZE0                     0x01c1
#define regUVD_VCPU_CACHE_SIZE0_BASE_IDX            1
#define regUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW        0x02fb
#define regUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW_BASE_IDX 1
#define regUVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH       0x02fc
#define regUVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH_BASE_IDX 1
#define regUVD_VCPU_CACHE_OFFSET1                   0x01c2
#define regUVD_VCPU_CACHE_OFFSET1_BASE_IDX          1
#define regUVD_VCPU_CACHE_SIZE1                     0x01c3
#define regUVD_VCPU_CACHE_SIZE1_BASE_IDX            1
#define regUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW        0x02ff
#define regUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW_BASE_IDX 1
#define regUVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH       0x0300
#define regUVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH_BASE_IDX 1
#define regUVD_VCPU_CACHE_OFFSET2                   0x01c4
#define regUVD_VCPU_CACHE_OFFSET2_BASE_IDX          1
#define regUVD_VCPU_CACHE_SIZE2                     0x01c5
#define regUVD_VCPU_CACHE_SIZE2_BASE_IDX            1
#define regUVD_LMI_VCPU_NC0_64BIT_BAR_LOW           0x02cb
#define regUVD_LMI_VCPU_NC0_64BIT_BAR_LOW_BASE_IDX  1
#define regUVD_LMI_VCPU_NC0_64BIT_BAR_HIGH          0x02cc
#define regUVD_LMI_VCPU_NC0_64BIT_BAR_HIGH_BASE_IDX 1
#define regUVD_VCPU_NONCACHE_OFFSET0                0x01d2
#define regUVD_VCPU_NONCACHE_OFFSET0_BASE_IDX       1
#define regUVD_VCPU_NONCACHE_SIZE0                  0x01d3
#define regUVD_VCPU_NONCACHE_SIZE0_BASE_IDX         1

#define UVD_VCPU_CNTL__CLK_EN_MASK                       0x00000200L
#define UVD_VCPU_CNTL__BLK_RST_MASK                      0x10000000L
#define UVD_MASTINT_EN__VCPU_EN_MASK                     0x00000002L
#define UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK          0x00000100L
#define UVD_LMI_CTRL__MASK_MC_URGENT_MASK                0x00001000L
#define UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK             0x00002000L
#define UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK        0x00200000L
#define VCN_RB_ENABLE__RB1_EN_MASK                       0x00000004L

#define regUVD_DPG_LMA_CTL                          0x0011
#define regUVD_DPG_LMA_CTL_BASE_IDX                 1
#define regUVD_DPG_LMA_DATA                         0x0012
#define regUVD_DPG_LMA_DATA_BASE_IDX                1
#define UVD_DPG_LMA_CTL__READ_WRITE__SHIFT          0x0
#define UVD_DPG_LMA_CTL__READ_WRITE_ADDR__SHIFT     0xe
#define UVD_LMI_CTRL__REQ_MODE_MASK                 0x00000200L
#define UVD_LMI_CTRL__CRC_RESET_MASK                0x00004000L
#define UVD_LMI_CTRL__WRITE_CLEAN_TIMER__SHIFT      0x0
#define UVD_LMI_CTRL2__RE_OFLD_MIF_WR_REQ_NUM__SHIFT 0x11
#define UVD_VCPU_CNTL__PRB_TIMEOUT_VAL__SHIFT       0x14
#define UVD_POWER_STATUS__UVD_POWER_STATUS_MASK     0x00000001L
#define UVD_POWER_STATUS__UVD_PG_MODE_MASK          0x00000004L
#define UVD_POWER_STATUS__UVD_PG_EN_MASK            0x00000100L
#define VCN_RB1_DB_CTRL__OFFSET__SHIFT              0x2
#define VCN_RB1_DB_CTRL__EN_MASK                    0x40000000L

#endif
