#ifndef DSP_LOAD_H
#define DSP_LOAD_H

/*
 * dsp_load.h
 * ----------
 * DSP-load display for any audio profile on this repo's transport, in the shape
 * sonora prints on the AK side: how long the audio block ISR took, and what
 * fraction of the block period that is.
 *
 * The HAL's load monitor (nora_spi_i2s_tdm_load_t) supplies the ISR time.
 * What it cannot supply is the denominator, so this module measures the BLOCK
 * PERIOD from the elapsed high-resolution foreground time divided by the exact
 * transport block-count delta. Measuring beats computing it from an assumed
 * sample rate: the dsPIC-master variant runs
 * brg=3 (fs ~= 48.8 kHz, 655.7 us/block) while a codec-master WM8904 at
 * 48.000 kHz gives 666.7 us, and an assumed constant would silently misreport
 * one of them.
 *
 * WAS boards/ev88g73a/ev88g73a_dsp_load.{c,h}
 * ------------------------------------------
 * Measured before moving it: zero pins, zero ports, zero board registers. What it
 * reads is the transport HAL and the high-res timer; what it writes now goes through
 * app/console_out.h instead of the EV88G73A UART functions it used to name (28
 * calls). It was already shared BETWEEN that board's two audio profiles, which is
 * half of the argument -- the other half is that nothing in it is EV88G73A's.
 *
 * (One of those two profiles, ev88g73a_tdm_master_loopback, has since been deleted;
 * see docs/ck_silicon_findings.md for what was kept from it.)
 *
 * Requires nora_high_res_timer to be initialised; everything degrades to
 * "n/a" if it is not.
 */

#include <stdint.h>
#include <stdbool.h>                 /* dsp_load_print()'s `detail` */

#include "nora_spi_i2s_tdm.h"   /* nora_spi_i2s_tdm_load_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Arm a foreground block-period reference. Call after the high-res timer is
 * initialized and immediately before starting the transport, while block_count
 * is still zero. No timer work is performed from the audio callback.
 */
void dsp_load_reset(void);

/*
 * Turn the audio clock into a FREQUENCY REFERENCE for the high-res timer, by
 * declaring how long one block is KNOWN to take, in HUNDREDTHS OF A MICROSECOND
 * (66667 for 32 frames at 48 kHz). 0 disables it.
 *
 * That unit is not arbitrary. It is the finest unit the existing count_to_us_x10()
 * reaches when it is handed ten times the count, so the comparison needs no new
 * conversion in the timer HAL and no 64-bit arithmetic here -- and 0.01 us on a
 * 666.67 us block is 15 ppm, which is 400 times finer than the error being
 * corrected. Nanoseconds were tried first and cost 168 program bytes in the setter
 * alone, for precision nothing can use.
 *
 * Why this exists. The load percentages were always right, but every absolute
 * microsecond printed here was 0.59 % long, because the high-res timer is told an
 * Fcy computed from the FRC's nominal 8 MHz and the FRC is not a reference. The
 * symptom the owner spotted is that `max` + `margin` came to 670.6 us on a stream
 * whose block period is exactly 32/48000 = 666.667 us. (The printf itself was
 * sound: margin is computed as deadline - max, so the two necessarily add up to
 * the deadline. What was off was the deadline, and both terms, by the same 0.59 %.)
 *
 * The fix inverts the measurement this module already makes. It divides elapsed
 * COUNTS by elapsed BLOCKS to get the period; given the period from a source that
 * does not come from Fcy, the same two numbers give the timer's true input clock
 * instead. One long window then corrects the divisor once, and every microsecond
 * printed afterwards -- here and everywhere else that uses this timer -- is true.
 *
 * WHO MAY CALL IT, AND WHO MAY NOT. Only when the frame clock is EXTERNAL to the
 * MCU. As codec master the WM8904 divides its own 12.288 MHz crystal, so the
 * block period really is 32/48000 s and it is a reference. As dsPIC master, FS is
 * brg-divided from the very Fcy being calibrated: the ratio is fixed by hardware
 * dividers, the measurement returns the nominal value by construction, and it
 * would certify the wrong number as measured. Pass 0 for that case -- which is
 * why this takes a period rather than reading the transport config itself; the
 * caller is the one that knows whether its clock is its own.
 *
 * Cheap enough to leave on: two extra 32-bit snapshots per report, and one 64-bit
 * divide ONCE, in foreground telemetry. Latched after it succeeds, so the printed
 * figures do not wobble by the +-0.03 % of a per-report window; a stream restart
 * (dsp_load_reset()) re-arms it.
 */
void dsp_load_set_block_period_ref_us_x100(uint32_t period_us_x100);

/*
 * Print the load figures in the same shape sonora uses on the AK side, so the
 * two consoles read alike:
 *
 *   max=112.5us(33.7%)margin=220.8us
 *
 * where max is the peak block-ISR time in the window, the percentage is
 * max/deadline, and margin is deadline - max (negative values print with a
 * leading '-', i.e. an overrun).
 *
 * `detail` appends the absolute-time figures:
 *
 *   ... last=299.7us deadline=671.2us
 *
 * where `last` distinguishes a one-off peak from the steady state. Pass false for
 * a periodic line and true for a one-shot query: repeated every couple of seconds
 * these two read as noise beside the percentage. The detail line therefore also
 * names the time base they are in (" tbase=100592000Hz(cal)"), which is the one
 * fact that decides whether to trust them -- see
 * dsp_load_set_block_period_ref_us_x100(). Before that reference exists they are scaled
 * by an ASSUMED Fcy roughly 0.6 % off on this board's FRC; after it, they are true.
 *
 * The deadline is the measured average block period over this foreground telemetry
 * window. Per-block min/max jitter is deliberately not collected: retaining it
 * required an SCCP read and 32-bit statistics updates in every callback. The
 * transport's hardware/deadline health counters remain exact. " dsp=n/a(no HW
 * timer)" when there is no time base.
 *
 * Call from the foreground poll with the snapshot from *_get_status(), including
 * its exact completed `block_count`. Each call advances the period reference, which
 * is why detail is a flag here rather than a second function: two calls would make
 * the second one describe only the short interval between prints.
 */
void dsp_load_print(const nora_spi_i2s_tdm_load_t *load,
                    uint32_t block_count,
                    bool detail);

/*
 * Print sonora's trailing tuple: " (run,act,blk,miss)=(1,1,14485866,0)".
 *
 * run/act live here rather than at the head of the line for the reason they were
 * gated before: they read 1 whenever anything works, so leading with them buries
 * the figures that move. In sonora's layout they are the tail, which reads well
 * and keeps them available without a separate fault branch.
 */
void dsp_load_print_tail(const nora_spi_i2s_tdm_status_t *st);

#ifdef __cplusplus
}
#endif

#endif /* DSP_LOAD_H */
