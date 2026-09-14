/*
 * ev08p02a_avas_pot.c -- see ev08p02a_avas_pot.h for the design: the measured turn
 * direction, why the sonora precedent mostly does not transfer, and why the arrow keys
 * take the pitch by a latch rather than by "whoever moved last".
 */

#ifndef DSPIC33CK_BOARD_EV08P02A
#error "boards/ev08p02a/ev08p02a_avas_pot.c is EV08P02A-owned. Build it only in the CK256MC005_EV08P02A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "ev08p02a_avas_pot.h"

/* No timer_app.h: this module keeps no period of its own -- the caller owns the cadence,
 * see ev08p02a_avas_pot_poll(). */
#include "console_out.h"
#include "ev08p02a_pot.h"
#include "wm8904_audio.h"

/* +200 cent at full travel -- equal to AVAS_LINE_CK_PITCH_LIMIT_CENT
 * (dsp/avas_synth_line_ck.h), so the knob's top end reaches exactly the engine's clamp
 * rather than stopping short of it or asking the clamp to catch an overshoot every time. */
#define EV08P02A_AVAS_POT_TOP_CENT (200)

/*
 * THE KNOB'S OWN SPAN, MEASURED -- not EV08P02A_ADC_FULL_SCALE.
 *
 * The POT Click's wiper does not swing across the whole converter range: with JP1 on
 * VREF its output tops out at the MCP1501's 2.048 V, which against a 3.3 V AVDD full
 * scale is about 2541 counts, and the measured travel agreed (*av1 min/max over a full
 * sweep, 2026-09-14). Scaling by 4095 therefore made the end of the travel arrive at
 * roughly +130 cent instead of +200 -- the top third of the range was unreachable and it
 * looked like the knob simply "did not go all the way".
 *
 * THE JP1 EXPLANATION ABOVE IS DISPUTED AND THIS NUMBER IS NOT MEASURED. 2541 is
 * calculated (2.048 / 3.3 * 4095), and the JP1 = VREF story was carried over from the
 * SAM HA1 XPRO bench (samha1-xpro-lab/docs/adc_pot_results.md), where a max of 2545
 * counts = 2048 mV matched it exactly. The owner reports there is no such jumper in this
 * build. That doc attributes JP1 and the MCP1501 to MIKROE-3402's own schematic v101
 * rather than to the mikroBUS converter PCB the SAM HA setup also had, and this board's
 * counter-clockwise end reads about 2540 -- consistent with the same 2.048 V ceiling --
 * so the two accounts have not been reconciled. Treat the reason as unknown, not settled.
 *
 * The MEASUREMENT was deliberately cancelled (owner, 2026-09-14): with the hardware
 * averaging in place the pot is good enough for the demo, and a real span needs a hand
 * sweeping the knob from stop to stop. So this stays a calculated number on purpose. The
 * clamp below keeps a too-small value safe (the last part of the travel just saturates at
 * +200) and a too-large one merely means the top end is not reached, which is why an
 * uncalibrated span is a resolution question and not a correctness one.
 */
#ifndef EV08P02A_AVAS_POT_SPAN_COUNTS
#define EV08P02A_AVAS_POT_SPAN_COUNTS (2541u)
#endif

/*
 * EMA shift, deadband and bottom-snap counts: starting points from sonora's doc
 * (https://github.com/sulaolab/dspic33ak-audio-dsp-sonora/blob/main/docs_public/avas_pitch_pot_design.md
 * section 3), which measured them against a
 * DIFFERENT pot on a different board -- see this module's header. Not verified by ear
 * here; worth revisiting after a listening pass.
 *
 * The snap is expressed as a fraction of the measured span rather than as sonora's
 * absolute 82 counts, because 82 counts meant "2 % of travel" on a pot whose travel was
 * the full converter range and would mean 3.2 % of this one's.
 */
#define EV08P02A_AVAS_POT_EMA_SHIFT       (4)
#define EV08P02A_AVAS_POT_DEADBAND_COUNTS (24)
#define EV08P02A_AVAS_POT_SNAP_COUNTS \
    ((uint16_t)((EV08P02A_AVAS_POT_SPAN_COUNTS * 2u) / 100u))

static uint16_t s_ema_counts;      /* integer EMA of the raw counts */
static uint16_t s_ref_counts;      /* the EMA value the last POT-driven write used */
static bool     s_seeded;
static bool     s_keys_hold;       /* arrow keys own the pitch until the next voice key */

/*
 * Counts -> cent. INVERTED: counter-clockwise is the high-count end on this board (see
 * the header's measurement), and clockwise has to raise the pitch, so the travel is
 * measured back from the span.
 *
 * The snap at the clockwise-minimum end makes "knob at that stop = exactly 0 cent" true
 * rather than nearly true. It is discontinuous by about 4 cent at the boundary, which
 * sonora's doc measured as inaudible in use; the continuous alternative costs a division
 * per sample for a difference nobody hears.
 */
static int16_t counts_to_cent(uint16_t counts)
{
    uint16_t travel;

    /* Clamp first: the measured span is not a guarantee, and a reading above it would
     * otherwise wrap this unsigned subtraction into a huge travel. */
    if (counts >= (uint16_t)EV08P02A_AVAS_POT_SPAN_COUNTS) {
        return 0;
    }

    travel = (uint16_t)((uint16_t)EV08P02A_AVAS_POT_SPAN_COUNTS - counts);

    if (travel <= EV08P02A_AVAS_POT_SNAP_COUNTS) {
        return 0;
    }

    /* Saturate at the top rather than hand avas_line_ck a value to clamp: the reply it
     * prints would then say "clamped from" on every reading at the end stop. */
    if (travel >= (uint16_t)EV08P02A_AVAS_POT_SPAN_COUNTS) {
        return (int16_t)EV08P02A_AVAS_POT_TOP_CENT;
    }

    return (int16_t)(((uint32_t)travel * (uint32_t)EV08P02A_AVAS_POT_TOP_CENT) /
                     (uint32_t)EV08P02A_AVAS_POT_SPAN_COUNTS);
}

void ev08p02a_avas_pot_init(void)
{
    uint16_t raw = ev08p02a_pot_read();

    /* Seed the EMA at the knob's current reading rather than at 0: an EMA that starts
     * at 0 and decays upward would report "knob at minimum" for its first several
     * samples regardless of where the knob actually is, and the very first pitch this
     * board applies would then disagree with the physical knob position it just read. */
    s_ema_counts = raw;
    s_ref_counts = raw;
    s_keys_hold  = false;
    s_seeded     = true;

    wm8904_audio_avas_pitch_pot_set(counts_to_cent(raw));
}

void ev08p02a_avas_pot_keys_took_over(void)
{
    if (s_keys_hold) {
        return;                    /* already theirs -- say it once, not per keypress */
    }

    s_keys_hold = true;
    console_out_str("      POT pitch OFF: arrow keys hold it until the next 'a' / 'A'\n");
}

void ev08p02a_avas_pot_rearm(void)
{
    if (!s_seeded) {
        return;
    }

    /*
     * Re-seat the reference at the knob's CURRENT position before handing control back.
     * Without this the knob may sit far from where it last wrote, and the first sample
     * after re-arming would jump the pitch across that whole gap -- a jump the operator
     * did not ask for by turning anything.
     */
    s_ema_counts = ev08p02a_pot_read();
    s_ref_counts = s_ema_counts;

    if (s_keys_hold) {
        s_keys_hold = false;
        console_out_str("      POT pitch ON again (turn the knob to take it)\n");
    }
}

void ev08p02a_avas_pot_poll(void)
{
    uint16_t raw;
    uint16_t delta;

    if (!s_seeded || s_keys_hold) {
        return;
    }

    raw = ev08p02a_pot_read();

    /* Integer EMA: ema += (raw - ema) >> shift. The subtraction is promoted to int and
     * so cannot wrap before the shift narrows it back down. */
    s_ema_counts = (uint16_t)((int32_t)s_ema_counts +
                              (((int32_t)raw - (int32_t)s_ema_counts) >>
                               EV08P02A_AVAS_POT_EMA_SHIFT));

    /*
     * Compare against where the POT itself last wrote FROM, not against the pitch
     * currently in force. That is what keeps ADC jitter from rewriting the pitch and
     * printing a line every sample; it is NOT arbitration against the keys -- the latch
     * above is, for the reason the header gives.
     */
    delta = (uint16_t)((s_ema_counts > s_ref_counts) ?
                       (s_ema_counts - s_ref_counts) : (s_ref_counts - s_ema_counts));
    if (delta < (uint16_t)EV08P02A_AVAS_POT_DEADBAND_COUNTS) {
        return;
    }

    s_ref_counts = s_ema_counts;
    wm8904_audio_avas_pitch_pot_set(counts_to_cent(s_ema_counts));
}
