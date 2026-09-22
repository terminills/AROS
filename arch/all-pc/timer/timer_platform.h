#ifndef _PC_TIMER_PLATFORM_H
#define _PC_TIMER_PLATFORM_H
/*
    Copyright (C) 2020-2026, The AROS Development Team. All rights reserved.

    Desc: PC timer device private structures and constants
*/

struct PlatformTimer
{
    ULONG               tb_Flags;       /* See below..                                  */
    LONG	            tb_TimerIRQNum;	/* Timer IRQ number			                    */
    struct timeval      tb_VBlankTime;	/* Software-emulated periodic timer interval    */
    UWORD               tb_InitValue;
    UWORD               tb_ReloadValue;
    UWORD               tb_ReloadMin;
#if defined(__AROSEXEC_SMP__)
    /*
     * Serialises the PIT latch/read/reload and the tb_prev_tick, tb_ticks_xxx,
     * tb_CurrentTime, tb_Elapsed update that follows it (EClockUpdate(),
     * EClockSet(), Timer0Setup()). Callers only hold Disable(), which is
     * per-CPU on SMP: two CPUs in GetSysTime()/ReadEClock() at once, or one
     * racing the IRQ handler's reload, each add the same PIT interval, and a
     * read that lands after the other CPU's reload sees "counter went up" and
     * charges a bogus 0x10000-tick rollover. Measured on a 4-CPU guest with
     * Chromium polling the clock from every core: the clock ran 7.9x fast.
     */
    void                *tb_ClockLock;
#endif
};

/* tb_Flags definitions */
#define PCTIMER_FLAGB_ENABLED   1
#define PCTIMER_FLAGF_ENABLED   (1 << PCTIMER_FLAGB_ENABLED)

#endif /* _PC_TIMER_PLATFORM_H */
