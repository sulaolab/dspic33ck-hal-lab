/*
 * timer_app.c -- the running time. See timer_app.h for what this is and why it is here.
 */

#include "timer_app.h"

#include <stddef.h>   /* NULL */

#include "nora_tick_timer.h"

/*
 * TICK PRIORITY 2, AND IT MUST STAY BELOW 4.
 *
 * The TDM RX block ISR runs at PRIO_TDM_DMA = 4 (hal_spi_i2s_tdm/nora_spi_i2s_tdm_dspic33ck_hw.c),
 * and that ISR is the one whose execution time is measured and compared against the block
 * period -- the load and margin figures the audio work is steered by. A tick above it would
 * preempt the block ISR, so those figures would include a millisecond interrupt that has
 * nothing to do with audio, at a rate that does not divide the block rate: the measurement
 * would jitter and the worst case would depend on where the tick happened to land.
 *
 * app/timer_1ms.c asked for 7 -- above the block ISR -- which is the value this replaces.
 * Nothing observed it going wrong, and nothing would: the cost is a measurement quietly
 * losing meaning, not a fault. Left at the bottom of the useful range instead, because a
 * counter increment has no latency requirement worth spending priority on: 1 is
 * indistinguishable in effect and 2 leaves a step for a future client of the hook that
 * needs to sit under the block ISR but above the idle level.
 *
 * NOT 0: on this family priority 0 does not disable a source, it makes it the lowest
 * enabled level, but the HAL documents 1..7 as the valid range and rejecting 0 there is
 * the shape to respect.
 */
#define TIMER_APP_IRQ_PRIORITY  2u

static volatile timer_app_tick_hook_t s_tick_hook;

static bool timer_app_start(uint32_t                            timer_clk_hz,
                            nora_tick_timer_clock_source_t source)
{
    const nora_tick_timer_config_t config = {
        .timer_clk_hz = timer_clk_hz,
        .clock_source = source,
        .irq_priority = TIMER_APP_IRQ_PRIORITY,

        /*
         * RUN IN IDLE. Nothing in this tree sleeps today, so this changes no behaviour --
         * but a running time that stops when the CPU idles is a running time with a
         * qualifier, and every cadence expressed against GetTicks() would inherit it.
         * Matches what timer_1ms.c asked for, so DM330030's inherited demo is unaffected.
         */
        .run_in_idle = true,
    };

    return nora_tick_timer_init(&config) == NORA_TICK_TIMER_OK;
}

bool timer_app_start_from_fcy(uint32_t fcy_hz)
{
    return timer_app_start(fcy_hz, NORA_TICK_TIMER_CLOCK_INTERNAL);
}

bool timer_app_start_from_frc(uint32_t frc_hz)
{
    return timer_app_start(frc_hz, NORA_TICK_TIMER_CLOCK_FRC);
}

uint32_t GetTicks(void)
{
    return nora_tick_timer_get_ms();
}

bool timer_app_running(void)
{
    return nora_tick_timer_is_initialized();
}

void timer_app_set_tick_hook(timer_app_tick_hook_t hook)
{
    s_tick_hook = hook;
}

/*
 * The one Timer1 vector in the tree.
 *
 * auto_psv, matching what app/timer_1ms.c used before it: this handler reaches
 * s_tick_hook, which on DM330030 leads into the vendor registry and from there into
 * board code, and none of that is written to be PSV-independent.
 *
 * The vendor comment that used to sit on this vector described a _T3Interrupt. dsPIC33CK
 * has no Timer2..Timer5 at all -- that role belongs to the SCCP/MCCP modules -- so the
 * comment was wrong in the direction that sends a reader looking for a timer this family
 * does not have. The code was always _T1Interrupt.
 */
void __attribute__((__interrupt__, auto_psv)) _T1Interrupt(void)
{
    const timer_app_tick_hook_t hook = s_tick_hook;

    nora_tick_timer_irq_handler();

    if (hook != NULL) {
        hook();
    }
}
