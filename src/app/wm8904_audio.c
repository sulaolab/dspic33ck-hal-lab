/*
 * wm8904_audio.c -- see the header for the split between this and a board.
 *
 * No board-ownership #error guard: there is not a pin, a port or a board register in
 * here. Everything electrical is reached through wm8904_audio_port_t.
 */

#include "wm8904_audio.h"

#include <stddef.h>
#include <stdint.h>                       /* INT32_MIN/MAX -- the chain's saturating add */

#include "app_config.h"                   /* PRE_/POST_GAIN_CODEC_DB_X10: the boot levels */
#include "console_out.h"
#include "nora_spi_i2s_tdm_conf.h"   /* geometry: SLOTS_PER_FS / BLOCK_FRAMES */
#include "nora_high_res_timer.h"     /* load-monitor time base */
#include "nora_clock.h"              /* Fcy for that time base */
#include "wm8904.h"
#include "wm8904_port.h"                  /* APP_USE_1_BIT_DELAY: the codec's framing choice,
                                           * which the transport's SPIFE must be derived from
                                           * and not restate -- see tcfg below */
#include "gain_ctrl.h"
#include "gain_db.h"                      /* PRE/POST: dB -> Q12 mantissa + saturating scale */
#include "avas_synth_line_ck.h"           /* the chain's AVAS stage; see path_chain() */
#include "dsp_load.h"
#include "timer_app.h"                    /* GetTicks() -- the print throttles are times */

/*
 * THE ONE PLACE BOTH SIDES OF THAT DEFAULT ARE VISIBLE.
 *
 * wm8904_audio.h defaults WM8904_AUDIO_ENABLE_TDM_DIAG to 0 when AVAS_CK_VOICE_BOTH is set,
 * but it can only see the -D form: it must not include the DSP engine's header just to read
 * a default, so a hand-edited default inside avas_synth_line_ck.h would slip past it. This
 * file includes both, so it is where the combination is checked.
 *
 * MEASURED, not assumed, but the measurement is CK64MC105-specific: with the diag commands
 * in, the BOTH=1 link on that 64K-flash part came up 620 PC units (930 B) short -- 82
 * unplaced sections and a "Could not allocate program memory" that names no cause. The
 * #error is therefore scoped to that device rather than to the combination in general: a
 * part with more flash (dsPIC33CK256MC005, 256K, ported 2026-08-26) has room for both, and
 * refusing the combination there would be enforcing a limit that does not apply. If a future
 * part this small enough to repeat the shortfall shows up, add it to the #if rather than
 * widening the guard back to every device.
 */
#if (AVAS_CK_VOICE_BOTH) && (WM8904_AUDIO_ENABLE_TDM_DIAG) && defined(__dsPIC33CK64MC105__)
#error "AVAS_CK_VOICE_BOTH=1 does not fit CK64MC105's 64K flash together with the TDM diag commands (measured 930 B short). Build it with WM8904_AUDIO_ENABLE_TDM_DIAG=0, which is what a -Define AVAS_CK_VOICE_BOTH=1 selects by itself on this part."
#endif

/* One ping/pong half, one direction (int32 samples). */
#define WM8904_AUDIO_HALF_SAMPLES \
    ((size_t)NORA_TDM_SLOTS_PER_FS * NORA_TDM_BLOCK_FRAMES)

/*
 * HOW MANY SLOTS EACH BLOCK LOOP BELOW HANDLES PER ITERATION -- and why unrolling is
 * where the remaining money is.
 *
 * The three paths were measured on EV88G73A at 48 kHz / 32 frames (256 slots, 666 us
 * period, Fcy 100 MHz), which turns their `max=` figures into cycles per slot:
 *
 *     mute (two zero stores per slot)   19.7 us  ->  7.7 cycles/slot
 *     copy (two loads + two stores)     20.9 us  ->  8.2 cycles/slot
 *     gain (loads, 2 muls, stores)      48.4 us  -> 18.9 cycles/slot
 *
 * `copy - mute` is 0.47 cycles per slot. Adding two word LOADS to every slot therefore
 * costs almost nothing, which says the mute loop is not store-bound and not memory-bound:
 * of its 7.7 cycles, the two stores are ~2 and essentially all the rest is the loop itself
 * -- the index compare, the conditional branch, and the two pointer increments, paid 256
 * times per block. Arithmetic is not where the time goes, so no cleverer arithmetic can
 * take it back; the loop OVERHEAD is, and unrolling is what removes it.
 *
 * 8 is one TDM frame at this geometry, so an iteration is "one frame's worth of slots" --
 * but nothing here depends on that, only on the total being a whole number of groups
 * (asserted below), which holds for every slot count and block size the transport allows.
 * Larger factors buy progressively less (the per-iteration cost is already amortised 8-fold)
 * while growing the ISR's flash footprint on a part that is 74% full.
 *
 * The unrolled bodies are written out rather than left to the optimiser. -O2 will not
 * unroll these on its own (measured: the loops above ARE the -O2 output), and a body of
 * `for (u = 0; u < 8; u++)` inside an outer loop only reintroduces the counter this exists
 * to delete unless the compiler chooses to peel it -- which is exactly the choice we are
 * trying not to depend on. The indices are constants so the compiler addresses every slot
 * as [Wn + literal] and pays ONE pointer increment per eight slots.
 */
#define WM8904_AUDIO_UNROLL 8u

_Static_assert((WM8904_AUDIO_HALF_SAMPLES % WM8904_AUDIO_UNROLL) == 0u,
               "a ping/pong half must be a whole number of unrolled slot groups; "
               "adjust WM8904_AUDIO_UNROLL to divide SLOTS_PER_FS * BLOCK_FRAMES");

static const wm8904_audio_config_t *s_cfg;
static bool     s_started;
static bool     s_use_gain;

/*
 * AVAS engine state: 494 bytes of .bss for 185 baseband phases plus 11 carriers and
 * envelopes, MEASURED (`xc-dsc-objdump -t wm8904_audio.o`, symbol _s_avas = 0x1ee)
 * rather than estimated. It was described here as "~1 KB", which was true of the
 * 32-bit phase and envelope state and stopped being true at the shipped 16/16 phase
 * and ENVFRAC=0 -- so the number that justified worrying about it was double the
 * real one. Regenerate it the same way after any width change; the widths are build
 * options, so this figure is not a constant of the source.
 *
 * That is still not free on CK64MC105: the map has data memory fully allocated
 * (5510 of 8192 static, the linker giving the remaining 2682 to the stack), so this
 * comes out of the stack margin. Worth watching if the stack ever grows -- it is
 * a static allocation precisely so the linker reports it rather than the ISR
 * discovering it. Nothing on '508, which has 24 KB.
 */
static avas_line_ck_t s_avas;
static bool           s_avas_rate_ok;   /* false => starting the AVAS stage refuses */

/*
 * THE CHAIN'S WORKING BUFFER -- TDMin -> AVAS -> Gain -> TDMout, one stage at a time
 * over plain int32_t samples instead of five mutually exclusive writers of the TX half.
 *
 * TWO SLOTS AND NOT EIGHT, deliberately. The WM8904 converts slots 0 and 1 and nothing on
 * this board sits in the rest of the frame, so a full-frame buffer would be 1 KB of a
 * part whose data memory the linker has already handed out (5510 of 8192 static, this
 * buffer and the AVAS engine's 494 bytes included; see the note above). 256 bytes -- the
 * measured size of _s_chain -- buys the same audio.
 *
 * Plain int32_t is what lets the gain stage be gain_ctrl_process_block() -- the API
 * gain_ctrl.h says a wire-slot caller must not use, and which the AK side shares. This is
 * its first CK caller; before the chain there was no buffer of that type to hand it.
 */
#define WM8904_AUDIO_CHAIN_SLOTS  2u

static int32_t s_chain[(size_t)NORA_TDM_BLOCK_FRAMES * WM8904_AUDIO_CHAIN_SLOTS];

/*
 * "Has the synth been started", the latch behind the AVAS voice keys -- and it is NOT derived
 * from the gate, for the reason sonora states in classic_controls.c: re-enabling during
 * the release fade is a supported path (it deliberately skips the phase reset there to
 * avoid a click), so a latch derived from the gate would answer "off" for a synth that is
 * about to be audible again.
 *
 * s_chain_avas_run is the ISR's copy of "run the stage this block". The main loop SETS it
 * on start; the ISR CLEARS it once the engine reports it has finished fading. That
 * division is what makes the fade render to its end without the ISR having to read the
 * 32-bit gate the foreground writes (the torn-write lesson from set_parts).
 */
static bool          s_avas_on;
static volatile bool s_chain_avas_run;

static bool     s_button_prev;
/* One throttled console line's state -- see due_ms(). */
typedef struct {
    uint32_t last_ms;
    bool     primed;    /* false until the first print; see due_ms() on why it exists */
} print_gate_t;

static print_gate_t s_status_gate;
static print_gate_t s_idle_gate;
/*
 * Set only by wm8904_audio_stop().  Distinguishes a deliberate quiet state from a
 * boot-time codec failure in the foreground status message -- and, since it carries the
 * mute's verification result, a *ts that actually silenced HPOUT from one that only
 * halted the transport.
 *
 * THREE VALUES AND NOT A bool, because the two failures are not the same failure and the
 * flash procedure acts on one of them. Combined with s_started (which stays the single
 * source of "running"), this spans the four states a reader needs:
 *
 *   running                  s_started
 *   stopped, mute verified   !s_started && STOP_MUTE_VERIFIED
 *   stopped, mute UNverified !s_started && STOP_MUTE_UNVERIFIED  <- transport is halted but
 *                                                                   HPOUT may still be live
 *   never started            !s_started && STOP_NONE             <- codec/transport bring-up
 *                                                                   failed; no *ts was issued
 *
 * Reporting the third as if it were the second is what buildtools/README.md's HEX-copy gate
 * would have accepted as "safe to reprogram now".
 */
typedef enum {
    WM8904_AUDIO_STOP_NONE = 0,          /* wm8904_audio_stop() has not run */
    WM8904_AUDIO_STOP_MUTE_VERIFIED,     /* codec confirmed the analog mute (I2C readback) */
    WM8904_AUDIO_STOP_MUTE_UNVERIFIED,   /* mute write failed or could not be confirmed */
} wm8904_audio_stop_state_t;

static wm8904_audio_stop_state_t s_stop_state;

/*
 * Ownership split for the ramp state, because the two sides run at different
 * priorities: gain_ctrl_process_block() executes in the RX DMA block ISR, while the
 * mute button is sampled in the main loop. s_gain holds several 32-bit fields
 * (prevGain/targetGain/rampStep/rampRemainSamples/...) and 32-bit access is NOT atomic
 * on this 16-bit core -- the transport HAL says as much where it masks its own RX DMA
 * IE to read 32-bit diag counters. Calling gain_ctrl_mute_set() straight from the main
 * loop would let the ISR observe a half-updated ramp (torn value, or a new step paired
 * with an old remaining-count), i.e. a gain jump, a wrong ramp time, or a ramp that
 * ends early or in the wrong direction.
 *
 * So s_gain is owned exclusively by the ISR side, and the main loop only publishes a
 * LEVEL: s_mute_request is a single byte (atomic to write on this core) holding the
 * state the button last asked for. The ISR compares it with its own s_mute_applied copy
 * and calls gain_ctrl_mute_set() itself when they differ. Level-based rather than an
 * event flag the ISR has to clear, so a request can never be lost in a
 * read-modify-write window -- the ISR simply converges on the latest published level.
 */
static gain_ctrl_t      s_gain;           /* ISR-owned once started */
static volatile uint8_t s_mute_request;   /* main loop writes only: 0 = unmuted, 1 = muted */
static uint8_t          s_mute_applied;   /* ISR-owned mirror of the last applied level */

/*
 * PRE / POST GAIN -- the dB calibration, at the two ends of the chain.
 *
 * PRE scales CODEC-IN before anything looks at it; POST scales what goes on the wire,
 * after the mix. Both come from the profile (PRE_/POST_GAIN_CODEC_DB_X10) and both are
 * settable while running (*ti / *to). They exist because this board's analog input gain is
 * set low to keep the input circuit's noise out of the converter, which leaves the digital
 * signal ~18.7 dB down; gain_db.h has the level-versus-SNR caveat.
 *
 * ONLY THE CHAIN PATH APPLIES THEM. copy/mute/tone/gain are diagnostic paths whose whole
 * value is that they are known quantities -- `copy` is the cost baseline the chain is
 * measured against, and a calibration silently in the middle of it would move that
 * baseline. So the two stages live in path_chain() and nowhere else.
 *
 * SAME OWNERSHIP SPLIT AS THE MUTE RAMP: the main loop publishes a single 16-bit
 * HALF-DECIBEL value (atomic to write on this core, and already snapped to the table's grid
 * so the console can print what it realised), and the ISR does its own table lookup when the
 * published value differs from the one it applied. Level-based, not an event flag, so a
 * request cannot be lost.
 *
 * The split originally existed because gain_db_t held a (mantissa, shift) PAIR and a torn
 * read would have given the ISR a new mantissa with an old shift -- a wrong OCTAVE, not a
 * rounding error. The fixed-shift form retires that hazard: one 16-bit mantissa cannot tear
 * on this core, so publishing it directly would now be safe. The split stays anyway, because
 * it is what keeps s_pre_gain/s_post_gain single-owner (ISR) like s_gain, and because a
 * half-decibel is the value the console reports and the table's own index -- one published
 * unit rather than two representations of the same setting.
 */
static volatile int16_t s_pre_half_request;    /* main loop writes only; half-decibels */
static volatile int16_t s_post_half_request;
static int16_t          s_pre_half_applied;    /* ISR-owned mirrors */
static int16_t          s_post_half_applied;
static gain_db_t        s_pre_gain  = GAIN_DB_UNITY_INIT;   /* ISR-owned once started */
static gain_db_t        s_post_gain = GAIN_DB_UNITY_INIT;

/*
 * Samples that CLAMPED, per stage. A clamp is not a fact of life here -- it means the
 * configured boost cannot fit the signal that arrived -- so it is counted and reported
 * (?ti / ?to) rather than silently absorbed, which is this repo's rule for a survivable
 * fault: ?du for the UART overrun, `miss` for the block ISR. 16 bit, so reading them from
 * the main loop needs no lock on this core; they wrap, and a wrapped count is still the
 * answer to the only question being asked (is it happening at all).
 */
static uint16_t s_pre_sat;    /* ISR-writes, main-reads-and-clears */
static uint16_t s_post_sat;

/*
 * The profile's boot values, tied back to the table that has to be able to express them.
 * app_config.h already refuses an off-grid or out-of-range dB with a message that names the
 * grid; this second pair of checks is what keeps that message honest if the table is ever
 * regenerated with a different range -- the literals there and the table here cannot drift
 * apart silently.
 */
#define WM8904_AUDIO_PRE_HALF   GAIN_DB_HALF_OF_X10(PRE_GAIN_CODEC_DB_X10)
#define WM8904_AUDIO_POST_HALF  GAIN_DB_HALF_OF_X10(POST_GAIN_CODEC_DB_X10)

#if (WM8904_AUDIO_PRE_HALF < GAIN_DB_HALF_MIN) || (WM8904_AUDIO_PRE_HALF > GAIN_DB_HALF_MAX)
#error "PRE_GAIN_CODEC_DB_X10 is outside what src/app/dsp/gain_db_tables.h covers -- regenerate the table with a wider grid or pick a value inside it."
#endif
#if (WM8904_AUDIO_POST_HALF < GAIN_DB_HALF_MIN) || (WM8904_AUDIO_POST_HALF > GAIN_DB_HALF_MAX)
#error "POST_GAIN_CODEC_DB_X10 is outside what src/app/dsp/gain_db_tables.h covers -- regenerate the table with a wider grid or pick a value inside it."
#endif

/*
 * WHAT THE BLOCK CALLBACK PUTS ON CODEC-IN -> CODEC-OUT.
 * ------------------------------------------------------
 * GAIN is the default on any board that can run it: CODEC-IN reaches CODEC-OUT through the
 * SW0 mute ramp, which scales all 256 slots of every block unconditionally. It is the
 * default because it is what this configuration exists to MEASURE -- a gain stage with a
 * branch-free, constant cost per slot, whose share of the block period is the number
 * quoted in docs/. A path that had to be switched into by hand is a path whose figures get
 * misattributed to the one that boots.
 *
 * The other three are the comparison points, and they exist because "sound comes out that
 * is not the input, and it keeps coming out with the input disconnected" does not say WHICH
 * DIRECTION is at fault, and one flash per hypothesis is the expensive way to find out:
 *
 *   COPY  the classic loopback: slots copied verbatim, no decode/scale/encode and no gain
 *         stage in the way. The cheapest callback possible (the transport header's
 *         wire-slot note: a raw passthrough needs no host<->wire conversion at all), so it
 *         is the baseline the gain path's cost is measured AGAINST.
 *   MUTE  digital zeros. If a tone survives this, nothing we transmit can be causing it --
 *         it is codec-internal or clocking, not our data path.
 *   TONE  a dsPIC-generated 1 kHz sine into slots 0/1, CODEC-IN ignored. A clean 1 kHz at
 *         the analog output proves the whole dsPIC->codec direction (slot alignment,
 *         32-bit encode, DAC path) and pins the fault on the RX side.
 *
 * TONE IS NOT IN THE *tp CYCLE (see wm8904_audio_path_next): it is the one position that
 * DRIVES the analog output with something loud and synthetic, and cycling onto it by
 * accident -- one keystroke past gain, into headphones -- is a different class of surprise
 * from cycling onto a copy or a silence. It stays reachable by index, `*tp0002`, which is
 * an explicit request that cannot be typed by walking a cycle.
 *
 * One byte, written only by the main loop and read only by the ISR, so no lock is
 * needed on this 16-bit core -- the same argument as s_mute_request above.
 */
typedef enum {
    WM8904_PATH_COPY = 0,   /* CODEC-IN -> CODEC-OUT, verbatim */
    WM8904_PATH_MUTE,       /* digital zeros */
    WM8904_PATH_TONE,       /* dsPIC-generated 1 kHz sine, CODEC-IN ignored */
    WM8904_PATH_GAIN,       /* CODEC-IN -> CODEC-OUT through the SW0 mute ramp */
    WM8904_PATH_CHAIN,      /* TDMin -> AVAS -> Gain -> TDMout; the normal path */
    WM8904_PATH_COUNT
} wm8904_path_t;

static volatile uint8_t s_path;   /* main loop writes only; wm8904_path_t */

/*
 * RX INSTRUMENTATION -- what the codec is actually sending us, without a scope.
 *
 * Two things are worth knowing and neither is visible on the console today: whether
 * CODEC-IN carries a signal at all, and WHICH SLOTS it arrives in. A level meter answers
 * the first; a per-slot activity mask answers the second, and a wrong slot map is one of
 * the few faults that produce steady output unrelated to the input.
 *
 * The callback does not calculate either one. The DMA half is reused before the slow
 * foreground poll can inspect it, so the callback publishes ONE rotating frame's top
 * words to a tiny seqlock-protected mailbox. The foreground consumes that mailbox at
 * 1 ms cadence and performs the magnitude, peak-hold and mask arithmetic. This is the
 * same hot-path rule used by Sonora's profiler: collect raw values in the callback;
 * do the work that can wait in foreground.
 *
 * The deliberate quality trade: this is a sampled meter, not a full 32-frame scan. A
 * non-zero low word with a zero top word is also treated as silent. At about 1 kHz this
 * is ample for the diagnostic question (is audio present, and in which slot?) while
 * removing the 256-slot branch-heavy scan from every 0.67 ms callback.
 */
#if (NORA_TDM_SLOTS_PER_FS > 16)
#error "s_rx_slot_mask holds 16 slots; widen it before configuring a wider TDM frame."
#endif

/* ISR publishes raw, one-frame observations. A sequence is odd while it writes and
 * even when the foreground may copy a coherent frame. All shared units are 16 bit,
 * therefore individually atomic on this core. */
static volatile uint16_t s_rx_observe_word[NORA_TDM_SLOTS_PER_FS];
static volatile uint16_t s_rx_observe_seq;
static uint16_t          s_rx_observe_seen;
static uint16_t          s_rx_observe_frame;  /* ISR-owned rotating frame index */

/* Foreground-owned aggregates. They no longer need volatile or an ISR/main clear race. */
static uint16_t s_rx_peak[2];      /* slots 0 and 1, top 16 bits of |sample| */
static uint16_t s_rx_slot_mask;    /* bit n: sampled top word was non-zero */

/*
 * 1 kHz at fs = 48 kHz is exactly 48 samples per period, so the table IS the period and
 * the tone has no phase error to accumulate -- deliberately not a recursive oscillator,
 * whose amplitude drifts, and deliberately a sine rather than a square: the artefact
 * being chased is itself square-ish, and a test signal that looks like the fault is a
 * test signal that cannot be told apart from it. Q15; 96 bytes of flash.
 *
 * The shift sets the level: 32767 << 14 = 0x1FFFC000, i.e. a quarter of full scale
 * (-12 dBFS). Loud enough to read on a scope and to hear over the artefact, quiet enough
 * to put on headphones by accident.
 */
#define WM8904_AUDIO_TONE_STEPS  48
#define WM8904_AUDIO_TONE_SHIFT  14

static const int16_t s_tone_q15[WM8904_AUDIO_TONE_STEPS] = {
         0,   4277,   8481,  12539,  16384,  19948,  23170,  25996,
     28377,  30273,  31650,  32486,  32767,  32486,  31650,  30273,
     28377,  25996,  23170,  19948,  16384,  12539,   8481,   4277,
         0,  -4277,  -8481, -12539, -16384, -19948, -23170, -25996,
    -28377, -30273, -31650, -32486, -32767, -32486, -31650, -30273,
    -28377, -25996, -23170, -19948, -16384, -12539,  -8481,  -4277
};

static uint16_t s_tone_phase;   /* ISR-owned */

/*
 * The periodic TDM1 load line, on or off at runtime (*tq0001 / *tq0000).
 *
 * DEFAULT ON, and that is a correction. Asked to stop showing
 * `last=/deadline=` -- two absolute-microsecond figures that read as
 * confusing beside a percentage -- this file first silenced the WHOLE line, which threw
 * away the running evidence that blocks are still arriving and that none were missed in
 * order to hide two fields of it. The fields are now what got dropped
 * (dsp_load_print()'s `detail`, reachable one line at a time with ?tq), and the line
 * itself is back on by default.
 *
 * The switch stays, because there is a real use for silencing the line: while a fault is
 * chased by ear and by scope, periodic traffic interleaves with every other message and
 * costs more than it tells. Off is a POSITION, reversible from the console.
 */
static bool s_load_line = true;

/* -------------------------------------------------------------------------- */
/* The transport HAL's port. Its configure_pins is handed straight through to   */
/* the board's, so the board sees the role this build asked for.                */
/* -------------------------------------------------------------------------- */
static bool wm8904_audio_configure_pins(nora_spi_i2s_tdm_clock_role_t role)
{
    if ((s_cfg == NULL) || (s_cfg->port == NULL) ||
        (s_cfg->port->configure_pins == NULL)) {
        return false;
    }

    return s_cfg->port->configure_pins(role);
}

static const nora_spi_i2s_tdm_port_t s_transport_port = {
    .configure_pins      = wm8904_audio_configure_pins,
    /*
     * NULL on purpose, in BOTH roles, and worth stating because it is easy to assume this
     * is where the 50%-duty FS comes from. It is not: this hook is the CLC bypass route
     * that fans a SLAVE's incoming clock back out to another device, and no profile here
     * has a second device to feed. The master path's CLC1 belongs to the HAL
     * (hal_spi_i2s_tdm/..._fs_clc.c), which locates the FS pin by reverse-scanning the
     * RPOR table for whatever the board routed FRMSYNC to -- so neither this module nor
     * any board needs to know a CLC exists.
     */
    .clc_passthrough     = 0,
    .clock_source_init   = 0,
    .clock_source_ready  = 0,   /* NULL => always ready, in either role (self-clocked
                                 * master, or slave to whichever side drives BCLK/FS) */
    .consume_clock_event = 0,
};

/* -------------------------------------------------------------------------- */
/* Block callback: one of the four paths above, plus the RX instrumentation.     */
/* -------------------------------------------------------------------------- */

/*
 * Magnitude of a slot, to 16 bits, WITHOUT decoding it.
 *
 * wire[0] is the MS half of the Q31 sample, which is exactly the level meter's
 * resolution, so this reads one word instead of assembling two. Negatives are folded with
 * a complement rather than a negate: it is one instruction, and it is off by one LSB of
 * a 16-bit magnitude, which no level display can show. Full scale reads 0x7FFF.
 */
static uint16_t rx_word_mag16(uint16_t ms)
{
    return (uint16_t)(((ms & 0x8000u) != 0u) ? (uint16_t)~ms : ms);
}

/*
 * Observe CODEC-IN. Runs in EVERY path, including MUTE and TONE, on purpose: in those
 * two the output is known, so the RX report is the only thing the block still measures --
 * "the tone survives a muted output AND the input meter reads zero" is a different fault
 * from "the meter reads full scale".
 */
static void rx_observe(const nora_tdm_slot_t *src)
{
    const nora_tdm_slot_t *frame =
        &src[(size_t)s_rx_observe_frame * (size_t)NORA_TDM_SLOTS_PER_FS];
    size_t slot;

    /* Seqlock publication: foreground discards an observation if this callback starts
     * or ends during its copy. No interrupt mask is needed, so the ISR never waits for
     * foreground code. */
    s_rx_observe_seq++;
    for (slot = 0u; slot < (size_t)NORA_TDM_SLOTS_PER_FS; slot++) {
        s_rx_observe_word[slot] = frame[slot].wire[0];
    }
    s_rx_observe_seq++;

    s_rx_observe_frame++;
    if (s_rx_observe_frame >= (uint16_t)NORA_TDM_BLOCK_FRAMES) {
        s_rx_observe_frame = 0u;
    }
}

/* Main-context half of rx_observe(). The board calls this from its wait loop at 1 ms;
 * wm8904_audio_poll() also calls it as a safe, slower fallback for another board. */
void wm8904_audio_rx_observe_poll(void)
{
    uint16_t observed[NORA_TDM_SLOTS_PER_FS];
    uint16_t before;
    uint16_t after;
    uint16_t mask = 0u;
    uint16_t mag;
    size_t   slot;

    if (!s_started) {
        return;
    }

    before = s_rx_observe_seq;
    if (((before & 1u) != 0u) || (before == s_rx_observe_seen)) {
        return;
    }

    for (slot = 0u; slot < (size_t)NORA_TDM_SLOTS_PER_FS; slot++) {
        observed[slot] = s_rx_observe_word[slot];
    }
    after = s_rx_observe_seq;
    if ((before != after) || ((after & 1u) != 0u)) {
        return;
    }
    s_rx_observe_seen = after;

    for (slot = 0u; slot < (size_t)NORA_TDM_SLOTS_PER_FS; slot++) {
        if (observed[slot] != 0u) {
            mask |= (uint16_t)(1u << slot);
        }
    }
    s_rx_slot_mask |= mask;

    mag = rx_word_mag16(observed[0]);
    if (mag > s_rx_peak[0]) {
        s_rx_peak[0] = mag;
    }
    mag = rx_word_mag16(observed[1]);
    if (mag > s_rx_peak[1]) {
        s_rx_peak[1] = mag;
    }
}

/* CODEC-IN -> CODEC-OUT, verbatim. No conversion: both sides are already in wire
 * order, so copying the slot is both correct and free. Unrolled; see
 * WM8904_AUDIO_UNROLL for why the loop and not the copy was the cost. */
static void path_copy(const nora_tdm_slot_t *src,
                      nora_tdm_slot_t       *dst)
{
    size_t groups = WM8904_AUDIO_HALF_SAMPLES / WM8904_AUDIO_UNROLL;

    /* Counting DOWN to zero, and `do`, not `for`: the exit test is then the decrement's own
     * Z flag with nothing to compare against, and the loop cannot be entered with a zero
     * count (the assert above rules that out). */
    do {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        dst[4] = src[4];
        dst[5] = src[5];
        dst[6] = src[6];
        dst[7] = src[7];

        src += WM8904_AUDIO_UNROLL;
        dst += WM8904_AUDIO_UNROLL;
    } while (--groups != 0u);
}

/* Digital zeros. Written as the wire words rather than through encode_s32() because
 * zero is the one value whose halves cannot be swapped wrongly.
 *
 * Still writes every slot of every block, deliberately: a "skip it, the half is already
 * zero from last time" test would make this path nearly free, and this path is the DOCUMENTED
 * BASELINE that the other two are compared against (docs/ck_silicon_findings.md). A baseline
 * that stops doing the work stops being one. */
static void path_mute(nora_tdm_slot_t *dst)
{
    size_t groups = WM8904_AUDIO_HALF_SAMPLES / WM8904_AUDIO_UNROLL;

    do {
        dst[0].wire[0] = 0u;  dst[0].wire[1] = 0u;
        dst[1].wire[0] = 0u;  dst[1].wire[1] = 0u;
        dst[2].wire[0] = 0u;  dst[2].wire[1] = 0u;
        dst[3].wire[0] = 0u;  dst[3].wire[1] = 0u;
        dst[4].wire[0] = 0u;  dst[4].wire[1] = 0u;
        dst[5].wire[0] = 0u;  dst[5].wire[1] = 0u;
        dst[6].wire[0] = 0u;  dst[6].wire[1] = 0u;
        dst[7].wire[0] = 0u;  dst[7].wire[1] = 0u;

        dst += WM8904_AUDIO_UNROLL;
    } while (--groups != 0u);
}

/*
 * 1 kHz sine into slots 0/1 (the two the WM8904 converts), zeros in the rest. CODEC-IN
 * is not read at all, so what comes out of the analog output in this path is entirely
 * ours -- which is the point of it.
 */
static void path_tone(nora_tdm_slot_t *dst)
{
    size_t frame;

    for (frame = 0u; frame < (size_t)NORA_TDM_BLOCK_FRAMES; frame++) {
        const size_t  base   = frame * (size_t)NORA_TDM_SLOTS_PER_FS;
        const int32_t sample = (int32_t)s_tone_q15[s_tone_phase] << WM8904_AUDIO_TONE_SHIFT;
        size_t        slot;

        s_tone_phase++;
        if (s_tone_phase >= (uint16_t)WM8904_AUDIO_TONE_STEPS) {
            s_tone_phase = 0u;
        }

        nora_tdm_slot_encode_s32(&dst[base + 0u], sample);
        nora_tdm_slot_encode_s32(&dst[base + 1u], sample);

        for (slot = 2u; slot < (size_t)NORA_TDM_SLOTS_PER_FS; slot++) {
            dst[base + slot].wire[0] = 0u;
            dst[base + slot].wire[1] = 0u;
        }
    }
}

/*
 * Apply a pending button request on the ISR side, so every write to s_gain happens at
 * this priority and the main loop never races it (see the ownership note above). Once per
 * block is soon enough: a block is ~0.65 ms at 48 kHz/32 frames, versus a main-loop poll
 * measured in hundreds of milliseconds.
 *
 * Shared by the gain path and the chain's gain stage. Exactly ONE of them runs per block
 * (the paths are exclusive), so the ramp still advances once per block either way -- which
 * is the property gain_ctrl.h's "a step is now a block" rests on.
 */
static void gain_stage_apply_pending(void)
{
    uint8_t want = s_mute_request;   /* published by the main loop, one byte */

    if (want != s_mute_applied) {
        s_mute_applied = want;
        gain_ctrl_mute_set(&s_gain, (want != 0u), s_cfg->gain_ramp_ms);
    }
}

/*
 * The same trick for PRE/POST: the ISR converges on whatever half-decibel value the main
 * loop last published, doing the table lookup at its own priority so no torn (mantissa,
 * shift) pair can exist. Two comparisons per block, and the lookup only when a *ti or *to
 * actually arrived.
 *
 * No ramp, deliberately. This is a calibration, not a fader: it is set once for a board and
 * then left, and the value it steps between while someone is tuning by ear is a step of
 * 0.5 dB -- below the level difference that reads as a click. gain_ctrl is where a ramp
 * belongs, and it is still in the chain right after this.
 */
static void gain_db_apply_pending(void)
{
    const int16_t pre  = s_pre_half_request;
    const int16_t post = s_post_half_request;

    if (pre != s_pre_half_applied) {
        s_pre_half_applied = pre;
        gain_db_from_half(pre, &s_pre_gain);
    }
    if (post != s_post_half_applied) {
        s_post_half_applied = post;
        gain_db_from_half(post, &s_post_gain);
    }
}

static void path_gain(const nora_tdm_slot_t *src,
                      nora_tdm_slot_t       *dst)
{
    gain_stage_apply_pending();

    /*
     * ONE gain for the whole block, hoisted out of the loop, and ONE FLAT LOOP over the
     * block's slots.
     *
     * Both halves of that are the point. The gain is fetched once per block instead of
     * once per frame, so it is loop-invariant and -Os keeps it in a W register instead of
     * re-calling gain_ctrl and reloading it 32 times; and with the gain no longer changing
     * per frame, the frame/slot nest was only counting contiguous samples the long way
     * round (frame-major indexing is what makes flattening safe). The cost is ramp
     * resolution -- one step per 0.67 ms block, about 0.2 dB on a 300 ms ramp -- which is
     * inaudible and was accepted deliberately; see gain_ctrl.h.
     *
     * The scale itself is the transport's own helper: wire order in, wire order out, no
     * decode/encode round trip, and exact at unity (which is 100% of the steady state).
     * The traversal is here rather than inside gain_ctrl because that module takes plain
     * int32_t buffers and must stay free of this part's DMA layout (see gain_ctrl.h).
     *
     * The loop is unrolled for the reason given at WM8904_AUDIO_UNROLL: after the Q15 change
     * the arithmetic is ~11 cycles of the ~19 this path spends per slot, and most of the
     * remainder is the loop, not the work.
     */
    {
        const uint16_t gain_q15 = gain_ctrl_next_block_gain_q15(&s_gain);
        size_t         groups   = WM8904_AUDIO_HALF_SAMPLES / WM8904_AUDIO_UNROLL;

        do {
            nora_tdm_slot_scale_q15(&src[0], &dst[0], gain_q15);
            nora_tdm_slot_scale_q15(&src[1], &dst[1], gain_q15);
            nora_tdm_slot_scale_q15(&src[2], &dst[2], gain_q15);
            nora_tdm_slot_scale_q15(&src[3], &dst[3], gain_q15);
            nora_tdm_slot_scale_q15(&src[4], &dst[4], gain_q15);
            nora_tdm_slot_scale_q15(&src[5], &dst[5], gain_q15);
            nora_tdm_slot_scale_q15(&src[6], &dst[6], gain_q15);
            nora_tdm_slot_scale_q15(&src[7], &dst[7], gain_q15);

            src += WM8904_AUDIO_UNROLL;
            dst += WM8904_AUDIO_UNROLL;
        } while (--groups != 0u);
    }
}

/* -------------------------------------------------------------------------- */
/* THE CHAIN: TDMin -> AVAS -> Gain -> TDMout.                                 */
/*                                                                            */
/* Four stages over one buffer, and the reason it is a chain and not a fifth   */
/* exclusive writer of the TX half is audible. As a path, the AVAS synth's     */
/* ~3 s release fade was never heard: stopping it closed the gate and switched  */
/* the path away in the same call, so the block ISR stopped calling the engine  */
/* and the fade was thrown away -- an abrupt mute, and a gate left frozen at    */
/* 1.0, which then skipped the phase reset and the 4 s attack on the NEXT       */
/* start. In a chain, "AVAS stopped" is simply the chain without that stage's   */
/* contribution, so the fade renders because nothing stops calling it. sonora   */
/* has always been shaped this way (fx_domain_48k.c calls its AVAS source       */
/* unconditionally), which is why the same time constants sound right there.    */
/*                                                                            */
/* MONO INTO BOTH CONVERTED SLOTS -- the model is a single measured spectrum,   */
/* not a stereo image, and duplicating it is what the AK engine's 48 kHz mono   */
/* source does before its own channel expansion.                              */
/*                                                                            */
/* MIXED, not substituted: the synth is added to whatever CODEC-IN carries,     */
/* which is what a chain means and what sonora does. That needs the saturating  */
/* add below; the old path could not overflow because it ignored the input.     */
/*                                                                            */
/* The level shift is deliberately the tone path's and not full scale. The      */
/* engine normalises its own 60 s peak to 0.9 of Q15 (that is what reproduces   */
/* the reference WAV), so << 16 would put 0.9 FS on the wire. << 14 is 12 dB    */
/* below that, for the same reason path_tone() is: quiet enough to put on       */
/* headphones by accident. Use 16 for a level A/B against out_lines_L1.wav.     */
/*                                                                            */
/* Cost: the engine measured 289 us of the 667 us block (doc section 24), and   */
/* the stages around it are the copy baseline's order of magnitude. Read it off */
/* `?tl`, not off the boot log; an overrun shows up as miss>0 and a negative    */
/* margin, not as bad audio.                                                   */
/* -------------------------------------------------------------------------- */
#define WM8904_AUDIO_AVAS_SHIFT  14

/*
 * Saturating 32-bit add, in unsigned arithmetic so the wrap it tests for is defined.
 * Overflow is "both operands share a sign the sum does not". No int64_t on purpose: a
 * 64-bit add here would pull in the helper measured at 88 cycles a sample in this engine's
 * own history (doc section: the ___muldi3 finding).
 */
static inline int32_t chain_sat_add(int32_t a, int32_t b)
{
    const uint32_t sum = (uint32_t)a + (uint32_t)b;

    if (((((uint32_t)a ^ sum) & ((uint32_t)b ^ sum)) & 0x80000000u) != 0u) {
        return (a < 0) ? INT32_MIN : INT32_MAX;
    }

    return (int32_t)sum;
}

static void chain_stage_avas(void)
{
    size_t frame;

    for (frame = 0u; frame < (size_t)NORA_TDM_BLOCK_FRAMES; frame++) {
        const int32_t sample = (int32_t)avas_line_ck_process_sample(&s_avas)
                               << WM8904_AUDIO_AVAS_SHIFT;
        const size_t  at     = frame * WM8904_AUDIO_CHAIN_SLOTS;

        s_chain[at + 0u] = chain_sat_add(s_chain[at + 0u], sample);
        s_chain[at + 1u] = chain_sat_add(s_chain[at + 1u], sample);
    }
}

static void path_chain(const nora_tdm_slot_t *src,
                       nora_tdm_slot_t       *dst)
{
    size_t frame;

    /*
     * 1. TDMin. The slots the chain does not process are passed through by the tuned,
     *    unrolled copy rather than by a second hand-written traversal here -- that loop
     *    is the one that was measured (WM8904_AUDIO_UNROLL), and slots 0/1 are simply
     *    overwritten by stage 4. Copying two slots twice is cheaper than paying this
     *    file's loop overhead a second time; `copy - mute` says so (0.47 cycles/slot).
     */
    path_copy(src, dst);

    /*
     * PRE and POST are resolved once per block, before either loop, and the mantissa is then
     * held in a local. That is the same hoist path_gain() does with its block gain and for
     * the same reason: fetched per frame it would be reloaded 32 times.
     *
     * The saturation counts are accumulated in LOCALS and folded in once at the end -- a
     * pointer straight at the static would make -Os reload and re-store it every sample,
     * for a counter that matters only between two ?ti's.
     */
    gain_db_apply_pending();

    {
    const uint16_t pre_mant  = s_pre_gain.mant;
    const uint16_t post_mant = s_post_gain.mant;
    uint16_t       pre_sat   = 0u;
    uint16_t       post_sat  = 0u;

    /*
     * 1b. PRE, folded into the decode. It is here rather than in a loop of its own because
     *     the sample is already in a register at this point -- the stage costs the multiply
     *     and nothing else.
     *
     *     UNITY IS SKIPPED, and the decision is made ONCE PER BLOCK. The first version
     *     applied the scale unconditionally, arguing that at 0.0 dB it is the identity on
     *     every audio bit (true) and that a per-sample branch would cost more than the
     *     multiply it skipped (also true). Both are true and the conclusion was still wrong,
     *     because the choice is not per sample: the mantissa cannot change inside a block, so
     *     ONE test buys the whole block. 64 samples of arithmetic for one compare, and the
     *     shipped configuration (POST 0.0 dB) stops paying for a stage it does not use.
     *
     *     Applied BEFORE the AVAS stage, so the synth is not scaled by it -- PRE means
     *     "the input", as it does in sonora (PRE_GAIN_CODEC_DB, and a separate
     *     PRE_GAIN_AVAS_SYNTH_DB for the synth). It is also therefore the level the peak
     *     meter and any input-driven decision see, which is the intended meaning.
     */
    if (GAIN_DB_IS_UNITY(pre_mant)) {
        for (frame = 0u; frame < (size_t)NORA_TDM_BLOCK_FRAMES; frame++) {
            const size_t base = frame * (size_t)NORA_TDM_SLOTS_PER_FS;
            const size_t at   = frame * WM8904_AUDIO_CHAIN_SLOTS;

            s_chain[at + 0u] = nora_tdm_slot_decode_s32(&src[base + 0u]);
            s_chain[at + 1u] = nora_tdm_slot_decode_s32(&src[base + 1u]);
        }
    } else {
        for (frame = 0u; frame < (size_t)NORA_TDM_BLOCK_FRAMES; frame++) {
            const size_t base = frame * (size_t)NORA_TDM_SLOTS_PER_FS;
            const size_t at   = frame * WM8904_AUDIO_CHAIN_SLOTS;

            s_chain[at + 0u] = gain_db_scale(nora_tdm_slot_decode_s32(&src[base + 0u]),
                                             pre_mant, &pre_sat);
            s_chain[at + 1u] = gain_db_scale(nora_tdm_slot_decode_s32(&src[base + 1u]),
                                             pre_mant, &pre_sat);
        }
    }

    /*
     * 2. AVAS. Run for as long as the engine says it is still rendering, which is the
     *    whole release fade and not just up to the stop request. The ISR clears the flag
     *    itself, so the stage costs nothing once the tail is below -50 dB and the engine
     *    is then untouched by this priority.
     */
    if (s_chain_avas_run) {
        chain_stage_avas();

        if (!avas_line_ck_is_active(&s_avas)) {
            s_chain_avas_run = false;
        }
    }

    /* 3. Gain -- the SW0 mute ramp, on the two slots that reach a converter. */
    gain_stage_apply_pending();
    gain_ctrl_process_block(&s_gain, s_chain, s_chain,
                            (size_t)NORA_TDM_BLOCK_FRAMES,
                            WM8904_AUDIO_CHAIN_SLOTS);

    /*
     * 4. TDMout, with POST folded into the encode -- the mirror of stage 1b.
     *
     *    AFTER the mix, so POST scales the AVAS synth by the same dB as the input. That is
     *    what "output gain" means, and it is why the analog-attenuation compensation is
     *    carried by PRE instead (0.0 dB ships here, i.e. the identity): PRE lifts only the
     *    input and leaves WM8904_AUDIO_AVAS_SHIFT's ear-set level alone. A non-zero POST
     *    moves the synth too, and the fix if that is then too loud is that shift, not this
     *    stage.
     *
     *    Unity is skipped per block, as in 1b -- and this is the stage where that matters
     *    most, because 0.0 dB is what ships here.
     */
    if (GAIN_DB_IS_UNITY(post_mant)) {
        for (frame = 0u; frame < (size_t)NORA_TDM_BLOCK_FRAMES; frame++) {
            const size_t base = frame * (size_t)NORA_TDM_SLOTS_PER_FS;
            const size_t at   = frame * WM8904_AUDIO_CHAIN_SLOTS;

            nora_tdm_slot_encode_s32(&dst[base + 0u], s_chain[at + 0u]);
            nora_tdm_slot_encode_s32(&dst[base + 1u], s_chain[at + 1u]);
        }
    } else {
        for (frame = 0u; frame < (size_t)NORA_TDM_BLOCK_FRAMES; frame++) {
            const size_t base = frame * (size_t)NORA_TDM_SLOTS_PER_FS;
            const size_t at   = frame * WM8904_AUDIO_CHAIN_SLOTS;

            nora_tdm_slot_encode_s32(&dst[base + 0u],
                                          gain_db_scale(s_chain[at + 0u], post_mant,
                                                        &post_sat));
            nora_tdm_slot_encode_s32(&dst[base + 1u],
                                          gain_db_scale(s_chain[at + 1u], post_mant,
                                                        &post_sat));
        }
    }

    /* Fold the block's clamp counts in. Saturating rather than wrapping AT the fold would
     * cost a branch to buy nothing: the question these answer is "does it clamp", and the
     * report says the count is since the last read. */
    s_pre_sat  = (uint16_t)(s_pre_sat + pre_sat);
    s_post_sat = (uint16_t)(s_post_sat + post_sat);
    }
}


static void wm8904_audio_block_cb(const nora_tdm_slot_t *src,
                                  nora_tdm_slot_t *dst,
                                  void *user)
{
    /* Read the mode ONCE. Re-reading it per path would let a *tp arriving mid-block
     * produce a half-copied, half-muted block, i.e. a click that belongs to the switch
     * rather than to the fault being measured. */
    const uint8_t mode = s_path;

    (void)user;

    rx_observe(src);

    switch (mode) {
    case (uint8_t)WM8904_PATH_MUTE:
        path_mute(dst);
        break;

    case (uint8_t)WM8904_PATH_TONE:
        path_tone(dst);
        break;

    case (uint8_t)WM8904_PATH_GAIN:
        path_gain(src, dst);
        break;

    case (uint8_t)WM8904_PATH_CHAIN:
        path_chain(src, dst);
        break;

    case (uint8_t)WM8904_PATH_COPY:
    default:
        /* default = COPY, so a mode byte that somehow went out of range degrades to the
         * plain loopback rather than to an unwritten TX buffer (which the DMA would
         * still transmit -- stale audio, not silence). */
        path_copy(src, dst);
        break;
    }
}

/* Short names, used by the bring-up banner and by *tp/?tp so one spelling appears
 * everywhere the mode is mentioned. */
static const char *path_name(uint8_t mode)
{
    switch (mode) {
    case (uint8_t)WM8904_PATH_COPY: return "copy (CODEC-IN -> CODEC-OUT, verbatim)";
    case (uint8_t)WM8904_PATH_MUTE: return "mute (digital zeros out)";
    case (uint8_t)WM8904_PATH_TONE: return "tone (dsPIC 1kHz sine out, input ignored)";
    case (uint8_t)WM8904_PATH_GAIN: return "gain (CODEC-IN -> CODEC-OUT via SW0 mute ramp)";
    case (uint8_t)WM8904_PATH_CHAIN: return "chain (TDMin -> AVAS -> Gain -> TDMout)";
    default:                        return "(unknown)";
    }
}

/* -------------------------------------------------------------------------- */
static void report_transport_error(const char *what)
{
    console_out_str("WM8904 audio: ");
    console_out_str(what);
    console_out_str(" failed, err=");
    console_out_u32((uint32_t)nora_spi_i2s_tdm_get_last_error());
    console_out_str("\n");
}

#if WM8904_AUDIO_ENABLE_TDM_DIAG
/* -------------------------------------------------------------------------- */
/* *tb / ?tb -- which parts of the engine run.                                 */
/*                                                                            */
/* The whole engine ONCE measured 115 % of the block period and could not be       */
/* selected at all; it now fits with margin and *tb0007 is the boot default (the    */
/* 115 % was generated code, not the algorithm -- see the *tb note in              */
/* boards/ev88g73a/main.c). So this is no longer how the engine is made to fit.     */
/*                                                                            */
/* It is kept because dropping ENVELOPE still produces sound (the clusters degrade  */
/* to pure tones at their centroid frequencies), which makes switching it on and    */
/* off the cheapest way to HEAR what the 185 lines contribute.                     */
/*                                                                            */
/* CAUTION, and it is why avas_line_ck_is_active() has a case for this: clearing    */
/* the GATE bit pins the gate wide open, so there is no fade left to render and a   */
/* stop takes effect immediately instead of over ~3 s.                             */
/* -------------------------------------------------------------------------- */
void wm8904_audio_avas_parts_set(uint8_t parts)
{
    avas_line_ck_set_parts(&s_avas, parts);
    console_out_str(" *tb: parts -> ");
    console_out_u32((uint32_t)s_avas.parts);
    console_out_str(((s_avas.parts & AVAS_TYPE_TY_CK_PART_CARRIERS) != 0u)
                        ? " carriers" : " (no carriers: silent, measurement only)");
    if ((s_avas.parts & AVAS_TYPE_TY_CK_PART_ENVELOPE) != 0u) {
        console_out_str(" +envelope");
    } else {
        console_out_str(" +FROZEN envelope (pure tones, thin by design)");
    }
    if ((s_avas.parts & AVAS_TYPE_TY_CK_PART_GATE) != 0u) {
        console_out_str(" +gate");
    } else {
        console_out_str(" +gate held open");
    }
#if AVAS_LINE_CK_HAVE_NOISE
    /* Echoed only where the bank exists, and it is the one part whose bit can be set
     * while nothing happens: the bank belongs to the Type_LB, so a Type_TY-sounding
     * build with 8 set is not lying, it is simply not the sounding voice. */
    console_out_str(((s_avas.parts & AVAS_TYPE_TY_CK_PART_NOISE) != 0u)
                        ? " +noise" : " +noise OFF (tone only)");
#endif
    console_out_str("\n");
}


void wm8904_audio_avas_parts_report(void)
{
    console_out_str(" ?tb: parts = ");
    console_out_u32((uint32_t)s_avas.parts);
    console_out_str(" (" AVAS_TYPE_TY_CK_PART_HELP ")\n");
}


void wm8904_audio_avas_parts_usage(void)
{
    console_out_str(" *tb: needs a mask -- " AVAS_TYPE_TY_CK_PART_HELP
                    ", e.g. *tb000f = all\n");
}


bool wm8904_audio_avas_parts_valid(uint16_t value)
{
    return (value <= (uint16_t)AVAS_TYPE_TY_CK_PART_ALL);
}

#endif /* WM8904_AUDIO_ENABLE_TDM_DIAG -- *tb */


/* -------------------------------------------------------------------------- */
/* Does the rate this transport will actually run at match the rate the AVAS    */
/* coefficient table was generated for?                                        */
/*                                                                            */
/* This has to be a RUNTIME check, not the #error avas_synth_line_ck.h offers, because */
/* the rate is a runtime property of the config: as dsPIC master, brg picks it   */
/* (brg=3 -> BCLK 12.5 MHz -> TDM8/32 -> 48828.125 Hz); as slave, the codec's    */
/* own BCLK/FS does (48000 Hz, measured at jumper A-XTAL). Both are legitimate.  */
/*                                                                            */
/* Why it is worth code at all: bb_step and car_step are both scaled by fs, so a */
/* table generated for 48000 running at 48828 plays 1.7 % sharp -- 29 cents. That */
/* is a pitch error, and a pitch error in a synthesised engine note is exactly the */
/* kind of wrong that sounds fine. The engine still works at either rate; it just */
/* has to be REGENERATED for the one in use, which is one command.               */
/* -------------------------------------------------------------------------- */
static bool avas_rate_matches(const wm8904_audio_config_t *cfg)
{
    const uint32_t table_fs = (uint32_t)AVAS_TYPE_TY_CK_TABLE_FS_HZ;
    uint32_t       run_fs;
    uint32_t       diff;

    if (cfg->dspic_is_master) {
        /* BCLK = Fp / (2 * (brg + 1)); one frame is SLOTS_PER_FS 32-bit wire slots. */
        const uint32_t bclk = nora_clock_get_fcy_hz()
                              / (2u * ((uint32_t)cfg->brg + 1u));
        run_fs = bclk / ((uint32_t)NORA_TDM_SLOTS_PER_FS * 32u);
    } else {
        run_fs = cfg->sample_rate_hz;
    }

    diff = (run_fs > table_fs) ? (run_fs - table_fs) : (table_fs - run_fs);

    /* 0.1 %: tight enough to catch 48000-vs-48828, loose enough to survive the
     * integer division above rounding a few Hz off.
     *
     * uint32 AND NOT uint64, which this was: `diff * 1000` cannot overflow here --
     * diff is a difference of audio sample rates, so 4 294 967 295 / 1000 = 4.29 MHz
     * of error is the bound, and anything remotely near it is refused by this very
     * test. The 64-bit form dragged a libgcc long-long helper into an image at 99 %
     * of a 64 KB part to compare two numbers under 10^8, which is the same
     * int64-costs-real-flash-and-cycles argument the engine itself is built on. */
    if ((diff * 1000u) <= table_fs) {
        return true;
    }

    console_out_str("WM8904 audio: AVAS path disabled -- table is for ");
    console_out_u32(table_fs);
    console_out_str(" Hz, transport runs at ");
    console_out_u32(run_fs);
    console_out_str(" Hz (regenerate: tools/gen_avas_type_ty_ck_tables.py)\n");
    return false;
}


void wm8904_audio_start(const wm8904_audio_config_t *cfg)
{
    nora_spi_i2s_tdm_inst_t *spi1;
    nora_spi_i2s_tdm_config_t tcfg;

    if ((cfg == NULL) || (cfg->port == NULL)) {
        console_out_str("WM8904 audio: no port/config supplied\n");
        return;
    }

    s_cfg      = cfg;
    s_started  = false;
    s_stop_state = WM8904_AUDIO_STOP_NONE;
    s_use_gain = (cfg->gain_ramp_ms != 0u) &&
                 (cfg->port->mute_button_pressed != NULL);

    console_out_str("WM8904 audio: ");
    console_out_str((cfg->port->wiring != NULL) ? cfg->port->wiring : "(wiring not stated)");
    console_out_str("\n");

    /*
     * Load monitor time base. This used to always fail on this family: the HAL was
     * written against Timer2/3, which no CK part has. It is now SCCP1-based, so the
     * block-ISR load figures are real measurements. Reported rather than silently
     * zeroed if it ever fails -- a zero that means "not measured" is indistinguishable
     * from a zero that means "no load".
     */
    {
        nora_high_res_timer_config_t htcfg;

        htcfg.timer_clk_hz = nora_clock_get_fcy_hz();
        htcfg.run_in_idle  = true;
        if (nora_high_res_timer_init(&htcfg) != NORA_HIGH_RES_TIMER_OK) {
            console_out_str(
                "WM8904 audio: load monitor UNAVAILABLE (high-res timer init failed)"
                " -- load figures below are not measurements\n");
        } else {
            console_out_str("WM8904 audio: load monitor on SCCP1 @ Fcy ");
            console_out_u32(htcfg.timer_clk_hz);
            console_out_str(" Hz\n");
        }
    }

    /*
     * STEP 1 USED TO BE MCLK, and there is no step 1 now (2026-08-04): THIS FIRMWARE HAS NO
     * CODE THAT GENERATES OR OUTPUTS AN MCLK, on any board or in either TDM role. How the
     * codec's SYSCLK is physically supplied is a board/jumper fact this module does not
     * control and does not read: on EV88G73A the A jumper selects the codec's MCLK input,
     * A-XTAL = the CODEC-A board's XTAL (codec masters BCLK/FS at 48 000 Hz, measured) and
     * A-extMCLK = an incoming BCLK, which in a dsPIC-master build is OURS (board fact, from
     * the person who wired it, 2026-08-09; plan doc 20.4 + 21). Note what follows for the
     * order: at A-extMCLK the codec has NO clock at all when step 2 below runs, and it still
     * answered every register write (plan doc 19) -- so "a WM8904 with no SYSCLK does not
     * answer on I2C", the reason the deleted MCLK step had to come first, does not hold for
     * register I/O. That hook, and what to do instead if some future board must source MCLK,
     * are described in wm8904_audio.h. (Narrowed 2026-08-09: this read "the codec arrives
     * clocked by the XTAL on its own board, on every board in this tree and in either TDM
     * role", which asserted a board fact that was UNKNOWN at the time.)
     */

    /* 1) I2C control bus. */
    if (!cfg->port->i2c_init()) {
        console_out_str("WM8904 audio: I2C init failed\n");
        return;
    }

    /*
     * 2) Bring up the codec with the role opposite the dsPIC's -- exactly one side of
     * the pair drives BCLK/FS/LRCLK, and deriving both from one field is what stops the
     * two ends from being configured to agree with nobody.
     *
     * wm8904_init() returns false unless the device ID readback AND every configuration
     * write/readback verified. Checked rather than discarded: arming the transport and
     * unmuting a codec that never confirmed its own ID is exactly the kind of
     * unverified assumption this lab's SPI/DMA bring-up was about
     * (docs/ck_silicon_findings.md).
     */
    if (!wm8904_init(cfg->i2c_inst_legacy, /*master_cfg=*/!cfg->dspic_is_master)) {
        console_out_str(
            "WM8904 audio: codec init failed (no ID / I2C write not verified); "
            "staying stopped\n");
        return;
    }

    /*
     * 3) Configure + open the dsPIC SPI1 as a TDM8/32-bit transport.
     *
     * set_port() is CHECKED since phase 2: it refuses (ERR_ALREADY_OPEN) while the transport
     * is open or any leg is running. On a first bring-up neither is true, and on a *tr restart
     * wm8904_audio_stop() has already stopped and closed -- so a false here means the restart
     * sequence itself is wrong, which is exactly the thing worth reporting rather than
     * discarding. Registering the SAME table again is what makes it survive a restart at all.
     */
    if (!nora_spi_i2s_tdm_set_port(&s_transport_port)) {
        report_transport_error("set_port");
        return;
    }

    spi1 = nora_spi_i2s_tdm_spi1();
    if (spi1 == 0) {
        console_out_str("WM8904 audio: no SPI1 instance\n");
        return;
    }

    tcfg.format          = NORA_SPI_I2S_TDM_FORMAT_TDM;
    tcfg.slots_per_fs    = NORA_TDM_SLOTS_PER_FS;   /* TDM8 */
    tcfg.word_bits       = 32u;
    tcfg.block_frames    = NORA_TDM_BLOCK_FRAMES;
    /* IGNROV/IGNTUR are no longer config fields: the HAL keeps both set as its own
     * continuity policy. This module used to pass true for both, so nothing changed. */

    if (cfg->dspic_is_master) {
        tcfg.clock_role        = NORA_SPI_I2S_TDM_CLOCK_MASTER;
        /* FS_50PCT engages the CLC1 50%-duty generator the master path was
         * scope-verified with (docs/ck_silicon_findings.md). */
        tcfg.fs_shape    = NORA_SPI_I2S_TDM_FS_50PCT;
        tcfg.brg         = cfg->brg;
    } else {
        tcfg.clock_role        = NORA_SPI_I2S_TDM_CLOCK_SLAVE;
        tcfg.fs_shape    = NORA_SPI_I2S_TDM_FS_PULSE;   /* slave: FS is an input */
        tcfg.brg         = 0u;                                /* ignored as slave */
    }

    /*
     * MCLKEN stays 0. It is a BRG SOURCE SELECT (reference clock/REFO vs FP), not an MCLK
     * output enable, so this is not a decision about the codec's SYSCLK at all -- setting
     * it would only change which clock this SPI's baud-rate generator divides, and FP is
     * what `brg` is computed against. This firmware supplies no MCLK on any board.
     * (This comment read "MCLK from the SPI CLKGEN is never used ... the transport's own
     * MCLK output would be a second source contending with it" until 2026-08-09, which
     * described a capability the part does not give this bit; before that it also offered
     * "either its own crystal or REFO1", whose REFO1 half is deleted -- see wm8904_audio.h.)
     */
    tcfg.mclk_enable = false;

    /*
     * DATA DELAY. This used to be a hardcoded `false` (SPIFE=0 = one-bit delayed) under a
     * comment claiming it matched "the WM8904 DSP/TDM output (no 1-bit delay)" -- the
     * comment and the value said opposite things, and the comment also admitted the trio
     * below had never been checked against a scope. The value was the wrong one:
     *
     *   wm8904.c writes AIF_FMT_DSP + AIF_LRCLK_INV when APP_USE_1_BIT_DELAY == 0, which
     *   is DSP mode B -- MSB on the FIRST BCLK rising edge after LRC rises, i.e. NO delay.
     *   SPIFE=0 makes this transport expect FS one BCLK AHEAD of the first data bit.
     *
     * So we sampled every 32-bit slot one bit late: the codec's sign bit was shifted out
     * and one bit of the next slot shifted in. Near silence the sign bit alternates every
     * sample, so a tiny signal came back as full-scale ± garbage -- an analog output loud,
     * square-ish, at the block rate, and unrelated to the input. Measured 2026-08-05 on
     * EV88G73A: scope showed the codec's first data transition coincident with the FS
     * edge (no delay), and ?tp's slot mask read `01.....7` -- slot 7 non-zero because its
     * LSB was catching the NEXT frame's slot-0 sign bit, which is the same one-bit shift
     * seen from the other end.
     *
     * Derived from APP_USE_1_BIT_DELAY rather than written as a literal, because that
     * macro is what wm8904.c configures the codec's framing from (default 0 in
     * chip_drivers/wm8904_port.h). Two sides of one wire cannot be allowed to carry
     * independent copies of the same decision -- that is exactly how this defect existed.
     * Same reason dspic_is_master hands the codec the opposite of one field.
     *
     * CKP/CKE remain provisional in the sense the old comment meant: BCLK polarity has
     * not been swept, only found to work at both roles.
     */
    tcfg.fs_coincides_first_bclk       = (APP_USE_1_BIT_DELAY == 0u);  /* SPIFE */
    tcfg.bclk_idle_high                = true;    /* CKP=1  */
    tcfg.bclk_change_on_active_to_idle = false;   /* CKE=0  */

    /*
     * WM8904_AUDIO_USE_SYSTEM_START (default 0) selects which ownership mode this board's
     * shipping bring-up uses. It exists to MEASURE phase 4's one-leg equivalence: the SYSTEM
     * route must reproduce the SINGLE route's load, slot mask and miss count, on the same
     * hardware, from the same tcfg. Only the four calls differ:
     *   0: inst_configure -> open -> inst_start        ... stop: inst_stop -> close
     *   1: configure_system -> open -> start_all_domains ... stop: stop_all_domains -> close
     * tcfg is built ONCE above and used by both, so a difference in the numbers cannot be a
     * difference in the framing. Default 0 keeps the shipping consumer on SINGLE with the
     * calls and the call order unchanged.
     */
#if WM8904_AUDIO_USE_SYSTEM_START
    {
        nora_spi_i2s_tdm_leg_setup_t setup;

        setup.stream      = tcfg;
        /* The domain SPI1 was seeded with in conf.h. One leg is built, so there is exactly
         * one domain and it has no co-clocked partner -- which is precisely the limitation
         * doc section 14 records: this build measures the API, not two legs latching one FS
         * edge. */
        setup.sync_domain = NORA_TDM_SPI1_SYNC_DOMAIN;

        if (!nora_spi_i2s_tdm_configure_system(&setup, 1u)) {
            report_transport_error("configure_system");
            return;
        }
    }
#else
    if (!nora_spi_i2s_tdm_inst_configure(spi1, &tcfg)) {
        report_transport_error("configure");
        return;
    }
#endif

    /*
     * Bring the ramp state up to unity BEFORE the callback can ever be invoked.
     * Registering the callback and then starting the transport is what arms the block
     * ISR, and the analog unmute below is an I2C write taking milliseconds -- several
     * audio blocks. Initialising the gain after that point would let those first blocks
     * run against a zeroed s_gain (gain 0 = silence) and then jump abruptly to unity:
     * an audible click at every bring-up.
     */
    gain_ctrl_init(&s_gain, cfg->sample_rate_hz, (uint16_t)NORA_TDM_BLOCK_FRAMES);

    /* Same argument as the gain stage above, and the same placement: the AVAS
     * engine has to hold a valid phase set before any block ISR can run the chain.
     * init() also leaves the gate closed, so the chain boots with the stage silent
     * and the `a` / `A` voice keys (or `*cy00` for Type_TY) start it. */
    s_avas_rate_ok   = avas_rate_matches(cfg);
    avas_line_ck_init(&s_avas);
    s_avas_on        = false;
    s_chain_avas_run = false;
    s_mute_request     = 0u;
    s_mute_applied     = 0u;

    /*
     * PRE/POST back to the PROFILE's values on every start, not to whatever a *ti left
     * behind. A start is where the boot configuration is re-established -- the same reason
     * the mute request and the AVAS gate are reset two lines up -- so *ts / *tr is also how
     * a tuning session is undone without a reset. Resolved straight into the ISR-owned pair
     * here, with the applied mirrors matched, because no block ISR can be running yet.
     */
    s_pre_half_request  = (int16_t)WM8904_AUDIO_PRE_HALF;
    s_post_half_request = (int16_t)WM8904_AUDIO_POST_HALF;
    s_pre_half_applied  = (int16_t)WM8904_AUDIO_PRE_HALF;
    s_post_half_applied = (int16_t)WM8904_AUDIO_POST_HALF;
    gain_db_from_half((int16_t)WM8904_AUDIO_PRE_HALF,  &s_pre_gain);
    gain_db_from_half((int16_t)WM8904_AUDIO_POST_HALF, &s_post_gain);
    s_pre_sat          = 0u;
    s_post_sat         = 0u;

    s_button_prev      = false;
    s_status_gate.primed = false;
    s_idle_gate.primed   = false;

    /*
     * THE CHAIN is the default, and the four single-purpose paths are what a fresh boot
     * is now one *tp away from rather than the other way round. The reason is that the
     * chain is what this board does in normal operation -- input through the mute ramp,
     * with the synth one keystroke from joining it -- and a default that has to be
     * switched into is a default whose load figures get quoted from the wrong path.
     *
     * Its idle cost (the AVAS stage skipped) is the copy baseline plus the two-slot
     * decode/gain/encode, so the boot load line still reports a constant, and
     * mute/copy/gain stay reachable, unchanged, as the documented baselines they are
     * (docs/ck_silicon_findings.md Part 5).
     */
    s_path         = (uint8_t)WM8904_PATH_CHAIN;
    s_tone_phase   = 0u;
    s_rx_observe_seq   = 0u;
    s_rx_observe_seen  = 0u;
    s_rx_observe_frame = 0u;
    s_rx_peak[0]   = 0u;
    s_rx_peak[1]   = 0u;
    s_rx_slot_mask = 0u;

    (void)nora_spi_i2s_tdm_set_block_callback(spi1, wm8904_audio_block_cb, 0);

    /*
     * open() takes no role since phase 2 -- it derives one from the config committed by the
     * inst_configure() above, which is why that call has to come first (it always did here).
     * tcfg.clock_role is therefore no longer passed anywhere: the value the transport routes
     * pins for and the value it programs SPI1 with are now the same stored field, and cannot
     * be made to disagree from this file.
     */
    if (!nora_spi_i2s_tdm_open()) {
        report_transport_error("open");
        return;
    }

    /*
     * The load display now derives its deadline in foreground from elapsed SCCP
     * ticks / exact block_count, rather than reading SCCP once per callback.
     * Arm that reference only after SCCP is initialized and immediately before
     * inst_start(): block_count is still zero and no DMA callback can race it.
     */
    /*
     * And hand it the audio clock as a FREQUENCY REFERENCE -- but only when that
     * clock is not our own. As codec master the WM8904 divides its 12.288 MHz
     * crystal, so one block is exactly BLOCK_FRAMES/sample_rate_hz and the timer's
     * FRC-derived divisor can be corrected against it. As dsPIC master, FS is
     * brg-divided from the very Fcy in question: the measurement would return the
     * nominal value by construction and certify a wrong number as measured, so no
     * reference is offered. Same asymmetry avas_rate_matches() reasons about above,
     * for the same reason -- one of these two clocks is a crystal and the other is
     * not.
     */
    /*
     * Hundredths of a microsecond, and deliberately 32-bit: 1e8 * 32 = 3.2e9 still
     * fits uint32 (4.29e9), so the whole reference is computed without a 64-bit
     * multiply -- which is what the unit was chosen for. The #error is the guard for
     * a future block size that would silently wrap it. Rounded, not truncated: 32
     * frames at 48 kHz is 66666.67, and 66667 is the value the measurement will read
     * back.
     */
#if (NORA_TDM_BLOCK_FRAMES > 42)
#error "100000000 * NORA_TDM_BLOCK_FRAMES overflows uint32; widen the reference"
#endif
    dsp_load_set_block_period_ref_us_x100(
        (cfg->dspic_is_master || (cfg->sample_rate_hz == 0u))
            ? 0u
            : ((100000000UL * (uint32_t)NORA_TDM_BLOCK_FRAMES) +
               (cfg->sample_rate_hz / 2u)) / cfg->sample_rate_hz);
    dsp_load_reset();
#if WM8904_AUDIO_USE_SYSTEM_START
    /* Phase-locked group start: arm every member of the domain, then release SPIEN
     * back-to-back with the clock master last. With one leg built this is the same work
     * inst_start() does, in a different order at the SPIEN boundary -- which is exactly the
     * equivalence being measured. */
    if (!nora_spi_i2s_tdm_start_all_domains()) {
        report_transport_error("start_all_domains");
        return;
    }
#else
    if (!nora_spi_i2s_tdm_inst_start(spi1)) {
        report_transport_error("start");
        return;
    }
#endif

    /* 4) Verify the transport is running before unmuting -- as slave that means it
     * locked to the incoming BCLK/FS; as master it means the self-generated BCLK/FS/DMA
     * stream is actually moving. */
    if (!nora_spi_i2s_tdm_is_running()) {
        console_out_str("WM8904 audio: transport not running; staying muted\n");
        return;
    }

    /* 5) Unmute the headphone output (startup default is analog mute).  A failure here is
     * worth a line for the same reason the mute in stop() is: the transport is running and
     * the banner below will say so, but a codec that did not take the unmute is silent, and
     * "no sound with a healthy-looking banner" is otherwise a wiring hunt. */
    if (!wm8904_set_analog_output_mute(cfg->i2c_inst_legacy, false)) {
        console_out_str("WM8904 audio: WARNING analog unmute not confirmed by the codec"
                        " (I2C); expect no sound\n");
    }

    s_started = true;
    console_out_str("WM8904 audio: passthrough running (TDM8/32-bit ");
    console_out_str(cfg->dspic_is_master ? "MASTER, WM8904 slave"
                                         : "SLAVE, WM8904 master");
    console_out_str(", path=");
    console_out_str(path_name(s_path));
    if (s_use_gain) {
        /* Both numbers, because the ramp is snapped to one of a few shift-pair curves
         * (gain_ctrl.h): stating only the requested value would hide the snap, and stating
         * only the effective one would look like a typo in this board's config. */
        console_out_str(", *tp cycles chain/gain/copy/mute, SW0 ramp ");
        console_out_u32((uint32_t)gain_ctrl_ramp_ms_effective(&s_gain, cfg->gain_ramp_ms));
        console_out_str(" ms (asked ");
        console_out_u32((uint32_t)cfg->gain_ramp_ms);
        console_out_str(" ms)");
    } else {
        console_out_str(", no gain stage in this build");
    }
    console_out_str(")\n");
}

void wm8904_audio_stop(void)
{
    nora_spi_i2s_tdm_inst_t *spi1;
    bool mute_ok = false;

    /*
     * This command exists for a very specific operational hazard: the board's
     * debugger presents a UART console, and bytes sent while drag-and-drop
     * programming is in progress can otherwise become data on the live TDM
     * loopback.  Muting at the codec comes FIRST.  The I2C transaction can take
     * far longer than one DMA block, so stopping DMA first would still leave a
     * short but audible interval in which arbitrary console bytes reach HPOUT.
     *
     * Attempted whenever a codec is configured, NOT only while s_started -- the point of
     * the command is "prove HPOUT is quiet", and both of the !s_started cases benefit:
     * a repeated *ts re-confirms the mute instead of degrading to "unverified", and on a
     * board whose bring-up failed this is the one place that says whether its codec I2C
     * answers at all.  The write is idempotent and costs one I2C transaction.
     *
     * The return value is the codec's confirmation (readback-checked, see wm8904.h) and it
     * is kept rather than discarded: a failed I2C mute leaves HPOUT in whatever state it
     * was in, which is exactly the hazard above, so it must not be reported as a stop.
     */
    if (s_cfg != NULL) {
        mute_ok = wm8904_set_analog_output_mute(s_cfg->i2c_inst_legacy, true);
    }

    /*
     * inst_stop() masks the instance DMA IRQ before touching SPI/DMA state and
     * clears pending flags and buffers.  Calling it even after a partial
     * bring-up makes `*ts` idempotent and avoids duplicating that delicate ISR
     * ordering here.  close() is retained for the HAL lifecycle contract; it
     * intentionally leaves PPS/CLC and external clocks alone.
     */
    spi1 = nora_spi_i2s_tdm_spi1();
#if WM8904_AUDIO_USE_SYSTEM_START
    /* The stop side MUST switch with the start side: a SYSTEM-committed stream refuses
     * inst_stop() with ERR_CONFIG_MODE, so leaving this call here would make *ts silently
     * fail to stop anything and *tr restart a transport that never stopped. */
    (void)spi1;   /* the group teardown needs no handle */
    (void)nora_spi_i2s_tdm_stop_all_domains();
#else
    if (spi1 != NULL) {
        /* inst_stop() can only fail on a bad handle, already NULL-checked, and its result is
         * deliberately not folded into s_stop_state -- that field reports MUTE state. */
        (void)nora_spi_i2s_tdm_inst_stop(spi1);
    }
#endif

    /*
     * close() is CHECKED since phase 2: it refuses (ERR_ALREADY_RUNNING) while any leg is
     * still running, and clearing the open state is what makes the *tr restart below a real
     * stop -> close -> open -> start rather than four calls that happen to succeed.
     *
     * A false here means the inst_stop() above did not take -- the transport is still
     * streaming while this function is reporting a stop, which is the one outcome the
     * pre-flash use of *ts must not report as quiet. Said out loud rather than swallowed;
     * the codec mute above still holds, so HPOUT is silent either way and the line
     * distinguishes "muted and stopped" from "muted but still running".
     */
    if (!nora_spi_i2s_tdm_close()) {
        report_transport_error("close");
    }

    s_started    = false;
    s_stop_state = mute_ok ? WM8904_AUDIO_STOP_MUTE_VERIFIED
                           : WM8904_AUDIO_STOP_MUTE_UNVERIFIED;

    /* No callback remains after inst_stop().  Clear foreground-only snapshots so
     * a later ?tp cannot present a pre-stop input peak as fresh signal evidence. */
    s_button_prev      = false;
    s_mute_request     = 0u;
    s_mute_applied     = 0u;
    s_rx_observe_seen  = s_rx_observe_seq;
    s_rx_peak[0]       = 0u;
    s_rx_peak[1]       = 0u;
    s_rx_slot_mask     = 0u;

    /* Same argument for the clamp counts: a count from before the stop is not evidence
     * about the chain that is not running. The dB values themselves are deliberately LEFT
     * as they are -- ?ti after a *ts should still say what the next start will use, and a
     * start re-reads the profile anyway. */
    s_pre_sat          = 0u;
    s_post_sat         = 0u;

    /*
     * THE EXACT PHRASE "analog mute verified" IS THE PRE-FLASH GATE, not prose --
     * buildtools/README.md tells the operator (and the flash script) to wait for it in the
     * monitor log before copying the HEX. The failure wording says "analog mute NOT
     * verified", which deliberately does NOT contain the gate substring, so a substring
     * match cannot pass on the one reply that means "HPOUT may still be live". Do not
     * reword either line without updating that README.
     */
    if (s_stop_state == WM8904_AUDIO_STOP_MUTE_VERIFIED) {
        console_out_str("WM8904 audio: stopped (analog mute verified, TDM/DMA halted)\n");
    } else {
        console_out_str("WM8904 audio: stopped (TDM/DMA halted) -- analog mute NOT verified:"
                        " codec I2C write/readback failed. HPOUT may still be live;"
                        " do NOT start programming on this reply.\n");
    }
}


/*
 * *tr -- stop and bring the whole path back up, without a reset.
 *
 * A FULL stop followed by the FULL bring-up (I2C -> codec verify -> configure -> open ->
 * start -> unmute), not a resume of a half-torn-down path: wm8904_audio_start() is written
 * to be re-runnable from any state and re-establishes the profile's PRE/POST/path/mute
 * defaults, which is what makes *ts -> *tr the documented way to undo a tuning session
 * (see the PRE/POST note in wm8904_audio.h).
 *
 * WHY IT EXISTS NOW. This board had no way to reach close() -> open() -> inst_start()
 * without a reset: *ts is terminal by design and CK never implemented sonora's *tr (the
 * letter was reserved for it in console_dispatch.c and nothing answered). Phase 2 of the
 * NORA alignment puts the lifecycle's failure semantics INTO those three calls, so a
 * command that walks them on hardware stopped being a convenience and became the way that
 * work is verified at all -- and it matches sonora's *tr, which is the fleet reason to
 * spell it the same way.
 *
 * Returns whether audio is running afterwards. Every stage that fails prints its own
 * reason first, so the bool is for the console's status byte, not the diagnosis.
 */
bool wm8904_audio_restart(void)
{
    const wm8904_audio_config_t *cfg = s_cfg;

    /*
     * Restart needs the config the board handed to start(). It is kept by pointer (a static
     * const in the board profile), so this survives any number of stops -- but a board where
     * start() was never called has nothing to restart, and inventing a config here would be
     * a second source for a decision the board owns.
     */
    if (cfg == NULL) {
        console_out_str(" *tr: audio was never started -- nothing to restart\n");
        return false;
    }

    console_out_str("WM8904 audio: *tr restart -- stop + close, then the full bring-up\n");
    wm8904_audio_stop();
    wm8904_audio_start(cfg);

    return s_started;
}


bool wm8904_audio_restart_declick(uint8_t declick_mask)
{
    /*
     * Keep this intentionally parallel to Sonora's audio_transport_restart_declick():
     * arm the selected strategy only around one complete restart, then restore the
     * shipping baseline so fault recovery and a later plain *tr cannot inherit an
     * experiment by accident.
     */
    console_out_str(" [declick] one-shot restart mask=0x");
    console_out_hex16((uint16_t)declick_mask);
    console_out_str("\n");
    wm8904_set_pending_declick(declick_mask);
    {
        const bool restarted = wm8904_audio_restart();
        wm8904_set_pending_declick((uint8_t)WM8904_DECLICK_NONE);
        return restarted;
    }
}


void wm8904_audio_declick_print_status(void)
{
    if (s_cfg == NULL) {
        console_out_str("   servo-captured: unavailable (audio was never started)\n");
        return;
    }
    console_out_str("   servo-captured: codec=");
    console_out_u32(wm8904_declick_servo_captured(s_cfg->i2c_inst_legacy) ? 1u : 0u);
    console_out_str(" (WARM_SERVO falls back to STARTUP until captured)\n");
}


#if WM8904_AUDIO_ENABLE_TDM_DIAG || WM8904_AUDIO_ENABLE_SYSTEM_PROBE
/* -------------------------------------------------------------------------- */
/* *tl -- exercise the transport's LIFECYCLE GATES where they must reject.     */
/*                                                                            */
/* Phase 2 of the NORA alignment made four bool return values load-bearing:    */
/* set_port() and inst_configure() refuse while the port is open,              */
/* close() refuses while a leg runs, and inst_start() refuses while closed.    */
/* Nothing in normal operation ever takes those branches, so without this      */
/* command the board cannot tell a working gate from a bool that is always     */
/* true -- which is precisely the criticism phase 1 recorded against itself.   */
/*                                                                            */
/* IT IS STATE-DEPENDENT ON PURPOSE, AND MUST BE RUN TWICE: once while the     */
/* stream runs and once after *ts. Each state includes a call that must        */
/* SUCCEED, because a gate that rejects in every state is not a gate either.   */
/*                                                                            */
/* Every attempt is chosen so that a MISSING gate is still non-destructive:    */
/* set_port() re-registers the SAME table, inst_configure() is handed a NULL    */
/* cfg (the open-state check precedes the NULL check, so a broken gate stores   */
/* nothing and answers BAD_ARG -- which is why the verdict compares the ERROR   */
/* CODE and not just the bool), and close() writes no hardware. The one attempt */
/* with a real consequence if its gate is missing is inst_start() while         */
/* stopped: it would arm the stream for real (behind the codec's analog mute).  */
/* That is reported, not hidden, and *ts then *tr recovers.                     */
/* -------------------------------------------------------------------------- */

static void probe_put_result(bool ok, nora_spi_i2s_tdm_error_t err)
{
    console_out_str(ok ? "true/" : "false/");

    /*
     * Names for the six codes this probe can legitimately produce, the number for anything
     * else. A full 15-entry table would read better on the one line that will never print,
     * at the cost of string space on a part with under 7 KB of program memory free.
     */
    switch (err) {
    case NORA_SPI_I2S_TDM_ERR_NONE:            console_out_str("NONE");        break;
    case NORA_SPI_I2S_TDM_ERR_BAD_ARGUMENT:    console_out_str("BAD_ARG");     break;
    case NORA_SPI_I2S_TDM_ERR_ALREADY_RUNNING: console_out_str("ALR_RUNNING"); break;
    case NORA_SPI_I2S_TDM_ERR_NOT_OPEN:        console_out_str("NOT_OPEN");    break;
    case NORA_SPI_I2S_TDM_ERR_ALREADY_OPEN:    console_out_str("ALR_OPEN");    break;
    case NORA_SPI_I2S_TDM_ERR_CONFIG_MODE:     console_out_str("CFG_MODE");    break;
    default:
        console_out_str("err=");
        console_out_u32((uint32_t)err);
        break;
    }
}

/*
 * One attempt, one verdict line. `got_ok` is the CALL, evaluated by the caller as this
 * argument -- so the call has already happened when get_last_error() is read here, which is
 * the only ordering this needs and the reason the call is not passed as a function pointer.
 */
static bool probe_check(const char *call, bool want_ok,
                        nora_spi_i2s_tdm_error_t want_err, bool got_ok)
{
    const nora_spi_i2s_tdm_error_t got_err = nora_spi_i2s_tdm_get_last_error();
    const bool pass = (got_ok == want_ok) && (got_err == want_err);

    console_out_str("  ");
    console_out_str(call);
    console_out_str(" -> ");
    probe_put_result(got_ok, got_err);
    if (pass) {
        console_out_str(" ok\n");
    } else {
        console_out_str("  FAIL, wanted ");
        probe_put_result(want_ok, want_err);
        console_out_str("\n");
    }

    return pass;
}

/*
 * The verdict line, shared by both probes. `want` is a parameter rather than a literal 4
 * because the two entry points below check a different number of calls -- and the count is
 * printed as "n/want" so a probe that silently skipped a line cannot read as a pass.
 */
static bool probe_report(uint8_t pass, uint8_t want)
{
    console_out_str("  ");
    console_out_u32((uint32_t)pass);
    console_out_str("/");
    console_out_u32((uint32_t)want);
    console_out_str(" as specified");
    if (pass != want) {
        console_out_str(" -- gate defect above; *ts then *tr to recover");
    }
    console_out_str("\n");

    return pass == want;
}

#if WM8904_AUDIO_ENABLE_TDM_DIAG

bool wm8904_audio_lifecycle_probe(void)
{
    nora_spi_i2s_tdm_inst_t *spi1 = nora_spi_i2s_tdm_spi1();
    bool running;
    uint8_t pass = 0u;

    if (spi1 == NULL) {
        console_out_str(" *tl: no SPI1 instance\n");
        return false;
    }
#if WM8904_AUDIO_ENABLE_SYSTEM_PROBE
    /*
     * Every expectation below assumes SINGLE ownership. Rather than grow a second set of
     * expected codes for a mode this command was never about, refuse in one line: under
     * SYSTEM the inst_* family answers CFG_MODE to everything, so *tl would report four
     * failures that are the mode gate working correctly. A *tm run from the stopped state
     * puts the board here permanently -- SYSTEM is a one-way commit -- so this is the normal
     * post-*tm state, not an error path, and *sr is the way out.
     *
     * GATED WITH *tm, and it has to be: nothing else commits SYSTEM, so in a probe-less image
     * this branch is unreachable by construction and the line it prints names a command the
     * image does not have. Unreachable is not the same as free -- the string would still be
     * linked (see the macro's comment in the header).
     */
    if (wm8904_audio_system_mode_held()) {
        console_out_str(" *tl: transport is in SYSTEM mode -- see *tm\n");
        return false;
    }
#endif
#if WM8904_AUDIO_USE_SYSTEM_START
    /* Same reason *tm refuses in this build, one degree milder: here the mode is SYSTEM from
     * the bring-up onwards, so inst_start() answers CFG_MODE where this probe wants
     * ALREADY_RUNNING and reports a gate defect that is not one (measured: 3/4). Nothing is
     * destructive -- every call still rejects -- but a probe whose failures are its own
     * expectations is worse than silent, so it declines and names the build. */
    (void)pass;
    (void)running;
    console_out_str(" *tl: not in a USE_SYSTEM_START=1 build -- these expectations are"
                    " SINGLE's, and this build ships SYSTEM (inst_start answers CFG_MODE,"
                    " not ALR_RUNNING). Build the default config for the gate checks.\n");
    return false;
#else

    /*
     * The HAL's own view of the state, not s_started: the question is what the transport
     * will do, and asking the module under test which state it is in is what makes the
     * expectations below meaningful rather than a restatement of this file's bookkeeping.
     */
    running = nora_spi_i2s_tdm_is_running();

    console_out_str(" *tl lifecycle gates, transport ");
    console_out_str(running ? "RUNNING\n" : "STOPPED (closed)\n");

    if (running) {
        pass += probe_check("set_port(same)", false, NORA_SPI_I2S_TDM_ERR_ALREADY_OPEN,
                            nora_spi_i2s_tdm_set_port(&s_transport_port)) ? 1u : 0u;
        pass += probe_check("inst_configure", false, NORA_SPI_I2S_TDM_ERR_ALREADY_OPEN,
                            nora_spi_i2s_tdm_inst_configure(spi1, NULL)) ? 1u : 0u;
        pass += probe_check("close         ", false, NORA_SPI_I2S_TDM_ERR_ALREADY_RUNNING,
                            nora_spi_i2s_tdm_close()) ? 1u : 0u;
        pass += probe_check("inst_start    ", false, NORA_SPI_I2S_TDM_ERR_ALREADY_RUNNING,
                            nora_spi_i2s_tdm_inst_start(spi1)) ? 1u : 0u;
    } else {
        /* inst_start FIRST, while the port is still closed -- that is the only state in
         * which ERR_NOT_OPEN is reachable, and a later close()/set_port() would not change
         * it but the order makes the intent obvious. */
        pass += probe_check("inst_start    ", false, NORA_SPI_I2S_TDM_ERR_NOT_OPEN,
                            nora_spi_i2s_tdm_inst_start(spi1)) ? 1u : 0u;
        /* Closed, so the open-state check must NOT fire and the NULL cfg must be what is
         * refused. This is the negative control for the ALR_OPEN line above. */
        pass += probe_check("inst_configure", false, NORA_SPI_I2S_TDM_ERR_BAD_ARGUMENT,
                            nora_spi_i2s_tdm_inst_configure(spi1, NULL)) ? 1u : 0u;
        /* Must SUCCEED: same call, same argument, opposite answer to the running case. */
        pass += probe_check("set_port(same)", true, NORA_SPI_I2S_TDM_ERR_NONE,
                            nora_spi_i2s_tdm_set_port(&s_transport_port)) ? 1u : 0u;
        /* Must SUCCEED, and be idempotent: already closed, nothing running. */
        pass += probe_check("close         ", true, NORA_SPI_I2S_TDM_ERR_NONE,
                            nora_spi_i2s_tdm_close()) ? 1u : 0u;
    }

    return probe_report(pass, 4u);
#endif /* WM8904_AUDIO_USE_SYSTEM_START */
}


/* -------------------------------------------------------------------------- */
/* *tl(virgin) -- the ONE state that proves phase 3's mode gate is real.       */
/*                                                                            */
/* Phase 3 made the inst_* family SINGLE-mode and primary-only. The trouble    */
/* with measuring that from the console is that the board is ALWAYS in SINGLE  */
/* mode by the time a console exists: inst_configure() commits the mode during */
/* bring-up and close() deliberately never resets it. mode == NONE therefore   */
/* exists only BEFORE the first inst_configure() -- so this probe is not a     */
/* command at all; the board calls it once from init, just before              */
/* wm8904_audio_start(). Run any later and it measures nothing.                */
/*                                                                            */
/* The pair of lines is the whole point, and it is the same shape §12.4 used:  */
/* THE SAME CALL, TWO DIFFERENT ANSWERS. inst_start(spi1) answers CFG_MODE     */
/* here and NOT_OPEN in *tl's stopped state -- which can only happen if the    */
/* mode check really does precede the opened check. Read either line alone and */
/* it proves nothing.                                                         */
/*                                                                            */
/* Non-destructive if a gate is MISSING, like every other line in *tl: with no */
/* mode gate inst_start() falls through to the opened check and answers        */
/* NOT_OPEN (nothing is open yet, so it cannot arm), inst_stop() tears down an */
/* already-stopped leg, and inst_configure() is handed NULL.                   */
/* -------------------------------------------------------------------------- */
bool wm8904_audio_lifecycle_probe_unconfigured(void)
{
    nora_spi_i2s_tdm_inst_t *spi1 = nora_spi_i2s_tdm_spi1();
    uint8_t pass = 0u;

    if (spi1 == NULL) {
        console_out_str(" *tl(virgin): no SPI1 instance\n");
        return false;
    }

    console_out_str(" *tl(virgin) transport NEVER CONFIGURED\n");

    /* Mode NONE, so not SINGLE: refused BEFORE the opened check. The port is closed, which
     * is exactly why this is the interesting line -- NOT_OPEN is also true of this state,
     * and the gate is what decides which of the two the caller is told. */
    pass += probe_check("inst_start    ", false, NORA_SPI_I2S_TDM_ERR_CONFIG_MODE,
                        nora_spi_i2s_tdm_inst_start(spi1)) ? 1u : 0u;
    /* Same gate on the teardown side. Before phase 3 this answered true/NONE -- a silent
     * success for tearing down a leg that was never configured. */
    pass += probe_check("inst_stop     ", false, NORA_SPI_I2S_TDM_ERR_CONFIG_MODE,
                        nora_spi_i2s_tdm_inst_stop(spi1)) ? 1u : 0u;
    /* NEGATIVE CONTROL, and the reason the mode gates are not simply "reject unless SINGLE":
     * NONE is the LEGAL starting point for inst_configure() -- it is how SINGLE is entered.
     * A CFG_MODE here would mean the board can never be configured at all. */
    pass += probe_check("inst_configure", false, NORA_SPI_I2S_TDM_ERR_BAD_ARGUMENT,
                        nora_spi_i2s_tdm_inst_configure(spi1, NULL)) ? 1u : 0u;

    return probe_report(pass, 3u);
}

#endif /* WM8904_AUDIO_ENABLE_TDM_DIAG -- *tl and *tl(virgin) */


#if WM8904_AUDIO_ENABLE_SYSTEM_PROBE
/* -------------------------------------------------------------------------- */
/* *tm -- exercise phase 4's SYSTEM / sync-domain API.                        */
/*                                                                            */
/* LAB IMAGES ONLY (WM8904_AUDIO_ENABLE_SYSTEM_PROBE=1).  The reasoning is in  */
/* the header, on the macro; the short form is that a shipping image does not  */
/* contain the domain API at all -- the very property the comment below is     */
/* about -- so there is nothing there for this probe to measure, and a stopped */
/* state *tm would leave a shipped board unable to restart audio until reset.  */
/*                                                                            */
/* THIS COMMAND IS THE REASON PHASE 4 IS NOT DEAD CODE. Both build configs    */
/* carry remove-unused-sections=true and NOTHING in the shipping consumer      */
/* calls the domain API, so without a caller the linker discards all five      */
/* functions: the ROM delta reads about 0 B and the firmware ships code that   */
/* has never once executed. That is finding F3 in the plan doc, and it is the  */
/* most convincing way for this phase to look successful while being useless.  */
/*                                                                            */
/* It is also the only thing that can see the gates at all. The contract check */
/* compares DECLARATIONS; the ownership mode is a file-static inside the HAL,   */
/* invisible to it, so a build with every mode gate deleted passes that check   */
/* unchanged. Only hardware can tell a live gate from a bool that is true.      */
/*                                                                            */
/* TWO STATES, LIKE *tl, AND IT REFUSES ANY OTHER:                            */
/*  - RUNNING: 5 rejections, every one of which returns BEFORE touching a      */
/*    register, so the audio does not even glitch. The interesting line is     */
/*    configure_system -> ALREADY_OPEN, not CFG_MODE: that call deliberately   */
/*    does NOT look at the mode (it is the way OUT of SINGLE ownership), and a  */
/*    CFG_MODE there means someone added a gate that strands a SINGLE-          */
/*    committed stream forever.                                                */
/*  - STOPPED/closed: the full 14-step sequence, which actually COMMITS         */
/*    SYSTEM mode, opens, and starts the transport for real through            */
/*    start_all_domains() -- arm-all-then-go executes here or nowhere.         */
/*                                                                            */
/* AND IT IS A SEQUENCE, NOT A ROUND TRIP -- measured 2026-08-09, against a    */
/* plan that said otherwise. SYSTEM is a ONE-WAY COMMIT: inst_configure() is   */
/* the only call that commits SINGLE and it refuses under SYSTEM (check 13     */
/* above is exactly that rejection), configure_system() only ever commits      */
/* SYSTEM, and close() leaves the mode alone on purpose. So no call sequence   */
/* returns to SINGLE -- only a reset. The consequence is real and is the       */
/* board's, not the probe's: *tr's bring-up goes through inst_configure() and  */
/* so answers CFG_MODE (err=14, measured), meaning audio cannot be restarted   */
/* until *sr. *tl refuses rather than turn its SINGLE expectations into a      */
/* guess. *ts still answers OK, and that is not a lie: it re-asserts the codec */
/* mute and discards inst_stop()'s result on purpose (see the comment at that  */
/* call), and in this default build SYSTEM can only be entered FROM the        */
/* stopped state, so the transport it claims is halted really is. The verdict  */
/* line names all three outcomes separately for exactly that reason -- an      */
/* earlier draft said "*ts / *tr / *tl are gated off" and hardware disagreed.  */
/*                                                                            */
/* The committed setup is READ BACK with inst_get_setup() and handed straight  */
/* to configure_system() rather than rebuilt from wm8904_audio_start()'s       */
/* locals. Two copies of one framing decision is the defect class this file    */
/* already carries a long comment about (SPIFE vs the codec's DSP mode B), and */
/* it makes step 3 load-bearing: a wrong sync_domain is caught by value.       */
/* -------------------------------------------------------------------------- */

/* True once *tm has committed SYSTEM mode. NOTHING CLEARS IT -- there is no   */
/* call that returns the HAL to SINGLE, so the flag has the same lifetime as   */
/* the condition it reports: set until reset.                                  */
static bool s_system_mode_held;

bool wm8904_audio_system_mode_held(void)
{
    return s_system_mode_held;
}

bool wm8904_audio_system_probe(void)
{
    nora_spi_i2s_tdm_inst_t      *spi1 = nora_spi_i2s_tdm_spi1();
    nora_spi_i2s_tdm_leg_setup_t  setups[WM8904_AUDIO_TM_MAX_LEGS];
    const uint8_t                      legs = nora_spi_i2s_tdm_instance_count();
    bool    running;
    bool    restored;
    uint8_t pass = 0u;

    if (spi1 == NULL) {
        console_out_str(" *tm: no SPI1 instance\n");
        return false;
    }
    /* The array is sized by a compile-time bound (this build has 1 leg); refuse rather than
     * overrun if a future build adds legs and this bound is not raised with it. */
    if ((legs == 0u) || (legs > WM8904_AUDIO_TM_MAX_LEGS)) {
        console_out_str(" *tm: leg count outside this probe's bound\n");
        return false;
    }
    if (s_system_mode_held) {
        console_out_str(" *tm: SYSTEM mode already committed by an earlier run"
                        " -- *sr (reset) to return to SINGLE\n");
        return false;
    }
#if WM8904_AUDIO_USE_SYSTEM_START
    /*
     * REFUSED IN THIS BUILD, and it took hardware to find out why (2026-08-09, run 2).
     *
     * Every expectation below assumes the board is in SINGLE, which is true only of the
     * default build. With USE_SYSTEM_START=1 the bring-up itself commits SYSTEM, so the
     * four domain calls in the RUNNING branch are LEGAL: they returned true/NONE and were
     * reported as four gate defects -- and, far worse, they RAN. start_domain, stop_domain,
     * start_all_domains and stop_all_domains actually restarted and then stopped the live
     * transport, under a line that says "audio untouched: every call above returned on its
     * first gate". That line is only true while the gates reject; here nothing rejected.
     *
     * A probe that silently stops the audio it claims not to touch is worse than no probe,
     * and rewriting it to carry a second set of expectations would double the file for a
     * config that exists purely to MEASURE one-leg equivalence -- the numbers, not the
     * gates. So it refuses at compile time, and the default build (where the gates are
     * what is under test) is the one that runs it.
     */
    (void)setups;
    (void)running;
    (void)restored;
    (void)pass;
    console_out_str(" *tm: not in a USE_SYSTEM_START=1 build -- this build already ships"
                    " SYSTEM, so the domain calls are legal here and would stop the live"
                    " transport. Build the default (SINGLE) config to exercise the gates;"
                    " this config is for the load/slot numbers.\n");
    return false;
#else

    running = nora_spi_i2s_tdm_is_running();

    console_out_str(" *tm SYSTEM/domain API, transport ");
    console_out_str(running ? "RUNNING\n" : "STOPPED (closed)\n");

    if (running) {
        /* All five reject on their FIRST gate, before any register write. */
        pass += probe_check("start_domain(0)   ", false, NORA_SPI_I2S_TDM_ERR_CONFIG_MODE,
                            nora_spi_i2s_tdm_start_domain(0u)) ? 1u : 0u;
        pass += probe_check("stop_domain(0)    ", false, NORA_SPI_I2S_TDM_ERR_CONFIG_MODE,
                            nora_spi_i2s_tdm_stop_domain(0u)) ? 1u : 0u;
        pass += probe_check("start_all_domains ", false, NORA_SPI_I2S_TDM_ERR_CONFIG_MODE,
                            nora_spi_i2s_tdm_start_all_domains()) ? 1u : 0u;
        pass += probe_check("stop_all_domains  ", false, NORA_SPI_I2S_TDM_ERR_CONFIG_MODE,
                            nora_spi_i2s_tdm_stop_all_domains()) ? 1u : 0u;
        /* THE ASYMMETRY LINE: no mode gate here, so the OPEN gate is what answers.
         * CFG_MODE here would be a defect, not a stricter check. */
        pass += probe_check("configure_system  ", false, NORA_SPI_I2S_TDM_ERR_ALREADY_OPEN,
                            nora_spi_i2s_tdm_configure_system(setups, legs)) ? 1u : 0u;

        console_out_str("  (audio untouched: every call above returned on its first gate)\n");
        return probe_report(pass, 5u);
    }

    /* ---- STOPPED/closed: the round trip that actually runs the new code. ---- */

    /* Read the COMMITTED setup back out of the HAL and reuse it, so this probe cannot
     * disagree with the framing the board actually booted with. 1: the count check. */
    pass += probe_check("cfg_system(n+1)   ", false, NORA_SPI_I2S_TDM_ERR_BAD_ARGUMENT,
                        nora_spi_i2s_tdm_configure_system(setups, (uint8_t)(legs + 1u))) ? 1u : 0u;

    /* 2: sync_domain must be < 32. Filled from the live setup below, but this check needs a
     * bad value, so it is issued first with a deliberately out-of-range domain. */
    if (nora_spi_i2s_tdm_inst_get_setup(spi1, &setups[0])) {
        setups[0].sync_domain = 32u;
    }
    pass += probe_check("cfg_system(dom32) ", false, NORA_SPI_I2S_TDM_ERR_TOPOLOGY,
                        nora_spi_i2s_tdm_configure_system(setups, legs)) ? 1u : 0u;

    /* 3: the query itself, asserted BY VALUE. A true with the wrong domain would let every
     * later step pass while configure_system() was handed a different topology. */
    {
        bool ok = true;

        for (uint8_t i = 0u; i < legs; i++) {
            nora_spi_i2s_tdm_inst_t *leg = nora_spi_i2s_tdm_inst(i);

            if ((leg == NULL) || !nora_spi_i2s_tdm_inst_get_setup(leg, &setups[i])) {
                ok = false;
                break;
            }
        }
        console_out_str("  inst_get_setup    -> ");
        if (ok) {
            console_out_str("true dom=");
            console_out_u32((uint32_t)setups[0].sync_domain);
        } else {
            console_out_str("false");
        }
        /* SPI1's committed domain is the conf.h seed, 0. Compared rather than trusted: this
         * is the value the whole round trip below is addressed to. */
        ok = ok && (setups[0].sync_domain == 0u);
        console_out_str(ok ? " ok\n" : "  FAIL, wanted true dom=0\n");
        pass += ok ? 1u : 0u;
    }

    /* 4: commit SYSTEM for real. From here the restore at the bottom is owed. */
    s_system_mode_held = true;
    pass += probe_check("cfg_system(good)  ", true, NORA_SPI_I2S_TDM_ERR_NONE,
                        nora_spi_i2s_tdm_configure_system(setups, legs)) ? 1u : 0u;

    /* 5-7: the mode gate now bites from the other side -- the whole inst_* family is refused
     * under SYSTEM. Three sites, because each carries its own gate. inst_configure is handed
     * NULL so a MISSING gate still stores nothing (it answers BAD_ARG instead). */
    pass += probe_check("inst_configure    ", false, NORA_SPI_I2S_TDM_ERR_CONFIG_MODE,
                        nora_spi_i2s_tdm_inst_configure(spi1, NULL)) ? 1u : 0u;
    pass += probe_check("inst_start        ", false, NORA_SPI_I2S_TDM_ERR_CONFIG_MODE,
                        nora_spi_i2s_tdm_inst_start(spi1)) ? 1u : 0u;
    pass += probe_check("inst_stop         ", false, NORA_SPI_I2S_TDM_ERR_CONFIG_MODE,
                        nora_spi_i2s_tdm_inst_stop(spi1)) ? 1u : 0u;

    /* 8: an unknown domain id is an ERROR in the stop direction too -- a typo must not read
     * as "stopped fine". Domain 1 has no member in this build. */
    pass += probe_check("stop_domain(1)    ", false, NORA_SPI_I2S_TDM_ERR_BAD_INSTANCE,
                        nora_spi_i2s_tdm_stop_domain(1u)) ? 1u : 0u;
    /* 9: and domain 0 exists, so stopping it while already stopped is idempotent success --
     * the negative control for the line above (same call, different id, opposite answer). */
    pass += probe_check("stop_domain(0)    ", true, NORA_SPI_I2S_TDM_ERR_NONE,
                        nora_spi_i2s_tdm_stop_domain(0u)) ? 1u : 0u;
    /* 10: configured but not open. This is the domain-API twin of *tl's NOT_OPEN line and it
     * proves the open gate is checked independently of the mode gate. */
    pass += probe_check("start_domain(0)   ", false, NORA_SPI_I2S_TDM_ERR_NOT_OPEN,
                        nora_spi_i2s_tdm_start_domain(0u)) ? 1u : 0u;

    /* 11: open the shared clock/pins/CLC from the SYSTEM-committed config. */
    pass += probe_check("open              ", true, NORA_SPI_I2S_TDM_ERR_NONE,
                        nora_spi_i2s_tdm_open()) ? 1u : 0u;
    /* 12: THE LINE THIS WHOLE PHASE EXISTS FOR -- arm every member, then release SPIEN
     * back-to-back with the clock master last. Executes here or nowhere. The codec is
     * analog-muted (*ts left it muted), so a real start is silent. */
    pass += probe_check("start_all_domains ", true, NORA_SPI_I2S_TDM_ERR_NONE,
                        nora_spi_i2s_tdm_start_all_domains()) ? 1u : 0u;
    /* 13: every domain is now ALL_RUNNING, which the classifier must report as idempotent
     * success -- NOT as ALREADY_RUNNING, and above all not by restarting live legs. */
    pass += probe_check("start_all (again) ", true, NORA_SPI_I2S_TDM_ERR_NONE,
                        nora_spi_i2s_tdm_start_all_domains()) ? 1u : 0u;
    /* 14: and the group teardown. */
    pass += probe_check("stop_all_domains  ", true, NORA_SPI_I2S_TDM_ERR_NONE,
                        nora_spi_i2s_tdm_stop_all_domains()) ? 1u : 0u;

    (void)probe_report(pass, 14u);

    /*
     * SYSTEM OWNERSHIP IS A ONE-WAY COMMIT, and this probe cannot undo it. Measured here on
     * 2026-08-09, against a plan that assumed close() + inst_configure() would restore SINGLE:
     * inst_configure() refuses under SYSTEM (ERR_CONFIG_MODE -- one of the 14 checks above
     * proves it), and configure_system() only ever commits SYSTEM. So no call sequence returns
     * the mode to SINGLE; close() does not reset it either, by the same deliberate decision
     * that makes *tr work. Only a reset does.
     *
     * That is not a defect to route around -- a mode that a caller can silently flip back
     * would defeat the ownership model -- so what this probe owes is an honest end state, not
     * a restore. The port IS closed (real, and worth doing), s_system_mode_held STAYS set so
     * *tl refuses instead of measuring a mode it was never about, and the operator is told the
     * one thing that recovers the board. Reported separately from the 14 for the same reason
     * as before: it is the board's usability, not a check of the API.
     */
    restored = nora_spi_i2s_tdm_close();
    console_out_str(restored ? "  closed. " : "  close FAILED. ");
    console_out_str("SYSTEM is a ONE-WAY commit -- *sr (reset) to return to SINGLE."
                    " Until then *tr answers CFG_MODE and *tl refuses; audio cannot"
                    " be restarted. (*ts still replies OK: it only re-asserts the codec"
                    " mute, and the transport is already stopped.)\n");

    return (pass == 14u) && restored;
#endif /* WM8904_AUDIO_USE_SYSTEM_START */
}
#endif /* WM8904_AUDIO_ENABLE_SYSTEM_PROBE */

#endif /* probe_*() helpers: compiled when EITHER *tl or *tm wants them */


void wm8904_audio_stop_report(void)
{
    console_out_str(" ?ts: WM8904 audio ");

    if (s_started) {
        console_out_str("running (codec unmuted, TDM/DMA active)\n");
        return;
    }

    /* Not running has three distinct causes and they call for different actions, so ?ts
     * states which one it is.  It used to answer "stopped (analog mute, ...)" to all of
     * them, i.e. it reported a codec that never came up as a successful deliberate stop. */
    switch (s_stop_state) {
    case WM8904_AUDIO_STOP_MUTE_VERIFIED:
        console_out_str("stopped by *ts (analog mute verified, TDM/DMA halted;"
                        " *tr or a reboot restarts audio)\n");
        break;
    case WM8904_AUDIO_STOP_MUTE_UNVERIFIED:
        console_out_str("stopped by *ts (TDM/DMA halted) -- analog mute NOT verified;"
                        " HPOUT may still be live. *tr or a reboot restarts audio.\n");
        break;
    default:
        console_out_str("never started (codec init or transport bring-up failed;"
                        " no *ts was issued). No audio, no DSP load.\n");
        break;
    }
}

bool wm8904_audio_started(void)
{
    return s_started;
}

/*
 * Time gate shared by the two throttled lines. `period_ms` of 0 means every report;
 * returns true when this report should print, and remembers when that was.
 *
 * THIS USED TO COUNT REPORTS, not milliseconds -- `due(uint16_t *countdown, uint16_t
 * every)`, decrementing once per call. The count made the same configured number mean
 * different things on the two boards (the header said so: "the two boards' main loops
 * differ by three orders of magnitude"), so every value had to be re-derived from the
 * loop it happened to be called in, and re-derived AGAIN whenever that loop changed
 * pace. A period in milliseconds means the same thing everywhere.
 *
 * `primed` IS NOT REDUNDANT with last_ms == 0. The counted version printed on its very
 * first call, whatever the period, and both of these lines want that: the status line is
 * the confirmation that audio came up, and the idle line is the explanation of why it did
 * not. A bare elapsed-time test would instead swallow the first one for up to a full
 * period after bring-up. Testing last_ms == 0 for the same purpose would misfire when the
 * tick genuinely reads 0, so the flag is separate.
 *
 * A STOPPED TICK therefore degrades to "print once, then never" rather than to a flood.
 * Both profiles start the tick in profile_bring_up().
 */
static bool due_ms(print_gate_t *gate, uint32_t period_ms)
{
    const uint32_t now = GetTicks();

    if (!gate->primed) {
        gate->primed  = true;
        gate->last_ms = now;
        return true;
    }

    if (period_ms == 0u) {
        return true;
    }

    if ((uint32_t)(now - gate->last_ms) < period_ms) {
        return false;
    }

    gate->last_ms = now;
    return true;
}

/*
 * The mute button, on its own so that its cadence can be chosen independently of the
 * status line's -- see the header. Split out of wm8904_audio_poll() on 2026-08-05, which
 * is where it used to live and where it was reached only once per main-loop iteration.
 * On EV88G73A that is 500 ms (one LED state), so a normal 100-300 ms tap fell between two
 * samples and the button appeared to need a deliberate long press. Nothing about the
 * sampling was wrong; it was being asked for at the wrong rate.
 *
 * IDEMPOTENT AND CHEAP, on purpose: it is one GPIO read plus an edge test, and it prints
 * only on a press. So it may be called as often as a board likes, and wm8904_audio_poll()
 * still calls it as well -- a board with no fast-poll path in its wait loop keeps exactly
 * the behaviour it had rather than silently losing the button.
 *
 * Edge-detected, so a held button toggles once. No debounce: even at the 100 ms cadence
 * this is designed for, contact bounce is one to two orders of magnitude shorter.
 */
void wm8904_audio_button_poll(void)
{
    bool pressed;

    if ((s_cfg == NULL) || !s_started) {
        return;
    }
    if (s_cfg->port->mute_button_pressed == NULL) {
        return;
    }

    pressed = s_cfg->port->mute_button_pressed();

    if (pressed && !s_button_prev) {
        /* Publish the requested level only -- the block ISR owns s_gain and performs the
         * actual gain_ctrl_mute_set(). One byte, so the write is atomic against the ISR on
         * this core. */
        uint8_t want = (uint8_t)((s_mute_request != 0u) ? 0u : 1u);

        s_mute_request = want;
        /* "gain:", not "WM8904:" -- this mute is the Gain stage's digital ramp and the codec
         * is never touched. The same firmware also has a real ANALOG mute (*ts, "analog mute
         * verified"), and the two must not read alike: one is a safety statement about HPOUT,
         * this one is arithmetic on samples. */
        console_out_str("gain: mute -> ");
        console_out_str((want != 0u) ? "on" : "off");
        console_out_str(" (ramp ");
        console_out_u32(s_cfg->gain_ramp_ms);
        console_out_str("ms)\n");
    }
    s_button_prev = pressed;
}

/*
 * Console RX health, appended to the periodic line -- RESIDENT BUT SILENT WHILE HEALTHY.
 *
 * It rides the TDM status line for one reason: this console has gone deaf once
 * (2026-08-10), and while it is deaf it cannot be asked anything, while the only
 * known recovery is a reprogram that resets every counter. The diagnosis has to
 * be on the wire before the failure, printed by the direction that still works.
 * See console_out.h's console_in_health_t for how to read the numbers.
 *
 * IT PRINTS NOTHING WHILE THE COUNTERS ARE CLEAN, which it earned: the fault it was
 * built to catch was found, understood and fixed (nora_uart_read_byte discards the
 * errored byte; owner-verified 2026-08-11, two clean runs of the sequence that used
 * to fail 100 % of the time), so a permanent field on every line would now be paying
 * line width and ROM to report "still fine". What it keeps is the part that cannot be
 * asked for after the fact: the moment a rescue happens, the line says so by itself.
 *
 * The trigger is the COUNTERS ONLY -- ovr and fer, both of which now advance on the
 * polled path. Deliberately not the registers: a register test here would put UxSTA
 * bit numbers in application code, which is the one thing console_out.h's seam exists
 * to prevent. The registers are still printed once something has triggered, because
 * that is when their bits are worth reading.
 *
 * Build with -DWM8904_RX_HEALTH_ALWAYS=1 to get the old unconditional field back --
 * for the case this trigger cannot see, a receiver switched off silently (URXEN
 * cleared) with no errored byte to count.
 *
 * Same tuple shape as dsp_load_print_tail()'s (run,act,blk,miss), because a log
 * reader should not have to learn a second grammar for the same kind of field.
 * "rx?" rather than a zero tuple when the board cannot answer: all-zero is a
 * legitimate reading -- a console nobody has typed at prints exactly that.
 *
 * The registers are fixed-width hex on purpose (console_out_hex16's whole
 * reason for existing): these get read bit by bit across many lines, and a
 * variable-width field moves the bit positions between lines, which is where a
 * misread becomes a wrong diagnosis.
 *
 * HEALTHY REFERENCE, measured on EV88G73A 2026-08-10 -- write down what normal
 * looks like or the first person to read this during an outage has to derive it:
 *
 *   MODE=80B0  UARTEN|BRGH|UTXEN|URXEN. URXEN going to 0 is the finding to look
 *              for; UARTEN going to 0 means the whole UART was shut down.
 *   STA =0000  fresh, and 0001 after any TX activity -- bit 0 is TXCIF, a sticky
 *              transmit-collision flag, so 0001 is NOT an RX finding. The RX
 *              bits are OERR (1), FERR (3), PERR (6); any of those set is real.
 *   fer =0     errored bytes discarded. NON-ZERO IS THE ANSWER TO THE ONE QUESTION
 *              this line was added for: edges reached the pin and were not
 *              characters. Reproduced on demand 2026-08-11 by powering the board
 *              from the motherboard before plugging the Nano's USB -- STA=000E,
 *              STAH=001D, byte=0. Reception now survives it (nora_uart_read_byte
 *              discards the errored byte), so fer counts what it cost.
 *   STAH=001E  URXBE|XON|RIDLE|UTXBF. Note UTXBF: the snapshot is taken while
 *              this very line is being transmitted, so the TX bits here are
 *              always busy and say nothing. Read only URXBF (0), URXBE (1) and
 *              RIDLE (3). URXBE=1 with RIDLE=1 while the host is typing is the
 *              signature of "no byte is arriving at the pin at all".
 */
#ifndef WM8904_RX_HEALTH_ALWAYS
#define WM8904_RX_HEALTH_ALWAYS 0
#endif

static void print_rx_health(void)
{
    console_in_health_t h;

    if (!console_in_health(&h)) {
        /* Still worth a word even in silent mode: "cannot answer" is not "healthy". */
        console_out_str(" rx?");
        return;
    }

    if ((WM8904_RX_HEALTH_ALWAYS == 0) &&
        (h.overrun == 0u) && (h.framing == 0u)) {
        return;
    }

    console_out_str(" RX!(byte,ovr,fer)=(");
    console_out_u32(h.bytes);
    console_out_str(",");
    console_out_u32(h.overrun);
    console_out_str(",");
    console_out_u32(h.framing);
    console_out_str(") reg(MODE,STA,STAH)=(");
    console_out_hex16(h.mode);
    console_out_str(",");
    console_out_hex16(h.sta);
    console_out_str(",");
    console_out_hex16(h.stah);
    console_out_str(")");
}

void wm8904_audio_poll(bool report)
{
    nora_spi_i2s_tdm_status_t st;

    /* Fallback for boards that only call this from their main loop. EV88G73A also
     * consumes observations from its 1 ms wait-loop service. */
    wm8904_audio_rx_observe_poll();

    if (s_cfg == NULL) {
        return;
    }

    if (!s_started) {
        if (report && (s_cfg->idle_report_period_ms != 0u) &&
            due_ms(&s_idle_gate, s_cfg->idle_report_period_ms)) {
            /*
             * Say why there is nothing to report, instead of returning silently. With no
             * codec attached, start() aborts before s_started, and a poll that printed
             * nothing left a reader unable to tell "the codec is not wired" from "the
             * load display is broken". Same principle as the load monitor printing its
             * reason rather than a zero.
             */
            if (s_stop_state == WM8904_AUDIO_STOP_MUTE_VERIFIED) {
                console_out_str(
                    "WM8904: stopped by *ts -- analog mute verified, TDM/DMA remain halted "
                    "until reboot.\n");
            } else if (s_stop_state == WM8904_AUDIO_STOP_MUTE_UNVERIFIED) {
                /* Repeated on every idle poll on purpose: an unconfirmed mute is a live
                 * hazard, not a one-off notice, and *ts's own reply may have scrolled away. */
                console_out_str(
                    "WM8904: stopped by *ts -- TDM/DMA halted, but the analog mute was NOT "
                    "verified (codec I2C failed). HPOUT may still be live.\n");
            } else {
                console_out_str(
                    "WM8904: not started (codec init failed) -- no audio, no DSP load."
                    " Check the codec's I2C wiring and power.\n");
            }
        }
        return;
    }

    /*
     * The button, deliberately OUTSIDE the report gate below -- throttling the console
     * must not throttle the button. This call is the FLOOR of the sampling rate, not the
     * intended one: a board that wants a responsive button samples it inside its own wait
     * loop as well (EV88G73A does, at 100 ms), and calling it from both is safe.
     */
    wm8904_audio_button_poll();

    /*
     * Everything above runs on EVERY call. Only the periodic status line below is gated
     * by the caller's report flag and then by this build's own counter.
     */
    if (!report) {
        return;
    }
    /* On by default (*tq0000 silences it). Checked BEFORE the time gate so that switching
     * it back on prints immediately rather than after a period of apparent inaction. */
    if (!s_load_line) {
        return;
    }
    if (!due_ms(&s_status_gate, s_cfg->status_period_ms)) {
        return;
    }

    if (!nora_spi_i2s_tdm_get_status(&st, true)) {
        return;
    }

    /* Same line shape as sonora's per-leg debug line, with one intentional
     * qualification: the HAL times one block in 16 by default.  "sampled max"
     * is honest about that diagnostic tradeoff; the run/act/block/miss safety
     * fields after it are still checked and counted on EVERY audio block. */
    console_out_str("TDM1: sampled ");
    dsp_load_print(&st.load, st.block_count, false);   /* periodic: no last/deadline -- see ?tq */
    dsp_load_print_tail(&st);
    print_rx_health();
    console_out_str("\n");
}

/* -------------------------------------------------------------------------- */
/* Console surface: *tp / *tp<NNNN> / ?tp (which path, and what CODEC-IN         */
/* carries) and *tq<NNNN> / ?tq (the periodic load line).                        */
/*                                                                              */
/* BARE *tp CYCLES, *tp<NNNN> SELECTS. The note that used to be here justified    */
/* cycling by claiming this board's grammar "is a deliberate subset with no       */
/* payload -- one letter, no arguments", which is FALSE: app_console.c decodes a   */
/* hex payload into msg->data[]/data_len exactly as the rest of the fleet does,    */
/* which is why *tq0000 below can take a value at all. So both forms exist, for    */
/* different jobs: the cycle names the position it landed on and needs no table to */
/* read, while the indexed form is the only way to reach a position deliberately   */
/* left OUT of the cycle (tone -- see the path note near the enum). Argument       */
/* parsing lives in the board's console handler; the functions here take values,   */
/* not ASCII.                                                                     */
/* -------------------------------------------------------------------------- */

/* The cycle, in order. CHAIN first because it is the default and what this board does in
 * normal operation; GAIN, COPY and MUTE after it because they are the comparison points
 * that cannot surprise anyone wearing headphones. TONE is deliberately absent, and the
 * cycle contains CHAIN so that a bare *tp is a way BACK to the default -- an off-cycle
 * position would otherwise be a one-way step out of it. */
static const uint8_t k_path_cycle[] = {
    (uint8_t)WM8904_PATH_CHAIN,
    (uint8_t)WM8904_PATH_GAIN,
    (uint8_t)WM8904_PATH_COPY,
    (uint8_t)WM8904_PATH_MUTE,
};

#define WM8904_PATH_CYCLE_COUNT (sizeof k_path_cycle / sizeof k_path_cycle[0])

static void local_path_apply(uint8_t path)
{
    /*
     * LEAVING THE CHAIN IS AN ABRUPT STOP, and deliberately so: a *tp is an explicit
     * request for another path, not a request to hear the synth fade first. What this
     * must not do is leave the engine mid-fade, because nothing would then be calling it
     * and the gate would freeze part-way -- which is exactly the state that used to skip
     * the phase reset and the 4 s attack on the next start. So the stage is stopped, the
     * gate is snapped shut, and the next start begins from t = 0 of the reference.
     *
     * Order matters: clearing the run flag first means the ISR stops touching the engine,
     * so the writes after it race nothing but the block already in progress (whose
     * remaining samples are about to be replaced by another path anyway).
     */
    if ((s_path == (uint8_t)WM8904_PATH_CHAIN) && (path != (uint8_t)WM8904_PATH_CHAIN)) {
        s_chain_avas_run = false;
        s_avas_on        = false;
        avas_line_ck_gate_off(&s_avas);
        s_avas.gate      = 0L;
    }

    s_path = path;

    console_out_str(" *tp: path -> ");
    console_out_str(path_name(path));
    console_out_str(s_started ? "\n"
                              : " (takes effect when audio startup succeeds)\n");
}

void wm8904_audio_path_next(void)
{
    size_t  at;
    uint8_t next;

    for (at = 0u; at < WM8904_PATH_CYCLE_COUNT; at++) {
        if (k_path_cycle[at] == s_path) {
            break;
        }
    }

    /*
     * A path that is NOT in the cycle -- tone, reached by index -- steps to the FIRST cycle
     * entry rather than to "whatever follows tone". From off-cycle the useful next step is
     * the default, and it makes a bare *tp the way back out of tone for someone who does
     * not remember which index they came from.
     */
    next = (at >= WM8904_PATH_CYCLE_COUNT)
               ? k_path_cycle[0]
               : k_path_cycle[(at + 1u) % WM8904_PATH_CYCLE_COUNT];

    /*
     * Skip GAIN on a build with no button and no ramp time: offering a mode that cannot
     * do anything reads as a broken command. Said out loud rather than skipped silently --
     * a cycle that quietly has three positions on one board and two on another is the
     * kind of difference that gets mistaken for a fault.
     */
    if ((next == (uint8_t)WM8904_PATH_GAIN) && !s_use_gain) {
        console_out_str(
            " *tp: no gain position in this build (no mute button or ramp time)\n");
        next = (uint8_t)WM8904_PATH_COPY;
    }

    local_path_apply(next);
}

bool wm8904_audio_path_set(uint16_t index)
{
    if (index >= (uint16_t)WM8904_PATH_COUNT) {
        console_out_str(" *tp: no such path -- 0000 copy, 0001 mute, 0002 tone,"
                        " 0003 gain, 0004 chain (bare *tp cycles chain/gain/copy/mute)\n");
        return false;
    }

    if ((index == (uint16_t)WM8904_PATH_GAIN) && !s_use_gain) {
        console_out_str(
            " *tp: no gain position in this build (no mute button or ramp time)\n");
        return false;
    }

    /*
     * The chain IS selectable by index and by the cycle, unlike the AVAS path it
     * replaced. That guard existed because selecting the path started the synth, and the
     * engine cost more than the block period at the time: an overrunning block callback
     * re-enters on every return, the foreground never runs again, and this kit has no
     * reset button. Neither half of that is true here -- the chain with the stage idle is
     * the copy baseline plus two slots of arithmetic, and starting the synth is now a
     * private voice-key operation that carries the warning (see avas_enable_set()).
     */
    local_path_apply((uint8_t)index);
    return true;
}


/* -------------------------------------------------------------------------- */
/* AVAS voice keys -- START and STOP the synth. Not a path any more.             */
/*                                                                            */
/* STOP DOES NOT TOUCH THE PATH, and that is the whole point: the chain keeps    */
/* running, so the engine's ~3 s release fade is rendered instead of discarded.  */
/* What used to happen here was gate_off() followed immediately by a switch to   */
/* copy, which stopped the ISR calling the engine -- the fade never reached the  */
/* speaker, and the gate froze at 1.0, which then skipped the phase reset and     */
/* the 4 s attack on the next start.                                            */
/*                                                                            */
/* The warning about an overrunning block ISR belongs HERE now (starting the     */
/* synth is what adds ~289 us to the block), and is printed BEFORE the stage is  */
/* armed so it is on the wire even if the foreground never gets another turn.    */
/* -------------------------------------------------------------------------- */
static void avas_enable_set(bool on)
{
    if (!on) {
        if (!s_avas_on && !avas_line_ck_is_active(&s_avas)) {
            console_out_str(" AVAS: synth already stopped\n");
            return;
        }

        s_avas_on = false;
        avas_line_ck_gate_off(&s_avas);

        /* s_chain_avas_run is left ALONE: the ISR owns it from here and clears it when
         * the engine says the tail is spent. That is what renders the fade.
         *
         * TWO WORDINGS, because with the GATE part masked off there IS no fade: *tb pins
         * the gate wide open, so avas_line_ck_is_active() answers from the request and the
         * stage stands down on the next block. Measured on the board (2026-08-07): `?tp`
         * reported the stage stopped immediately. Announcing a 3 s fade there would be the
         * same defect this session came to test -- a message that says the opposite of what
         * the code does -- so it is said correctly rather than approximately. */
        console_out_str(((s_avas.parts & AVAS_TYPE_TY_CK_PART_GATE) != 0u)
                            ? " AVAS: synth stop -- fading out over ~3 s\n"
                            : " AVAS: synth stop -- immediate, because *tb has the"
                              " gate part masked off (no fade exists to render)\n");
        return;
    }

    if (!s_avas_rate_ok) {
        console_out_str(" AVAS: refused -- coefficient table is for another sample rate"
                        " (see bring-up log)\n");
        return;
    }

    if (s_avas_on) {
        console_out_str(" AVAS: synth already started\n");
        return;
    }

    console_out_str(" AVAS: synth start, voice = ");
    console_out_str(avas_line_ck_voice_name(avas_line_ck_voice(&s_avas)));
    console_out_str(". It adds ~289us to a 667us block; if the"
                    " block ISR overruns, this console goes silent and recovery is a"
                    " reprogram.\n");
    console_out_str("      Gate opens over 4 s (~9 s to -1 dB), so silence at first"
                    " is correct.\n");

    /* The chain is the only path that has an AVAS stage, so a start from anywhere else
     * selects it first -- with the same "path ->" line every other route prints. */
    if (s_path != (uint8_t)WM8904_PATH_CHAIN) {
        local_path_apply((uint8_t)WM8904_PATH_CHAIN);
    }

    /* gate_on() restarts every oscillator from its measured phase set -- i.e. from t = 0
     * of the reference -- but only if the previous release has finished. Pressed during
     * a fade it deliberately does NOT reset, because a phase jump there is a click; the
     * synth simply ramps back up from wherever it had decayed to. sonora's
     * app_avas_ty_set_enable() is the same rule. */
    s_avas_on = true;
    avas_line_ck_gate_on(&s_avas);
    s_chain_avas_run = true;
}

/* -------------------------------------------------------------------------- */
/* `a` / `A` / `*cy00` -- WHICH VOICE, and why the exclusivity lives here.      */
/*                                                                            */
/* One engine, two coefficient sets, and switching re-seeds every oscillator     */
/* from the new voice's measured t = 0 phases -- so a switch while the engine    */
/* is sounding is a discontinuity in all of them at once, which at these levels  */
/* is a loud click. avas_line_ck_voice_set() therefore refuses unless the gate   */
/* is fully shut, and this layer's job is to turn that refusal into a sentence   */
/* naming what to do about it, plus to know the one state the engine cannot see: */
/* a stop that has been asked for is still ~3 s of fade, and `?tp` already        */
/* reports that as its own state for the same reason.                           */
/*                                                                            */
/* THIS IS SONORA'S RULE, DELIBERATELY. There the two AVAS engines are resident  */
/* together and fully exclusive at run time, `a` picks Type_TY and `A` the         */
/* Type_LB, and asking for one while the other sounds prints a refusal        */
/* instead of crossfading. The two boards are listened to in one session, by ear, */
/* switching back and forth -- so a key that means something different on each is */
/* the mistake, and matching it is worth more than any improvement to it.         */
/* -------------------------------------------------------------------------- */

/* The board's key bindings speak WM8904_AUDIO_AVAS_VOICE_*, the engine speaks
 * AVAS_LINE_CK_VOICE_*, and this is the one place both are visible -- so it is the one
 * place the equality can be a build failure instead of a swapped voice. */
typedef char wm8904_audio_avas_voice_ids_agree[
    (((WM8904_AUDIO_AVAS_VOICE_TYPE_TY) == (AVAS_LINE_CK_VOICE_TYPE_TY)) &&
     ((WM8904_AUDIO_AVAS_VOICE_TYPE_LB) == (AVAS_LINE_CK_VOICE_TYPE_LB))) ? 1 : -1];

/* Everything that means "the engine still owns its oscillators": started, or        */
/* stopped-but-fading. s_chain_avas_run is the ISR's own copy and outlives the       */
/* request by exactly the fade, which is the state this has to catch.                */
static bool avas_is_sounding(void)
{
    return s_avas_on || s_chain_avas_run || avas_line_ck_is_active(&s_avas);
}


static uint8_t avas_voice(void)
{
    return avas_line_ck_voice(&s_avas);
}


static bool avas_voice_set(uint8_t voice)
{
    if (!avas_line_ck_voice_present(voice)) {
        console_out_str(" AVAS: refused -- this image holds only the ");
        console_out_str(avas_line_ck_voice_name(avas_voice()));
        console_out_str(" voice. Build with -DAVAS_CK_VOICE_BOTH=1 for both.\n");
        return false;
    }

    if (avas_is_sounding()) {
        console_out_str(" AVAS: refused -- the ");
        console_out_str(avas_line_ck_voice_name(avas_voice()));
        console_out_str(" voice is still sounding. Stop it first (its key, or *cy00 for"
                        " Type_TY) and let"
                        " the ~3 s fade finish; switching mid-fade would click.\n");
        return false;
    }

    if (!avas_line_ck_voice_set(&s_avas, voice)) {
        /* Not reachable through the two questions above -- both layers now ask
         * avas_line_ck_is_active(), and this one asks it through s_chain_avas_run too.
         * If it does happen they disagree about what "silent" means, which is worth
         * saying rather than papering over with a retry. */
        console_out_str(" AVAS: refused by the engine -- it is still sounding\n");
        return false;
    }

    console_out_str(" AVAS: voice = ");
    console_out_str(avas_line_ck_voice_name(voice));
    console_out_str("\n");
    return true;
}


/* One key's worth of behaviour, so `a` and `A` cannot drift apart: select this   */
/* voice and start, stop it if it is the one already sounding, refuse if the other */
/* is. A bare toggle per key would let both be "on" in the user's head while only  */
/* one engine exists.                                                            */
void wm8904_audio_avas_voice_key(uint8_t voice)
{
    if (avas_is_sounding()) {
        if (avas_voice() == voice) {
            avas_enable_set(false);
        } else {
            (void)avas_voice_set(voice);   /* prints the reason */
        }
        return;
    }

    if (!avas_voice_set(voice)) {
        return;                                        /* prints the reason */
    }
    avas_enable_set(true);
}

void wm8904_audio_path_report(void)
{
    static const char digits[] = "0123456789abcdef";
    const uint16_t    peak0    = s_rx_peak[0];
    const uint16_t    peak1    = s_rx_peak[1];
    const uint16_t    mask     = s_rx_slot_mask;
    size_t            slot;

    console_out_str(" ?tp: path = ");
    console_out_str(path_name(s_path));
    console_out_str(s_started
                        ? "\n"
                        : " (transport NOT running -- nothing is being processed)\n");

    /*
     * The synth's state, and THREE values rather than on/off: "fading" is a real state
     * that lasts ~3 s, costs the full engine while it lasts, and is the one in which a
     * start deliberately does not reset the phase. Reporting it as "stopped" would make
     * both of those look like faults.
     */
    if (s_path == (uint8_t)WM8904_PATH_CHAIN) {
        console_out_str(" ?tp: avas stage = ");
        if (s_avas_on) {
            console_out_str("started (gate opening/open)\n");
        } else if (s_chain_avas_run || avas_line_ck_is_active(&s_avas)) {
            console_out_str("fading out (~3 s release, still rendering)\n");
        } else {
            console_out_str("stopped (skipped entirely -- costs nothing)\n");
        }
    }

    /*
     * The two numbers and the mask are the whole point of this command: they say whether
     * CODEC-IN carries a signal, and in WHICH SLOTS it arrives. A steady output unrelated
     * to the input looks the same from the analog side whether the input is silent or
     * landing in slots nobody reads, and those two need different fixes.
     */
    console_out_str(" ?tp: CODEC-IN peak slot0=");
    console_out_u32((uint32_t)peak0);
    console_out_str(" slot1=");
    console_out_u32((uint32_t)peak1);
    console_out_str(" of 32767, sampled active slots=");
    for (slot = 0u; slot < (size_t)NORA_TDM_SLOTS_PER_FS; slot++) {
        console_out_char(((mask & (uint16_t)(1u << slot)) != 0u) ? digits[slot] : '.');
    }
    console_out_str("  (sampled peak-hold since the last ?tp; cleared now)\n");

    s_rx_peak[0]   = 0u;
    s_rx_peak[1]   = 0u;
    s_rx_slot_mask = 0u;
}

/* -------------------------------------------------------------------------- */
/* PRE / POST gain -- the console side.                                        */
/*                                                                            */
/* One implementation with a `post` flag rather than two near-identical pairs:  */
/* the two knobs differ only in WHICH end of the chain they scale, and every    */
/* word of the reporting -- the snap, the realised value, the clamp count -- is  */
/* the same sentence about a different stage. Two copies of it would be two     */
/* places for the wording of a level report to drift.                          */
/* -------------------------------------------------------------------------- */

/* Signed tenths as a decimal, with the sign always shown: "+6.0", "-0.5", "+0.0". The
 * sign is not decoration here -- a gain report is read to answer "up or down", and a
 * leading '+' means a missing '-' cannot be a rendering accident. */
static void gain_db_print_x10(int16_t x10)
{
    uint16_t mag = (uint16_t)((x10 < 0) ? -x10 : x10);

    console_out_char((x10 < 0) ? '-' : '+');
    console_out_u32((uint32_t)(mag / 10u));
    console_out_char('.');
    console_out_u32((uint32_t)(mag % 10u));
}

static bool gain_db_console_set(bool post, int16_t db_x10)
{
    gain_db_t resolved;

    /* Snap here, in the main loop, and publish the SNAPPED value -- so the number printed
     * below is the number the ISR will apply, not the request. gain_db.h refuses a value
     * outside the table (a different mistake from off-grid: see its comment) and leaves
     * `resolved` untouched, so nothing is published on a refusal. */
    if (!gain_db_from_x10(db_x10, &resolved)) {
        console_out_str(post ? " *to" : " *ti");
        console_out_str(": out of range -- ");
        gain_db_print_x10((int16_t)(GAIN_DB_HALF_MIN * 5));
        console_out_str(" dB to ");
        gain_db_print_x10((int16_t)(GAIN_DB_HALF_MAX * 5));
        console_out_str(" dB\n");
        return false;
    }

    {
        const int16_t half = (int16_t)(resolved.db_x10 / 5);

        if (post) {
            s_post_half_request = half;
        } else {
            s_pre_half_request = half;
        }

        console_out_str(post ? " *to: POST = " : " *ti: PRE = ");
        gain_db_print_x10(resolved.db_x10);
        console_out_str(" dB");

        /* Say so when the request was moved. A snap that is silent is a knob that appears
         * to have finer resolution than it has, and the operator then hears no change and
         * blames the audio path. Same policy as gain_ctrl_mute_set()'s ramp_ms. */
        if (resolved.db_x10 != db_x10) {
            console_out_str(" (snapped from ");
            gain_db_print_x10(db_x10);
            console_out_str(" -- the grid is 0.5 dB)");
        }
        console_out_str(s_started ? "\n"
                                  : "  [transport NOT running -- a start reloads the"
                                    " profile's value]\n");
    }

    return true;
}

static void gain_db_console_report(bool post)
{
    const int16_t half = post ? s_post_half_request : s_pre_half_request;
    const uint16_t sat = post ? s_post_sat : s_pre_sat;

    console_out_str(post ? " ?to: POST (chain output, after the mix) = "
                         : " ?ti: PRE (chain input, before AVAS) = ");
    gain_db_print_x10((int16_t)(half * 5));
    console_out_str(" dB");
    if (half == 0) {
        console_out_str(" -- unity, bit-exact");
    }
    console_out_str("\n");

    /*
     * The clamp count, and it is reported even when zero: "0" is the answer that says the
     * configured boost fits the signal, which is the thing worth knowing after raising it.
     * Cleared on read, so the count always belongs to a known interval.
     */
    console_out_str(post ? " ?to: clamped " : " ?ti: clamped ");
    console_out_u32((uint32_t)sat);
    console_out_str(" samples since the last read");
    if (sat != 0u) {
        console_out_str("  <-- REDUCE THE GAIN: a clamp is a hard-limited sample, not"
                        " level (wraps at 65535)");
    }
    console_out_str("\n");

    if (!s_started) {
        console_out_str(post ? " ?to: transport not running -- nothing is being scaled\n"
                             : " ?ti: transport not running -- nothing is being scaled\n");
    } else if (s_path != (uint8_t)WM8904_PATH_CHAIN) {
        /* The other four paths deliberately do not apply it (see the ownership note), and
         * a dB reported while one of them is selected would otherwise look effective. */
        console_out_str(post ? " ?to: path is " : " ?ti: path is ");
        console_out_str(path_name(s_path));
        console_out_str(", which does NOT apply PRE/POST -- only the chain does (*tp"
                        " to it)\n");
    }

    if (post) {
        s_post_sat = 0u;
    } else {
        s_pre_sat = 0u;
    }
}

bool wm8904_audio_pre_gain_set(int16_t db_x10)
{
    return gain_db_console_set(false, db_x10);
}

bool wm8904_audio_post_gain_set(int16_t db_x10)
{
    return gain_db_console_set(true, db_x10);
}

void wm8904_audio_pre_gain_report(void)
{
    gain_db_console_report(false);
}

void wm8904_audio_post_gain_report(void)
{
    gain_db_console_report(true);
}

/* -------------------------------------------------------------------------
 * AVAS PITCH TRIM -- *tc / ?tc and the arrow-key hotkeys (ev88g73a_console_out.c)
 * share this pair of helpers so the two input paths print identically and neither
 * can drift from what avas_line_ck actually clamped to.  There is no POT on this
 * board to arbitrate against, unlike sonora's, so the value is simply owned by
 * whichever of the two last wrote it -- see avas_pitch_pot_design.md's "why POT
 * and keys never coexisted there" for the complexity this board does not have.
 * ------------------------------------------------------------------------- */
static void avas_pitch_print_cent(int16_t cent)
{
    const uint16_t mag = (uint16_t)((cent < 0) ? -cent : cent);

    console_out_char((cent < 0) ? '-' : '+');
    console_out_u32((uint32_t)mag);
}

static void avas_pitch_request_and_report(const char *prefix, int16_t cent)
{
    int16_t applied;

    avas_line_ck_request_pitch_cent(&s_avas, cent);
    applied = avas_line_ck_get_pitch_cent_req(&s_avas);

    console_out_str(prefix);
    avas_pitch_print_cent(applied);
    console_out_str(" cent");
    if (applied != cent) {
        console_out_str(" (clamped from ");
        avas_pitch_print_cent(cent);
        console_out_str(")");
    }
    console_out_str("\n");
}

bool wm8904_audio_avas_pitch_set(int16_t cent)
{
    /* Always OK: out-of-range clamps rather than refuses, same as *ti and *to never
     * refuse a legal-but-extreme dB -- the clamp count in the reply is how the
     * caller finds out it did not get exactly what it asked for. */
    avas_pitch_request_and_report(" *tc: pitch request = ", cent);
    return true;
}

void wm8904_audio_avas_pitch_report(void)
{
    const int16_t committed = avas_line_ck_get_pitch_cent(&s_avas);
    const int16_t requested = avas_line_ck_get_pitch_cent_req(&s_avas);

    console_out_str(" ?tc: pitch = ");
    avas_pitch_print_cent(committed);
    console_out_str(" cent");
    if (requested != committed) {
        console_out_str(" (");
        avas_pitch_print_cent(requested);
        console_out_str(" cent requested, applies at the next rebuild)");
    }
    console_out_str(" -- range +/-");
    console_out_u32((uint32_t)AVAS_LINE_CK_PITCH_LIMIT_CENT);
    console_out_str(" cent\n");
}

/* Called from the up/down arrow hotkeys: nudge relative to the last REQUESTED
 * value rather than the committed one, so two presses typed faster than one
 * envelope rebuild (<1 ms) add up instead of the second one clobbering the
 * first. */
void wm8904_audio_avas_pitch_step(int16_t delta_cent)
{
    const int32_t next = (int32_t)avas_line_ck_get_pitch_cent_req(&s_avas)
                        + (int32_t)delta_cent;

    avas_pitch_request_and_report(" pitch (arrow key) = ", (int16_t)next);
}

void wm8904_audio_load_line_set(bool on)
{
    s_load_line = on;

    /* Un-prime the gate so turning it ON prints at the next report rather than a full
     * period later, which would read as the command not having worked. */
    s_status_gate.primed = false;

    console_out_str(" *tq: TDM1 periodic line ");
    console_out_str(on ? "ON\n" : "OFF\n");
}

bool wm8904_audio_load_line_on(void)
{
    return s_load_line;
}

void wm8904_audio_load_line_report(void)
{
    nora_spi_i2s_tdm_status_t st;

    console_out_str(" ?tq: TDM1 periodic line ");
    console_out_str(s_load_line ? "ON" : "OFF (*tq0001 turns it on)");
    console_out_str("\n");

    /*
     * Then ONE line with the detail the periodic one leaves out. This is the whole point
     * of having dropped those fields rather than deleted them: the absolute-time figures
     * are still available, at the moment they are wanted, and a single line is short
     * enough to carry its own caveat -- every microsecond here is scaled by the REQUESTED
     * Fcy, and this board's FRC runs about 0.68 % fast, so the percentage is exact while
     * the us figures read 0.68 % high.
     */
    if (!s_started) {
        console_out_str(" ?tq: transport not running (audio startup did not complete)\n");
        return;
    }
    if (!nora_spi_i2s_tdm_get_status(&st, true)) {
        console_out_str(" ?tq: status unavailable\n");
        return;
    }

    // See the periodic line: timing is sampled, transport-health counts are exact.
    console_out_str("TDM1: sampled ");
    dsp_load_print(&st.load, st.block_count, true);
    dsp_load_print_tail(&st);
    print_rx_health();
    console_out_str("\n");
}
