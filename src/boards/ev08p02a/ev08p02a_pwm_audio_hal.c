/* hal_pwm_audio board backend: EV08P02A socket 3 MPQ6612A (CK256MC005).
 *
 * PG4 H/L -> IN1/IN2 through PPS. ECPLL uses 196.608 MHz / 4096;
 * jumper-free FRCPLL uses 200 MHz / 4166 and an elastic duty FIFO.
 * PG4 TRIGA (mid-period) -> PWM Event A -> DMA2 streams 16-bit PG4DC; each DMA
 * half is one 32-frame block from the core FIFO, or neutral duty.
 *
 * The board-side glue (WM8904 output hooks, main-loop poll, report) uses the
 * ev08p02a_pwm_audio_* names. This is the only PWM path: the first-light
 * ev08p02a_pwm_audio.c was removed on 2026-10-15 (see git history).
 */
#include "app_config.h"
#if !defined(DSPIC33CK_BOARD_EV08P02A)
#error "EV08P02A PWM audio is board-owned"
#endif
#include "nora_pwm_audio_backend.h"
#if NORA_PWM_AUDIO_ENABLE

#include <stdint.h>
#include <xc.h>

#include "ev08p02a_pwm_audio.h"
#include "ev08p02a_board.h"
#include "nora_clock.h"
#include "nora_dma.h"
#include "nora_gpio.h"
#include "nora_pps.h"
#include "nora_pwm.h"
#include "nora_spi_i2s_tdm_conf.h"
#include "timer_app.h"
#include "gain_db.h"
#include "console_out.h"
#include "nora_high_res_timer.h"

#if NORA_TDM_BLOCK_FRAMES != 32 || NORA_TDM_SLOTS_PER_FS != 8
#error "The PWM handoff requires TDM8 blocks of 32 frames"
#endif

#if EV08P02A_PWM_CLOCK_FRC
/* Even periods keep the shared modulator's centre exactly at 50%. */
#define PWM_PERIOD (((200000000UL / (48000UL * EV08P02A_PWM_CARRIER_FACTOR)) + 1u) & ~1u)
#else
#define PWM_PERIOD (4096u / EV08P02A_PWM_CARRIER_FACTOR)
#endif
#define PWM_NEUTRAL   (PWM_PERIOD / 2u)
#if EV08P02A_PWM_SCOPE_ONLY
#define PWM_MIN_DUTY ((PWM_PERIOD + 512u) / 1024u)
#else
#define PWM_MIN_DUTY  100u  /* >= 500 ns at either PWM clock */
#endif
/* The core modulates into fractional duty (PWM_FRAC_BITS below one count) so
 * the quantizer here can carry each period's rounding error into the next
 * carrier period (error feedback). Q values stay <= 16672 at every carrier. */
#if EV08P02A_PWM_CARRIER_FACTOR == 4
#define PWM_FRAC_BITS 4u
#elif EV08P02A_PWM_CARRIER_FACTOR == 2
#define PWM_FRAC_BITS 3u
#else
#define PWM_FRAC_BITS 2u
#endif
#define PWM_FRAC_ONE  ((int16_t)(1 << PWM_FRAC_BITS))
#define PWM_HALF      32u
#define PWM_DMA_HALF (PWM_HALF * EV08P02A_PWM_CARRIER_FACTOR)
#define PWM_DMA_WORDS (2u * PWM_DMA_HALF)

#if EV08P02A_PWM_AUDIO_HAL_FRC
#define PWM_ELASTIC_WORDS       256u
#define PWM_ELASTIC_PRIME_WORDS 128u
#define PWM_ELASTIC_TARGET      112u
#define PWM_ELASTIC_DELTA_LIMIT 655 /* +/-1% of one input sample per PWM cycle */
#endif

static volatile uint16_t s_dma_words[PWM_DMA_WORDS];
static int32_t s_pcm_q31[NORA_PWM_AUDIO_FRAMES_PER_BLOCK];
static bool s_hw_up;
static volatile bool s_released;
static bool s_lab_mode;
static volatile bool s_neutral;
static volatile gain_db_t s_pwm_gain;
static volatile uint8_t s_ns_order = EV08P02A_PWM_NS_ORDER;
static uint8_t s_ns_active; /* DMA2 ISR only, with the two error terms */
static int16_t s_ns_e1, s_ns_e2;
/* Bench isolation: 0 normal, 1 internal tone after every FIFO stage,
 * 2 elastic pass-through (no interpolation or rate servo; drifts and reprimes),
 * 3 duty 20%/80% alternating every carrier period, 4 the same every 16 periods.
 * 3/4 overwrite the DMA words after the normal refill, so the FIFO keeps
 * running; a scope then shows whether every period takes its own DMA word. */
static volatile uint8_t s_test_mode;
/* DMA2 ISR cost, high-res timer counts, max since the last report. */
static volatile uint32_t s_isr_max_count;
static volatile uint16_t s_duty_min = PWM_NEUTRAL;
static volatile uint16_t s_duty_max = PWM_NEUTRAL;
static volatile uint32_t s_dma_halves;
static volatile uint32_t s_dma_errors;
#if EV08P02A_PWM_AUDIO_HAL_FRC
/* Only DMA2's ISR accesses this ring. The HAL core still owns PCM->duty,
 * write() acceptance, and its own block queue; this ring changes the average
 * duty-stream consumption rate without changing the shared HAL. */
static uint16_t s_elastic_fifo[PWM_ELASTIC_WORDS];
static uint16_t s_elastic_head;
static uint16_t s_elastic_tail;
static uint16_t s_elastic_phase_q16;
static int32_t s_elastic_level_filtered_q8;
static bool s_elastic_primed;
static volatile int16_t s_elastic_delta_q16;
static volatile uint16_t s_elastic_level;
static volatile uint16_t s_elastic_min;
static volatile uint16_t s_elastic_max;
static volatile uint16_t s_elastic_prefill_halves;
static volatile uint16_t s_elastic_underruns;
static volatile uint16_t s_elastic_stretched_halves;
static volatile uint16_t s_elastic_compressed_halves;
#endif

static void fill_neutral(uint16_t from, uint16_t count)
{
    uint16_t i;
    for (i = 0u; i < count; ++i) s_dma_words[from + i] = PWM_NEUTRAL;
}

/* EMERGENCY_SAFE: bridge off first, then both inputs low. No waits. */
static void board_emergency_safe(void)
{
    s_released = false;
    (void)nora_gpio_clear(EV08P02A_PWM_SLEEP_PIN);
    (void)nora_pwm_generator_override(NORA_PWM_GEN_4, true, false, false);
}

static bool stage_fault(void)
{
    return !EV08P02A_PWM_SCOPE_ONLY && !s_lab_mode &&
        nora_gpio_read(EV08P02A_PWM_FAULT_PIN) != NORA_GPIO_LEVEL_HIGH;
}

/* Fractional duty -> one count per carrier period. Order 0 rounds; order 1/2
 * subtract the shaped past error, NTF (1 - z^-1)^order at the carrier rate,
 * so quantization noise moves above the audio band. Each error term is
 * clamped to one count: only an end-limit clamp can exceed half a count. */
static uint16_t ns_quantize(int16_t target_q)
{
    int16_t u = target_q;
    int16_t q, e;
    if (s_ns_active == 1u) u = (int16_t)(u - s_ns_e1);
    else if (s_ns_active == 2u) u = (int16_t)(u - 2 * s_ns_e1 + s_ns_e2);
    q = (int16_t)((u + PWM_FRAC_ONE / 2) >> PWM_FRAC_BITS);
    if (q < (int16_t)PWM_MIN_DUTY) q = (int16_t)PWM_MIN_DUTY;
    if (q > (int16_t)(PWM_PERIOD - PWM_MIN_DUTY)) q = (int16_t)(PWM_PERIOD - PWM_MIN_DUTY);
    e = (int16_t)((q << PWM_FRAC_BITS) - u);
    if (e > PWM_FRAC_ONE) e = PWM_FRAC_ONE;
    if (e < -PWM_FRAC_ONE) e = (int16_t)-PWM_FRAC_ONE;
    s_ns_e2 = s_ns_e1;
    s_ns_e1 = e;
    return (uint16_t)q;
}

static void store_duty(uint16_t offset, uint16_t frame, uint16_t duty_q)
{
    uint16_t repeat;
    if (s_ns_active == 0u) {
        volatile uint16_t *dst = &s_dma_words[offset + frame * EV08P02A_PWM_CARRIER_FACTOR];
        uint16_t duty = (uint16_t)(((uint16_t)duty_q + (uint16_t)(PWM_FRAC_ONE / 2)) >> PWM_FRAC_BITS);
        if (duty < PWM_MIN_DUTY) duty = PWM_MIN_DUTY;
        if (duty > PWM_PERIOD - PWM_MIN_DUTY) duty = PWM_PERIOD - PWM_MIN_DUTY;
        for (repeat = 0u; repeat < EV08P02A_PWM_CARRIER_FACTOR; ++repeat) dst[repeat] = duty;
        if (duty < s_duty_min) s_duty_min = duty;
        if (duty > s_duty_max) s_duty_max = duty;
        return;
    }
    for (repeat = 0u; repeat < EV08P02A_PWM_CARRIER_FACTOR; ++repeat) {
        uint16_t duty = ns_quantize((int16_t)duty_q);
        s_dma_words[offset + frame * EV08P02A_PWM_CARRIER_FACTOR + repeat] = duty;
        if (duty < s_duty_min) s_duty_min = duty;
        if (duty > s_duty_max) s_duty_max = duty;
    }
}

static void ns_take_order(void)
{
    uint8_t order = s_ns_order;
    if (order != s_ns_active) {
        s_ns_active = order;
        s_ns_e1 = 0;
        s_ns_e2 = 0;
    }
}

#if EV08P02A_PWM_AUDIO_HAL_FRC
/* 1 kHz sine at the *tp0002 level (-12 dBFS of the duty span), generated in
 * the DMA2 ISR so only the shaper, PWM and power stage remain in the path. */
static uint16_t test_tone_q(void)
{
    static const int16_t sine[48] = {
             0,   4277,   8481,  12539,  16383,  19947,  23170,  25996,
         28377,  30273,  31650,  32487,  32767,  32487,  31650,  30273,
         28377,  25996,  23170,  19947,  16383,  12539,   8481,   4277,
             0,  -4277,  -8481, -12539, -16383, -19947, -23170, -25996,
        -28377, -30273, -31650, -32487, -32767, -32487, -31650, -30273,
        -28377, -25996, -23170, -19947, -16384, -12539,  -8481,  -4277
    };
    static uint8_t idx;
    const int32_t amp = (int32_t)(((uint32_t)(PWM_NEUTRAL - PWM_MIN_DUTY) << PWM_FRAC_BITS) / 4u);
    int32_t v = ((int32_t)PWM_NEUTRAL << PWM_FRAC_BITS) + (((int32_t)sine[idx] * amp) >> 15);
    if (++idx >= 48u) idx = 0u;
    return (uint16_t)v;
}

/* A core block is staged only when core_ready() says take() would hand one
 * over, so pre-filling the local elastic FIFO never charges a HAL underrun. */
static bool stage_core_block(void)
{
    const nora_pwm_audio_duty_t *duty[NORA_PWM_AUDIO_MAX_CHANNELS];
    uint16_t head = s_elastic_head;
    uint16_t tail = s_elastic_tail;
    uint16_t i;

    if (((tail - head - 1u) & (PWM_ELASTIC_WORDS - 1u)) < PWM_HALF) return false;
    if (!nora_pwm_audio_core_ready()) return false;
    if (!nora_pwm_audio_core_take(duty)) return false;
    for (i = 0u; i < PWM_HALF; ++i) {
        s_elastic_fifo[head] = duty[0][i];
        head = (uint16_t)((head + 1u) & (PWM_ELASTIC_WORDS - 1u));
    }
    s_elastic_head = head;
    nora_pwm_audio_core_release();
    return true;
}

static void refill_half(uint16_t offset)
{
    uint16_t i, tail, available, consumed = 0u;
    uint16_t staged;
    uint8_t mode;
    uint32_t phase, step;
    int32_t delta;

    /* Drain accepted blocks, bounded by the core queue capacity. With a
     * slightly faster producer, limiting this to one block per output half
     * fills the core queue even while the local FIFO still has room. */
    for (staged = 0u; staged < NORA_PWM_AUDIO_FIFO_BLOCKS - 1u; ++staged) {
        if (!stage_core_block()) break;
    }
    tail = s_elastic_tail;
    available = (uint16_t)((s_elastic_head - tail) & (PWM_ELASTIC_WORDS - 1u));
    if (!s_elastic_primed && available < PWM_ELASTIC_PRIME_WORDS) {
        fill_neutral(offset, PWM_DMA_HALF);
        ++s_elastic_prefill_halves;
        return;
    }
    if (!s_elastic_primed) s_elastic_primed = true;
    /* One extra block can rescue a near-empty FIFO without waiting a half. */
    if (available < 34u && stage_core_block()) {
        available = (uint16_t)((s_elastic_head - tail) & (PWM_ELASTIC_WORDS - 1u));
    }
    if (available < 34u) {
        const nora_pwm_audio_duty_t *unused[NORA_PWM_AUDIO_MAX_CHANNELS];
        /* This is an actual output underrun. If the core has nothing to give
         * either, an empty take records it in the portable status and makes
         * the core re-prime. */
        if (!nora_pwm_audio_core_ready()) (void)nora_pwm_audio_core_take(unused);
        fill_neutral(offset, PWM_DMA_HALF);
        ++s_elastic_underruns;
        s_elastic_primed = false;
        s_elastic_phase_q16 = 0u;
        s_elastic_delta_q16 = 0;
        s_elastic_level_filtered_q8 = (int32_t)PWM_ELASTIC_TARGET * 256;
        return;
    }

    ns_take_order();
    mode = s_test_mode;
    phase = (mode == 2u) ? 0u : s_elastic_phase_q16;
    step = (mode == 2u) ? 65536u :
        (uint32_t)((int32_t)65536 + (int32_t)s_elastic_delta_q16);
    for (i = 0u; i < PWM_HALF; ++i) {
        int32_t a = s_elastic_fifo[tail];
        int32_t b = s_elastic_fifo[(tail + 1u) & (PWM_ELASTIC_WORDS - 1u)];
        uint16_t v = (mode == 2u) ? (uint16_t)a :
            (uint16_t)(a + (((b - a) * (int32_t)phase) >> 16));
        if (mode == 1u) v = test_tone_q();
        store_duty(offset, i, v);
        phase += step;
        consumed = (uint16_t)(consumed + (uint16_t)(phase >> 16));
        tail = (uint16_t)((tail + (uint16_t)(phase >> 16)) & (PWM_ELASTIC_WORDS - 1u));
        phase &= 0xffffu;
    }
    s_elastic_tail = tail;
    s_elastic_phase_q16 = (uint16_t)phase;
    if (consumed < PWM_HALF) ++s_elastic_stretched_halves;
    if (consumed > PWM_HALF) ++s_elastic_compressed_halves;

    available = (uint16_t)((s_elastic_head - tail) & (PWM_ELASTIC_WORDS - 1u));
    s_elastic_level = available;
    if (available < s_elastic_min) s_elastic_min = available;
    if (available > s_elastic_max) s_elastic_max = available;
    s_elastic_level_filtered_q8 +=
        (((int32_t)available * 256) - s_elastic_level_filtered_q8) / 16;
    delta = (s_elastic_level_filtered_q8 - (int32_t)PWM_ELASTIC_TARGET * 256) / 16;
    if (delta < -PWM_ELASTIC_DELTA_LIMIT) delta = -PWM_ELASTIC_DELTA_LIMIT;
    if (delta > PWM_ELASTIC_DELTA_LIMIT) delta = PWM_ELASTIC_DELTA_LIMIT;
    s_elastic_delta_q16 = (int16_t)delta;
}
#else
static void refill_half(uint16_t offset)
{
    const nora_pwm_audio_duty_t *duty[NORA_PWM_AUDIO_MAX_CHANNELS];
    uint16_t i;
    if (!nora_pwm_audio_core_take(duty)) {
        fill_neutral(offset, PWM_DMA_HALF);
        return;
    }
    ns_take_order();
    for (i = 0u; i < PWM_HALF; ++i) store_duty(offset, i, duty[0][i]);
    nora_pwm_audio_core_release();
}
#endif

static void test_pattern(uint16_t offset)
{
    const uint8_t mode = s_test_mode;
    uint16_t i;
    if (mode < 3u) return;
    for (i = 0u; i < PWM_DMA_HALF; ++i) {
        const uint16_t n = (uint16_t)(offset + i);
        const bool high = (mode == 3u) ? ((n & 1u) != 0u) : (((n >> 4) & 1u) != 0u);
        s_dma_words[n] = high ? (uint16_t)(PWM_PERIOD * 4u / 5u) : (uint16_t)(PWM_PERIOD / 5u);
    }
}

void __attribute__((__interrupt__, no_auto_psv)) _DMA2Interrupt(void)
{
    const uint32_t t0 = nora_high_res_timer_get_count();
    nora_dma_status_t status = nora_dma_isr_snapshot(NORA_DMA_CHANNEL_2);
    nora_dma_half_t half = nora_dma_half_from_status(status);
    uint32_t dt;
    if (nora_dma_status_has_overrun(status) ||
        nora_dma_status_has_half_done_conflict(status)) {
        ++s_dma_errors;
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
        return;
    }
    if (half == NORA_DMA_HALF_FIRST) { ++s_dma_halves; refill_half(0u); test_pattern(0u); }
    else if (half == NORA_DMA_HALF_SECOND) {
        ++s_dma_halves; refill_half(PWM_DMA_HALF); test_pattern(PWM_DMA_HALF);
    }
    dt = nora_high_res_timer_get_count() - t0;
    if (dt > s_isr_max_count) s_isr_max_count = dt;
}

nora_pwm_audio_result_t nora_pwm_audio_backend_init(uint8_t channel_count,
                                                    nora_pwm_audio_modulator_t *mod)
{
    if (channel_count != 1u) return NORA_PWM_AUDIO_UNSUPPORTED;
    static bool gain_set;
    gain_db_t g;
    if (!gain_set) {
        /* board_profile.h #errors on an out-of-range gain, so this cannot
         * fail; if it did, it is a board fault, not a caller's argument.
         * No pin is touched yet. */
        if (!gain_db_from_x10(EV08P02A_PWM_SCOPE_ONLY ? 0 : EV08P02A_PWM_GAIN_DB_X10, &g))
            return NORA_PWM_AUDIO_FAULT;
        s_pwm_gain.mant = g.mant;
        s_pwm_gain.db_x10 = g.db_x10;
        gain_set = true; /* a runtime *cp57 survives later re-inits */
    }
    if (!nora_gpio_config_digital_output(EV08P02A_PWM_SLEEP_PIN, false) ||
        !nora_gpio_config_digital_input(EV08P02A_PWM_FAULT_PIN)) {
        return NORA_PWM_AUDIO_FAULT;
    }
    return nora_pwm_audio_modulator_setup(mod, (uint32_t)PWM_PERIOD << PWM_FRAC_BITS,
                                          (uint32_t)PWM_MIN_DUTY << PWM_FRAC_BITS)
               ? NORA_PWM_AUDIO_OK : NORA_PWM_AUDIO_FAULT;
}

nora_pwm_audio_result_t nora_pwm_audio_backend_start(void)
{
    nora_pwm_generator_cfg_t pwm = {0};
    nora_dma_channel_cfg_t dma = {0};
    uint32_t start_ms;

    s_released = false;
    fill_neutral(0u, PWM_DMA_WORDS);
    s_duty_min = PWM_NEUTRAL;
    s_duty_max = PWM_NEUTRAL;
    s_dma_halves = 0u;
    s_dma_errors = 0u;
    s_ns_active = s_ns_order;
    s_ns_e1 = 0;
    s_ns_e2 = 0;
#if EV08P02A_PWM_AUDIO_HAL_FRC
    s_elastic_head = 0u;
    s_elastic_tail = 0u;
    s_elastic_phase_q16 = 0u;
    s_elastic_level_filtered_q8 = (int32_t)PWM_ELASTIC_TARGET * 256;
    s_elastic_primed = false;
    s_elastic_delta_q16 = 0;
    s_elastic_level = 0u;
    s_elastic_min = PWM_ELASTIC_WORDS - 1u;
    s_elastic_max = 0u;
    s_elastic_prefill_halves = 0u;
    s_elastic_underruns = 0u;
    s_elastic_stretched_halves = 0u;
    s_elastic_compressed_halves = 0u;
#endif
    if (!timer_app_running() || !g_ev08p02a_clock_on_target ||
        (nora_clock_get_fosc_hz() != EV08P02A_TARGET_FOSC_HZ)) {
        return NORA_PWM_AUDIO_NOT_READY;
    }
    (void)nora_gpio_clear(EV08P02A_PWM_SLEEP_PIN);
    if (!nora_gpio_rp_config_digital_output(EV08P02A_PWM_IN1_RP, false) ||
        !nora_gpio_rp_config_digital_output(EV08P02A_PWM_IN2_RP, false) ||
        !nora_pps_route_output(NORA_PPS_OUTPUT_PWM4H, EV08P02A_PWM_IN1_RP) ||
        !nora_pps_route_output(NORA_PPS_OUTPUT_PWM4L, EV08P02A_PWM_IN2_RP) ||
        !nora_pwm_module_init(NORA_PWM_MCLK_STANDARD)) {
        return NORA_PWM_AUDIO_FAULT;
    }
    s_hw_up = true;

    pwm.period = PWM_PERIOD;
    pwm.duty_init = PWM_NEUTRAL;
    pwm.pen_h = true;
    pwm.pen_l = true;
    pwm.output_mode = NORA_PWM_MODE_COMPLEMENTARY;
    pwm.is_module_master = true;
    if (!nora_pwm_generator_init(NORA_PWM_GEN_4, &pwm)) return NORA_PWM_AUDIO_FAULT;
    /* Both inputs low (coast) at once; release later at the next SOC. */
    (void)nora_pwm_generator_override(NORA_PWM_GEN_4, true, false, false);
    /* DMA trigger at mid-period, not EOC. Triggered at EOC, the DMA write to
     * PG4DC raced the SOC that applies the previous buffered update: while
     * UPDATE is pending the data registers are not writable, so writes were
     * lost and the scope showed 20%/80% per-period test patterns as runs of
     * one value (2026-10-14). Mid-period, every write lands a half period
     * before the SOC that applies it. */
    PG4TRIGA = PWM_PERIOD / 2u;
    PG4EVTLbits.PGTRGSEL = 1u; /* TRIGA compare */
    PG4EVTLbits.UPDTRG = 1u;   /* DMA write to DC requests buffered update */
    PWMEVTAbits.EVTAPGS = 3u;  /* PG4 */
    PWMEVTAbits.EVTASEL = 0u;  /* PGTRGSEL = EOC */

    dma.src = s_dma_words;
    dma.dst = &PG4DC;
    dma.count = PWM_DMA_WORDS;
    dma.src_mode = NORA_DMA_ADDR_INCREMENT;
    dma.dst_mode = NORA_DMA_ADDR_FIXED;
    dma.size = NORA_DMA_SIZE_HALFWORD;
    dma.tr_mode = NORA_DMA_TRMODE_REPEAT_ONESHOT;
    dma.reload_src = true;
    dma.reload_count = true;
    dma.half_int_en = true;
    dma.done_int_en = true;
    dma.trigger = NORA_DMA_TRIGGER_PWM_EVENT_A;
    dma.irq_priority_set = true;
    /* ABOVE the SPI1 RX block ISR (4): that ISR runs for up to ~610 us of
     * its 667 us block with Type_LB AVAS, which starved this refill and
     * raised a DMA overrun fault (dma_errors=2, 2026-10-14). The refill is
     * short and the core FIFO is single-producer/single-consumer. */
    dma.irq_priority = 5u;
    dma.irq_enable = true;
    if (!nora_dma_channel_config(NORA_DMA_CHANNEL_2, &dma) ||
        !nora_dma_channel_enable(NORA_DMA_CHANNEL_2, true)) {
        return NORA_PWM_AUDIO_FAULT;
    }

    if (!EV08P02A_PWM_SCOPE_ONLY || s_lab_mode)
        (void)nora_gpio_set(EV08P02A_PWM_SLEEP_PIN);
    start_ms = GetTicks();
    while ((uint32_t)(GetTicks() - start_ms) < 2u) { /* >= 1 ms; tDELAY max 500 us */ }
    if (stage_fault()) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
        return NORA_PWM_AUDIO_FAULT;
    }
    if (!nora_pwm_generator_override(NORA_PWM_GEN_4, false, false, true)) {
        return NORA_PWM_AUDIO_FAULT;
    }
    s_released = true;
    return NORA_PWM_AUDIO_OK;
}

/* STOP_SAFE as the legacy CK path: nSLEEP low first, then DMA off, both
 * inputs low, PG4 off. */
void nora_pwm_audio_backend_stop(void)
{
    s_released = false;
    (void)nora_gpio_clear(EV08P02A_PWM_SLEEP_PIN);
    if (!s_hw_up) return;
    s_hw_up = false;
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_2, false);
    nora_dma_irq_enable(NORA_DMA_CHANNEL_2, false);
    (void)nora_pwm_generator_override(NORA_PWM_GEN_4, true, false, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_4, false);
}

/* ---- board glue (same names as the legacy path) ------------------------- */

bool ev08p02a_pwm_audio_start(void)
{
    static const nora_pwm_audio_config_t cfg = {
        NORA_PWM_AUDIO_SAMPLE_RATE_HZ, NORA_PWM_AUDIO_FRAMES_PER_BLOCK, 1u
    };
    nora_pwm_audio_status_t st;
    nora_pwm_audio_get_status(&st);
    /* The legacy path re-armed on every WM8904 start; keep that, which also
     * clears a latched fault the way the draft asks (explicit re-init while
     * stopped). */
    if (nora_pwm_audio_init(&cfg) != NORA_PWM_AUDIO_OK) return false;
    return nora_pwm_audio_start() == NORA_PWM_AUDIO_OK;
}

void ev08p02a_pwm_audio_stop(void)
{
    nora_pwm_audio_stop();
}

bool ev08p02a_pwm_audio_lab_control(bool enabled)
{
    ev08p02a_pwm_audio_stop();
    s_lab_mode = enabled;
    return !enabled || ev08p02a_pwm_audio_start();
}

void ev08p02a_pwm_audio_set_neutral(bool neutral)
{
    s_neutral = neutral;
}

bool ev08p02a_pwm_audio_set_ns_order(uint8_t order)
{
    if (order > 2u) return false;
    s_ns_order = order; /* DMA2 ISR adopts it at the next half */
    return true;
}

bool ev08p02a_pwm_audio_set_test_mode(uint8_t mode)
{
    if (mode > 4u) return false;
    s_test_mode = mode; /* DMA2 ISR reads it once per half */
    return true;
}

bool ev08p02a_pwm_audio_set_gain_x10(int16_t db_x10)
{
    gain_db_t g;
    if (EV08P02A_PWM_SCOPE_ONLY || !gain_db_from_x10(db_x10, &g)) return false;
    s_pwm_gain.mant = g.mant; /* one word: the SPI1 RX ISR reads it once per block */
    s_pwm_gain.db_x10 = g.db_x10;
    return true;
}

/* SPI1 RX DMA ISR: final TDM slot 0 -> board PWM gain -> Q1.31.
 * Scope and 0 dB preserve the signed PCM word exactly. */
void ev08p02a_pwm_audio_block(const nora_tdm_slot_t *block)
{
    nora_pwm_audio_block_t out;
    uint16_t frame;
    uint16_t saturated = 0u;
    const uint16_t mant = s_pwm_gain.mant;
    if (block == 0) return;
    for (frame = 0u; frame < NORA_PWM_AUDIO_FRAMES_PER_BLOCK; ++frame) {
        int32_t sample = nora_tdm_slot_decode_s32(&block[(uint16_t)(frame * 8u)]);
        s_pcm_q31[frame] = s_neutral ? 0 :
            (GAIN_DB_IS_UNITY(mant) ? sample :
             gain_db_scale(sample, mant, &saturated));
    }
    out.left_q31 = s_pcm_q31;
    out.right_q31 = 0;
    (void)nora_pwm_audio_write(&out);   /* BUSY / NOT_READY are counted */
}

/* Console fault injection (*cp5901): the exact path poll() takes on a low
 * nFAULT, so EMERGENCY_SAFE can be checked without touching the stage. The
 * latch clears only through an explicit re-init (*cp5501, *tr, reset). */
void ev08p02a_pwm_audio_inject_stage_fault(void)
{
    board_emergency_safe();
    nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
}

/* Main loop: nFAULT and PLL lock, at the legacy cadence. */
void ev08p02a_pwm_audio_poll(void)
{
    static uint32_t last_clock_check;
    if (!s_released) return;
    if (stage_fault()) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
        return;
    }
    if ((uint32_t)(GetTicks() - last_clock_check) >= 10u) {
        last_clock_check = GetTicks();
        if (nora_clock_get_fosc_hz() != EV08P02A_TARGET_FOSC_HZ ||
            OSCCONbits.LOCK == 0u) {
            board_emergency_safe();
            nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_CLOCK);
        }
    }
}

void ev08p02a_pwm_audio_report(void)
{
    nora_pwm_audio_status_t st;
    uint16_t min_duty, max_duty;
    uint32_t halves, errors, isr_max;
    bool irq_enabled = nora_dma_irq_is_enabled(NORA_DMA_CHANNEL_2);
    nora_dma_irq_enable(NORA_DMA_CHANNEL_2, false);
    min_duty = s_duty_min; max_duty = s_duty_max;
    halves = s_dma_halves; errors = s_dma_errors;
    isr_max = s_isr_max_count; s_isr_max_count = 0u;
    s_duty_min = s_duty_max = PWM_NEUTRAL;
    nora_dma_irq_restore(NORA_DMA_CHANNEL_2, irq_enabled);
    nora_pwm_audio_get_status(&st);
    console_out_str("PWMcontrol lab="); console_out_u32(s_lab_mode);
    console_out_str(" scope="); console_out_u32(EV08P02A_PWM_SCOPE_ONLY);
    console_out_str(" neutral="); console_out_u32(s_neutral);
    console_out_str(" pg="); console_out_u32(PG4CONLbits.ON);
    console_out_str(" dma="); console_out_u32(DMACH2bits.CHEN);
    console_out_str(" nSLEEP="); console_out_u32(nora_gpio_read_output(EV08P02A_PWM_SLEEP_PIN) == NORA_GPIO_LEVEL_HIGH);
    console_out_str(" nFAULT="); console_out_u32(nora_gpio_read(EV08P02A_PWM_FAULT_PIN) == NORA_GPIO_LEVEL_HIGH);
    console_out_str(" carrier_hz="); console_out_u32(EV08P02A_TARGET_FOSC_HZ / PWM_PERIOD);
    console_out_str(" period="); console_out_u32(PWM_PERIOD);
    console_out_str(" factor="); console_out_u32(EV08P02A_PWM_CARRIER_FACTOR);
    console_out_str(" limit_min="); console_out_u32(PWM_MIN_DUTY);
    console_out_str(" limit_max="); console_out_u32(PWM_PERIOD - PWM_MIN_DUTY);
    console_out_str(" duty_min="); console_out_u32(min_duty);
    console_out_str(" duty_max="); console_out_u32(max_duty);
    console_out_str(" halves="); console_out_u32(halves);
    console_out_str(" dma_errors="); console_out_u32(errors);
    console_out_str(" gain_db_x10=");
    {
        int16_t db = s_pwm_gain.db_x10;
        if (db < 0) { console_out_str("-"); db = (int16_t)-db; }
        console_out_u32((uint16_t)db);
    }
    console_out_str(" ns="); console_out_u32(s_ns_order);
    console_out_str(" frac_bits="); console_out_u32(PWM_FRAC_BITS);
    console_out_str(" test="); console_out_u32(s_test_mode);
    console_out_str(" dma2_isr_max_us_x10="); console_out_u32(nora_high_res_timer_count_to_us_x10(isr_max));
    console_out_str("\n");
    console_out_str("PWMregs PG4CONL="); console_out_hex16(PG4CONL);
    console_out_str(" CONH="); console_out_hex16(PG4CONH);
    console_out_str(" IOCONL="); console_out_hex16(PG4IOCONL);
    console_out_str(" IOCONH="); console_out_hex16(PG4IOCONH);
    console_out_str(" EVTL="); console_out_hex16(PG4EVTL);
    console_out_str(" EVTH="); console_out_hex16(PG4EVTH);
    console_out_str(" STAT="); console_out_hex16(PG4STAT);
    console_out_str(" PER="); console_out_hex16(PG4PER);
    console_out_str(" TRIGA="); console_out_hex16(PG4TRIGA);
    console_out_str(" DC="); console_out_hex16(PG4DC);
    console_out_str(" PCLKCON="); console_out_hex16(PCLKCON);
    console_out_str(" PWMEVTA="); console_out_hex16(PWMEVTA);
    console_out_str(" DMACH2="); console_out_hex16(DMACH2);
    console_out_str(" DMAINT2="); console_out_hex16(DMAINT2);
    console_out_str(" DMACON="); console_out_hex16(DMACON);
    console_out_str("\n");
    console_out_str("Socket 3 MPQ (hal_pwm_audio): ");
    console_out_str(s_released ? "running" : (st.fault_flags != 0u ? "FAULT stopped" : "stopped"));
    console_out_str(" written=");
    console_out_u32(st.written_blocks);
    console_out_str(" rejected=");
    console_out_u32(st.rejected_blocks);
    console_out_str(" underrun=");
    console_out_u32(st.underrun_blocks);
    console_out_str(" fault=");
    console_out_u32(st.fault_flags);
    console_out_str(" DMA2 remaining=");
    console_out_u32(nora_dma_read_count(NORA_DMA_CHANNEL_2));
#if EV08P02A_PWM_AUDIO_HAL_FRC
    console_out_str(" FRCPLL ELASTIC fifo=");
    console_out_u32(s_elastic_level);
    console_out_str(" min=");
    console_out_u32(s_elastic_min == (PWM_ELASTIC_WORDS - 1u) ? 0u : s_elastic_min);
    console_out_str(" max=");
    console_out_u32(s_elastic_max);
    console_out_str(" delta_q16=");
    if (s_elastic_delta_q16 < 0) {
        console_out_str("-");
        console_out_u32((uint16_t)(-s_elastic_delta_q16));
    } else {
        console_out_u32((uint16_t)s_elastic_delta_q16);
    }
    console_out_str(" prime=");
    console_out_u32(s_elastic_prefill_halves);
    console_out_str(" local_underrun=");
    console_out_u32(s_elastic_underruns);
    console_out_str(" stretch=");
    console_out_u32(s_elastic_stretched_halves);
    console_out_str(" compress=");
    console_out_u32(s_elastic_compressed_halves);
    s_elastic_min = PWM_ELASTIC_WORDS - 1u;
    s_elastic_max = 0u;
#endif
    console_out_str("\n");
}

#endif /* NORA_PWM_AUDIO_ENABLE */
