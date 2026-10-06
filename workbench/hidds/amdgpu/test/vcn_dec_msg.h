/*
 * VCN decode message layouts, from Mesa src/amd/common/ac_vcn_dec.h.
 *
 * Copyright 2017 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VCN_DEC_MSG_H
#define VCN_DEC_MSG_H

#define RDECODE_IB_PARAM_DECODE_BUFFER                               (0x00000001)
#define RDECODE_CMDBUF_FLAGS_MSG_BUFFER                              (0x00000001)
#define RDECODE_CMDBUF_FLAGS_DPB_BUFFER                              (0x00000002)
#define RDECODE_CMDBUF_FLAGS_BITSTREAM_BUFFER                        (0x00000004)
#define RDECODE_CMDBUF_FLAGS_DECODING_TARGET_BUFFER                  (0x00000008)
#define RDECODE_CMDBUF_FLAGS_FEEDBACK_BUFFER                         (0x00000010)
#define RDECODE_CMDBUF_FLAGS_IT_SCALING_BUFFER                       (0x00000200)
#define RDECODE_CMDBUF_FLAGS_CONTEXT_BUFFER                          (0x00000800)
#define RDECODE_CMDBUF_FLAGS_SESSION_CONTEXT_BUFFER                  (0x00100000)
#define RDECODE_MSG_CREATE                                  0x00000000
#define RDECODE_MSG_DECODE                                  0x00000001
#define RDECODE_MSG_DESTROY                                 0x00000002
#define RDECODE_CODEC_H264_PERF                             0x00000007
#define RDECODE_ARRAY_MODE_ADDRLIB_SEL_GFX11                0x00000003
#define RDECODE_H264_PROFILE_BASELINE                       0x00000000
#define RDECODE_H264_PROFILE_MAIN                           0x00000001
#define RDECODE_H264_PROFILE_HIGH                           0x00000002
#define RDECODE_SW_MODE_LINEAR                              0x00000000
#define RDECODE_VCN5_256B_D                                 0x00000001
#define RDECODE_MESSAGE_CREATE                              0x00000001
#define RDECODE_MESSAGE_DECODE                              0x00000002
#define RDECODE_MESSAGE_AVC                                 0x00000006
#define RDECODE_SPS_INFO_H264_DIRECT_8X8_INFERENCE_FLAG_SHIFT            0
#define RDECODE_SPS_INFO_H264_MB_ADAPTIVE_FRAME_FIELD_FLAG_SHIFT         1
#define RDECODE_SPS_INFO_H264_FRAME_MBS_ONLY_FLAG_SHIFT                  2
#define RDECODE_SPS_INFO_H264_DELTA_PIC_ORDER_ALWAYS_ZERO_FLAG_SHIFT     3
#define RDECODE_SPS_INFO_H264_GAPS_IN_FRAME_NUM_VALUE_ALLOWED_FLAG_SHIFT 5
#define RDECODE_SPS_INFO_H264_EXTENSION_SUPPORT_FLAG_SHIFT               7
#define RDECODE_SESSION_CONTEXT_SIZE (128 * 1024)

#define RADEON_VCN_ENGINE_INFO          (0x30000001)
#define RADEON_VCN_ENGINE_TYPE_DECODE   (0x00000003)
#define RADEON_VCN_ENGINE_INFO_SIZE     (0x00000010)

typedef struct rvcn_dec_message_index_s {
   unsigned int message_id;
   unsigned int offset;
   unsigned int size;
   unsigned int filled;
} rvcn_dec_message_index_t;

typedef struct rvcn_dec_message_header_s {
   unsigned int header_size;
   unsigned int total_size;
   unsigned int num_buffers;
   unsigned int msg_type;
   unsigned int stream_handle;
   unsigned int status_report_feedback_number;

   rvcn_dec_message_index_t index[1];
} rvcn_dec_message_header_t;

typedef struct rvcn_dec_message_create_s {
   unsigned int stream_type;
   unsigned int session_flags;
   unsigned int width_in_samples;
   unsigned int height_in_samples;
} rvcn_dec_message_create_t;

typedef struct rvcn_dec_message_decode_s {
   unsigned int stream_type;
   unsigned int decode_flags;
   unsigned int width_in_samples;
   unsigned int height_in_samples;

   unsigned int bsd_size;
   unsigned int dpb_size;
   unsigned int dt_size;
   unsigned int sct_size;
   unsigned int sc_coeff_size;
   unsigned int hw_ctxt_size;
   unsigned int sw_ctxt_size;
   unsigned int pic_param_size;
   unsigned int mb_cntl_size;
   unsigned int reserved0[4];
   unsigned int decode_buffer_flags;

   unsigned int db_pitch;
   unsigned int db_aligned_height;
   unsigned int db_tiling_mode;
   unsigned int db_swizzle_mode;
   unsigned int db_array_mode;
   unsigned int db_field_mode;
   unsigned int db_surf_tile_config;

   unsigned int dt_pitch;
   unsigned int dt_uv_pitch;
   unsigned int dt_tiling_mode;
   unsigned int dt_swizzle_mode;
   unsigned int dt_array_mode;
   unsigned int dt_field_mode;
   unsigned int dt_out_format;
   unsigned int dt_surf_tile_config;
   unsigned int dt_uv_surf_tile_config;
   unsigned int dt_luma_top_offset;
   unsigned int dt_luma_bottom_offset;
   unsigned int dt_chroma_top_offset;
   unsigned int dt_chroma_bottom_offset;
   unsigned int dt_chromaV_top_offset;
   unsigned int dt_chromaV_bottom_offset;

   unsigned int mif_wrc_en;
   unsigned int db_pitch_uv;

   unsigned char reserved1[20];
} rvcn_dec_message_decode_t;

typedef struct {
   unsigned short viewOrderIndex;
   unsigned short viewId;
   unsigned short numOfAnchorRefsInL0;
   unsigned short viewIdOfAnchorRefsInL0[15];
   unsigned short numOfAnchorRefsInL1;
   unsigned short viewIdOfAnchorRefsInL1[15];
   unsigned short numOfNonAnchorRefsInL0;
   unsigned short viewIdOfNonAnchorRefsInL0[15];
   unsigned short numOfNonAnchorRefsInL1;
   unsigned short viewIdOfNonAnchorRefsInL1[15];
} radeon_mvcElement_t;

typedef struct rvcn_dec_message_avc_s {
   unsigned int profile;
   unsigned int level;

   unsigned int sps_info_flags;
   unsigned int pps_info_flags;
   unsigned char chroma_format;
   unsigned char bit_depth_luma_minus8;
   unsigned char bit_depth_chroma_minus8;
   unsigned char log2_max_frame_num_minus4;

   unsigned char pic_order_cnt_type;
   unsigned char log2_max_pic_order_cnt_lsb_minus4;
   unsigned char num_ref_frames;
   unsigned char reserved_8bit;

   signed char pic_init_qp_minus26;
   signed char pic_init_qs_minus26;
   signed char chroma_qp_index_offset;
   signed char second_chroma_qp_index_offset;

   unsigned char num_slice_groups_minus1;
   unsigned char slice_group_map_type;
   unsigned char num_ref_idx_l0_active_minus1;
   unsigned char num_ref_idx_l1_active_minus1;

   unsigned short slice_group_change_rate_minus1;
   unsigned short reserved_16bit_1;

   unsigned char scaling_list_4x4[6][16];
   unsigned char scaling_list_8x8[2][64];

   unsigned int frame_num;
   unsigned int frame_num_list[16];
   int curr_field_order_cnt_list[2];
   int field_order_cnt_list[16][2];

   unsigned int decoded_pic_idx;
   unsigned int curr_pic_ref_frame_num;
   unsigned char ref_frame_list[16];

   unsigned int reserved[122];

   struct {
      unsigned int numViews;
      unsigned int viewId0;
      radeon_mvcElement_t mvcElements[1];
   } mvc;

   unsigned short non_existing_frame_flags;
   unsigned int used_for_reference_flags;
} rvcn_dec_message_avc_t;

typedef struct rvcn_dec_feature_index_s {
   unsigned int feature_id;
   unsigned int offset;
   unsigned int size;
   unsigned int filled;
} rvcn_dec_feature_index_t;

typedef struct rvcn_dec_feedback_header_s {
   unsigned int header_size;
   unsigned int total_size;
   unsigned int num_buffers;
   unsigned int status_report_feedback_number;
   unsigned int status;
   unsigned int value;
   unsigned int errorBits;
   rvcn_dec_feature_index_t index[1];
} rvcn_dec_feedback_header_t;

typedef struct rvcn_decode_buffer_s {
   unsigned int valid_buf_flag;
   unsigned int msg_buffer_address_hi;
   unsigned int msg_buffer_address_lo;
   unsigned int dpb_buffer_address_hi;
   unsigned int dpb_buffer_address_lo;
   unsigned int target_buffer_address_hi;
   unsigned int target_buffer_address_lo;
   unsigned int session_contex_buffer_address_hi;
   unsigned int session_contex_buffer_address_lo;
   unsigned int bitstream_buffer_address_hi;
   unsigned int bitstream_buffer_address_lo;
   unsigned int context_buffer_address_hi;
   unsigned int context_buffer_address_lo;
   unsigned int feedback_buffer_address_hi;
   unsigned int feedback_buffer_address_lo;
   unsigned int luma_hist_buffer_address_hi;
   unsigned int luma_hist_buffer_address_lo;
   unsigned int prob_tbl_buffer_address_hi;
   unsigned int prob_tbl_buffer_address_lo;
   unsigned int sclr_coeff_buffer_address_hi;
   unsigned int sclr_coeff_buffer_address_lo;
   unsigned int it_sclr_table_buffer_address_hi;
   unsigned int it_sclr_table_buffer_address_lo;
   unsigned int sclr_target_buffer_address_hi;
   unsigned int sclr_target_buffer_address_lo;
   unsigned int subsample_hi;
   unsigned int subsample_lo;
   unsigned int mpeg2_pic_param_buffer_address_hi;
   unsigned int mpeg2_pic_param_buffer_address_lo;
   unsigned int mpeg2_mb_control_buffer_address_hi;
   unsigned int mpeg2_mb_control_buffer_address_lo;
   unsigned int mpeg2_idct_coeff_buffer_address_hi;
   unsigned int mpeg2_idct_coeff_buffer_address_lo;
} rvcn_decode_buffer_t;

typedef struct rvcn_decode_ib_package_s {
   unsigned int package_size;
   unsigned int package_type;
} rvcn_decode_ib_package_t;

#endif
