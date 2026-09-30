/*
 * Copyright 2019 Advanced Micro Devices, Inc.
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
 */

/*
 * AROS VENDORED EXCERPT (acd-project: keep-current recipe)
 *
 * Source: torvalds/linux drivers/gpu/drm/amd/include/discovery.h
 *         (binary/table struct layout, MIT-licensed by AMD)
 *       + drivers/gpu/drm/amd/include/soc15_hw_ip.h (HW_ID values)
 *       + drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.{c,h} (TMR consts,
 *         MM_INDEX/MM_DATA indirect VRAM read, memsize register)
 * Fetched: 2026-07-18 from master.
 *
 * The IP-discovery table is a binary blob AMD's firmware leaves near
 * the top of VRAM. It enumerates every IP block instance and its
 * register base address(es). Parsing it is how the driver stops
 * hardcoding per-ASIC offset tables — the single mechanism that lets
 * one driver body track new generations (GFX11/12 and beyond) by data
 * rather than code. This is the concrete form of the "keep current"
 * rationale in ACD-amdgpu-driver-stack-split.
 *
 * Only the fields this parser consumes are excerpted. Structures are
 * byte-packed on the wire (little-endian).
 */

#ifndef AROS_DISCOVERY_EXCERPT_H
#define AROS_DISCOVERY_EXCERPT_H

#include <exec/types.h>

/* --- indirect VRAM access via the BIF MM aperture (register dwords) --- */
#define AMDGPU_mmMM_INDEX               0x0
#define AMDGPU_mmMM_INDEX_HI            0x6
#define AMDGPU_mmMM_DATA                0x1

/* VRAM size in MB, readable pre-discovery on all generations.
   (amdgpu_discovery.c: mmRCC_CONFIG_MEMSIZE) */
#define AMDGPU_mmRCC_CONFIG_MEMSIZE     0xde3
/* IP discovery format probe (amdgpu_discovery.c: mmIP_DISCOVERY_VERSION) */
#define AMDGPU_mmIP_DISCOVERY_VERSION   0x16A00

/* Trusted-memory-region carve-out at the top of VRAM (amdgpu_discovery.h) */
#define AMDGPU_DISCOVERY_TMR_OFFSET     (64 << 10)   /* 65536 */
#define AMDGPU_DISCOVERY_TMR_SIZE       (10 << 10)   /* 10240 */

/* binary_header.binary_signature: bytes 0x07,0x14,0x21,0x28 */
#define AMDGPU_BINARY_SIGNATURE         0x28211407
/* ip_discovery_header.signature */
#define AMDGPU_DISCOVERY_TABLE_SIGNATURE 0x53445049

/* binary_header.table_list index of the IP discovery table */
#define AMDGPU_IP_DISCOVERY             0

/* --- HW_ID values (soc15_hw_ip.h) — only the blocks this driver maps --- */
#define AMDGPU_MP0_HWID                 255   /* PSP */
#define AMDGPU_MP1_HWID                 1     /* SMU */
#define AMDGPU_PWR_HWID                 10
#define AMDGPU_VCN_HWID                 12    /* video codec (UVD_HWID) */
#define AMDGPU_GC_HWID                  11    /* graphics + compute */
#define AMDGPU_SDMA0_HWID               42
#define AMDGPU_SDMA1_HWID               43
#define AMDGPU_MMHUB_HWID               34
#define AMDGPU_ATHUB_HWID               35
#define AMDGPU_OSSSYS_HWID              40    /* interrupt handler ring */
#define AMDGPU_HDP_HWID                 41
#define AMDGPU_DMU_HWID                 271   /* display */
#define AMDGPU_UMC_HWID                 150   /* memory controller phys */
#define AMDGPU_DF_HWID                  46    /* data fabric */
#define AMDGPU_NBIF_HWID                108   /* NBIO / bus interface */
#define AMDGPU_SMUIO_HWID               4
#define AMDGPU_THM_HWID                 3
#define AMDGPU_VPE_HWID                 21

/* On-wire structures (byte-packed, little-endian). Field names and
   layout follow discovery.h verbatim; we read via helpers so host
   endianness never matters. */

struct amdgpu_table_info {
    UWORD offset;
    UWORD checksum;
    UWORD size;
    UWORD padding;
};

#define AMDGPU_TOTAL_TABLES 6

struct amdgpu_binary_header {
    ULONG binary_signature;
    UWORD version_major;
    UWORD version_minor;
    UWORD binary_checksum;
    UWORD binary_size;
    struct amdgpu_table_info table_list[AMDGPU_TOTAL_TABLES];
};

/* ip_discovery_header: we only need signature, num_dies, die_info[],
   and the v4 64-bit flag. die_info immediately follows the fixed head. */
struct amdgpu_die_info {
    UWORD die_id;
    UWORD die_offset;   /* to the die_header */
};

struct amdgpu_die_header {
    UWORD die_id;
    UWORD num_ips;
};

/* ip_v4 fixed head (base_address list follows, 32- or 64-bit) */
struct amdgpu_ip_v4_head {
    UWORD hw_id;
    UBYTE instance_number;
    UBYTE num_base_address;
    UBYTE major;
    UBYTE minor;
    UBYTE revision;
    UBYTE sub_revision_variant;   /* sub_revision:4 | variant:4 */
};

#endif /* AROS_DISCOVERY_EXCERPT_H */
