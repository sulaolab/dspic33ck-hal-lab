/*
 * ev08p02a_pot.c -- see ev08p02a_pot.h for the socket-to-channel map and why this is
 * a file of its own.
 */

#ifndef DSPIC33CK_BOARD_EV08P02A
#error "boards/ev08p02a/ev08p02a_pot.c is EV08P02A-owned. Build it only in the CK256MC005_EV08P02A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "ev08p02a_pot.h"

#include <stddef.h>

#include "console_out.h"
#include "ev08p02a_pins.h"
#include "nora_adc.h"
#include "timer_app.h"

/*
 * The socket -> pin -> AN number map, the socket default and the wiring string are all
 * in ev08p02a_pins.h, which is also where the pin ev08p02a_board.c configures comes
 * from. Deliberately not restated here: a second copy of the channel number is exactly
 * the drift that turns a socket change into a silently wrong reading.
 */

/*
 * The internal band gap, AN21 on this part (DS70005633B Figure 13-2 draws it on the
 * shared core's multiplexer as "Band Gap 1.2V (AN21)"; Figure 13-1 note 1 says it is
 * not available on any device pin). AN20 next to it is the temperature-sensor diode
 * and is deliberately not read here: it needs a two-point calibration to mean
 * anything, and this module is a pot reader.
 */
#define EV08P02A_ADC_BANDGAP_CHANNEL (21u)
#define EV08P02A_ADC_BANDGAP_MV      (1200ul)

/*
 * Shared-core sample duration in TADCORE cycles, the same 0x30 DM330030's pot uses and
 * for the same reason: what sets the minimum is the source impedance of a hand-turned
 * wiper, and nothing here is rate-critical. The POT Click buffers its wiper through an
 * OPA344 (schematic v101), so this is generous rather than necessary -- and generous is
 * the right side to be on when the alternative is a reading that lags the knob.
 */
#define EV08P02A_POT_SAMPLE_TIME_TAD (0x30u)

/*
 * HARDWARE AVERAGING -- filter 0, 32x, averaging mode.
 *
 * WHY, with the measurement: at one conversion per reading this input showed 165 counts of
 * peak-to-peak jitter over its quietest ten-sample window (*av1 stream, 2026-09-14), and
 * ev08p02a_avas_pot.c's deadband is 24 counts against an EMA that only divides the noise
 * by four. So the pitch was being rewritten with the knob standing still. Averaging in
 * hardware attacks that at the sampling end; widening the deadband to swallow 75 counts of
 * residual was rejected on purpose, because 75 counts is 3 % of this pot's travel and that
 * is resolution thrown away to hide a noise problem.
 *
 * AVERAGING MODE, NOT OVERSAMPLING: the result stays a plain 12-bit count, so
 * EV08P02A_AVAS_POT_SPAN_COUNTS, the deadband and counts_to_cent() all keep meaning
 * exactly what they meant. Oversampling mode would hand back 12.4 and quietly change the
 * units of every one of them.
 *
 * FILTER 0 IS A CHOICE THIS BOARD MAKES, not something the HAL handed out -- there are
 * four and nothing else on this board asks for one yet. If a second consumer appears,
 * nora_adc_filter_configure() answers BUSY rather than letting the two share a result.
 *
 * 32x costs about 110 us per reading at the sample time below, against a 100 ms sampling
 * period -- so the ratio was picked for noise, not for time.
 */
#define EV08P02A_POT_FILTER_INDEX      (0u)
#define EV08P02A_POT_FILTER_RATIO_LOG2 (5u)   /* 2^5 = 32x */

/*
 * TWO TIMEOUTS, NOT ONE -- READY once at init (has the shared core powered up?) and
 * READ per conversion (has this one finished?). DM330030's file explains the split;
 * both are poll counts and both are insurance against a peripheral that never answers,
 * not tuned deadlines.
 */
#define EV08P02A_POT_READY_TIMEOUT_COUNT (1000000UL)
#define EV08P02A_POT_READ_TIMEOUT_COUNT  (1000000UL)

/*
 * Full scale. The HAL leaves ADCON3L.REFSEL at 000 = AVDD/AVSS, so full scale IS the
 * target supply -- 3.3 V as the Nano's debugger sets it by default, and adjustable
 * 3.0-3.6 V (DS70005656A section 6.1), which is why this is called nominal and why
 * millivolts are reported beside the raw counts rather than instead of them. Raw counts
 * are the bring-up evidence; millivolts are a convenience.
 */
/* EV08P02A_ADC_FULL_SCALE is ev08p02a_pot.h's -- shared with ev08p02a_avas_pot.c. */
#define EV08P02A_ADC_AVDD_MV     (3300ul)

/*
 * +/-10% around the expected band-gap reading. Wide on purpose: its job is to separate
 * "the converter converts" from "it does not", not to grade accuracy. It has to swallow
 * the band gap's own tolerance, an AVDD that is 3.3 V only nominally, and an
 * uncalibrated converter.
 */
#define EV08P02A_ADC_BANDGAP_EXPECT_COUNTS \
    ((uint16_t)((EV08P02A_ADC_BANDGAP_MV * EV08P02A_ADC_FULL_SCALE) / EV08P02A_ADC_AVDD_MV))
#define EV08P02A_ADC_BANDGAP_LO_COUNTS \
    ((uint16_t)(((uint32_t)EV08P02A_ADC_BANDGAP_EXPECT_COUNTS * 90ul) / 100ul))
#define EV08P02A_ADC_BANDGAP_HI_COUNTS \
    ((uint16_t)(((uint32_t)EV08P02A_ADC_BANDGAP_EXPECT_COUNTS * 110ul) / 100ul))

/*
 * 200 ms: fast enough that a knob being turned reads as continuous, slow enough that
 * one short line at 230400 baud is nowhere near filling the link.
 *
 * A FLOOR, NOT THE RATE. ev08p02a_pot_poll() is called from profile_poll(), which runs
 * every 500 ms, so this gate never actually wins and the stream comes out at about that
 * -- measured 126 lines in 60 s on 2026-09-14. It is the PITCH path that was moved to
 * wait_next_led_edge()'s 100 ms cadence (see main.c); the stream is diagnostics and was
 * left where it was. Stated here because the number above otherwise reads as a promise:
 * anything timing a knob movement from this stream is timing 500 ms samples.
 */
#define EV08P02A_POT_STREAM_PERIOD_MS (200u)

/*
 * TWO HANDLES OVER ONE PERIPHERAL, and it is not redundancy.
 *
 * The CK HAL binds a handle to exactly ONE channel: nora_adc_trigger() refuses any
 * channel other than the one nora_adc_init() was given and answers UNSUPPORTED
 * (hal_adc/nora_adc_dspic33ck.c, adc_handle_channel_is_valid()). Measured the hard way
 * -- *ab reported "FAIL: UNSUPPORTED" on the first run against real silicon while the
 * pot itself was reading correctly, which is the HAL saying no, not the band gap being
 * absent.
 *
 * So the band gap gets a handle of its own. Both describe the SAME peripheral with
 * identical settings, and nora_adc_init() writes nothing channel-specific -- the channel
 * reaches ADCON3L.CNVCHSEL at trigger time, not at init -- so initialising twice in a
 * row leaves exactly the register state one init would, and both handles are then valid
 * for their own channel. The second init is a peripheral restart, which is why both
 * happen once at bring-up and never per read.
 */
static nora_adc_handle_t s_pot_adc;
static nora_adc_handle_t s_bandgap_adc;
static bool              s_pot_adc_ready;
static bool              s_bandgap_adc_ready;
static bool              s_pot_filter_ready;
static const char       *s_pot_filter_fault;
static uint16_t          s_pot_last_value;
static const char       *s_pot_fault;
static bool              s_pot_stream;
static uint32_t          s_pot_stream_last_ms;
static uint16_t          s_pot_min = 0xFFFFu;
static uint16_t          s_pot_max;
static bool              s_pot_seen;
/*
 * A SECOND MIN/MAX PAIR, over the UNFILTERED reading -- and it is not bookkeeping for its
 * own sake. Printed beside the filtered pair it is the improvement measurement itself: the
 * two spans over the same interval, same knob, same wiring, differing only by the filter.
 * Without it the only way to judge the averaging is to post-process the log, and a change
 * whose effect cannot be read off the console is a change nobody will re-check.
 */
static uint16_t          s_pot_raw_min = 0xFFFFu;
static uint16_t          s_pot_raw_max;
static bool              s_pot_raw_seen;

static uint32_t pot_counts_to_mv(uint16_t counts)
{
    /* Single-ended against AVDD: 0 counts is 0 V and full scale is AVDD. Rounded to the
     * nearest millivolt, and 4095 * 3300 has room to spare in 32 bits. */
    return (((uint32_t)counts * EV08P02A_ADC_AVDD_MV) + (EV08P02A_ADC_FULL_SCALE / 2u)) /
           EV08P02A_ADC_FULL_SCALE;
}

/*
 * One conversion. Returns false on failure and leaves *out untouched, rather than
 * handing back some other channel's last value -- the first version returned the cached
 * POT reading when the band-gap conversion was refused, and the console then printed a
 * pot voltage under the heading "bandgap", which is a lie a diagnostic cannot afford.
 */
static bool pot_convert(nora_adc_handle_t *handle, uint8_t channel, uint16_t *out)
{
    uint32_t          raw_value = 0u;
    nora_adc_result_t result;

    result = nora_adc_read_blocking(handle,
                                    channel,
                                    &raw_value,
                                    EV08P02A_POT_READ_TIMEOUT_COUNT);
    if (result != NORA_ADC_RESULT_OK) {
        s_pot_fault = nora_adc_result_str(result);
        return false;
    }

    s_pot_fault = NULL;
    *out = (uint16_t)raw_value;
    return true;
}

/*
 * One HARDWARE-AVERAGED reading of the pot, or one plain conversion if the filter never
 * came up. The fallback is deliberate and it is loud: *ai / ?ai say the filter is not
 * running, because a pot that silently went back to single conversions would look like the
 * averaging simply did not help.
 */
static bool pot_convert_filtered(uint16_t *out)
{
    uint32_t          value = 0u;
    nora_adc_result_t result;

    if (!s_pot_filter_ready) {
        return pot_convert(&s_pot_adc, (uint8_t)EV08P02A_POT_AN_CHANNEL, out);
    }

    result = nora_adc_filter_read_blocking(&s_pot_adc, &value, EV08P02A_POT_READ_TIMEOUT_COUNT);
    if (result != NORA_ADC_RESULT_OK) {
        s_pot_fault = nora_adc_result_str(result);
        return false;
    }

    s_pot_fault = NULL;
    *out = (uint16_t)value;
    return true;
}

bool ev08p02a_pot_init(void)
{
    const nora_adc_config_t adc_config = {
        .instance = NORA_ADC_INSTANCE_1,
        /* channel == positive_input is required by the CK HAL for a single-ended input;
         * the field pair exists to keep the AK/sonora configuration shape. */
        .channel = EV08P02A_POT_AN_CHANNEL,
        .positive_input = EV08P02A_POT_AN_CHANNEL,
        .sample_time_tad = EV08P02A_POT_SAMPLE_TIME_TAD,
        .calibrate = false,
        .ready_timeout_count = EV08P02A_POT_READY_TIMEOUT_COUNT,
        .calibration_timeout_count = 0u,
    };

    nora_adc_config_t bandgap_config = adc_config;

    bandgap_config.channel        = EV08P02A_ADC_BANDGAP_CHANNEL;
    bandgap_config.positive_input = EV08P02A_ADC_BANDGAP_CHANNEL;

    s_pot_last_value = 0u;
    s_pot_fault      = NULL;
    s_pot_adc_ready  = nora_adc_init(&s_pot_adc, &adc_config);

    if (!s_pot_adc_ready) {
        /* The handle records why, even though init only returns a bare false. */
        s_pot_fault = nora_adc_result_str(nora_adc_get_last_result(&s_pot_adc));
    }

    /* The band gap's own handle -- see the note on the two handles above. Its failure
     * does NOT fail this function: the pot is what this module is for, and *ab reports
     * its own state. */
    s_bandgap_adc_ready = nora_adc_init(&s_bandgap_adc, &bandgap_config);

    /*
     * The filter LAST, after both inits. nora_adc_init() writes ADCON1L = 0 on the way in,
     * so configuring the filter before the second init would leave it pointed at a
     * peripheral that was then restarted underneath it.
     */
    s_pot_filter_ready = false;
    s_pot_filter_fault = NULL;
    if (s_pot_adc_ready) {
        const nora_adc_filter_config_t filter_config = {
            .filter     = EV08P02A_POT_FILTER_INDEX,
            .ratio_log2 = EV08P02A_POT_FILTER_RATIO_LOG2,
            .mode       = NORA_ADC_FILTER_MODE_AVERAGING,
        };
        const nora_adc_result_t filter_result =
            nora_adc_filter_configure(&s_pot_adc, &filter_config);

        s_pot_filter_ready = (filter_result == NORA_ADC_RESULT_OK);
        if (!s_pot_filter_ready) {
            s_pot_filter_fault = nora_adc_result_str(filter_result);
        }
    }

    return s_pot_adc_ready;
}

uint16_t ev08p02a_pot_read(void)
{
    uint16_t value;

    if (!s_pot_adc_ready) {
        return s_pot_last_value;
    }

    if (!pot_convert_filtered(&value)) {
        return s_pot_last_value;
    }

    s_pot_last_value = value;
    if (!s_pot_seen || (value < s_pot_min)) {
        s_pot_min = value;
    }
    if (!s_pot_seen || (value > s_pot_max)) {
        s_pot_max = value;
    }
    s_pot_seen = true;
    return value;
}

/*
 * One UNFILTERED conversion, for the report line only -- never for the pitch.
 *
 * It runs through the same handle the filter is attached to, and that is not a mistake: the
 * filter accumulates every conversion of its channel no matter who triggered it, so this
 * reading also becomes one of the 32 the next filtered read averages. The filtered result
 * is still a 32-sample average -- the loop in nora_adc_filter_read_blocking() simply
 * contributes one conversion fewer -- so the two numbers on a line are the same wiring at
 * the same moment, which is exactly what makes them comparable.
 */
static bool pot_read_unfiltered(uint16_t *out)
{
    if (!s_pot_adc_ready ||
        !pot_convert(&s_pot_adc, (uint8_t)EV08P02A_POT_AN_CHANNEL, out)) {
        return false;
    }

    if (!s_pot_raw_seen || (*out < s_pot_raw_min)) {
        s_pot_raw_min = *out;
    }
    if (!s_pot_raw_seen || (*out > s_pot_raw_max)) {
        s_pot_raw_max = *out;
    }
    s_pot_raw_seen = true;
    return true;
}

bool ev08p02a_pot_read_bandgap(uint16_t *counts)
{
    /* Not through ev08p02a_pot_read(): the band gap is not the pot, so it must not move
     * the min/max the knob's travel is being judged by. */
    if ((counts == NULL) || !s_bandgap_adc_ready) {
        s_pot_fault = nora_adc_result_str(nora_adc_get_last_result(&s_bandgap_adc));
        return false;
    }

    return pot_convert(&s_bandgap_adc, (uint8_t)EV08P02A_ADC_BANDGAP_CHANNEL, counts);
}

const char *ev08p02a_pot_fault(void)
{
    return s_pot_fault;
}

/*
 * The reading, as one line -- RAW AND AVERAGED SIDE BY SIDE, with a min/max span for each.
 *
 * Both columns exist so the hardware averaging can be judged from the console instead of
 * from a log post-processed offline: over a stretch with the knob standing still, the two
 * spans are the before and after of the same noise. Raw comes first because it is the
 * unprocessed evidence; millivolts are computed from the averaged value and follow, because
 * AVDD is nominal (see EV08P02A_ADC_AVDD_MV) and mV is a convenience either way.
 *
 * The averaged column's span is also the one to calibrate
 * EV08P02A_AVAS_POT_SPAN_COUNTS against: a max taken from a noisy single conversion is a
 * noise peak, not the end of the knob's travel.
 */
static void pot_reading_line(void)
{
    uint16_t unfiltered = 0u;
    bool     have_unfiltered = pot_read_unfiltered(&unfiltered);
    uint16_t averaged = ev08p02a_pot_read();

    console_out_str("[adc] raw ");
    if (have_unfiltered) {
        console_out_u32(unfiltered);
    } else {
        console_out_str("-");
    }
    console_out_str("  avg ");
    console_out_u32(averaged);
    console_out_str("  ");
    console_out_u32(pot_counts_to_mv(averaged));
    console_out_str(" mV  raw[min ");
    console_out_u32(s_pot_raw_seen ? s_pot_raw_min : 0u);
    console_out_str(" max ");
    console_out_u32(s_pot_raw_max);
    console_out_str("]  avg[min ");
    console_out_u32(s_pot_seen ? s_pot_min : 0u);
    console_out_str(" max ");
    console_out_u32(s_pot_max);
    console_out_str("]");
    if (!s_pot_filter_ready) {
        /* Said on every line, not once at init: this is the case where the numbers in the
         * two columns mean the same thing and the "avg" heading would otherwise lie. */
        console_out_str("  NO FILTER");
        if (s_pot_filter_fault != NULL) {
            console_out_str(" ");
            console_out_str(s_pot_filter_fault);
        }
    }
    if (s_pot_fault != NULL) {
        console_out_str("  FAULT ");
        console_out_str(s_pot_fault);
    }
    console_out_str("\n");
}

void ev08p02a_pot_report(void)
{
    console_out_str("POT Click ADC: " EV08P02A_POT_WIRING_STR
                    ", shared core, 12-bit, REFSEL=AVDD (");
    console_out_u32(EV08P02A_ADC_AVDD_MV);
    console_out_str(" mV nominal full scale)\n");

    console_out_str("  averaging: ");
    if (s_pot_filter_ready) {
        console_out_str("filter ");
        console_out_u32(EV08P02A_POT_FILTER_INDEX);
        console_out_str(", hardware ");
        console_out_u32(1uL << EV08P02A_POT_FILTER_RATIO_LOG2);
        console_out_str("x average, 12-bit result (same units as raw)\n");
    } else {
        console_out_str("OFF -- single conversions");
        if (s_pot_filter_fault != NULL) {
            console_out_str(", filter refused: ");
            console_out_str(s_pot_filter_fault);
        }
        console_out_str("\n");
    }

    if (!s_pot_adc_ready) {
        console_out_str("  ADC NOT READY");
        if (s_pot_fault != NULL) {
            console_out_str(" -- ");
            console_out_str(s_pot_fault);
        }
        console_out_str("\n");
        return;
    }

    pot_reading_line();
}

/*
 * *ab -- the band gap. PASS here means the converter converts, and it means that
 * WITHOUT a pin, a Click board or a single wire being involved, so a failing pot read
 * beside a passing *ab points at the wiring rather than at the ADC.
 */
static void pot_report_bandgap(void)
{
    uint16_t counts = 0u;

    console_out_str("adc bandgap: internal 1.2 V (AN21, not on any pin)\n");

    if (!ev08p02a_pot_read_bandgap(&counts)) {
        console_out_str("  -> FAIL: no conversion -- ");
        console_out_str((s_pot_fault != NULL) ? s_pot_fault : "reason not recorded");
        console_out_str("\n     No number is printed on purpose: the last POT reading is\n"
                        "     not a band-gap measurement and must not be shown as one.\n");
        return;
    }

    console_out_str("  counts : ");
    console_out_u32(counts);
    console_out_str("\n  volts  : ");
    console_out_u32(pot_counts_to_mv(counts));
    console_out_str(" mV (expected about ");
    console_out_u32(EV08P02A_ADC_BANDGAP_MV);
    console_out_str(" mV = ");
    console_out_u32(EV08P02A_ADC_BANDGAP_EXPECT_COUNTS);
    console_out_str(" counts)\n");

    if ((counts >= EV08P02A_ADC_BANDGAP_LO_COUNTS) &&
        (counts <= EV08P02A_ADC_BANDGAP_HI_COUNTS)) {
        console_out_str("  -> PASS: the ADC converts. No pin, no Click board and no\n"
                        "     wiring were involved in this result.\n");
    } else {
        console_out_str("  -> FAIL: outside the +/-10% sanity band ");
        console_out_u32(EV08P02A_ADC_BANDGAP_LO_COUNTS);
        console_out_str("..");
        console_out_u32(EV08P02A_ADC_BANDGAP_HI_COUNTS);
        console_out_str(".\n     A stable number here is not proof of anything on its own.\n");
    }
}

void ev08p02a_pot_console_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

    if (msg->kind == (uint8_t)'?') {
        switch (msg->name) {
        case 'i':
            ev08p02a_pot_report();
            msg->status = APP_CONSOLE_OK;
            return;

        case 'v':
            pot_reading_line();
            msg->status = APP_CONSOLE_OK;
            return;

        default:
            msg->status = APP_CONSOLE_ERR_NOT_FOUND;
            return;
        }
    }

    if (msg->kind != (uint8_t)'*') {
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        return;
    }

    switch (msg->name) {
    case 'v':
        if (msg->data_len < 1u) {
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            return;
        }
        if (msg->data[0] == 0u) {
            s_pot_stream = false;
            console_out_str("adc: stream stopped\n");
        } else {
            s_pot_stream = true;
            /* First line immediately, then on the period: a command that answers
             * nothing for 200 ms reads as a command that did nothing. */
            s_pot_stream_last_ms = GetTicks();
            console_out_str("adc: stream running, one line every ");
            console_out_u32(EV08P02A_POT_STREAM_PERIOD_MS);
            console_out_str(" ms (*av00 to stop)\n");
            pot_reading_line();
        }
        msg->status = APP_CONSOLE_OK;
        return;

    case 'b':
        pot_report_bandgap();
        msg->status = APP_CONSOLE_OK;
        return;

    case 's':
        s_pot_min      = 0xFFFFu;
        s_pot_max      = 0u;
        s_pot_seen     = false;
        /* Both spans, or the next comparison would put a freshly-started averaged span
         * beside a raw one that has been accumulating since the last reset. */
        s_pot_raw_min  = 0xFFFFu;
        s_pot_raw_max  = 0u;
        s_pot_raw_seen = false;
        console_out_str("adc: min/max cleared (raw and avg)\n");
        msg->status = APP_CONSOLE_OK;
        return;

    default:
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        return;
    }
}

void ev08p02a_pot_poll(bool report)
{
    /* Its own period, not the shared status-line throttle: what makes a turning knob
     * legible is a steady 200 ms, and the status line is seconds apart. */
    (void)report;

    if (!s_pot_stream || !s_pot_adc_ready) {
        return;
    }

    if ((uint32_t)(GetTicks() - s_pot_stream_last_ms) < EV08P02A_POT_STREAM_PERIOD_MS) {
        return;
    }
    s_pot_stream_last_ms = GetTicks();
    pot_reading_line();
}
