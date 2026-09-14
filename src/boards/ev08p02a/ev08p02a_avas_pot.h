#ifndef EV08P02A_AVAS_POT_H
#define EV08P02A_AVAS_POT_H

/*
 * ev08p02a_avas_pot.{c,h} -- the POT Click drives AVAS pitch trim, arbitrated with the
 * arrow-key hotkeys that already write the same value (main.c, wm8904_audio.c's *tc).
 *
 * THE PRECEDENT, AND WHY LITTLE OF IT TRANSFERS
 * ---------------------------------------------
 * https://github.com/sulaolab/dspic33ak-audio-dsp-sonora/blob/main/docs_public/avas_pitch_pot_design.md
 * designed this question for the AK512 board. It is worth reading for the reasoning,
 * but almost none of its ANSWERS apply here, and two attempts to carry them over were
 * both wrong on hardware:
 *
 *   - it chose "the POT is the only control, no keys", because on that board the POT is
 *     also a 400 ms engine-synth throttle decision that is mutually exclusive with AVAS.
 *     This board has no engine synth, so that constraint simply is not present;
 *   - its POT is a DIFFERENT PART on a different circuit. Its counts, its span and its
 *     turn direction say nothing about the MIKROE-3402 POT Click in a mikroBUS socket
 *     here, and its tuned constants (EMA shift, deadband, snap) were measured against
 *     its own travel.
 *
 * So the constants below start from that doc because they are a reasonable starting
 * point for any hand-turned pot, and everything about THIS pot -- direction, span,
 * feel -- is measured on THIS board or left explicitly unverified.
 *
 * MAPPING (measured on this board, 2026-09-14, with the owner turning the knob)
 * -----------------------------------------------------------------------------
 * One-sided, 0 .. +EV08P02A_AVAS_POT_TOP_CENT. Chosen over a bipolar -200..+200 mapping
 * for the reason sonora's doc gives: an end stop can be found without looking, where a
 * detent-less knob's centre cannot.
 *
 * TURN DIRECTION IS INVERTED RELATIVE TO THE RAW COUNTS, and that is a measurement, not
 * a preference: on this Click board FULLY COUNTER-CLOCKWISE reads the HIGHEST count
 * (owner, at the board: with the reading shown as 0..2200 mV, the knob fully
 * counter-clockwise was already at the maximum voltage).
 * Clockwise must raise the pitch, so cent rises as counts FALL. Getting this backwards
 * is what shipped first, and it is worth knowing how the wrong version survived a check:
 * the sampler was also running 5x slower than its stated period (main.c,
 * wait_next_led_edge()), and an 8-second lag makes a knob appear to work backwards
 * whichever way it is wired -- so the lag had to be fixed before the direction could be
 * judged at all. Two defects, one symptom, and the second one hid the first.
 *
 * WHY wm8904_audio_avas_pitch_pot_set() AND NOT _pitch_set()
 * ---------------------------------------------------------
 * The console setter PRINTS a line per write, which for a knob being turned is a flood --
 * and it was one: the log filled with `*tc: pitch request = ...` at the sample rate. The
 * pot setter is silent, clamps to 0..AVAS_LINE_CK_PITCH_LIMIT_CENT (unipolar, exactly
 * this mapping's range), and is REMEMBERED across an AVAS restart, so the knob's position
 * survives a stop/start without being re-read. Read the value back with ?tc; nothing here
 * prints per sample, on purpose.
 *
 * That function is DM330030's, added by the session doing that board's RGB/pot work, and
 * this board is its second caller. The two samplers converged on the same shape
 * independently -- seed, reference position, deadband -- which is why the shared half is
 * in wm8904_audio.c and only the mapping is per board.
 *
 * WHAT THIS FILE OWNS
 * --------------------
 * Sampling the pot, conditioning it (EMA + deadband + snap), deciding whether the knob
 * or the keys hold the pitch, and calling wm8904_audio_avas_pitch_pot_set(). It does not own
 * the pitch value itself (avas_line_ck does, via wm8904_audio.c) and it keeps no period
 * of its own -- see ev08p02a_avas_pot_poll().
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Call once, after ev08p02a_pot_init() (the ADC must be ready) and after
 * wm8904_audio_start() (avas_line_ck_init() runs inside it -- calling the pitch API
 * before that is calling into an uninitialised instance). Seeds the EMA and the
 * reference position at the knob's CURRENT reading and writes that as the starting
 * pitch, so the board does not boot to a pitch the knob's physical position disagrees
 * with.
 */
void ev08p02a_avas_pot_init(void);

/*
 * Sample the knob and, if it moved, apply the pitch. THE CALLER OWNS THE PERIOD -- this
 * function keeps none, so calling it faster or slower changes the response directly.
 * main.c calls it from wait_next_led_edge() on EV08P02A_AVAS_POT_SAMPLE_PERIOD_MS; see
 * the note there for why profile_poll() is the wrong place (it runs 5x slower than the
 * period asked for, which was measured as an 8-second lag).
 *
 * Silent while the arrow keys hold the pitch -- see ev08p02a_avas_pot_keys_took_over().
 */
void ev08p02a_avas_pot_poll(void);

/*
 * The arrow keys just set the pitch: stop sampling the knob until AVAS is next keyed.
 *
 * WHY A LATCH AND NOT "WHOEVER MOVED LAST" (changed 2026-09-14 at the owner's request,
 * after soft takeover was tried on hardware and did not behave the way it reads on
 * paper). Soft takeover compares the knob against its own last write, so it should in
 * principle leave a key-set value alone until the knob moves -- but the knob is a live
 * analog input whose EMA keeps drifting by a count or two, so it creeps across the
 * deadband on its own and takes the pitch back seconds after a key press, with nothing
 * having been touched. A latch cannot do that: once the keys have spoken, the knob is
 * out until an explicit event puts it back.
 *
 * The re-arming event is a voice key press ('a' / 'A'), i.e. the AVAS off/on control --
 * so "the knob works again after you restart the voice", which is a rule that can be
 * stated in one sentence to whoever is holding the board.
 */
void ev08p02a_avas_pot_keys_took_over(void);

/* A voice key was pressed (AVAS off/on): hand the pitch back to the knob, and re-seat
 * the reference at its current position so re-arming does not itself cause a jump. */
void ev08p02a_avas_pot_rearm(void);

#ifdef __cplusplus
}
#endif

#endif /* EV08P02A_AVAS_POT_H */
