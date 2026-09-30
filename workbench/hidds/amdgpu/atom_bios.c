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

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdint.h>

#include "atom_bios.h"

#define D(x) x
#define bug Printf

/* Timeout for stuck loops (in iterations, not jiffies) */
#define ATOM_CMD_TIMEOUT_ITERS  500000
#define ATOM_IIO_TIMEOUT_ITERS   16384
#define ATOM_CMD_MAX_OPS       200000

/* ---- byte access macros (little-endian safe) ---- */
static inline uint8_t get_u8(void *bios, int ptr)
{
    return ((unsigned char *)bios)[ptr];
}

static inline uint16_t get_u16(void *bios, int ptr)
{
    return get_u8(bios, ptr) | (((uint16_t)get_u8(bios, ptr + 1)) << 8);
}

static inline uint32_t get_u32(void *bios, int ptr)
{
    return get_u16(bios, ptr) | (((uint32_t)get_u16(bios, ptr + 2)) << 16);
}

/* Macros for execution context (ctx is atom_exec_context *) */
#define U8(ptr)   get_u8(ctx->ctx->bios, (ptr))
#define U16(ptr)  get_u16(ctx->ctx->bios, (ptr))
#define U32(ptr)  get_u32(ctx->ctx->bios, (ptr))

/* Macros for atom_context * (parse-time) */
#define CU8(ptr)  get_u8(ctx->bios, (ptr))
#define CU16(ptr) get_u16(ctx->bios, (ptr))
#define CU32(ptr) get_u32(ctx->bios, (ptr))

/* ---- Execution context ---- */
typedef struct {
    struct atom_context *ctx;
    uint32_t *ps, *ws;
    int ps_size, ws_size;
    int ps_shift;
    uint16_t start;
    unsigned last_jump;
    unsigned long last_jump_count;
    int abort;
} atom_exec_context;

/* ---- Lookup tables ---- */
static uint32_t atom_arg_mask[8] = {
    0xFFFFFFFF, 0xFFFF, 0xFFFF00, 0xFFFF0000,
    0xFF, 0xFF00, 0xFF0000, 0xFF000000
};

static int atom_arg_shift[8] = { 0, 0, 8, 16, 0, 8, 16, 24 };

static int atom_dst_to_src[8][4] = {
    {0, 0, 0, 0},
    {1, 2, 3, 0},
    {1, 2, 3, 0},
    {1, 2, 3, 0},
    {4, 5, 6, 7},
    {4, 5, 6, 7},
    {4, 5, 6, 7},
    {4, 5, 6, 7},
};

static int atom_def_dst[8] = { 0, 0, 1, 2, 0, 1, 2, 3 };

/* Forward declarations */
static int atom_execute_table_locked(struct atom_context *ctx, int index,
                                     uint32_t *params, int params_size);

/* ---- IIO (Indirect I/O) execution ---- */
static uint32_t atom_iio_execute(struct atom_context *ctx, int base,
                                 uint32_t index, uint32_t data)
{
    uint32_t temp = 0xCDCDCDCD;
    unsigned iio_steps = 0;

    while (iio_steps++ < ATOM_IIO_TIMEOUT_ITERS) {
        if (ctx->card && ctx->card->abort_requested) {
            D(bug("[ATOM] IIO aborted by card callback request\n"));
            return 0;
        }

        switch (get_u8(ctx->bios, base)) {
        case ATOM_IIO_NOP:
            base++;
            break;
        case ATOM_IIO_READ:
            temp = ctx->card->reg_read(ctx->card, get_u16(ctx->bios, base + 1));
            base += 3;
            break;
        case ATOM_IIO_WRITE:
            ctx->card->reg_write(ctx->card, get_u16(ctx->bios, base + 1), temp);
            base += 3;
            break;
        case ATOM_IIO_CLEAR:
            temp &= ~((0xFFFFFFFF >> (32 - get_u8(ctx->bios, base + 1))) <<
                       get_u8(ctx->bios, base + 2));
            base += 3;
            break;
        case ATOM_IIO_SET:
            temp |= (0xFFFFFFFF >> (32 - get_u8(ctx->bios, base + 1))) <<
                     get_u8(ctx->bios, base + 2);
            base += 3;
            break;
        case ATOM_IIO_MOVE_INDEX:
            temp &= ~((0xFFFFFFFF >> (32 - get_u8(ctx->bios, base + 1))) <<
                       get_u8(ctx->bios, base + 3));
            temp |= ((index >> get_u8(ctx->bios, base + 2)) &
                      (0xFFFFFFFF >> (32 - get_u8(ctx->bios, base + 1)))) <<
                     get_u8(ctx->bios, base + 3);
            base += 4;
            break;
        case ATOM_IIO_MOVE_DATA:
            temp &= ~((0xFFFFFFFF >> (32 - get_u8(ctx->bios, base + 1))) <<
                       get_u8(ctx->bios, base + 3));
            temp |= ((data >> get_u8(ctx->bios, base + 2)) &
                      (0xFFFFFFFF >> (32 - get_u8(ctx->bios, base + 1)))) <<
                     get_u8(ctx->bios, base + 3);
            base += 4;
            break;
        case ATOM_IIO_MOVE_ATTR:
            temp &= ~((0xFFFFFFFF >> (32 - get_u8(ctx->bios, base + 1))) <<
                       get_u8(ctx->bios, base + 3));
            temp |= ((ctx->io_attr >> get_u8(ctx->bios, base + 2)) &
                      (0xFFFFFFFF >> (32 - get_u8(ctx->bios, base + 1)))) <<
                     get_u8(ctx->bios, base + 3);
            base += 4;
            break;
        case ATOM_IIO_END:
            return temp;
        default:
            D(bug("[ATOM] Unknown IIO opcode 0x%02x\n",
                  get_u8(ctx->bios, base)));
            return 0;
        }
    }

    D(bug("[ATOM] IIO execution timeout (index=0x%08lx data=0x%08lx)\n",
          (unsigned long)index, (unsigned long)data));
    return 0;
}

/* ---- Source operand fetch ---- */
static uint32_t atom_get_src_int(atom_exec_context *ctx, uint8_t attr,
                                 int *ptr, uint32_t *saved, int print)
{
    uint32_t idx, val = 0xCDCDCDCD, align, arg;
    struct atom_context *gctx = ctx->ctx;
    arg = attr & 7;
    align = (attr >> 3) & 7;

    switch (arg) {
    case ATOM_ARG_REG:
        idx = U16(*ptr);
        (*ptr) += 2;
        idx += gctx->reg_block;
        switch (gctx->io_mode) {
        case ATOM_IO_MM:
            val = gctx->card->reg_read(gctx->card, idx);
            break;
        case ATOM_IO_PCI:
        case ATOM_IO_SYSIO:
            val = 0;
            break;
        default:
            if (!(gctx->io_mode & 0x80)) { val = 0; break; }
            if (!gctx->iio[gctx->io_mode & 0x7F]) { val = 0; break; }
            val = atom_iio_execute(gctx, gctx->iio[gctx->io_mode & 0x7F],
                                   idx, 0);
        }
        break;
    case ATOM_ARG_PS:
        idx = U8(*ptr);
        (*ptr)++;
        /* Match Linux atom.c semantics: PS is indexed in DWORD slots. */
        if (idx < (uint32_t)ctx->ps_size)
            val = ctx->ps[idx];
        break;
    case ATOM_ARG_WS:
        idx = U8(*ptr);
        (*ptr)++;
        switch (idx) {
        case ATOM_WS_QUOTIENT:   val = gctx->divmul[0]; break;
        case ATOM_WS_REMAINDER:  val = gctx->divmul[1]; break;
        case ATOM_WS_DATAPTR:    val = gctx->data_block; break;
        case ATOM_WS_SHIFT:      val = gctx->shift; break;
        case ATOM_WS_OR_MASK:    val = 1 << gctx->shift; break;
        case ATOM_WS_AND_MASK:   val = ~(1 << gctx->shift); break;
        case ATOM_WS_FB_WINDOW:  val = gctx->fb_base; break;
        case ATOM_WS_ATTRIBUTES: val = gctx->io_attr; break;
        case ATOM_WS_REGPTR:     val = gctx->reg_block; break;
        default:
            if (idx < ctx->ws_size)
                val = ctx->ws[idx];
        }
        break;
    case ATOM_ARG_ID:
        idx = U16(*ptr);
        (*ptr) += 2;
        val = U32(idx + gctx->data_block);
        break;
    case ATOM_ARG_FB:
        idx = U8(*ptr);
        (*ptr)++;
        if ((gctx->fb_base + (idx * 4)) <= gctx->scratch_size_bytes)
            val = gctx->scratch[(gctx->fb_base / 4) + idx];
        else
            val = 0;
        break;
    case ATOM_ARG_IMM:
        switch (align) {
        case ATOM_SRC_DWORD:
            val = U32(*ptr); (*ptr) += 4; break;
        case ATOM_SRC_WORD0:
        case ATOM_SRC_WORD8:
        case ATOM_SRC_WORD16:
            val = U16(*ptr); (*ptr) += 2; break;
        case ATOM_SRC_BYTE0:
        case ATOM_SRC_BYTE8:
        case ATOM_SRC_BYTE16:
        case ATOM_SRC_BYTE24:
            val = U8(*ptr); (*ptr)++; break;
        }
        return val;
    case ATOM_ARG_PLL:
        idx = U8(*ptr); (*ptr)++;
        val = gctx->card->pll_read(gctx->card, idx);
        break;
    case ATOM_ARG_MC:
        idx = U8(*ptr); (*ptr)++;
        val = gctx->card->mc_read(gctx->card, idx);
        break;
    }

    if (saved)
        *saved = val;
    val &= atom_arg_mask[align];
    val >>= atom_arg_shift[align];
    return val;
}

static void atom_skip_src_int(atom_exec_context *ctx, uint8_t attr, int *ptr)
{
    uint32_t align = (attr >> 3) & 7, arg = attr & 7;
    switch (arg) {
    case ATOM_ARG_REG:
    case ATOM_ARG_ID:
        (*ptr) += 2; break;
    case ATOM_ARG_PLL:
    case ATOM_ARG_MC:
    case ATOM_ARG_PS:
    case ATOM_ARG_WS:
    case ATOM_ARG_FB:
        (*ptr)++; break;
    case ATOM_ARG_IMM:
        switch (align) {
        case ATOM_SRC_DWORD: (*ptr) += 4; return;
        case ATOM_SRC_WORD0:
        case ATOM_SRC_WORD8:
        case ATOM_SRC_WORD16: (*ptr) += 2; return;
        default: (*ptr)++; return;
        }
    }
}

static uint32_t atom_get_src(atom_exec_context *ctx, uint8_t attr, int *ptr)
{
    return atom_get_src_int(ctx, attr, ptr, NULL, 0);
}

static uint32_t atom_get_src_direct(atom_exec_context *ctx, uint8_t align, int *ptr)
{
    uint32_t val = 0;
    switch (align) {
    case ATOM_SRC_DWORD:  val = U32(*ptr); (*ptr) += 4; break;
    case ATOM_SRC_WORD0:
    case ATOM_SRC_WORD8:
    case ATOM_SRC_WORD16: val = U16(*ptr); (*ptr) += 2; break;
    default:              val = U8(*ptr); (*ptr)++; break;
    }
    return val;
}

static uint32_t atom_get_dst(atom_exec_context *ctx, int arg, uint8_t attr,
                             int *ptr, uint32_t *saved, int print)
{
    return atom_get_src_int(ctx,
        arg | atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3] << 3,
        ptr, saved, print);
}

static void atom_skip_dst(atom_exec_context *ctx, int arg, uint8_t attr, int *ptr)
{
    atom_skip_src_int(ctx,
        arg | atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3] << 3, ptr);
}

/* ---- Destination store ---- */
static void atom_put_dst(atom_exec_context *ctx, int arg, uint8_t attr,
                         int *ptr, uint32_t val, uint32_t saved)
{
    uint32_t align = atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
    uint32_t idx;
    struct atom_context *gctx = ctx->ctx;

    val &= atom_arg_mask[align] >> atom_arg_shift[align];
    val <<= atom_arg_shift[align];
    val &= atom_arg_mask[align];
    saved &= ~atom_arg_mask[align];
    val |= saved;

    switch (arg) {
    case ATOM_ARG_REG:
        idx = U16(*ptr);
        (*ptr) += 2;
        idx += gctx->reg_block;
        switch (gctx->io_mode) {
        case ATOM_IO_MM:
            /* MM_INDEX (register 0) expects byte offsets, not DWORD indices. */
            if (idx == 0)
                gctx->card->reg_write(gctx->card, idx, val << 2);
            else
                gctx->card->reg_write(gctx->card, idx, val);
            break;
        case ATOM_IO_PCI:
        case ATOM_IO_SYSIO:
            break;
        default:
            if (!(gctx->io_mode & 0x80)) break;
            if (!gctx->iio[gctx->io_mode & 0xFF]) break;
            atom_iio_execute(gctx, gctx->iio[gctx->io_mode & 0xFF], idx, val);
        }
        break;
    case ATOM_ARG_PS:
        idx = U8(*ptr);
        (*ptr)++;
        if (idx < (uint32_t)ctx->ps_size)
            ctx->ps[idx] = val;
        break;
    case ATOM_ARG_WS:
        idx = U8(*ptr); (*ptr)++;
        switch (idx) {
        case ATOM_WS_QUOTIENT:   gctx->divmul[0] = val; break;
        case ATOM_WS_REMAINDER:  gctx->divmul[1] = val; break;
        case ATOM_WS_DATAPTR:    gctx->data_block = val; break;
        case ATOM_WS_SHIFT:      gctx->shift = val; break;
        case ATOM_WS_OR_MASK:
        case ATOM_WS_AND_MASK:   break;
        case ATOM_WS_FB_WINDOW:  gctx->fb_base = val; break;
        case ATOM_WS_ATTRIBUTES: gctx->io_attr = val; break;
        case ATOM_WS_REGPTR:     gctx->reg_block = val; break;
        default:
            if (idx < ctx->ws_size)
                ctx->ws[idx] = val;
        }
        break;
    case ATOM_ARG_FB:
        idx = U8(*ptr); (*ptr)++;
        if ((gctx->fb_base + (idx * 4)) <= gctx->scratch_size_bytes)
            gctx->scratch[(gctx->fb_base / 4) + idx] = val;
        break;
    case ATOM_ARG_PLL:
        idx = U8(*ptr); (*ptr)++;
        gctx->card->pll_write(gctx->card, idx, val);
        break;
    case ATOM_ARG_MC:
        idx = U8(*ptr); (*ptr)++;
        gctx->card->mc_write(gctx->card, idx, val);
        break;
    }
}

/* ---- Opcode implementations ---- */
static void atom_op_add(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src, saved;
    int dptr = *ptr;
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    src = atom_get_src(ctx, attr, ptr);
    dst += src;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_and(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src, saved;
    int dptr = *ptr;
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    src = atom_get_src(ctx, attr, ptr);
    dst &= src;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_beep(atom_exec_context *ctx, int *ptr, int arg)
{
    D(bug("[ATOM] BEEP!\n"));
}

static void atom_op_calltable(atom_exec_context *ctx, int *ptr, int arg)
{
    int idx = U8((*ptr)++);
    int ret = 0;
    if (get_u16(ctx->ctx->bios, ctx->ctx->cmd_table + 4 + 2 * idx))
        ret = atom_execute_table_locked(ctx->ctx, idx,
                                        ctx->ps + ctx->ps_shift,
                                        ctx->ps_size - ctx->ps_shift);
    if (ret)
        ctx->abort = 1;
}

static void atom_op_clear(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t saved;
    int dptr = *ptr;
    attr &= 0x38;
    attr |= atom_def_dst[attr >> 3] << 6;
    atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    atom_put_dst(ctx, arg, attr, &dptr, 0, saved);
}

static void atom_op_compare(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 0);
    src = atom_get_src(ctx, attr, ptr);
    ctx->ctx->cs_equal = (dst == src);
    ctx->ctx->cs_above = (dst > src);
}

static void atom_op_delay(atom_exec_context *ctx, int *ptr, int arg)
{
    unsigned count = U8((*ptr)++);
    if (arg == ATOM_UNIT_MICROSEC) {
        /* Busy-wait for microseconds */
        volatile int i;
        for (i = 0; i < (int)count * 100; i++) ;
    } else {
        /* Milliseconds — use Delay() (1 tick = ~20ms on AROS) */
        if (count >= 20)
            Delay(count / 20);
        else {
            volatile int i;
            for (i = 0; i < (int)count * 100000; i++) ;
        }
    }
}

static void atom_op_div(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 0);
    src = atom_get_src(ctx, attr, ptr);
    if (src != 0) {
        ctx->ctx->divmul[0] = dst / src;
        ctx->ctx->divmul[1] = dst % src;
    } else {
        ctx->ctx->divmul[0] = 0;
        ctx->ctx->divmul[1] = 0;
    }
}

static void atom_op_div32(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src;
    uint64_t val64;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 0);
    src = atom_get_src(ctx, attr, ptr);
    if (src != 0) {
        val64 = dst;
        val64 |= ((uint64_t)ctx->ctx->divmul[1]) << 32;
        ctx->ctx->divmul[0] = (uint32_t)(val64 / src);
        ctx->ctx->divmul[1] = (uint32_t)(val64 >> 32);
    } else {
        ctx->ctx->divmul[0] = 0;
        ctx->ctx->divmul[1] = 0;
    }
}

static void atom_op_eot(atom_exec_context *ctx, int *ptr, int arg)
{
    /* end of table — nop */
}

static void atom_op_jump(atom_exec_context *ctx, int *ptr, int arg)
{
    int execute = 0;
    uint16_t target = U16(*ptr);
    (*ptr) += 2;

    switch (arg) {
    case ATOM_COND_ABOVE:        execute = ctx->ctx->cs_above; break;
    case ATOM_COND_ABOVEOREQUAL: execute = ctx->ctx->cs_above || ctx->ctx->cs_equal; break;
    case ATOM_COND_ALWAYS:       execute = 1; break;
    case ATOM_COND_BELOW:        execute = !(ctx->ctx->cs_above || ctx->ctx->cs_equal); break;
    case ATOM_COND_BELOWOREQUAL: execute = !ctx->ctx->cs_above; break;
    case ATOM_COND_EQUAL:        execute = ctx->ctx->cs_equal; break;
    case ATOM_COND_NOTEQUAL:     execute = !ctx->ctx->cs_equal; break;
    }

    if (execute) {
        if (ctx->last_jump == (unsigned)(ctx->start + target)) {
            ctx->last_jump_count++;
            if (ctx->last_jump_count > ATOM_CMD_TIMEOUT_ITERS) {
                D(bug("[ATOM] Stuck in loop, aborting\n"));
                ctx->abort = 1;
            }
        } else {
            ctx->last_jump = ctx->start + target;
            ctx->last_jump_count = 0;
        }
        *ptr = ctx->start + target;
    }
}

static void atom_op_mask(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, mask, src, saved;
    int dptr = *ptr;
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    mask = atom_get_src_direct(ctx, ((attr >> 3) & 7), ptr);
    src = atom_get_src(ctx, attr, ptr);
    dst &= mask;
    dst |= src;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_move(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t src, saved;
    int dptr = *ptr;
    if (((attr >> 3) & 7) != ATOM_SRC_DWORD)
        atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    else {
        atom_skip_dst(ctx, arg, attr, ptr);
        saved = 0xCDCDCDCD;
    }
    src = atom_get_src(ctx, attr, ptr);
    atom_put_dst(ctx, arg, attr, &dptr, src, saved);
}

static void atom_op_mul(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 0);
    src = atom_get_src(ctx, attr, ptr);
    ctx->ctx->divmul[0] = dst * src;
}

static void atom_op_mul32(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src;
    uint64_t val64;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 0);
    src = atom_get_src(ctx, attr, ptr);
    val64 = (uint64_t)dst * (uint64_t)src;
    ctx->ctx->divmul[0] = (uint32_t)val64;
    ctx->ctx->divmul[1] = (uint32_t)(val64 >> 32);
}

static void atom_op_nop(atom_exec_context *ctx, int *ptr, int arg)
{
}

static void atom_op_or(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src, saved;
    int dptr = *ptr;
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    src = atom_get_src(ctx, attr, ptr);
    dst |= src;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_postcard(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t val = U8((*ptr)++);
    D(bug("[ATOM] POST: 0x%02x\n", val));
}

static void atom_op_repeat(atom_exec_context *ctx, int *ptr, int arg)
{
    /* unimplemented */
}

static void atom_op_savereg(atom_exec_context *ctx, int *ptr, int arg)
{
    /* unimplemented */
}

static void atom_op_restorereg(atom_exec_context *ctx, int *ptr, int arg)
{
    /* unimplemented */
}

static void atom_op_setdatablock(atom_exec_context *ctx, int *ptr, int arg)
{
    int idx = U8(*ptr);
    (*ptr)++;
    if (!idx)
        ctx->ctx->data_block = 0;
    else if (idx == 255)
        ctx->ctx->data_block = ctx->start;
    else
        ctx->ctx->data_block = get_u16(ctx->ctx->bios,
                                       ctx->ctx->data_table + 4 + 2 * idx);
}

static void atom_op_setfbbase(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    ctx->ctx->fb_base = atom_get_src(ctx, attr, ptr);
}

static void atom_op_setport(atom_exec_context *ctx, int *ptr, int arg)
{
    switch (arg) {
    case ATOM_PORT_ATI: {
        uint16_t port = U16(*ptr);
        if (!port)
            ctx->ctx->io_mode = ATOM_IO_MM;
        else
            ctx->ctx->io_mode = ATOM_IO_IIO | port;
        (*ptr) += 2;
        break;
    }
    case ATOM_PORT_PCI:
        ctx->ctx->io_mode = ATOM_IO_PCI;
        (*ptr)++;
        break;
    case ATOM_PORT_SYSIO:
        ctx->ctx->io_mode = ATOM_IO_SYSIO;
        (*ptr)++;
        break;
    }
}

static void atom_op_setregblock(atom_exec_context *ctx, int *ptr, int arg)
{
    ctx->ctx->reg_block = U16(*ptr);
    (*ptr) += 2;
}

static void atom_op_shift_left(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++), shift;
    uint32_t saved, dst;
    int dptr = *ptr;
    attr &= 0x38;
    attr |= atom_def_dst[attr >> 3] << 6;
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    shift = atom_get_src_direct(ctx, ATOM_SRC_BYTE0, ptr);
    dst <<= shift;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shift_right(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++), shift;
    uint32_t saved, dst;
    int dptr = *ptr;
    attr &= 0x38;
    attr |= atom_def_dst[attr >> 3] << 6;
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    shift = atom_get_src_direct(ctx, ATOM_SRC_BYTE0, ptr);
    dst >>= shift;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shl(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++), shift;
    uint32_t saved, dst;
    int dptr = *ptr;
    uint32_t dst_align = atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    dst = saved;
    shift = atom_get_src(ctx, attr, ptr);
    dst <<= shift;
    dst &= atom_arg_mask[dst_align];
    dst >>= atom_arg_shift[dst_align];
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shr(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++), shift;
    uint32_t saved, dst;
    int dptr = *ptr;
    uint32_t dst_align = atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    dst = saved;
    shift = atom_get_src(ctx, attr, ptr);
    dst >>= shift;
    dst &= atom_arg_mask[dst_align];
    dst >>= atom_arg_shift[dst_align];
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_sub(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src, saved;
    int dptr = *ptr;
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    src = atom_get_src(ctx, attr, ptr);
    dst -= src;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_switch(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t src, val, target;
    src = atom_get_src(ctx, attr, ptr);
    while (U16(*ptr) != ATOM_CASE_END) {
        if (U8(*ptr) == ATOM_CASE_MAGIC) {
            (*ptr)++;
            val = atom_get_src(ctx, (attr & 0x38) | ATOM_ARG_IMM, ptr);
            target = U16(*ptr);
            if (val == src) {
                *ptr = ctx->start + target;
                return;
            }
            (*ptr) += 2;
        } else {
            D(bug("[ATOM] Bad case in switch\n"));
            return;
        }
    }
    (*ptr) += 2;
}

static void atom_op_test(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src;
    dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 0);
    src = atom_get_src(ctx, attr, ptr);
    ctx->ctx->cs_equal = ((dst & src) == 0);
}

static void atom_op_xor(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t attr = U8((*ptr)++);
    uint32_t dst, src, saved;
    int dptr = *ptr;
    dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
    src = atom_get_src(ctx, attr, ptr);
    dst ^= src;
    atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_debug(atom_exec_context *ctx, int *ptr, int arg)
{
    uint8_t val = U8((*ptr)++);
    D(bug("[ATOM] DEBUG: 0x%02x\n", val));
}

static void atom_op_processds(atom_exec_context *ctx, int *ptr, int arg)
{
    uint16_t val = U16(*ptr);
    (*ptr) += val + 2;
}

/* ---- Opcode dispatch table ---- */
static struct {
    void (*func)(atom_exec_context *, int *, int);
    int arg;
} opcode_table[ATOM_OP_CNT] = {
    { NULL, 0 },
    { atom_op_move, ATOM_ARG_REG },     /* 0x01 */
    { atom_op_move, ATOM_ARG_PS },
    { atom_op_move, ATOM_ARG_WS },
    { atom_op_move, ATOM_ARG_FB },
    { atom_op_move, ATOM_ARG_PLL },
    { atom_op_move, ATOM_ARG_MC },       /* 0x06 */
    { atom_op_and, ATOM_ARG_REG },       /* 0x07 */
    { atom_op_and, ATOM_ARG_PS },
    { atom_op_and, ATOM_ARG_WS },
    { atom_op_and, ATOM_ARG_FB },
    { atom_op_and, ATOM_ARG_PLL },
    { atom_op_and, ATOM_ARG_MC },        /* 0x0C */
    { atom_op_or, ATOM_ARG_REG },        /* 0x0D */
    { atom_op_or, ATOM_ARG_PS },
    { atom_op_or, ATOM_ARG_WS },
    { atom_op_or, ATOM_ARG_FB },
    { atom_op_or, ATOM_ARG_PLL },
    { atom_op_or, ATOM_ARG_MC },         /* 0x12 */
    { atom_op_shift_left, ATOM_ARG_REG },  /* 0x13 */
    { atom_op_shift_left, ATOM_ARG_PS },
    { atom_op_shift_left, ATOM_ARG_WS },
    { atom_op_shift_left, ATOM_ARG_FB },
    { atom_op_shift_left, ATOM_ARG_PLL },
    { atom_op_shift_left, ATOM_ARG_MC },   /* 0x18 */
    { atom_op_shift_right, ATOM_ARG_REG }, /* 0x19 */
    { atom_op_shift_right, ATOM_ARG_PS },
    { atom_op_shift_right, ATOM_ARG_WS },
    { atom_op_shift_right, ATOM_ARG_FB },
    { atom_op_shift_right, ATOM_ARG_PLL },
    { atom_op_shift_right, ATOM_ARG_MC },  /* 0x1E */
    { atom_op_mul, ATOM_ARG_REG },       /* 0x1F */
    { atom_op_mul, ATOM_ARG_PS },
    { atom_op_mul, ATOM_ARG_WS },
    { atom_op_mul, ATOM_ARG_FB },
    { atom_op_mul, ATOM_ARG_PLL },
    { atom_op_mul, ATOM_ARG_MC },        /* 0x24 */
    { atom_op_div, ATOM_ARG_REG },       /* 0x25 */
    { atom_op_div, ATOM_ARG_PS },
    { atom_op_div, ATOM_ARG_WS },
    { atom_op_div, ATOM_ARG_FB },
    { atom_op_div, ATOM_ARG_PLL },
    { atom_op_div, ATOM_ARG_MC },        /* 0x2A */
    { atom_op_add, ATOM_ARG_REG },       /* 0x2B */
    { atom_op_add, ATOM_ARG_PS },
    { atom_op_add, ATOM_ARG_WS },
    { atom_op_add, ATOM_ARG_FB },
    { atom_op_add, ATOM_ARG_PLL },
    { atom_op_add, ATOM_ARG_MC },        /* 0x30 */
    { atom_op_sub, ATOM_ARG_REG },       /* 0x31 */
    { atom_op_sub, ATOM_ARG_PS },
    { atom_op_sub, ATOM_ARG_WS },
    { atom_op_sub, ATOM_ARG_FB },
    { atom_op_sub, ATOM_ARG_PLL },
    { atom_op_sub, ATOM_ARG_MC },        /* 0x36 */
    { atom_op_setport, ATOM_PORT_ATI },  /* 0x37 */
    { atom_op_setport, ATOM_PORT_PCI },
    { atom_op_setport, ATOM_PORT_SYSIO },
    { atom_op_setregblock, 0 },          /* 0x3A */
    { atom_op_setfbbase, 0 },            /* 0x3B */
    { atom_op_compare, ATOM_ARG_REG },   /* 0x3C */
    { atom_op_compare, ATOM_ARG_PS },
    { atom_op_compare, ATOM_ARG_WS },
    { atom_op_compare, ATOM_ARG_FB },
    { atom_op_compare, ATOM_ARG_PLL },
    { atom_op_compare, ATOM_ARG_MC },    /* 0x41 */
    { atom_op_switch, 0 },               /* 0x42 */
    { atom_op_jump, ATOM_COND_ALWAYS },  /* 0x43 */
    { atom_op_jump, ATOM_COND_EQUAL },
    { atom_op_jump, ATOM_COND_BELOW },
    { atom_op_jump, ATOM_COND_ABOVE },
    { atom_op_jump, ATOM_COND_BELOWOREQUAL },
    { atom_op_jump, ATOM_COND_ABOVEOREQUAL },
    { atom_op_jump, ATOM_COND_NOTEQUAL },  /* 0x49 */
    { atom_op_test, ATOM_ARG_REG },      /* 0x4A */
    { atom_op_test, ATOM_ARG_PS },
    { atom_op_test, ATOM_ARG_WS },
    { atom_op_test, ATOM_ARG_FB },
    { atom_op_test, ATOM_ARG_PLL },
    { atom_op_test, ATOM_ARG_MC },       /* 0x4F */
    { atom_op_delay, ATOM_UNIT_MILLISEC }, /* 0x50 */
    { atom_op_delay, ATOM_UNIT_MICROSEC },
    { atom_op_calltable, 0 },            /* 0x52 */
    { atom_op_repeat, 0 },
    { atom_op_clear, ATOM_ARG_REG },     /* 0x54 */
    { atom_op_clear, ATOM_ARG_PS },
    { atom_op_clear, ATOM_ARG_WS },
    { atom_op_clear, ATOM_ARG_FB },
    { atom_op_clear, ATOM_ARG_PLL },
    { atom_op_clear, ATOM_ARG_MC },      /* 0x59 */
    { atom_op_nop, 0 },                  /* 0x5A */
    { atom_op_eot, 0 },                  /* 0x5B = ATOM_OP_EOT */
    { atom_op_mask, ATOM_ARG_REG },      /* 0x5C */
    { atom_op_mask, ATOM_ARG_PS },
    { atom_op_mask, ATOM_ARG_WS },
    { atom_op_mask, ATOM_ARG_FB },
    { atom_op_mask, ATOM_ARG_PLL },
    { atom_op_mask, ATOM_ARG_MC },       /* 0x61 */
    { atom_op_postcard, 0 },             /* 0x62 */
    { atom_op_beep, 0 },                 /* 0x63 */
    { atom_op_savereg, 0 },
    { atom_op_restorereg, 0 },
    { atom_op_setdatablock, 0 },         /* 0x66 */
    { atom_op_xor, ATOM_ARG_REG },       /* 0x67 */
    { atom_op_xor, ATOM_ARG_PS },
    { atom_op_xor, ATOM_ARG_WS },
    { atom_op_xor, ATOM_ARG_FB },
    { atom_op_xor, ATOM_ARG_PLL },
    { atom_op_xor, ATOM_ARG_MC },        /* 0x6C */
    { atom_op_shl, ATOM_ARG_REG },       /* 0x6D */
    { atom_op_shl, ATOM_ARG_PS },
    { atom_op_shl, ATOM_ARG_WS },
    { atom_op_shl, ATOM_ARG_FB },
    { atom_op_shl, ATOM_ARG_PLL },
    { atom_op_shl, ATOM_ARG_MC },        /* 0x72 */
    { atom_op_shr, ATOM_ARG_REG },       /* 0x73 */
    { atom_op_shr, ATOM_ARG_PS },
    { atom_op_shr, ATOM_ARG_WS },
    { atom_op_shr, ATOM_ARG_FB },
    { atom_op_shr, ATOM_ARG_PLL },
    { atom_op_shr, ATOM_ARG_MC },        /* 0x78 */
    { atom_op_debug, 0 },               /* 0x79 */
    { atom_op_processds, 0 },           /* 0x7A */
    { atom_op_mul32, ATOM_ARG_PS },      /* 0x7B */
    { atom_op_mul32, ATOM_ARG_WS },
    { atom_op_div32, ATOM_ARG_PS },      /* 0x7D */
    { atom_op_div32, ATOM_ARG_WS },      /* 0x7E */
};

/* ---- Table execution ---- */
static int atom_execute_table_locked(struct atom_context *ctx, int index,
                                     uint32_t *params, int params_size)
{
    int base = get_u16(ctx->bios, ctx->cmd_table + 4 + 2 * index);
    int len, ws, ps, ptr;
    unsigned char op;
    unsigned long op_steps = 0;
    atom_exec_context ectx;
    int ret = 0;

    if (!base)
        return -1;

    len = get_u16(ctx->bios, base + ATOM_CT_SIZE_PTR);
    ws  = get_u8(ctx->bios, base + ATOM_CT_WS_PTR);
    ps  = get_u8(ctx->bios, base + ATOM_CT_PS_PTR) & ATOM_CT_PS_MASK;
    ptr = base + ATOM_CT_CODE_PTR;

    D(bug("[ATOM] Execute table %d at 0x%04x (len=%d ws=%d ps=%d)\n",
          index, base, len, ws, ps));

    ectx.ctx = ctx;
    ectx.ps_shift = ps / 4;
    ectx.start = base;
    ectx.ps = params;
    ectx.ps_size = params_size;
    if (ectx.ps_size < ectx.ps_shift)
        ectx.ps_size = ectx.ps_shift;
    ectx.abort = 0;
    ectx.last_jump = 0;
    ectx.last_jump_count = 0;

    if (ws) {
        ectx.ws = AllocVec(4 * ws, MEMF_PUBLIC | MEMF_CLEAR);
        if (!ectx.ws)
            return -1;
        ectx.ws_size = ws;
    } else {
        ectx.ws = NULL;
        ectx.ws_size = 0;
    }

    while (1) {
        if (ctx->card && ctx->card->abort_requested) {
            D(bug("[ATOM] Execution aborted by card callback request\n"));
            ret = -1;
            break;
        }

        if (++op_steps > ATOM_CMD_MAX_OPS) {
            D(bug("[ATOM] Execution exceeded opcode budget (%lu), aborting\n",
                  (unsigned long)ATOM_CMD_MAX_OPS));
            ret = -1;
            break;
        }

        op = get_u8(ctx->bios, ptr++);

        if (ectx.abort) {
            D(bug("[ATOM] Execution aborted at 0x%04x\n", ptr - 1));
            ret = -1;
            break;
        }

        if (op < ATOM_OP_CNT && op > 0 && opcode_table[op].func)
            opcode_table[op].func(&ectx, &ptr, opcode_table[op].arg);
        else
            break;

        if (op == ATOM_OP_EOT)
            break;
    }

    if (ectx.ws)
        FreeVec(ectx.ws);

    return ret;
}

int atom_execute_table(struct atom_context *ctx, int index,
                       uint32_t *params, int params_size)
{
    /* Reset interpreter state */
    ctx->data_block = 0;
    ctx->reg_block = 0;
    ctx->fb_base = 0;
    ctx->io_mode = ATOM_IO_MM;
    ctx->divmul[0] = 0;
    ctx->divmul[1] = 0;

    return atom_execute_table_locked(ctx, index, params, params_size);
}

/* ---- IIO index table ---- */
static int atom_iio_len[] = { 1, 2, 3, 3, 3, 3, 4, 4, 4, 3 };

static void atom_index_iio(struct atom_context *ctx, int base)
{
    unsigned entries = 0;

    ctx->iio = AllocVec(2 * 256, MEMF_PUBLIC | MEMF_CLEAR);
    if (!ctx->iio)
        return;

    while (CU8(base) == ATOM_IIO_START && entries < 256) {
        unsigned steps = 0;

        ctx->iio[CU8(base + 1)] = base + 2;
        base += 2;

        while (CU8(base) != ATOM_IIO_END) {
            uint8_t op = CU8(base);

            if (op > ATOM_IIO_END) {
                D(bug("[ATOM] Invalid IIO opcode 0x%02x while indexing\n", op));
                return;
            }

            base += atom_iio_len[op];
            if (++steps > ATOM_IIO_TIMEOUT_ITERS) {
                D(bug("[ATOM] IIO index timeout\n"));
                return;
            }
        }

        base += 3;
        entries++;
    }

    if (entries == 256) {
        D(bug("[ATOM] IIO index table limit reached\n"));
    }
}

/* ---- Public API ---- */

struct atom_context *atom_parse(struct atom_card_info *card, void *bios)
{
    int base;
    struct atom_context *ctx;
    uint16_t iio_off;

    ctx = AllocVec(sizeof(struct atom_context), MEMF_PUBLIC | MEMF_CLEAR);
    if (!ctx)
        return NULL;

    ctx->card = card;
    ctx->bios = bios;

    /* Verify BIOS magic */
    if (CU16(0) != ATOM_BIOS_MAGIC) {
        D(bug("[ATOM] Invalid BIOS magic 0x%04x\n", CU16(0)));
        FreeVec(ctx);
        return NULL;
    }

    /* Find ROM table */
    base = CU16(ATOM_ROM_TABLE_PTR);
    if (memcmp((char *)bios + base + ATOM_ROM_MAGIC_PTR, ATOM_ROM_MAGIC, 4) != 0) {
        D(bug("[ATOM] Invalid ATOM magic at offset 0x%04x\n", base));
        FreeVec(ctx);
        return NULL;
    }

    ctx->cmd_table = CU16(base + ATOM_ROM_CMD_PTR);
    ctx->data_table = CU16(base + ATOM_ROM_DATA_PTR);

    D(bug("[ATOM] Parsed: cmd_table=0x%04x data_table=0x%04x\n",
          ctx->cmd_table, ctx->data_table));

    /* Build IIO index */
    iio_off = CU16(ctx->data_table + ATOM_DATA_IIO_PTR);
    if (iio_off)
        atom_index_iio(ctx, iio_off + 4);

    if (!ctx->iio) {
        /* IIO is needed for indirect register access — allocate empty */
        ctx->iio = AllocVec(2 * 256, MEMF_PUBLIC | MEMF_CLEAR);
    }

    /* Allocate scratch space for FB workspace (16KB) */
    ctx->scratch_size_bytes = 16384;
    ctx->scratch = AllocVec(ctx->scratch_size_bytes, MEMF_PUBLIC | MEMF_CLEAR);

    return ctx;
}

int atom_asic_init(struct atom_context *ctx)
{
    int hwi;
    uint32_t ps[16];
    int ret;

    hwi = get_u16(ctx->bios, ctx->data_table + ATOM_DATA_FWI_PTR);

    memset(ps, 0, sizeof(ps));

    ps[0] = get_u32(ctx->bios, hwi + ATOM_FWI_DEFSCLK_PTR);
    ps[1] = get_u32(ctx->bios, hwi + ATOM_FWI_DEFMCLK_PTR);

    D(bug("[ATOM] ASIC_Init: defSCLK=%u defMCLK=%u\n", ps[0], ps[1]));

    if (!ps[0] || !ps[1]) {
        D(bug("[ATOM] ASIC_Init: zero clocks, skipping\n"));
        return -1;
    }

    /* Check that ASIC_Init command table exists */
    if (!get_u16(ctx->bios, ctx->cmd_table + 4 + 2 * ATOM_CMD_INIT)) {
        D(bug("[ATOM] ASIC_Init command table not present\n"));
        return -1;
    }

    ret = atom_execute_table(ctx, ATOM_CMD_INIT, ps, 16);
    if (ret) {
        D(bug("[ATOM] ASIC_Init execution failed: %d\n", ret));
    } else {
        D(bug("[ATOM] ASIC_Init completed successfully!\n"));
    }

    return ret;
}

void atom_destroy(struct atom_context *ctx)
{
    if (!ctx)
        return;
    if (ctx->iio)
        FreeVec(ctx->iio);
    if (ctx->scratch)
        FreeVec(ctx->scratch);
    FreeVec(ctx);
}
