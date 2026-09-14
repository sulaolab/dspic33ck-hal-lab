/*
 * dsp_load.c -- see the header for what this is and why the period is measured
 * rather than assumed.  The important placement detail is that the measurement
 * runs in FOREGROUND telemetry, not in the block callback: block_count is the
 * ISR's cheap publication, while division and timer bookkeeping are not audio
 * work and must not consume the block deadline.
 *
 * No board-ownership guard: what it reads is the transport HAL's load counters and
 * the high-res timer, and what it writes is the console seam. That is why it left
 * boards/ev88g73a/.
 */

#include "dsp_load.h"

#include <stdbool.h>
#include <stddef.h>

#include "console_out.h"
#include "nora_high_res_timer.h"
#include "timer_app.h"                     /* GetTicks() -- the window's wall clock */

/*
 * Foreground-only period reference.  `block_count` is copied by get_status()
 * while its RX-DMA ISR is masked, then handed to dsp_load_print(); these fields
 * are consequently never touched by the ISR and need neither volatile nor a
 * seqlock.  This replaces the former per-block timer read, 32-bit min/max/sum
 * updates and sequence bookkeeping in wm8904_audio_block_cb().
 *
 * THE WINDOW MUST BE BOUNDED BY A WALL CLOCK, NOT BY A BLOCK COUNT.
 * ----------------------------------------------------------------
 * `elapsed_count = now - s_ref_count` is correct across the SCCP counter's wrap for
 * exactly as long as LESS THAN ONE FULL WRAP has elapsed -- about 42.9 s at Fcy 100 MHz.
 * Past that, modulo arithmetic keeps returning a small, entirely plausible number, and the
 * division below turns it into a believable block period, deadline and load percentage that
 * are all wrong.  There is no in-band way to notice; the value has to be rejected on age.
 *
 * This used to be bounded by a 60000-block cap justified as "40 s at 32 frames / 48 kHz".
 * That reasoning holds only at 48 kHz, and the sign is the wrong way round: a SLOWER rate
 * makes 60000 blocks take LONGER, so 44.1 kHz (43.54 s) already exceeds one wrap before the
 * cap fires, and 8 kHz (240 s) misses nearly six wraps.  Fixing it by deriving a cap from
 * the sample rate would work, but it would hand this module a sample rate and a block size
 * it otherwise has no reason to know -- and it would still be an indirect proxy for the one
 * thing that actually matters, which is elapsed TIME.
 *
 * So the reference carries a millisecond timestamp from GetTicks() (independent 32-bit
 * counter, ~49 day wrap, and wrap-safe as a difference) and is rejected once it is older
 * than the counter's own wrap period, minus a margin.  Rejection is not a loss: the
 * reference re-bases and the next report supplies the denominator, which is what the old
 * cap did too -- correctly this time, at every rate and every block size.
 */
static uint32_t s_ref_count;
static uint32_t s_ref_ticks;
static uint32_t s_ref_block_count;
static bool     s_ref_valid;

/*
 * How old the reference may be, in ms: one full 32-bit wrap of the high-res counter, less
 * 1/8 for skew between the two clocks and for the ms tick's quantisation.  Derived from the
 * timer's own configured input clock rather than hardcoded, so it stays right if Fcy or the
 * timer's source ever changes -- which is the whole failing of the constant it replaces.
 * Cached because it is a property of the HAL's configuration, not of this call.
 */
static uint32_t foreground_window_limit_ms(void)
{
    static uint32_t s_limit_ms;

    /* Deliberately cached on "computed once" and NOT keyed on the timer's clock, even
     * though calibration can now change that clock.  Keying it self-invalidates but
     * measured +78 program bytes on an image with ~100 free, and buys nothing usable:
     * this is a staleness bound with 1/8 of slack in it, so a 0.59 % scale error moves
     * a ~37 s limit by 0.2 s.  The figures that must be true are the printed ones, and
     * those convert through the HAL on every call. */
    if (s_limit_ms == 0u) {
        /* Wrap period in us -> ms.  count_to_us() returns 0 while the timer's clock is
         * unknown, in which case this stays 0 and is recomputed on the next call. */
        const uint32_t wrap_ms =
            nora_high_res_timer_count_to_us(0xFFFFFFFFu) / 1000u;

        s_limit_ms = wrap_ms - (wrap_ms / 8u);
    }

    return s_limit_ms;
}

/* -------------------------------------------------------------------------- */
/* Timer calibration against the audio clock -- see the header for WHY and for  */
/* who is allowed to supply the reference.                                     */
/* -------------------------------------------------------------------------- */

/*
 * How long a window to measure the timer's clock over.  The one-block ambiguity
 * between the block_count snapshot and the SCCP read (see
 * dsp_load_period_from_foreground) is the accuracy floor, so it is worth spending
 * time rather than reports: over ~2 s it is 0.03 %, over 10 s it is 0.007 %.  Since
 * the error being corrected is 0.59 %, 10 s buys a correction two orders of
 * magnitude better than the fault, and it is paid exactly once per stream.
 */
#define DSP_LOAD_CAL_WINDOW_US_X100  (1000000000u)   /* 10 s, in 0.01 us units */

/*
 * THE ARITHMETIC IS DELIBERATELY 32-BIT, AND THAT IS NOT MICRO-OPTIMISATION.
 *
 * The obvious form, clk = counts * 1e9 / (blocks * period_ns), needs 64-bit
 * multiply and divide. Written that way it MEASURED +771 program bytes and took the
 * accepted type_lb+noise image from 98 % to 99 % -- three bytes free, on a part that
 * already refuses to place its last ~139 B to fragmentation (section 11). A
 * diagnostic must not be the reason the next real change fails to link.
 *
 * Two changes made it cheap, and both fell out of asking what precision is USABLE:
 *
 *  - Compare TIMES, not frequencies, in the unit the existing converter already
 *    reaches. count_to_us_x10(count * 10) is the same conversion in 0.01 us units,
 *    so the only 64-bit divide involved is the one that was already there.
 *  - Scale by the DIFFERENCE rather than the ratio:
 *
 *        clk_true = clk + (clk / expected) * (measured - expected)
 *
 *    which is clk * measured / expected rearranged so no product exceeds 32 bits.
 *    One unit of difference is 1500 Hz (15 ppm), and the truncation in
 *    clk / expected lands on the DIFFERENCE -- 0.6 % of the total -- so it costs
 *    about 4 ppm. Both are far below the 0.59 % being corrected.
 *
 * It is also self-correcting rather than one-shot-only: `measured` is converted with
 * whatever divisor is installed, so once the clock is right the difference is zero
 * and a re-measurement is a no-op instead of applying the correction twice.
 */

/*
 * Widest correction that will be accepted, as a divisor of the configured clock:
 * 1/20 = +-5 %.  The FRC's own spec is far tighter than this, so anything outside
 * the band is not a calibration, it is a wrong reference period -- the one mistake
 * this feature can make that would be invisible afterwards, because the result
 * would be reported as measured truth.  Refuse and keep the nominal instead.
 */
#define DSP_LOAD_CAL_BAND_DIV   (20u)

static uint32_t s_cal_expect_x100;    /* 0 = no reference supplied; feature off    */
static uint32_t s_cal_blocks_needed;  /* derived from the period, at setup         */
static uint32_t s_cal_count;
static uint32_t s_cal_blocks;
static bool     s_cal_armed;
static bool     s_cal_done;
static bool     s_cal_refused;        /* reference was out of band; say so once    */

void dsp_load_set_block_period_ref_us_x100(uint32_t period_us_x100)
{
    s_cal_expect_x100   = 0u;
    s_cal_blocks_needed = 0u;
    s_cal_armed         = false;
    s_cal_done          = false;
    s_cal_refused       = false;

    /* Refuse a period so short that the +-5 % band collapses to nothing: below
     * DSP_LOAD_CAL_BAND_DIV the limit truncates to zero and every measurement would
     * be rejected as out of band.  0 (the "my clock is my own" case) fails here too. */
    if (period_us_x100 < DSP_LOAD_CAL_BAND_DIV) {
        return;
    }

    /* No arithmetic on the clock at all now -- the reference is already in the unit
     * the measurement will be compared in, which is the whole point of the x100
     * choice.  Only the window length is derived, and it is a 32-bit divide. */
    s_cal_expect_x100   = period_us_x100;
    s_cal_blocks_needed = DSP_LOAD_CAL_WINDOW_US_X100 / period_us_x100;

    if (s_cal_blocks_needed == 0u) {
        s_cal_blocks_needed = 1u;
    }
}

/*
 * Called from the foreground period measurement with the same two snapshots it
 * already took.  Keeps its OWN reference pair rather than sharing the report's:
 * the report re-bases every couple of seconds by design, and the calibration wants
 * one long uninterrupted window.
 */
static void dsp_load_calibrate(uint32_t now, uint32_t completed_blocks)
{
    uint32_t blocks;
    uint32_t measured;
    uint32_t limit;
    int32_t  delta;

    if ((s_cal_expect_x100 == 0u) || s_cal_done || s_cal_refused) {
        return;
    }

    /* Arm, or re-arm after a restart, or re-arm rather than let the window outlive the
     * counter's wrap while blocks are not advancing (a stalled stream keeps the block
     * delta below the target for as long as it is stopped).
     *
     * The staleness bound is in COUNTS, not ms, which is both cheaper and more honest:
     * what must not be outlived is the counter's own 32-bit wrap, so the guard belongs
     * in the counter's unit and needs no clock, no ms tick, and no second timestamp.
     * 0xC0000000 is 3/4 of a wrap -- 32 s at Fcy 100 MHz, comfortably past the 10 s
     * window and comfortably short of the wrap at any clock this part runs at. */
    if (!s_cal_armed ||
        (completed_blocks < s_cal_blocks) ||
        ((now - s_cal_count) >= 0xC0000000u)) {
        s_cal_count  = now;
        s_cal_blocks = completed_blocks;
        s_cal_armed  = true;
        return;
    }

    blocks = completed_blocks - s_cal_blocks;
    if (blocks < s_cal_blocks_needed) {
        return;
    }

    /*
     * Average block period over the window, in the reference's own unit.  Counts per
     * block is a 32-bit divide; handing ten times that to the existing 0.1 us
     * converter yields 0.01 us units with no new conversion anywhere.  All 32-bit:
     * counts/block is ~67 k here, so the x10 product is ~670 k against uint32's
     * 4.29e9, and the window bound stays the counter's own wrap bound.
     *
     * Truncating counts/block costs at most one count -- 15 ppm -- which is 400
     * times smaller than the 0.59 % being corrected.
     */
    measured = nora_high_res_timer_count_to_us_x10(
        ((now - s_cal_count) / blocks) * 10u);
    delta    = (int32_t)measured - (int32_t)s_cal_expect_x100;
    limit    = s_cal_expect_x100 / DSP_LOAD_CAL_BAND_DIV;

    if ((delta > (int32_t)limit) || (-delta > (int32_t)limit)) {
        s_cal_refused = true;
        return;
    }

    {
        const uint32_t clk = nora_high_res_timer_clk_hz();
        const uint32_t hz_per_unit = clk / s_cal_expect_x100;

        /*
         * clk * measured / expected, rearranged as clk + (clk/expected)*delta so no
         * product exceeds 32 bits.  Nothing needs undoing afterwards: `measured` was
         * converted with whatever divisor is installed, so once the clock is right a
         * re-measurement reads the reference back exactly and delta is zero.  That is
         * what makes re-arming on a stream restart converge rather than apply the
         * same correction twice.
         */
        if (nora_high_res_timer_set_clk_hz(
                (uint32_t)((int32_t)clk + ((int32_t)hz_per_unit * delta))) !=
            NORA_HIGH_RES_TIMER_OK) {
            return;
        }

        s_cal_done = true;
    }
}

void dsp_load_reset(void)
{
    s_ref_count       = 0u;
    s_ref_ticks       = 0u;
    s_ref_block_count = 0u;
    s_ref_valid       = false;

    /* Re-arm the clock calibration with the stream, keeping the reference period
     * itself: the window must not straddle a stop, and a restart is the natural
     * place to re-measure.  The already-installed divisor is left in place, so the
     * figures printed before the new window closes are the previous calibration's
     * rather than a reversion to the nominal. */
    s_cal_armed   = false;
    s_cal_done    = false;
    s_cal_refused = false;

    /* Called by the foreground immediately before inst_start().  Taking the
     * reference here means the first ordinary status line already spans real
     * audio blocks; no callback needs to read SCCP just to establish it. */
    if (nora_high_res_timer_is_initialized()) {
        s_ref_count = nora_high_res_timer_get_count();
        s_ref_ticks = GetTicks();
        s_ref_valid = true;
    }
}

/*
 * Derive one average block period from two FOREGROUND snapshots.  `block_count`
 * is exact at the status snapshot boundary; the SCCP read follows a few cycles
 * later, so the two quantities are not an atomic pair.  Over the normal ~3000
 * blocks / two-second reporting window that can perturb the answer by at most
 * one block (about 0.03 %), vastly below the precision of a 0.1-us console
 * display and preferable to spending work on every 666-us callback.
 *
 * This intentionally does NOT retain per-block min/max jitter.  That diagnostic
 * was useful while validating ISR masking effects, but it cost a coherent 32-bit
 * timer read plus several 32-bit volatile updates on every audio block.  The
 * transport's deadline miss, DMA overrun and frame-error counters remain exact
 * per-block safety diagnostics; this function supplies only the load display's
 * average-period denominator.
 */
static bool dsp_load_period_from_foreground(uint32_t completed_blocks,
                                            uint32_t *period_count)
{
    uint32_t now;
    uint32_t now_ms;
    uint32_t elapsed_count;
    uint32_t elapsed_blocks;

    if ((period_count == NULL) || !nora_high_res_timer_is_initialized()) {
        return false;
    }

    now    = nora_high_res_timer_get_count();
    now_ms = GetTicks();

    /* Before this report converts anything: correct the divisor if the calibration
     * window has closed.  Deliberately ahead of the rebase checks below, because the
     * calibration keeps its own window and must keep accumulating across the reports
     * this function declines to produce. */
    dsp_load_calibrate(now, completed_blocks);

    /* A stopped/restarted stream resets block_count.  Rebase instead of treating
     * its unsigned underflow as billions of completed audio blocks. */
    if (!s_ref_valid || (completed_blocks < s_ref_block_count)) {
        s_ref_count       = now;
        s_ref_ticks       = now_ms;
        s_ref_block_count = completed_blocks;
        s_ref_valid       = true;
        return false;
    }

    elapsed_blocks = completed_blocks - s_ref_block_count;
    if (elapsed_blocks == 0u) {
        /* Preserve the old reference while stalled.  If blocks resume before
         * the next report, the displayed long average exposes the pause instead
         * of silently measuring only the final short portion of it. */
        return false;
    }

    /* Too old to trust: see the header comment on the wall-clock bound.  The subtraction
     * is the fleet's wrap-safe tick idiom (timer_app.h), so the ms counter's own 49-day
     * rollover does not make an old reference look fresh. */
    if ((uint32_t)(now_ms - s_ref_ticks) >= foreground_window_limit_ms()) {
        s_ref_count       = now;
        s_ref_ticks       = now_ms;
        s_ref_block_count = completed_blocks;
        return false;
    }

    elapsed_count = now - s_ref_count;  /* modulo arithmetic: window < one wrap, checked */
    s_ref_count       = now;
    s_ref_ticks       = now_ms;
    s_ref_block_count = completed_blocks;

    *period_count = elapsed_count / elapsed_blocks;
    return (*period_count != 0u);
}

/* Print a 0.1 us value as "X.Yus". No printf in this profile. */
static void write_us10(uint32_t us10)
{
    console_out_u32(us10 / 10u);
    console_out_str(".");
    console_out_u32(us10 % 10u);
    console_out_str("us");
}

/*
 * Print value/total as "X.Y%". Both arguments are 0.1 us units. value_us10 is a
 * block-ISR time, so at most a few thousand -- value * 1000 cannot overflow
 * uint32 for any plausible input, and a 64-bit divide is not needed.
 */
static void write_pct_of(uint32_t value_us10, uint32_t total_us10)
{
    uint32_t pct_x10;

    if (total_us10 == 0u) {
        console_out_str("--.-%");
        return;
    }

    pct_x10 = (value_us10 * 1000u) / total_us10;
    console_out_u32(pct_x10 / 10u);
    console_out_str(".");
    console_out_u32(pct_x10 % 10u);
    console_out_str("%");
}

void dsp_load_print(const nora_spi_i2s_tdm_load_t *load,
                    uint32_t block_count,
                    bool detail)
{
    uint32_t period_us10;

    if ((load == NULL) || !nora_high_res_timer_is_initialized()) {
        console_out_str(" dsp=n/a(no HW timer)");
        return;
    }

    /*
     * The ms tick is what BOUNDS the measurement window (see the wall-clock note at the
     * top).  Without it GetTicks() stands still, every reference reads as brand new, and a
     * window that has crossed a counter wrap would be divided into a plausible wrong load
     * instead of being rejected.  Say so rather than print that number: this file's own
     * rule is that a figure which means "not measured" must not look like a measurement.
     */
    if (!timer_app_running()) {
        console_out_str(" dsp=n/a(no ms tick to bound the window)");
        return;
    }

    /*
     * Denominator = AVERAGE block period across the foreground telemetry window.
     * This is intentionally outside the RX-DMA ISR; see
     * dsp_load_period_from_foreground() for the accuracy and cost tradeoff.
     */
    if (!dsp_load_period_from_foreground(block_count, &period_us10)) {
        console_out_str(" dsp=n/a(no foreground block-period window)");
        return;
    }
    period_us10 = nora_high_res_timer_count_to_us_x10(period_us10);

    /*
     * Same shape as sonora's per-leg line (audio_transport.c
     * dbg_print_leg_begin): peak first, then peak-as-percentage-of-deadline, then
     * the deadline headroom. These three answer the only question a PERIODIC line has
     * to answer -- are we inside the deadline, and by how much -- so they always print.
     *
     * WHY THE SPLIT AT `detail`. `last`/`deadline` are absolute microsecond figures,
     * and they read as confusing next to a percentage. They are diagnostic depth,
     * wanted on demand, not in the running log. Off is not a deletion: they are still
     * reachable with detail=true (`?tq` on EV88G73A prints exactly one such line),
     * and that line now carries the time base they are in.
     *
     * The percentage is the figure that was NEVER affected by the FRC's error, and
     * this is why: numerator and denominator are both counts converted by the same
     * divisor, so a wrong divisor cancels. The owner's observation that
     * `max + margin` exceeded 666.6 us was the same cancellation seen from the other
     * side -- margin is deadline - max, so those two always sum to the deadline
     * exactly, and what was 0.59 % long was the deadline itself. Calibration fixes
     * all three absolute figures at once; it cannot change the percentage.
     */
    console_out_str(" max=");
    write_us10(load->max_us10);
    console_out_str("(");
    write_pct_of(load->max_us10, period_us10);
    console_out_str(")margin=");
    if (load->max_us10 > period_us10) {
        /* Overrun: the ISR took longer than a block. Signed print, like sonora. */
        console_out_str("-");
        write_us10(load->max_us10 - period_us10);
    } else {
        write_us10(period_us10 - load->max_us10);
    }
    if (!detail) {
        return;
    }

    console_out_str(" last=");
    write_us10(load->last_us10);
    console_out_str(" deadline=");
    write_us10(period_us10);

    /*
     * WHICH TIME BASE these microseconds are in.  It belongs next to them and only
     * to them: it is the one fact that decides whether they are true or 0.59 % long,
     * and printing it in the periodic line would repeat a constant forever.
     *
     * "cal" means the divisor was measured against the audio clock; "nom" means it
     * is still the FRC-derived Fcy that init() assumed, either because no reference
     * was offered or because the window has not closed yet. "BADREF" is the case
     * worth shouting about: a reference was supplied and the measurement disagreed
     * with the configured clock by more than 5 %, which is a wrong reference far
     * more likely than a wrong oscillator.
     *
     * Three short words rather than five sentences, because five spellings of this
     * state cost real flash on a 99 %-full part -- and the header is where the
     * explanation belongs anyway.
     */
    console_out_str(" tbase=");
    console_out_u32(nora_high_res_timer_clk_hz());
    console_out_str(s_cal_done ? "Hz(cal)"
                               : (s_cal_refused ? "Hz(BADREF)" : "Hz(nom)"));
}

void dsp_load_print_tail(const nora_spi_i2s_tdm_status_t *st)
{
    if (st == NULL) {
        return;
    }

    console_out_str(" (run,act,blk,miss)=(");
    console_out_u32((uint32_t)st->running);
    console_out_str(",");
    console_out_u32((uint32_t)st->active);
    console_out_str(",");
    console_out_u32(st->block_count);
    console_out_str(",");
    console_out_u32(st->block_deadline_miss_count);
    console_out_str(")");
}
