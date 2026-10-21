/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: laptop diag (not for commit): a status strip painted into the scanout
          buffer from the timer interrupt, so a wedged first screen reports
          itself without DOS, USB or timer.device.
*/

#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <exec/interrupts.h>
#include <exec/execbase.h>
#include <hardware/intbits.h>
#include <stdarg.h>
#include <stdio.h>

#include "../amdgpu_kms.h"

struct Amdgpu_KMS *amdgpu_diag_kms;

/* who spins on whose compat spinlock (include/linux/spinlock.h) */
struct Task *amdgpu_diag_spinner;
struct Task *amdgpu_diag_spin_owner;
void *amdgpu_diag_spin_lock;
unsigned long amdgpu_diag_spins;

extern ULONG amdgpu_2d_diag[8];
extern unsigned long amdgpu_diag_vbl[16];
extern void compat_irq_counts(ULONG *raw, ULONG *handled);
extern unsigned long amdgpu_compat_ms(void);
extern volatile ULONG *amdgpu_diag_irq_pending;

#define STRIP_TASKS 12

static struct
{
    struct Interrupt    is;
    ULONG               ticks;          /* IRQ0 arrivals */
    ULONG               dly[8];         /* Delay(10) loops, one per entry of strip_pri[] */
    ULONG               painted;
    struct Task         *task[STRIP_TASKS];
    ULONG               count[STRIP_TASKS];
    ULONG               other;
    ULONG               notrun;        /* samples whose task was not in TS_RUN */
} strip;

/* 3x5 glyphs, rows top to bottom, '1' is a lit pixel */
static const char *strip_glyph(char c)
{
    switch (c)
    {
    case '0': return "111101101101111";
    case '1': return "010110010010111";
    case '2': return "111001111100111";
    case '3': return "111001111001111";
    case '4': return "101101111001001";
    case '5': return "111100111001111";
    case '6': return "111100111101111";
    case '7': return "111001001001001";
    case '8': return "111101111101111";
    case '9': return "111101111001111";
    case 'A': return "010101111101101";
    case 'B': return "110101110101110";
    case 'C': return "111100100100111";
    case 'D': return "110101101101110";
    case 'E': return "111100111100111";
    case 'F': return "111100111100100";
    case 'G': return "111100101101111";
    case 'H': return "101101111101101";
    case 'I': return "111010010010111";
    case 'J': return "001001001101111";
    case 'K': return "101101110101101";
    case 'L': return "100100100100111";
    case 'M': return "101111111101101";
    case 'N': return "110101101101101";
    case 'O': return "111101101101111";
    case 'P': return "111101111100100";
    case 'Q': return "111101101111001";
    case 'R': return "110101110101101";
    case 'S': return "111100111001111";
    case 'T': return "111010010010010";
    case 'U': return "101101101101111";
    case 'V': return "101101101101010";
    case 'W': return "101101111111101";
    case 'X': return "101101010101101";
    case 'Y': return "101101010010010";
    case 'Z': return "111001010100111";
    case ':': return "000010000010000";
    case '/': return "001001010100100";
    case '.': return "000000000000010";
    case '#': return "101111101111101";
    case '-': return "000000111000000";
    default:  return "000000000000000";
    }
}

#define SCALE   3
#define CELLW   (4 * SCALE)
#define CELLH   (6 * SCALE)
#define LINES   4

static void strip_text(struct Amdgpu_FB *fb, ULONG y0, const char *s)
{
    ULONG x = SCALE;
    const char *p;

    for (p = s; *p && x + CELLW <= fb->width; p++, x += CELLW)
    {
        char c = (*p >= 'a' && *p <= 'z') ? *p - 'a' + 'A' : *p;
        const char *g = strip_glyph(c);
        ULONG r, col, sy, sx;

        for (r = 0; r < 5; r++)
            for (col = 0; col < 3; col++)
            {
                ULONG v = g[r * 3 + col] == '1' ? 0x00ffffff : 0;
                for (sy = 0; sy < SCALE; sy++)
                {
                    ULONG *line = (ULONG *)((UBYTE *)fb->map + (y0 + r * SCALE + sy) * fb->pitch);
                    for (sx = 0; sx < SCALE; sx++)
                        line[x + col * SCALE + sx] = v;
                }
            }
    }
}

static int strip_fmt(char *b, int n, const char *fmt, ...)
{
    va_list ap;
    int l;

    va_start(ap, fmt);
    l = vsnprintf(b, n, fmt, ap);
    va_end(ap);
    return l;
}

static void strip_paint(void)
{
    struct Amdgpu_FB *fb = amdgpu_diag_kms ? amdgpu_diag_kms->curfb : NULL;
    ULONG raw = 0, handled = 0, y0, y, i, j;
    char line[160];
    int len;

    if (!fb || !fb->map || fb->bpp != 32 || fb->height < LINES * CELLH + 2 || fb->width < 64)
        return;

    y0 = fb->height - LINES * CELLH - 1;
    for (y = y0; y < fb->height; y++)
    {
        ULONG *l = (ULONG *)((UBYTE *)fb->map + y * fb->pitch);
        for (i = 0; i < fb->width; i++)
            l[i] = 0;
    }

    compat_irq_counts(&raw, &handled);
    strip_fmt(line, sizeof(line), "UP %lu T%lu D0:%lu D25:%lu D29:%lu D31:%lu D35:%lu D50:%lu IRQ %lu PEND %lu 2D %lu/%lu FL %lu/%lu TO %lu VBL %lu/%lu",
              amdgpu_compat_ms() / 1000, (unsigned long)strip.ticks, (unsigned long)strip.dly[0], (unsigned long)strip.dly[1],
              (unsigned long)strip.dly[2], (unsigned long)strip.dly[3], (unsigned long)strip.dly[4], (unsigned long)strip.dly[5],
              (unsigned long)raw, amdgpu_diag_irq_pending ? (unsigned long)*amdgpu_diag_irq_pending : 0UL, (unsigned long)amdgpu_2d_diag[2], (unsigned long)amdgpu_2d_diag[3],
              (unsigned long)amdgpu_2d_diag[4], (unsigned long)amdgpu_2d_diag[5], (unsigned long)amdgpu_2d_diag[7],
              (unsigned long)amdgpu_diag_vbl[3], (unsigned long)amdgpu_diag_vbl[13]);
    strip_text(fb, y0 + SCALE, line);

    /* the CPU's last second, most sampled task first */
    len = 0;
    for (j = 0; j < 4 && len < (int)sizeof(line) - 24; j++)
    {
        ULONG best = 0, bi = STRIP_TASKS;
        for (i = 0; i < STRIP_TASKS; i++)
            if (strip.task[i] && strip.count[i] > best)
            {
                best = strip.count[i];
                bi = i;
            }
        if (bi == STRIP_TASKS)
            break;
        len += strip_fmt(line + len, sizeof(line) - len, "%.14s P%d %lu  ",
                         strip.task[bi]->tc_Node.ln_Name ? strip.task[bi]->tc_Node.ln_Name : "?", (int)strip.task[bi]->tc_Node.ln_Pri, (unsigned long)best);
        strip.count[bi] = 0;
    }
    strip_fmt(line + len, sizeof(line) - len, "OTHER %lu NR %lu", (unsigned long)strip.other, (unsigned long)strip.notrun);
    strip.notrun = 0;
    strip_text(fb, y0 + SCALE + CELLH, line);

    strip_fmt(line, sizeof(line), "PAINT %lu  %lux%lu", (unsigned long)++strip.painted,
              (unsigned long)fb->width, (unsigned long)fb->height);
    strip_text(fb, y0 + SCALE + 2 * CELLH, line);

    {
        struct Task *sp = amdgpu_diag_spinner, *ow = amdgpu_diag_spin_owner;
        strip_fmt(line, sizeof(line), "SPIN %lu %.16s ON %.16s LOCK %lx", amdgpu_diag_spins,
                  sp && sp->tc_Node.ln_Name ? sp->tc_Node.ln_Name : "-",
                  ow && ow->tc_Node.ln_Name ? ow->tc_Node.ln_Name : "-",
                  (unsigned long)(IPTR)amdgpu_diag_spin_lock);
        strip_text(fb, y0 + SCALE + 3 * CELLH, line);
    }

    for (i = 0; i < STRIP_TASKS; i++)
    {
        strip.task[i] = NULL;
        strip.count[i] = 0;
    }
    strip.other = 0;
}

static AROS_INTH1(strip_tick, void *, data)
{
    AROS_INTFUNC_INIT

    struct Task *t = FindTask(NULL);
    ULONG i;

    strip.ticks++;
    if (t->tc_State != TS_RUN)
        strip.notrun++;
    for (i = 0; i < STRIP_TASKS; i++)
    {
        if (strip.task[i] == t)
        {
            strip.count[i]++;
            break;
        }
        if (!strip.task[i])
        {
            strip.task[i] = t;
            strip.count[i] = 1;
            break;
        }
    }
    if (i == STRIP_TASKS)
        strip.other++;

    if ((strip.ticks % 50) == 0)
        strip_paint();

    return FALSE;

    AROS_INTFUNC_EXIT
}

static const BYTE strip_pri[6] = { 0, 25, 29, 31, 35, 50 };

static void strip_delay(void)
{
    ULONG *counter = (ULONG *)FindTask(NULL)->tc_UserData;

    for (;;)
    {
        Delay(10);
        (*counter)++;
    }
}

void amdgpu_diag_strip_start(void)
{
    if (strip.is.is_Code)
        return;
    strip.is.is_Node.ln_Type = NT_INTERRUPT;
    strip.is.is_Node.ln_Pri = -60;
    strip.is.is_Node.ln_Name = "Amdgpu Strip";
    strip.is.is_Code = (VOID_FUNC)strip_tick;
    strip.is.is_Data = &strip;
    AddIntServer(INTB_KERNEL + 0, &strip.is);
    {
        ULONG i;
        for (i = 0; i < sizeof(strip_pri); i++)
            CreateNewProcTags(NP_Name, (IPTR)"Amdgpu Strip Delay", NP_Priority, strip_pri[i], NP_Affinity, TASKAFFINITY_ANY,
                              NP_Entry, (IPTR)strip_delay, NP_StackSize, 16 * 1024, NP_UserData, (IPTR)&strip.dly[i], TAG_DONE);
    }
}
