#include "nora_tick_timer.h"

#include <xc.h>
#include <stdint.h>

#define DSPIC33CK_TICK_TIMER_HZ             1000u
#define DSPIC33CK_TICK_TIMER_MAX_PRIORITY   7u

#if defined(T1CON) && defined(TMR1) && defined(PR1) && \
    defined(_T1IF) && defined(_T1IE) && defined(_T1IP)
#define DSPIC33CK_TICK_TIMER_PRESENT         1
#else
#define DSPIC33CK_TICK_TIMER_PRESENT         0
#endif

typedef struct {
    uint16_t divisor;
    uint8_t tckps;
} prescaler_option_t;

static const prescaler_option_t prescaler_options[] = {
    { 1u,   0b00u },
    { 8u,   0b01u },
    { 64u,  0b10u },
    { 256u, 0b11u },
};

static volatile uint32_t tick_ms = 0u;
static volatile bool tick_initialized = false;

static nora_tick_timer_status_t calc_period_reg(
    const nora_tick_timer_config_t *config,
    uint16_t *period_reg,
    uint8_t *tckps);
static nora_tick_timer_status_t apply_clock_source(
    nora_tick_timer_clock_source_t clock_source);

nora_tick_timer_status_t nora_tick_timer_init(
    const nora_tick_timer_config_t *config)
{
    uint16_t period_reg;
    uint8_t tckps;
    nora_tick_timer_status_t status;

    status = calc_period_reg(config, &period_reg, &tckps);
    if (status != NORA_TICK_TIMER_OK) {
        return status;
    }

#if DSPIC33CK_TICK_TIMER_PRESENT
    _T1IE = 0;
    T1CONbits.TON = 0;
    _T1IF = 0;
    tick_initialized = false;

    T1CON = 0u;
    TMR1 = 0u;
    PR1 = period_reg;

    status = apply_clock_source(config->clock_source);
    if (status != NORA_TICK_TIMER_OK) {
        return status;
    }

    T1CONbits.TSYNC = 0u;
    T1CONbits.TCKPS = tckps;
    T1CONbits.TSIDL = config->run_in_idle ? 0u : 1u;
    _T1IP = config->irq_priority;
    tick_ms = 0u;
    tick_initialized = true;
    _T1IF = 0;
    _T1IE = 1;
    T1CONbits.TON = 1;

    return NORA_TICK_TIMER_OK;
#else
    return NORA_TICK_TIMER_ERR_NOT_PRESENT;
#endif
}

nora_tick_timer_status_t nora_tick_timer_deinit(void)
{
#if DSPIC33CK_TICK_TIMER_PRESENT
    if (!tick_initialized) {
        return NORA_TICK_TIMER_ERR_NOT_INITIALIZED;
    }

    _T1IE = 0;
    T1CONbits.TON = 0;
    _T1IF = 0;
    TMR1 = 0u;
    PR1 = 0u;
    T1CON = 0u;
    tick_ms = 0u;
    tick_initialized = false;

    return NORA_TICK_TIMER_OK;
#else
    return NORA_TICK_TIMER_ERR_NOT_PRESENT;
#endif
}

bool nora_tick_timer_is_present(void)
{
#if DSPIC33CK_TICK_TIMER_PRESENT
    return true;
#else
    return false;
#endif
}

/*
 * MEASURED, NOT ASSUMED (2026-08-03). XC-DSC can read a 32-bit value with a single MOV.D,
 * and Microchip's own guidance is that whether it actually does depends on the code
 * generated -- check the listing. Checked, on the EV88G73A production build:
 *
 *   _dspic33ck_tick_timer_get_ms:
 *       mov.w   0x1052, w0      <- low half
 *       mov.w   0x1054, w1      <- high half
 *
 * TWO instructions, so the read is NOT atomic against the Timer1 ISR that increments
 * tick_ms. The failure is narrow but real: if the ISR lands between those two MOVs at the
 * exact millisecond the low half carries, the caller gets the new low half (0x0000) paired
 * with the old high half -- or the reverse -- i.e. a value about 65,536 ms away from the
 * truth, once per 65.5 s of low-half wrap. Every cadence in the tree is
 * `(uint32_t)(GetTicks() - last) >= period` (app/timer_app.h), and one such reading makes
 * that comparison fire immediately or hold off for 65 s.
 *
 * So take a snapshot with the tick interrupt masked. Masking IE leaves T1IF to latch, and
 * the pending interrupt runs as soon as IE is restored, so this critical section -- a few
 * instructions, far shorter than the 1 ms period -- does not lose a tick. T1IF latches one
 * pending interrupt, not a count, so that argument is about the length of this section and
 * does not extend to a caller that masks the tick for longer. The previous IE state
 * is saved rather than forced back on, so this is safe to call from an interrupt context
 * that already masked it. Same technique the TDM transport uses to read its own 32-bit diag
 * counters, and about four instructions.
 *
 * MEASURED AGAIN (2026-08-07), because those four instructions were the wrong four.
 * `_T1IE = saved_ie` reads like a bit write and is not one: the VALUE is a variable, so
 * the compiler cannot emit bset/bclr and has to build the word instead --
 *
 *     mov.w  0x820, w3      <- read ALL of IEC0
 *     bclr.w w3, #0x1
 *     ior.w  w2, w3, w3
 *     mov.w  w3, 0x820      <- write ALL of IEC0 back
 *
 * IEC0 is a shared word: on this part it also holds U1RXIE, DMA0/1IE, CCP1/CCT1IE and
 * SPI1RX/TXIE. Anything that changed one of those between the load and the store is
 * undone by the store -- and GetTicks() is called from every cadence in the tree, so the
 * window is open constantly rather than rarely. This is a live candidate for the UART's
 * own rx_ie_lost_count: if the RX ISR sets U1RXIE inside that window, this function turns
 * it back off and the console stops receiving until the stall recovery notices.
 *
 * Writing a CONSTANT to the alias is what makes it one instruction. Restoring only the
 * set case is enough because the bit is already 0 here, and it is one instruction cheaper
 * than the original as well: bset.b in one arm, nothing in the other.
 *
 * The general rule, which is the part worth carrying to the other repos: a DFP bit alias
 * gives an atomic access only when the value assigned is a compile-time constant. Check
 * the listing for a `mov.w wN, 0x8xx` -- that store is the defect, and it is invisible in
 * the C.
 */
uint32_t nora_tick_timer_get_ms(void)
{
#if DSPIC33CK_TICK_TIMER_PRESENT
    uint32_t snapshot;
    uint8_t saved_ie;

    if (!tick_initialized) {
        return 0u;
    }

    saved_ie = (uint8_t)_T1IE;
    _T1IE = 0;
    snapshot = tick_ms;
    if (saved_ie != 0u) {
        _T1IE = 1;
    }

    return snapshot;
#else
    return 0u;
#endif
}

bool nora_tick_timer_is_initialized(void)
{
    return tick_initialized;
}

void nora_tick_timer_irq_handler(void)
{
#if DSPIC33CK_TICK_TIMER_PRESENT
    _T1IF = 0;

    if (tick_initialized) {
        tick_ms++;
    }
#endif
}

static nora_tick_timer_status_t calc_period_reg(
    const nora_tick_timer_config_t *config,
    uint16_t *period_reg,
    uint8_t *tckps)
{
    uint8_t i;

    if (!nora_tick_timer_is_present()) {
        return NORA_TICK_TIMER_ERR_NOT_PRESENT;
    }

    if ((config == 0) || (period_reg == 0) || (tckps == 0) ||
        (config->timer_clk_hz == 0u) ||
        (config->irq_priority == 0u) ||
        (config->irq_priority > DSPIC33CK_TICK_TIMER_MAX_PRIORITY) ||
        ((config->clock_source != NORA_TICK_TIMER_CLOCK_INTERNAL) &&
         (config->clock_source != NORA_TICK_TIMER_CLOCK_FRC))) {
        return NORA_TICK_TIMER_ERR_INVALID_ARG;
    }

    /*
     * EXACT DIVISORS ONLY. Two independent reasons to skip a prescaler:
     *
     *   remainder      -- this divisor cannot produce 1.000 ms at all
     *   count overflow -- it can, but PR1 is 16 bits; a LARGER prescaler gives a smaller
     *                     count, so the ones left to try are exactly the ones that help
     *
     * The overflow case is why the loop exists. Neither case may return early, and the flag
     * has to record the EXACT case, not the inexact one -- the two walls are not symmetric:
     *
     *   a remainder at divisor d implies a remainder at every LATER one (1|8|64|256), so
     *   seeing one says nothing about the divisors already tried;
     *   an overflow at d says nothing about later ones, which is what the loop continues for.
     *
     * So "some divisor divided exactly, none of them fitted" is out-of-range, and only "no
     * divisor divided exactly at all" is inexact -- which is what the header says each name
     * means. Flagging the inexact case instead gets 65,537,000 Hz wrong: prescaler 1 gives
     * exactly 65,537 counts (one past PR1's 16 bits) and 8/64/256 all leave a remainder, so
     * the honest answer is OUT_OF_RANGE, and the caller who is told INEXACT_PERIOD goes
     * looking for a clock that divides by 1000 when theirs already does.
     *
     * THIS USED TO ROUND -- `(clk + denominator/2) / denominator`, no remainder check,
     * OK returned. Harmless on both current boards (100 MHz / 8 / 1000 = 12500 and
     * 8 MHz / 1 / 1000 = 8000, both exact) but it made the header's contract false, and
     * that contract is now load-bearing: every cadence in the tree is milliseconds of
     * this tick. See ERR_INEXACT_PERIOD in the header.
     */
    {
        bool saw_exact = false;

        for (i = 0u;
             i < (uint8_t)(sizeof(prescaler_options) / sizeof(prescaler_options[0]));
             i++) {
            const uint64_t denominator =
                (uint64_t)prescaler_options[i].divisor * DSPIC33CK_TICK_TIMER_HZ;
            uint64_t counts;

            if (((uint64_t)config->timer_clk_hz % denominator) != 0u) {
                continue;
            }

            counts = (uint64_t)config->timer_clk_hz / denominator;

            if (counts == 0u) {
                continue;
            }

            saw_exact = true;

            if ((counts - 1u) <= UINT16_MAX) {
                *period_reg = (uint16_t)(counts - 1u);
                *tckps = prescaler_options[i].tckps;
                return NORA_TICK_TIMER_OK;
            }
        }

        /* Nothing fitted. Say WHICH wall was hit: out-of-range means the period IS exact
         * but this timer's divisors cannot bring the count inside 16 bits, while inexact
         * is a design error the caller must fix at the clock. */
        return saw_exact ? NORA_TICK_TIMER_ERR_OUT_OF_RANGE
                         : NORA_TICK_TIMER_ERR_INEXACT_PERIOD;
    }
}

static nora_tick_timer_status_t apply_clock_source(
    nora_tick_timer_clock_source_t clock_source)
{
#if DSPIC33CK_TICK_TIMER_PRESENT
    switch (clock_source) {
    case NORA_TICK_TIMER_CLOCK_INTERNAL:
        T1CONbits.TCS = 0u;
        break;

    case NORA_TICK_TIMER_CLOCK_FRC:
        T1CONbits.TECS = 0b11u;
        T1CONbits.TCS = 1u;
        break;

    default:
        return NORA_TICK_TIMER_ERR_INVALID_ARG;
    }

    return NORA_TICK_TIMER_OK;
#else
    return NORA_TICK_TIMER_ERR_NOT_PRESENT;
#endif
}
