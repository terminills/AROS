/*
 * AtomBIOS bytecode interpreter for NexOS/AROS
 *
 * Ported from Linux kernel drivers/gpu/drm/amd/amdgpu/atom.c
 * Original author: Stanislaw Skowronek
 * Copyright 2008 Advanced Micro Devices, Inc.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#ifndef ATOM_BIOS_H
#define ATOM_BIOS_H

#include <exec/types.h>
#include <stdint.h>

/* Forward declaration */
struct aros_radv_winsys;

/* ROM magic values */
#define ATOM_BIOS_MAGIC     0xAA55
#define ATOM_ROM_TABLE_PTR  0x48
#define ATOM_ROM_MAGIC      "ATOM"
#define ATOM_ROM_MAGIC_PTR  4

/* ROM header offsets */
#define ATOM_ROM_CMD_PTR    0x1E
#define ATOM_ROM_DATA_PTR   0x20
#define ATOM_ROM_CFG_PTR    0x0C
#define ATOM_ROM_MSG_PTR    0x10

/* Command table indices */
#define ATOM_CMD_INIT       0
#define ATOM_CMD_SETSCLK    0x0A
#define ATOM_CMD_SETMCLK    0x0B
#define ATOM_CMD_SETPCLK    0x0C

/* Data table offsets */
#define ATOM_DATA_FWI_PTR   0xC
#define ATOM_DATA_IIO_PTR   0x32

/* Firmware info offsets */
#define ATOM_FWI_DEFSCLK_PTR    8
#define ATOM_FWI_DEFMCLK_PTR    0xC

/* Command table header */
#define ATOM_CT_SIZE_PTR    0
#define ATOM_CT_WS_PTR      4
#define ATOM_CT_PS_PTR      5
#define ATOM_CT_PS_MASK     0x7F
#define ATOM_CT_CODE_PTR    6

/* Opcode count and special opcodes */
#define ATOM_OP_CNT         127
#define ATOM_OP_EOT         91

/* Switch/case */
#define ATOM_CASE_MAGIC     0x63
#define ATOM_CASE_END       0x5A5A

/* Argument types */
#define ATOM_ARG_REG        0
#define ATOM_ARG_PS         1
#define ATOM_ARG_WS         2
#define ATOM_ARG_FB         3
#define ATOM_ARG_ID         4
#define ATOM_ARG_IMM        5
#define ATOM_ARG_PLL        6
#define ATOM_ARG_MC         7

/* Source alignment */
#define ATOM_SRC_DWORD      0
#define ATOM_SRC_WORD0      1
#define ATOM_SRC_WORD8      2
#define ATOM_SRC_WORD16     3
#define ATOM_SRC_BYTE0      4
#define ATOM_SRC_BYTE8      5
#define ATOM_SRC_BYTE16     6
#define ATOM_SRC_BYTE24     7

/* Workspace special indices */
#define ATOM_WS_QUOTIENT    0x40
#define ATOM_WS_REMAINDER   0x41
#define ATOM_WS_DATAPTR     0x42
#define ATOM_WS_SHIFT       0x43
#define ATOM_WS_OR_MASK     0x44
#define ATOM_WS_AND_MASK    0x45
#define ATOM_WS_FB_WINDOW   0x46
#define ATOM_WS_ATTRIBUTES  0x47
#define ATOM_WS_REGPTR      0x48

/* IIO opcodes */
#define ATOM_IIO_NOP        0
#define ATOM_IIO_START      1
#define ATOM_IIO_READ       2
#define ATOM_IIO_WRITE      3
#define ATOM_IIO_CLEAR      4
#define ATOM_IIO_SET        5
#define ATOM_IIO_MOVE_INDEX 6
#define ATOM_IIO_MOVE_ATTR  7
#define ATOM_IIO_MOVE_DATA  8
#define ATOM_IIO_END        9

/* I/O modes */
#define ATOM_IO_MM          0
#define ATOM_IO_PCI         1
#define ATOM_IO_SYSIO       2
#define ATOM_IO_IIO         0x80

/* Condition codes */
#define ATOM_COND_ABOVE         0
#define ATOM_COND_ABOVEOREQUAL  1
#define ATOM_COND_ALWAYS        2
#define ATOM_COND_BELOW         3
#define ATOM_COND_BELOWOREQUAL  4
#define ATOM_COND_EQUAL         5
#define ATOM_COND_NOTEQUAL      6

/* Port types */
#define ATOM_PORT_ATI       0
#define ATOM_PORT_PCI       1
#define ATOM_PORT_SYSIO     2

/* Delay units */
#define ATOM_UNIT_MICROSEC  0
#define ATOM_UNIT_MILLISEC  1

/* Hardware callbacks */
struct atom_card_info {
    struct aros_radv_winsys *aws;
    void     (*reg_write)(struct atom_card_info *info, uint32_t reg, uint32_t val);
    uint32_t (*reg_read)(struct atom_card_info *info, uint32_t reg);
    void     (*mc_write)(struct atom_card_info *info, uint32_t reg, uint32_t val);
    uint32_t (*mc_read)(struct atom_card_info *info, uint32_t reg);
    void     (*pll_write)(struct atom_card_info *info, uint32_t reg, uint32_t val);
    uint32_t (*pll_read)(struct atom_card_info *info, uint32_t reg);
    /* Runtime guardrails to prevent hosted bring-up hangs in bad Atom loops. */
    uint32_t last_reg_write;
    uint32_t last_val_write;
    uint32_t repeat_reg_writes;
    uint32_t suppressed_reg_logs;
    uint8_t  abort_requested;
    uint8_t  abort_reported;
};

/* AtomBIOS interpreter context */
struct atom_context {
    struct atom_card_info *card;
    void *bios;
    uint32_t cmd_table, data_table;
    uint16_t *iio;

    uint16_t data_block;
    uint32_t fb_base;
    uint32_t divmul[2];
    uint16_t io_attr;
    uint16_t reg_block;
    uint8_t shift;
    int cs_equal, cs_above;
    int io_mode;
    uint32_t *scratch;
    int scratch_size_bytes;
};

/* Public API */
struct atom_context *atom_parse(struct atom_card_info *card, void *bios);
int atom_execute_table(struct atom_context *ctx, int index,
                       uint32_t *params, int params_size);
int atom_asic_init(struct atom_context *ctx);
void atom_destroy(struct atom_context *ctx);

#endif /* ATOM_BIOS_H */
