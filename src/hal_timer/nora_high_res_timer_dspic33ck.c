/*
 * nora_high_res_timer_dspic33ck.c
 * --------------------------
 * Free-running 32-bit high-resolution counter, CK sibling of
 * dspic33ak_high_res_timer (AK has a single 32-bit Timer2). See the header.
 *
 * BACKEND: SCCP1 in 32-bit timer mode.
 *
 * This file originally paired Timer2+Timer3 via T2CON.T32=1, on the assumption
 * (stated in the old header comment) that "Timer1 is owned by the 1 ms tick;
 * Timer2/3 are free". That assumption was wrong for this whole family: the
 * dsPIC33CK generation replaced Timer2-Timer5 with SCCP/MCCP modules, so
 * T2CON/T3CON do not exist on EITHER part this repo targets --
 *
 *     dsPIC33CK256MP508 : T1CON only, + CCP1..CCP9
 *     dsPIC33CK64MC105  : T1CON only, + CCP1..CCP4
 *
 * -- and the `#if defined(T2CON) ...` guard therefore compiled the whole HAL
 * out to "not present" everywhere. The block-ISR load monitor consequently
 * printed "load monitor UNAVAILABLE" on every CK board and had never once
 * produced a measurement.
 *
 * SCCP1 gives back exactly what T2/T3 was supposed to: a 32-bit free-running
 * up-counter clocked at Fcy with 1:1 prescale, no interrupt
 * (CCP1PRH:CCP1PRL = 0xFFFFFFFF). Only the plain timer function is used --
 * CLKSEL selects the peripheral clock, so none of the CCP-fed-from-PLL2 /
 * external-reference machinery that exists on the AK side is involved here.
 *
 * The Timer2/3 path is kept below, guarded, in case a future CK variant does
 * expose it; SCCP is preferred when both are available.
 *
 * Which CCP: hardcoded to CCP1. Moving it means editing the register names in
 * this file (they cannot be parameterised without token pasting, which would
 * make a mis-selected module a silent bug rather than a compile error). CCP1 is
 * unused by every profile in this repo.
 */

#include "nora_high_res_timer.h"

#include <xc.h>

#if defined(CCP1CON1L) && defined(CCP1PRL) && defined(CCP1PRH) && \
    defined(CCP1TMRL) && defined(CCP1TMRH)
#define DSPIC33CK_HIGH_RES_TIMER_PRESENT 1
#define DSPIC33CK_HIGH_RES_TIMER_BACKEND_SCCP 1
#elif defined(T2CON) && defined(T3CON) && defined(TMR2) && defined(TMR3) && \
    defined(TMR3HLD) && defined(PR2) && defined(PR3)
#define DSPIC33CK_HIGH_RES_TIMER_PRESENT 1
#define DSPIC33CK_HIGH_RES_TIMER_BACKEND_SCCP 0
#else
#define DSPIC33CK_HIGH_RES_TIMER_PRESENT 0
#define DSPIC33CK_HIGH_RES_TIMER_BACKEND_SCCP 0
#endif

static uint32_t high_res_timer_clk_hz = 0u;
static volatile bool high_res_timer_initialized = false;

static uint32_t count_to_units(uint32_t count, uint64_t units_per_second);

nora_high_res_timer_status_t nora_high_res_timer_init(
    const nora_high_res_timer_config_t *config)
{
    if ((config == 0) || (config->timer_clk_hz == 0u)) {
        return NORA_HIGH_RES_TIMER_ERR_INVALID_ARG;
    }

    if (!nora_high_res_timer_is_present()) {
        return NORA_HIGH_RES_TIMER_ERR_NOT_PRESENT;
    }

#if DSPIC33CK_HIGH_RES_TIMER_BACKEND_SCCP
    high_res_timer_initialized = false;

#if defined(_CCP1MD)
    _CCP1MD = 0;                  /* leave peripheral-module-disable off        */
#endif

    CCP1CON1Lbits.CCPON = 0;      /* stop before reconfiguring                  */
    CCP1CON1L = 0u;
    CCP1CON1H = 0u;
    CCP1CON2L = 0u;
    CCP1CON2H = 0u;
    CCP1CON3H = 0u;

    CCP1CON1Lbits.CCSEL  = 0;     /* timer/output-compare mode, not capture     */
    CCP1CON1Lbits.MOD    = 0b0000;/* plain timer (no output pulse generation)   */
    CCP1CON1Lbits.T32    = 1;     /* 32-bit timer (CCP1TMRH:CCP1TMRL)           */
    CCP1CON1Lbits.TMRPS  = 0b00;  /* 1:1 prescale (max resolution)              */
    CCP1CON1Lbits.CLKSEL = 0b000; /* peripheral clock = Fcy, same as TCS=0 was   */
    CCP1CON1Lbits.CCPSIDL = config->run_in_idle ? 0u : 1u;

    CCP1TMRH = 0x0000u;
    CCP1TMRL = 0x0000u;
    CCP1PRH  = 0xFFFFu;           /* full-scale period: rolls over at 2^32-1    */
    CCP1PRL  = 0xFFFFu;

    high_res_timer_clk_hz = config->timer_clk_hz;
    high_res_timer_initialized = true;

    CCP1CON1Lbits.CCPON = 1;

    return NORA_HIGH_RES_TIMER_OK;
#elif DSPIC33CK_HIGH_RES_TIMER_PRESENT
    T2CONbits.TON = 0;
    T3CONbits.TON = 0;
    high_res_timer_initialized = false;

    /* Timer2 is the master in 32-bit mode; Timer3 control is largely ignored. */
    T2CON = 0u;
    T3CON = 0u;
    T2CONbits.TCS   = 0;          /* Fcy peripheral clock                       */
    T2CONbits.TCKPS = 0b00;       /* 1:1 prescale (max resolution)              */
    T2CONbits.TGATE = 0;
    T2CONbits.T32   = 1;          /* pair T2+T3 into one 32-bit timer           */
    T2CONbits.TSIDL = config->run_in_idle ? 0u : 1u;

    TMR3 = 0x0000u;               /* write MSW before LSW per 32-bit timer note */
    TMR2 = 0x0000u;
    PR3  = 0xFFFFu;
    PR2  = 0xFFFFu;

    high_res_timer_clk_hz = config->timer_clk_hz;
    high_res_timer_initialized = true;

    T2CONbits.TON = 1;            /* T32 mode: TON on Timer2 runs the pair       */

    return NORA_HIGH_RES_TIMER_OK;
#else
    return NORA_HIGH_RES_TIMER_ERR_NOT_PRESENT;
#endif
}

nora_high_res_timer_status_t nora_high_res_timer_deinit(void)
{
    if (!nora_high_res_timer_is_present()) {
        return NORA_HIGH_RES_TIMER_ERR_NOT_PRESENT;
    }

#if DSPIC33CK_HIGH_RES_TIMER_BACKEND_SCCP
    if (!high_res_timer_initialized) {
        return NORA_HIGH_RES_TIMER_ERR_NOT_INITIALIZED;
    }

    CCP1CON1Lbits.CCPON = 0;
    CCP1TMRH = 0;
    CCP1TMRL = 0;
    CCP1PRH  = 0;
    CCP1PRL  = 0;
    CCP1CON1L = 0u;
    high_res_timer_clk_hz = 0u;
    high_res_timer_initialized = false;

    return NORA_HIGH_RES_TIMER_OK;
#elif DSPIC33CK_HIGH_RES_TIMER_PRESENT
    if (!high_res_timer_initialized) {
        return NORA_HIGH_RES_TIMER_ERR_NOT_INITIALIZED;
    }

    T2CONbits.TON = 0;
    T3CONbits.TON = 0;
    TMR2 = 0;
    TMR3 = 0;
    PR2 = 0;
    PR3 = 0;
    T2CON = 0u;
    T3CON = 0u;
    high_res_timer_clk_hz = 0u;
    high_res_timer_initialized = false;

    return NORA_HIGH_RES_TIMER_OK;
#else
    return NORA_HIGH_RES_TIMER_ERR_NOT_PRESENT;
#endif
}

bool nora_high_res_timer_is_present(void)
{
#if DSPIC33CK_HIGH_RES_TIMER_PRESENT
    return true;
#else
    return false;
#endif
}

bool nora_high_res_timer_is_initialized(void)
{
    return high_res_timer_initialized;
}

uint32_t nora_high_res_timer_get_count(void)
{
#if DSPIC33CK_HIGH_RES_TIMER_BACKEND_SCCP
    uint16_t hi;
    uint16_t lo;
    uint16_t hi_again;

    if (!high_res_timer_initialized) {
        return 0u;
    }

    /*
     * SCCP has no read-holding register for the high word (unlike Timer2/3's
     * TMR3HLD), so a plain CCP1TMRL-then-CCP1TMRH read can straddle a low-word
     * carry and report a value ~65536 counts off. Read high, low, high again and
     * retry while the high word moved: at Fcy = 100 MHz the low word carries once
     * every 655 us, so uninterrupted execution normally retries at most once. A
     * long preempting ISR between the reads costs another iteration, not a wrong
     * value -- the loop is the guarantee, the "at most once" is only the cost.
     */
    do {
        hi       = CCP1TMRH;
        lo       = CCP1TMRL;
        hi_again = CCP1TMRH;
    } while (hi != hi_again);

    return ((uint32_t)hi << 16) | (uint32_t)lo;
#elif DSPIC33CK_HIGH_RES_TIMER_PRESENT
    uint16_t lsw;
    uint16_t msw;

    if (!high_res_timer_initialized) {
        return 0u;
    }

    /* Reading TMR2 latches TMR3 into TMR3HLD, so the 32-bit read is coherent. */
    lsw = TMR2;
    msw = TMR3HLD;
    return ((uint32_t)msw << 16) | (uint32_t)lsw;
#else
    return 0u;
#endif
}

uint32_t nora_high_res_timer_elapsed_count(uint32_t start_count)
{
#if DSPIC33CK_HIGH_RES_TIMER_PRESENT
    if (!high_res_timer_initialized) {
        return 0u;
    }

    return nora_high_res_timer_get_count() - start_count;
#else
    (void)start_count;
    return 0u;
#endif
}

uint32_t nora_high_res_timer_clk_hz(void)
{
    return high_res_timer_clk_hz;
}

nora_high_res_timer_status_t nora_high_res_timer_set_clk_hz(uint32_t clk_hz)
{
    if (clk_hz == 0u) {
        return NORA_HIGH_RES_TIMER_ERR_INVALID_ARG;
    }
    if (!high_res_timer_initialized) {
        return NORA_HIGH_RES_TIMER_ERR_NOT_INITIALIZED;
    }

    /* Divisor only -- see the header. No register is written, so the counter does
     * not jump and an interval straddling this call is still a valid interval; it
     * simply converts with the corrected scale. */
    high_res_timer_clk_hz = clk_hz;

    return NORA_HIGH_RES_TIMER_OK;
}

uint32_t nora_high_res_timer_count_to_us(uint32_t count)
{
    return count_to_units(count, 1000000ULL);
}

uint32_t nora_high_res_timer_count_to_us_x10(uint32_t count)
{
    return count_to_units(count, 10000000ULL);
}

uint32_t nora_high_res_timer_elapsed_us(uint32_t start_count)
{
    return nora_high_res_timer_count_to_us(
        nora_high_res_timer_elapsed_count(start_count));
}

uint32_t nora_high_res_timer_elapsed_us_x10(uint32_t start_count)
{
    return nora_high_res_timer_count_to_us_x10(
        nora_high_res_timer_elapsed_count(start_count));
}

static uint32_t count_to_units(uint32_t count, uint64_t units_per_second)
{
    uint64_t converted;

    if (high_res_timer_clk_hz == 0u) {
        return 0u;
    }

    converted = ((uint64_t)count * units_per_second) / high_res_timer_clk_hz;
    if (converted > UINT32_MAX) {
        return UINT32_MAX;
    }

    return (uint32_t)converted;
}
