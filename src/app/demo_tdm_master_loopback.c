#include "demo_tdm_master_loopback.h"
#include "app_config.h"

#if DEMO_ENABLE_TDM_MASTER_LOOPBACK

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "nora_spi_i2s_tdm.h"
#include "nora_spi_i2s_tdm_conf.h"   /* geometry: SLOTS_PER_FS / BLOCK_FRAMES */
#include "nora_clock.h"
#include "nora_high_res_timer.h"

#include "timer_app.h"   /* GetTicks() -- the status line's cadence is a time */

/* No gpio/pps include, and that is the point: this module routes no pin. See the note in
 * demo_tdm_master_loopback.h about the four RP numbers that used to be here. */

/* One ping/pong half, one direction (int32 samples). */
#define DEMO_HALF_SAMPLES  ((size_t)NORA_TDM_SLOTS_PER_FS * NORA_TDM_BLOCK_FRAMES)

/*
 * The 32-bit value transmitted in slots 1 and 2 (the rest of the frame is silent -- see
 * demo_block_cb). Overridable from the build so a different scope question can be asked
 * without editing this file:
 *
 *     buildtools\build.ps1 ... -Define @('...','DEMO_TDM_TX_PATTERN=0x80017FFEu')
 *
 * The two patterns that have earned their keep, and what each is FOR:
 *
 *   0xFFFF_0000  HALFWORD ORDER (the default). Halves are all-ones and all-zeros, so the
 *                order is readable by inspection with no 1-BCLK features to alias at
 *                50 MS/s: high half first -> the burst OPENS with 16 BCLK high then goes low;
 *                low half first -> it opens with 16 BCLK LOW (merging into the silent gap)
 *                and the high run appears second. **The high half leads, as it must**. Check
 *                that the burst also ENDS ~64 BCLK after the FS edge: that distinguishes a
 *                swap inside the slot from a 16-BCLK delay of the whole stream.
 *
 *   0x8001_7FFE  BOUNDARY MARKING (select with -D). MSB-first on the wire:
 *
 *                    1000000000000001 0111111111111110
 *                    \____ 0x8001 ___/ \___ 0x7FFE ___/
 *                     high half-word    low half-word
 *
 *                bit31 gives a 1-BCLK spike at the start of the slot, bit16/bit15 straddle
 *                the 16-bit wire-word boundary (the unit FRMCNT actually counts in), and the
 *                14-long low/high runs make bit order and polarity unmistakable. Use it when
 *                the question is WHERE a boundary sits. This is the pattern that closed the
 *                FS-vs-data alignment question.
 *
 *                Do NOT use it to judge halfword order, which is what went wrong on
 *                2026-08-03: its 1-BCLK spikes are ~4 samples at 50 MS/s and merge with the
 *                adjacent 14-runs (bit16's spike + bit15's single low + 14 highs read as one
 *                ~16 BCLK block at 1 us/div), so both orders can be argued from the same
 *                trace. 0xFFFF_0000 has no such ambiguity -- use that instead.
 *
 *   0xFFFF_0000  HALFWORD ORDER, spike-free confirmation. Same question as above with no
 *                1-BCLK features at all: high half first -> the burst OPENS with 16 BCLK
 *                high (1.28 us at 12.5 MHz) then goes low; low half first -> it opens with
 *                16 BCLK low, merging into the silent gap, and the high run appears second.
 *                Nothing to alias at 50 MS/s, and nothing to infer about merged edges. Worth
 *                reaching for if the order is ever doubted again.
 */
#ifndef DEMO_TDM_TX_PATTERN
#define DEMO_TDM_TX_PATTERN  0xFFFF0000u
#endif

/*
 * WHAT THE FIRST TWO SLOTS CARRY. The fixed pattern above answers a WIRE question
 * (bit order, slot boundaries, FS-vs-data alignment) and stays the default because that
 * is the standing regression test. The other two modes answer a DATA-PATH question --
 * "is a changing sample getting through, in the right order, at the right rate" -- which
 * a constant cannot show at all:
 *
 *     DEMO_TDM_TX_MODE_FIXED (0)  DEMO_TDM_TX_PATTERN, every frame identical.
 *     DEMO_TDM_TX_MODE_RAMP  (1)  count-up: +DEMO_TDM_RAMP_STEP per FRAME (not per slot),
 *                                 so the value is a frame counter on the wire and a
 *                                 sawtooth after a codec. Per-frame is the load-bearing
 *                                 part: a per-SLOT ramp is what made an earlier version
 *                                 useless for alignment, because then no two slots of a
 *                                 frame agree and nothing marks slot 1.
 *     DEMO_TDM_TX_MODE_SINE  (2)  full-scale/2 sine from a 16-entry Q31 table, one entry
 *                                 per frame => f = fs/16. At the default BRG=3 (BCLK
 *                                 12.5 MHz, TDM8/32 => fs 48828 Hz) that is 3051.8 Hz.
 *
 * Select at build time, nothing else changes:
 *     buildtools\build.ps1 ... -Define @('...','DEMO_TDM_TX_MODE=2')
 *
 * All three keep the frame SHAPE identical -- value in slots 1-2, silence in the rest --
 * so the isolated-burst landmark that closed the alignment question survives, and a scope
 * trace taken in ramp or sine mode is still readable frame by frame.
 */
#define DEMO_TDM_TX_MODE_FIXED  0
#define DEMO_TDM_TX_MODE_RAMP   1
#define DEMO_TDM_TX_MODE_SINE   2

#ifndef DEMO_TDM_TX_MODE
#define DEMO_TDM_TX_MODE  DEMO_TDM_TX_MODE_FIXED
#endif

#if (DEMO_TDM_TX_MODE != DEMO_TDM_TX_MODE_FIXED) && \
    (DEMO_TDM_TX_MODE != DEMO_TDM_TX_MODE_RAMP) && \
    (DEMO_TDM_TX_MODE != DEMO_TDM_TX_MODE_SINE)
#error "DEMO_TDM_TX_MODE must be 0 (fixed pattern), 1 (count-up ramp) or 2 (sine)."
#endif

/*
 * Ramp step per frame. 0x0100_0000 = 1/256 of the full 32-bit modulo range, so the count-up wraps
 * every 256 frames (5.2 ms at fs 48828 Hz) -- a sawtooth slow enough to see on a scope in
 * one screen and fast enough to be audible as ~190 Hz through a codec. Larger steps make
 * the wire pattern easier to read; smaller ones make the audio tone lower.
 */
#ifndef DEMO_TDM_RAMP_STEP
#define DEMO_TDM_RAMP_STEP  0x01000000u
#endif

#if DEMO_TDM_TX_MODE == DEMO_TDM_TX_MODE_SINE
/*
 * One cycle of sine in 16 Q31 steps at HALF full scale (0x4000_0000), i.e. -6 dBFS.
 * Half scale on purpose: a full-scale table would put 0x7FFF_FFFF/0x8000_0000 on the wire,
 * where an off-by-one in slot encoding or a sign error reads as a legitimate sample. At
 * -6 dBFS the sign and the envelope are both unambiguous, and there is headroom for any
 * gain stage after the codec.
 *
 * A TABLE, not a computed sine: this runs inside the block callback (an ISR-time path
 * whose budget is what dsp_load measures), and sin() from libm on a dsPIC would dominate
 * it. 16 entries also means the phase index is a 4-bit mask, so there is no modulo.
 */
static const int32_t s_sine_q31[16] = {
             0,   410862743,   759250459,   992008597,
    1073741824,   992008597,   759250459,   410862743,
             0,  -410862743,  -759250459,  -992008597,
   -1073741824,  -992008597,  -759250459,  -410862743
};
#endif

/* The board's half, handed in by demo_tdm_master_loopback_start(). Held in a static
 * because the transport HAL's port hooks are called back later, from open(). */
static const demo_tdm_master_loopback_port_t *s_port;

/* -------------------------------------------------------------------------- */
/* Board/clock port: forwarded straight to the board's hook, which open(MASTER) */
/* reaches through here. The transport core does no pin routing itself.         */
/*                                                                            */
/* The only thing this adds is the role check. It belongs here and not in the    */
/* board, because "this demo only drives MASTER" is the DEMO's constraint --      */
/* the boards can route either direction and should not be asked to refuse one    */
/* on a demo's behalf. That is the inverse of how it used to be.                  */
/* -------------------------------------------------------------------------- */
static bool demo_configure_pins(nora_spi_i2s_tdm_clock_role_t role)
{
    if (role != NORA_SPI_I2S_TDM_CLOCK_MASTER) {
        return false;   /* this demo only drives the master role */
    }
    if ((s_port == 0) || (s_port->configure_pins == 0)) {
        return false;
    }

    return s_port->configure_pins(role);
}

static const nora_spi_i2s_tdm_port_t s_demo_port = {
    .configure_pins      = demo_configure_pins,
    /*
     * NULL, and the reason is worth stating because both boards leave it NULL for
     * DIFFERENT reasons and neither said so. This hook is the CLC bypass route used to
     * fan out a slave's incoming clock. Here the role is MASTER, so there is no incoming
     * clock to fan out -- and the 50%-duty FS this demo asks for is NOT this hook's job:
     * the HAL's own fs_clc module owns CLC1 and finds the FS pin itself.
     */
    .clc_passthrough     = 0,
    .clock_source_init   = 0,
    .clock_source_ready  = 0,   /* NULL => always ready (self-clocked master) */
    .consume_clock_event = 0,
};

/* -------------------------------------------------------------------------- */
/* Block callback: put DEMO_TDM_TX_PATTERN in the first two slots and silence in the  */
/* rest, so SDO shows an isolated, scope-legible burst with no external source. (This  */
/* was a per-sample ramp once; see the note in the loop for why a ramp -- and equally  */
/* a constant in EVERY slot -- cannot answer an alignment question.) RX is available    */
/* with the SDO->SDI jumper but unused here.                                          */
/* -------------------------------------------------------------------------- */
static void demo_block_cb(const nora_tdm_slot_t *src,
                          nora_tdm_slot_t *dst,
                          void *user)
{
    size_t i;
    (void)user;
    (void)src;   /* RX is available for a future echo-verify; unused here */

    /*
     * A fixed pattern in the FIRST TWO SLOTS ONLY; every later slot transmits zero.
     * See DEMO_TDM_TX_PATTERN above for the pattern itself and what each choice shows.
     *
     * The two-slot restriction is the load-bearing part, and it was learned the hard way.
     * This used to be a per-sample monotonic ramp (dst[i] = g_ramp++ << 8), which carries NO
     * frame landmark at all: consecutive slots differ only in their low bits, so nothing in
     * the data says "this is slot 1" and an FS-vs-data offset cannot be measured, only
     * guessed at. Three sessions argued about J-K phase, FRMCNT arithmetic and SPIFE/CKP/CKE
     * from a waveform incapable of showing the answer.
     *
     * A CONSTANT pattern in EVERY slot is just as landmark-free, which is the less obvious
     * trap: a value like 0x8001_7FFE puts a 1-BCLK spike at both bit31 and bit16, so each
     * 32-bit slot looks like two similar 16-bit wire words and a 4-slot frame reads as eight
     * of them -- that frame was in fact misread as "8 channels of data". A boundary that is
     * continuous with its neighbours cannot be told apart from them.
     *
     * Filling only slots 1 and 2 leaves the rest of the frame silent, so the data is an
     * isolated 2-slot burst followed by a long low gap (6 slots = 192 BCLK at TDM8). The
     * first rising edge out of that gap is unambiguously the start of slot 1 -- the landmark
     * the FS edge is compared against.
     */
    for (i = 0u; i < DEMO_HALF_SAMPLES; i++) {
        const size_t slot = i % (size_t)NORA_TDM_SLOTS_PER_FS;
        int32_t      value;

        if (slot >= 2u) {
            value = 0;              /* silence: the gap that marks where slot 1 begins */
        } else {
#if DEMO_TDM_TX_MODE == DEMO_TDM_TX_MODE_FIXED
            value = (int32_t)DEMO_TDM_TX_PATTERN;
#else
            /*
             * ADVANCED ON SLOT 0 ONLY, so slots 1 and 2 of one frame carry the SAME
             * value and the step is per frame. Advancing per slot would make the two
             * slots of a frame differ, which is exactly the landmark-free waveform the
             * note above warns about.
             */
            static uint32_t phase;      /* ramp accumulator / sine table index */

            if (slot == 0u) {
                phase++;
            }
#if DEMO_TDM_TX_MODE == DEMO_TDM_TX_MODE_RAMP
            value = (int32_t)(phase * DEMO_TDM_RAMP_STEP);
#else
            value = s_sine_q31[phase & 0x0Fu];
#endif
#endif
        }

        /* Encoded, not assigned: the buffer element is a wire slot, so the value's
         * halves have to be placed in transmit order. This is also what makes the
         * pattern mean what the boot banner says it means -- 0xFFFF0000 must appear on
         * the wire as 16 highs THEN 16 lows, which is the defect-7 acceptance test. */
        nora_tdm_slot_encode_s32(&dst[i], value);
    }
}

/* -------------------------------------------------------------------------- */
void demo_tdm_master_loopback_start(const demo_tdm_master_loopback_port_t *port)
{
    nora_spi_i2s_tdm_inst_t *spi1;
    nora_spi_i2s_tdm_config_t cfg;
    nora_high_res_timer_config_t htcfg;

    /* Refuse before touching the transport, rather than opening it with no pins routed:
     * a running SPI on unrouted pins is a silent failure, and this one is reportable. */
    if ((port == 0) || (port->configure_pins == 0)) {
        printf("TDM master loopback: no board port (pin routing hook missing)\n");
        return;
    }
    s_port = port;

    /* Load monitor: run Timer2/3 at Fcy so get_load() reports real ISR time. */
    htcfg.timer_clk_hz = nora_clock_get_fcy_hz();
    htcfg.run_in_idle  = true;
    (void)nora_high_res_timer_init(&htcfg);

    /* Checked since phase 2: set_port() refuses (ERR_ALREADY_OPEN) while the transport is
     * open or streaming. This demo starts once from a fresh boot, so a false would mean
     * something already brought the transport up -- worth a line rather than a mystery. */
    if (!nora_spi_i2s_tdm_set_port(&s_demo_port)) {
        printf("TDM master loopback: set_port refused (err=%d)\n",
               (int)nora_spi_i2s_tdm_get_last_error());
        return;
    }

    spi1 = nora_spi_i2s_tdm_spi1();
    if (spi1 == 0) {
        printf("TDM master loopback: no SPI1 instance\n");
        return;
    }

    /* Master TDM8 / 32-bit, 50%-duty FS via the CLC1 J-K path.
     *
     * BOTH FS legs are verified, and neither was ever at fault. FS_PULSE (CLC out of the
     * path) and FS_50PCT (CLC J-K toggle) each place the data at the conventional 1-bit
     * delay after the FS edge, with the burst spanning exactly slots 1-2; 10 consecutive
     * resets gave an identical picture, so the J-K startup phase is deterministic and
     * correct. Flip this one line to ..._FS_PULSE for the CLC-free comparison -- nothing
     * else needs to change, and it is still the quickest way to separate a framing question
     * from a slot-contents one. */
    cfg.format                        = NORA_SPI_I2S_TDM_FORMAT_TDM;
    cfg.clock_role                          = NORA_SPI_I2S_TDM_CLOCK_MASTER;
    cfg.slots_per_fs                  = NORA_TDM_SLOTS_PER_FS;
    cfg.word_bits                     = 32u;
    cfg.fs_shape                      = NORA_SPI_I2S_TDM_FS_50PCT;
    cfg.block_frames                  = NORA_TDM_BLOCK_FRAMES;
    /* Self-clocked BCLK = Fp / (2*(BRG+1)). At Fcy=100 MHz (device max) BRG=3 gives
     * 12.5 MHz BCLK -> TDM8/32-bit fs ~= 48.8 kHz (a realistic, scope-visible rate).
     * Rate-agnostic transport; adjust for the desired frame rate. */
    cfg.brg                           = 3u;
    cfg.mclk_enable                   = false;
    cfg.fs_coincides_first_bclk       = false;   /* SPIFE=0 (provisional; HW-tune) */
    cfg.bclk_idle_high                = true;    /* CKP=1 (provisional) */
    cfg.bclk_change_on_active_to_idle = false;   /* CKE=0 (provisional) */
    /* IGNROV/IGNTUR left config_t: the HAL keeps both set as policy. This demo passed
     * true for both ("loopback: non-critical"), so the register outcome is unchanged. */

    if (!nora_spi_i2s_tdm_inst_configure(spi1, &cfg)) {
        printf("TDM master loopback: configure failed (err=%d)\n",
               (int)nora_spi_i2s_tdm_get_last_error());
        return;
    }
    (void)nora_spi_i2s_tdm_set_block_callback(spi1, demo_block_cb, 0);

    /* No role argument since phase 2: open() derives MASTER from the cfg committed just
     * above. The literal that used to be passed here was a second copy of cfg.clock_role,
     * and two copies of one decision is what the phase-2 change removes. */
    if (!nora_spi_i2s_tdm_open()) {
        printf("TDM master loopback: open failed (err=%d)\n",
               (int)nora_spi_i2s_tdm_get_last_error());
        return;
    }
    if (!nora_spi_i2s_tdm_inst_start(spi1)) {
        printf("TDM master loopback: start failed (err=%d)\n",
               (int)nora_spi_i2s_tdm_get_last_error());
        return;
    }

    /* Report the FS shape from cfg, not as a literal. This line used to say
     * "FS_50PCT via CLC1" unconditionally, so it kept claiming CLC was in the path
     * after fs_shape had been changed to FS_PULSE -- a banner that contradicts the
     * running configuration is worse than no banner.
     *
     * These are EXPECTED values derived from cfg, not a register readback. A readback of
     * SPI1CON1H's FRMSYPW/FRMCNT was tried and removed: this file is board- and
     * instance-agnostic (it works through the spi1 handle) and must not name a chip SFR,
     * and the HAL exposes no accessor for those fields. So the line states what the scope
     * SHOULD show; a disagreement between it and the trace is a real finding, not noise. */
    /* Slot count from cfg, not the literal "TDM8": the geometry is a -D
     * (NORA_TDM_SLOTS_PER_FS), so a hard-coded 8 misreports every other build. */
    printf("TDM master loopback: SPI1 running (TDM%u/%u-bit, FS=%s, CLC %s)\n",
           (unsigned)cfg.slots_per_fs,
           (unsigned)cfg.word_bits,
           (cfg.fs_shape == NORA_SPI_I2S_TDM_FS_50PCT) ? "50PCT" : "PULSE",
           (cfg.fs_shape == NORA_SPI_I2S_TDM_FS_50PCT) ? "engaged" : "bypassed");
    {
        /* Expected on a scope, from cfg alone: FS pulse width is FRMSYPW-selected
         * (PULSE -> 1 BCLK), and the FS period is slots * word_bits BCLKs. */
        const unsigned bclk_per_frame = (unsigned)cfg.slots_per_fs * (unsigned)cfg.word_bits;
        /* State the stimulus, not just the framing: a trace is worthless if the pattern
         * that produced it has to be recalled rather than read. */
#if DEMO_TDM_TX_MODE == DEMO_TDM_TX_MODE_FIXED
        printf("TDM master loopback: TX 0x%08lX in slots 1-2, %u slots silent\n",
               (unsigned long)DEMO_TDM_TX_PATTERN,
               (unsigned)(cfg.slots_per_fs - 2u));
#elif DEMO_TDM_TX_MODE == DEMO_TDM_TX_MODE_RAMP
        printf("TDM master loopback: TX count-up +0x%08lX per frame in slots 1-2, "
               "%u slots silent\n",
               (unsigned long)DEMO_TDM_RAMP_STEP,
               (unsigned)(cfg.slots_per_fs - 2u));
#else
        /* fs from cfg + BRG, so the tone frequency printed is the one actually running
         * rather than the 3051.8 Hz the header quotes for the default BRG. */
        {
            const uint32_t bclk_hz = nora_clock_get_fcy_hz() / (2UL * ((uint32_t)cfg.brg + 1UL));
            const uint32_t fs_hz   = bclk_hz / ((uint32_t)cfg.slots_per_fs * (uint32_t)cfg.word_bits);

            printf("TDM master loopback: TX sine -6dBFS, 16 steps/cycle => %lu Hz "
                   "(fs %lu Hz) in slots 1-2, %u slots silent\n",
                   (unsigned long)(fs_hz / 16UL),
                   (unsigned long)fs_hz,
                   (unsigned)(cfg.slots_per_fs - 2u));
        }
#endif
        printf("TDM master loopback: expect FS width %s, FS period %u BCLK\n",
               (cfg.fs_shape == NORA_SPI_I2S_TDM_FS_50PCT)
                   /* single '%': this is a %s ARGUMENT, not the format string */
                   ? "50% of frame (CLC1 J-K toggle)" : "1 BCLK (FRMSYPW=0)",
               bclk_per_frame);
    }
    /* Which wires to put a scope on. From the board, because the pins are the board's --
     * this line used to be four RP numbers hard-coded in this file. */
    printf("TDM master loopback: %s\n",
           (port->wiring != 0) ? port->wiring : "(board did not state its wiring)");
}

/*
 * How often the status line goes out. A TIME, not a call count -- which is the whole
 * point of the change recorded below.
 */
#define DEMO_TDM_STATUS_PERIOD_MS   5000u

void demo_tdm_master_loopback_poll(void)
{
    static uint32_t last_ms;
    static bool     printed_once;
    nora_spi_i2s_tdm_status_t st;

    /*
     * Main loop is tight on one board and paced on the other; print on a WALL-CLOCK
     * period so the UART is not flooded either way.
     *
     * WHAT THIS REPLACED, and why a call count could not work here: `if ((throttle++ &
     * 0xFFFFu) != 0u) return;` -- one line every 65536 calls. On DM330030, whose loop
     * runs free, that is a line every few seconds; on EV88G73A, where profile_poll() is
     * reached once per 500 ms LED edge, the same constant is a line every NINE HOURS.
     * Same code, same demo, two cadences three orders of magnitude apart, and neither
     * was stated anywhere. A count of loop iterations is not a measure of time.
     *
     * THE FIRST CALL STILL PRINTS IMMEDIATELY. It did before (throttle starts at 0),
     * and it is the line that says whether the transport came up at all -- so it is a
     * property to keep, not an accident. It also makes this correct if the tick is not
     * running: GetTicks() stands still at 0, the periodic line never comes, and the
     * one line that matters has already gone out.
     */
    if (!printed_once) {
        printed_once = true;
        last_ms      = GetTicks();
    } else if ((uint32_t)(GetTicks() - last_ms) < DEMO_TDM_STATUS_PERIOD_MS) {
        return;
    } else {
        last_ms = GetTicks();
    }

    if (!nora_spi_i2s_tdm_get_status(&st, true)) {
        return;
    }

    printf("TDM: run=%d blocks=%lu miss=%lu load(us10) last=%lu max=%lu\n",
           (int)st.running,
           (unsigned long)st.block_count,
           (unsigned long)st.block_deadline_miss_count,
           (unsigned long)st.load.last_us10,
           (unsigned long)st.load.max_us10);
}

#endif /* DEMO_ENABLE_TDM_MASTER_LOOPBACK */
