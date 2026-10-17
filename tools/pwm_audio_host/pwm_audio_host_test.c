/* Host test for hal_pwm_audio: the modulator contract and the core FIFO /
 * counter / fault contract, compiled from the firmware sources unchanged
 * against a fake backend. Run through run_pwm_audio_host_test.py. */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nora_pwm_audio_backend.h"

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++s_failures; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- modulator reference: exact rational arithmetic, independent code ---- */

/* round(n / d) for n >= 0, ties up (away from centre). */
static uint64_t ref_round(unsigned __int128 n, uint64_t d)
{
    return (uint64_t)((n * 2u + d) / (2u * (unsigned __int128)d));
}

static uint32_t ref_modulate(int32_t x, const nora_pwm_audio_modulator_t *m)
{
    const uint64_t centre = m->period / 2u;
    const uint64_t amp = centre - m->min_duty;
    if (x < 0) {
        uint64_t mag = (uint64_t)(-(int64_t)x);
        return (uint32_t)(centre - ref_round((unsigned __int128)mag * amp, 1ull << 31));
    }
    return (uint32_t)(centre + ref_round((unsigned __int128)(uint64_t)x * amp, 0x7fffffffull));
}

static uint64_t s_rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)s_rng;
}

static void check_modulator_case(uint32_t period, uint32_t min_duty)
{
    nora_pwm_audio_modulator_t m;
    const int32_t probes[] = { INT32_MIN, INT32_MIN + 1, -1073741824, -2, -1, 0, 1, 2,
                               1073741823, 1073741824, INT32_MAX - 1, INT32_MAX };
    uint32_t prev;
    int64_t x;
    unsigned i;

    CHECK(nora_pwm_audio_modulator_setup(&m, period, min_duty),
          "setup(%" PRIu32 ", %" PRIu32 ") refused", period, min_duty);
    CHECK(m.max_duty == period - min_duty, "max_duty");
    CHECK(nora_pwm_audio_modulate_q31(INT32_MIN, &m) == min_duty, "INT32_MIN -> min_duty (P=%" PRIu32 ")", period);
    CHECK(nora_pwm_audio_modulate_q31(0, &m) == period / 2u, "0 -> P/2 (P=%" PRIu32 ")", period);
    CHECK(nora_pwm_audio_modulate_q31(INT32_MAX, &m) == m.max_duty, "INT32_MAX -> max_duty (P=%" PRIu32 ")", period);
    CHECK(nora_pwm_audio_modulator_neutral(&m) == period / 2u, "neutral");

    for (i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
        uint32_t got = nora_pwm_audio_modulate_q31(probes[i], &m);
        CHECK(got == ref_modulate(probes[i], &m), "probe %" PRId32 " P=%" PRIu32 " got %" PRIu32 " want %" PRIu32,
              probes[i], period, got, ref_modulate(probes[i], &m));
    }
    for (i = 0; i < 200000u; ++i) {
        int32_t s = (int32_t)rnd32();
        uint32_t got = nora_pwm_audio_modulate_q31(s, &m);
        if (got != ref_modulate(s, &m)) {
            CHECK(0, "random %" PRId32 " P=%" PRIu32 " got %" PRIu32 " want %" PRIu32, s, period, got, ref_modulate(s, &m));
            break;
        }
        CHECK(got >= m.min_duty && got <= m.max_duty, "clamp");
    }
    /* Exact rounding ties: samples whose ideal offset is k + 1/2 must round
     * away from centre on both sides. */
    for (i = 0; i < 64u; ++i) {
        const uint64_t amp = period / 2u - min_duty;
        uint64_t k = rnd32() % (amp ? amp : 1u);
        /* negative: |x| * amp = (k + 1/2) * 2^31 has integer solutions only
         * when amp divides; test those directly when amp is a power of two. */
        if (amp != 0u && ((amp & (amp - 1u)) == 0u)) {
            unsigned __int128 n = ((unsigned __int128)(2u * k + 1u) << 30);
            if (n % amp == 0u) {
                uint64_t mag = (uint64_t)(n / amp);
                if (mag <= (1ull << 31)) {
                    int32_t s = (int32_t)(-(int64_t)mag);
                    CHECK(nora_pwm_audio_modulate_q31(s, &m) == period / 2u - (uint32_t)(k + 1u),
                          "negative tie P=%" PRIu32 " k=%" PRIu64, period, k);
                }
            }
        }
    }
    /* Monotonic over a dense sweep, and +/- symmetric up to the Q1.31 asymmetry:
     * x and -x land at the same distance from centre or one count apart. */
    prev = 0u;
    for (x = INT32_MIN; x <= INT32_MAX; x += 65537) {
        uint32_t got = nora_pwm_audio_modulate_q31((int32_t)x, &m);
        CHECK(got >= prev, "monotonic at %" PRId64, x);
        prev = got;
        if (x > INT32_MIN) {
            int64_t up = (int64_t)nora_pwm_audio_modulate_q31((int32_t)-x, &m) - period / 2u;
            int64_t dn = (int64_t)period / 2 - (int64_t)got;
            CHECK(llabs(up - dn) <= 1, "symmetry at %" PRId64, x);
        }
        if (s_failures > 20) return;
    }
}

static void test_modulator(void)
{
    nora_pwm_audio_modulator_t m;
    /* CK X1 ECPLL: 4096 counts, 100-count minimum pulse. */
    check_modulator_case(4096u, 100u);
    /* CK FRCPLL scope: 4166 counts. */
    check_modulator_case(4166u, 100u);
    /* AK512 MPQ, 798.72 MHz / 48 kHz = 16640 counts, Q4: period 266240,
     * min 500 ns = 399 counts << 4. */
    check_modulator_case(16640u << 4, (798720000u / 2000000u) << 4);
    /* AK512 MIC4607 96 kHz: 8320 counts, Q4, 0x100 guard. */
    check_modulator_case(8320u << 4, 256u);
    check_modulator_case(2u * 0xffffu + 200u, 100u);
    check_modulator_case(2u * 0x10000u + 200u, 100u);
    check_modulator_case(2u, 0u);
    check_modulator_case(2u, 1u);
    check_modulator_case(0x80000000u, 0u);

    CHECK(!nora_pwm_audio_modulator_setup(&m, 4095u, 100u), "odd period accepted");
    CHECK(!nora_pwm_audio_modulator_setup(&m, 0u, 0u), "zero period accepted");
    CHECK(!nora_pwm_audio_modulator_setup(&m, 4096u, 2049u), "min > P/2 accepted");
    CHECK(!nora_pwm_audio_modulator_setup(NULL, 4096u, 0u), "NULL accepted");
}

/* ---- fake backend ------------------------------------------------------- */

static int s_backend_init_calls, s_backend_start_calls, s_backend_stop_calls;
static nora_pwm_audio_result_t s_backend_start_result = NORA_PWM_AUDIO_OK;
static uint32_t s_fault_during_start;
static nora_pwm_audio_duty_t s_out[NORA_PWM_AUDIO_MAX_CHANNELS][NORA_PWM_AUDIO_FRAMES_PER_BLOCK];
static uint8_t s_board_channels = NORA_PWM_AUDIO_MAX_CHANNELS;

#define FAKE_PERIOD 4096u
#define FAKE_MIN    100u

nora_pwm_audio_result_t nora_pwm_audio_backend_init(uint8_t channel_count,
                                                    nora_pwm_audio_modulator_t *mod)
{
    ++s_backend_init_calls;
    if (channel_count > s_board_channels) return NORA_PWM_AUDIO_UNSUPPORTED;
    return nora_pwm_audio_modulator_setup(mod, FAKE_PERIOD, FAKE_MIN) ? NORA_PWM_AUDIO_OK
                                                                      : NORA_PWM_AUDIO_FAULT;
}

nora_pwm_audio_result_t nora_pwm_audio_backend_start(void)
{
    ++s_backend_start_calls;
    if (s_fault_during_start != 0u) nora_pwm_audio_core_fault(s_fault_during_start);
    return s_backend_start_result;
}

void nora_pwm_audio_backend_stop(void) { ++s_backend_stop_calls; }

/* One DMA refill: what the backend ISR does. Returns true if data was used. */
static bool refill(void)
{
    const nora_pwm_audio_duty_t *duty[NORA_PWM_AUDIO_MAX_CHANNELS];
    unsigned ch, i;
    if (nora_pwm_audio_core_take(duty)) {
        for (ch = 0; ch < NORA_PWM_AUDIO_MAX_CHANNELS; ++ch)
            for (i = 0; i < NORA_PWM_AUDIO_FRAMES_PER_BLOCK; ++i) s_out[ch][i] = duty[ch][i];
        nora_pwm_audio_core_release();
        return true;
    }
    for (ch = 0; ch < NORA_PWM_AUDIO_MAX_CHANNELS; ++ch)
        for (i = 0; i < NORA_PWM_AUDIO_FRAMES_PER_BLOCK; ++i) s_out[ch][i] = FAKE_PERIOD / 2u;
    return false;
}

static int32_t s_l[NORA_PWM_AUDIO_FRAMES_PER_BLOCK], s_r[NORA_PWM_AUDIO_FRAMES_PER_BLOCK];

static void fill_block(int32_t lv, int32_t rv)
{
    unsigned i;
    for (i = 0; i < NORA_PWM_AUDIO_FRAMES_PER_BLOCK; ++i) { s_l[i] = lv; s_r[i] = rv; }
}

static nora_pwm_audio_status_t status(void)
{
    nora_pwm_audio_status_t st;
    nora_pwm_audio_get_status(&st);
    return st;
}

static nora_pwm_audio_config_t cfg_for(uint8_t ch)
{
    nora_pwm_audio_config_t c = { NORA_PWM_AUDIO_SAMPLE_RATE_HZ, NORA_PWM_AUDIO_FRAMES_PER_BLOCK, ch };
    return c;
}

static void test_core(void)
{
    nora_pwm_audio_config_t cfg = cfg_for(NORA_PWM_AUDIO_MAX_CHANNELS);
    nora_pwm_audio_block_t blk = { s_l, (NORA_PWM_AUDIO_MAX_CHANNELS == 2) ? s_r : NULL };
    nora_pwm_audio_status_t st;
    unsigned i, accepted = 0;

    /* Before init. */
    CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_NOT_READY, "start before init");
    CHECK(nora_pwm_audio_write(&blk) == NORA_PWM_AUDIO_NOT_READY, "write before init");
    nora_pwm_audio_stop();
    CHECK(s_backend_stop_calls == 0, "stop before init reached backend");

    /* init argument checks. */
    CHECK(nora_pwm_audio_init(NULL) == NORA_PWM_AUDIO_INVALID_ARGUMENT, "init NULL");
    cfg.sample_rate_hz = 44100u;
    CHECK(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_UNSUPPORTED, "init 44.1k");
    cfg = cfg_for(NORA_PWM_AUDIO_MAX_CHANNELS); cfg.frames_per_block = 16u;
    CHECK(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_UNSUPPORTED, "init 16 frames");
    cfg = cfg_for(NORA_PWM_AUDIO_MAX_CHANNELS + 1u);
    CHECK(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_UNSUPPORTED, "init too many ch");
    cfg = cfg_for(0u);
    CHECK(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_INVALID_ARGUMENT, "init 0 ch");
    CHECK(s_backend_init_calls == 0, "backend init reached on bad cfg");

    cfg = cfg_for(NORA_PWM_AUDIO_MAX_CHANNELS);
    CHECK(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_OK, "init ok");
    CHECK(nora_pwm_audio_write(&blk) == NORA_PWM_AUDIO_NOT_READY, "write before start");
    CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_OK, "start");
    CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_OK, "start again");
    CHECK(s_backend_start_calls == 1, "start not idempotent (%d)", s_backend_start_calls);

    /* A failed config check changes nothing: a running output keeps running. */
    {
        nora_pwm_audio_config_t bad = cfg;
        int inits = s_backend_init_calls, stops = s_backend_stop_calls;
        bad.sample_rate_hz = 44100u;
        CHECK(nora_pwm_audio_init(&bad) == NORA_PWM_AUDIO_UNSUPPORTED, "bad init while running");
        CHECK(nora_pwm_audio_init(NULL) == NORA_PWM_AUDIO_INVALID_ARGUMENT, "NULL init while running");
        CHECK(s_backend_init_calls == inits && s_backend_stop_calls == stops,
              "bad init while running reached the backend");
        CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_OK && s_backend_start_calls == 1,
              "bad init stopped the running output");
    }

    /* Start-up neutral before any data is not an underrun. */
    CHECK(!nora_pwm_audio_core_ready(), "ready with an empty FIFO");
    for (i = 0; i < 5; ++i) CHECK(!refill(), "refill without data used data");
    CHECK(s_out[0][0] == FAKE_PERIOD / 2u, "start-up neutral");
    CHECK(status().underrun_blocks == 0u, "start-up neutral counted as underrun");

    /* Write invalid. */
    CHECK(nora_pwm_audio_write(NULL) == NORA_PWM_AUDIO_INVALID_ARGUMENT, "write NULL");
#if NORA_PWM_AUDIO_MAX_CHANNELS == 2
    {
        nora_pwm_audio_block_t half = { s_l, NULL };
        CHECK(nora_pwm_audio_write(&half) == NORA_PWM_AUDIO_INVALID_ARGUMENT, "stereo without R");
    }
#endif

    /* Fill until BUSY: N slots buffer N-1 blocks, the rest is rejected whole. */
    fill_block(INT32_MAX, INT32_MIN);
    for (i = 0; i < NORA_PWM_AUDIO_FIFO_BLOCKS + 2u; ++i) {
        nora_pwm_audio_result_t r = nora_pwm_audio_write(&blk);
        if (r == NORA_PWM_AUDIO_OK) ++accepted;
        else CHECK(r == NORA_PWM_AUDIO_BUSY, "overflow result %d", (int)r);
    }
    CHECK(accepted == NORA_PWM_AUDIO_FIFO_BLOCKS - 1u, "accepted %u", accepted);
    st = status();
    CHECK(st.written_blocks == accepted, "written count");
    CHECK(st.rejected_blocks == 3u, "rejected count %" PRIu32, st.rejected_blocks);

    /* Drain: every channel of a block comes out together. */
    for (i = 0; i < accepted; ++i) {
        CHECK(refill(), "refill %u had no data", i);
        CHECK(s_out[0][0] == FAKE_PERIOD - FAKE_MIN, "L duty %u", (unsigned)s_out[0][0]);
#if NORA_PWM_AUDIO_MAX_CHANNELS == 2
        CHECK(s_out[1][NORA_PWM_AUDIO_FRAMES_PER_BLOCK - 1u] == FAKE_MIN, "R duty");
#endif
    }
    /* Running out after data is an underrun, one per neutral block, all channels. */
    CHECK(!refill(), "refill after drain used data");
    CHECK(!refill(), "refill after drain used data");
    st = status();
    CHECK(st.underrun_blocks == 2u, "underrun %" PRIu32, st.underrun_blocks);
    CHECK(s_out[NORA_PWM_AUDIO_MAX_CHANNELS - 1u][0] == FAKE_PERIOD / 2u, "underrun neutral all ch");

    /* After an underrun the consumer re-primes before playing again. */
    fill_block(0, 0);
    CHECK(nora_pwm_audio_write(&blk) == NORA_PWM_AUDIO_OK, "write after underrun");
#if NORA_PWM_AUDIO_PRIME_BLOCKS > 1
    /* ready() is a pure query: polled while re-priming it neither counts an
     * underrun nor lets take() skip the prime. */
    for (i = 0; i < 3; ++i) CHECK(!nora_pwm_audio_core_ready(), "ready before re-prime");
    CHECK(status().underrun_blocks == 2u, "ready() counted an underrun");
    CHECK(!refill(), "played before re-prime");
    CHECK(status().underrun_blocks == 3u, "re-prime neutral not counted");
    CHECK(nora_pwm_audio_write(&blk) == NORA_PWM_AUDIO_OK, "write 2 after underrun");
#endif
    CHECK(nora_pwm_audio_core_ready(), "not ready after re-prime");
    CHECK(refill(), "no play after re-prime");
    CHECK(s_out[0][5] == FAKE_PERIOD / 2u, "zero sample duty");
    /* Once primed, one queued block is enough. */
#if NORA_PWM_AUDIO_PRIME_BLOCKS > 1
    CHECK(nora_pwm_audio_core_ready(), "primed, one block left, not ready");
    CHECK(refill(), "primed, one block left, not played");
#endif
    CHECK(!nora_pwm_audio_core_ready(), "ready with an empty FIFO after play");
    CHECK(!refill(), "empty after re-prime drain");

    /* stop keeps counters; write is NOT_READY; stop is repeatable. */
    st = status();
    nora_pwm_audio_stop();
    nora_pwm_audio_stop();
    CHECK(nora_pwm_audio_write(&blk) == NORA_PWM_AUDIO_NOT_READY, "write after stop");
    CHECK(status().written_blocks == st.written_blocks, "stop reset counters");
    CHECK(!refill(), "refill after stop used data");
    CHECK(nora_pwm_audio_write(&blk) == NORA_PWM_AUDIO_NOT_READY && !nora_pwm_audio_core_ready(),
          "ready while stopped");
    CHECK(status().underrun_blocks == st.underrun_blocks, "stopped refill counted underrun");

    /* Restart discards stale FIFO contents. */
    CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_OK, "restart");
    CHECK(!refill(), "stale data after restart");

    /* Fault latch: write and start report FAULT until re-init. */
    nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE | NORA_PWM_AUDIO_FAULT_DMA);
    st = status();
    CHECK(st.fault_flags == (NORA_PWM_AUDIO_FAULT_STAGE | NORA_PWM_AUDIO_FAULT_DMA), "fault flags");
    CHECK(nora_pwm_audio_write(&blk) == NORA_PWM_AUDIO_FAULT, "write after fault");
    nora_pwm_audio_stop();
    CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_FAULT, "start after fault");
    /* A failed config check keeps the fault latched. */
    {
        nora_pwm_audio_config_t bad = cfg;
        bad.frames_per_block = 16u;
        CHECK(nora_pwm_audio_init(&bad) == NORA_PWM_AUDIO_UNSUPPORTED, "bad init in fault");
        CHECK(status().fault_flags == (NORA_PWM_AUDIO_FAULT_STAGE | NORA_PWM_AUDIO_FAULT_DMA),
              "bad init cleared the fault");
        CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_FAULT, "start after bad init in fault");
    }
    CHECK(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_OK, "re-init");
    st = status();
    CHECK(st.fault_flags == 0u && st.written_blocks == 0u && st.underrun_blocks == 0u, "re-init reset");

    /* A fault raised while the backend starts fails start(). */
    s_fault_during_start = NORA_PWM_AUDIO_FAULT_CLOCK;
    s_backend_stop_calls = 0;
    CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_FAULT, "fault during start");
    CHECK(s_backend_stop_calls >= 1, "fault during start not stopped");
    s_fault_during_start = 0u;
    CHECK(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_OK, "re-init 2");

    /* A failing backend start leaves the core stopped. */
    s_backend_start_result = NORA_PWM_AUDIO_FAULT;
    CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_FAULT, "backend start failure");
    CHECK(nora_pwm_audio_write(&blk) == NORA_PWM_AUDIO_NOT_READY, "write after failed start");
    s_backend_start_result = NORA_PWM_AUDIO_OK;

    /* A backend that rejects init() stops the running output first and leaves
     * the module uninitialised; no backend_stop() follows the failure. */
    CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_OK, "start before backend init failure");
    s_board_channels = 0u;
    s_backend_stop_calls = 0;
    CHECK(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_UNSUPPORTED, "backend init failure");
    CHECK(s_backend_stop_calls == 1, "running output not stopped before backend init");
    CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_NOT_READY, "start after backend init failure");
    CHECK(nora_pwm_audio_write(&blk) == NORA_PWM_AUDIO_NOT_READY, "write after backend init failure");
    nora_pwm_audio_stop();
    CHECK(s_backend_stop_calls == 1, "stop while uninitialised reached the backend");
    s_board_channels = NORA_PWM_AUDIO_MAX_CHANNELS;
    CHECK(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_OK, "re-init 3");

#if NORA_PWM_AUDIO_MAX_CHANNELS == 2
    /* A stereo board can be run mono: R is ignored and may be NULL. */
    cfg = cfg_for(1u);
    CHECK(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_OK, "mono on stereo board");
    CHECK(nora_pwm_audio_start() == NORA_PWM_AUDIO_OK, "mono start");
    {
        nora_pwm_audio_block_t mono = { s_l, NULL };
        fill_block(INT32_MIN, 0);
        CHECK(nora_pwm_audio_write(&mono) == NORA_PWM_AUDIO_OK, "mono write");
        CHECK(nora_pwm_audio_write(&mono) == NORA_PWM_AUDIO_OK, "mono write 2");
    }
    CHECK(refill(), "mono refill");
    CHECK(s_out[0][0] == FAKE_MIN, "mono L");
#endif
    nora_pwm_audio_stop();
}

int main(void)
{
    test_modulator();
    test_core();
    if (s_failures != 0) {
        printf("FAILED: %d check(s), MAX_CHANNELS=%u\n", s_failures, (unsigned)NORA_PWM_AUDIO_MAX_CHANNELS);
        return 1;
    }
    printf("PASS hal_pwm_audio host test (MAX_CHANNELS=%u, FIFO=%u, PRIME=%u)\n",
           (unsigned)NORA_PWM_AUDIO_MAX_CHANNELS, (unsigned)NORA_PWM_AUDIO_FIFO_BLOCKS,
           (unsigned)NORA_PWM_AUDIO_PRIME_BLOCKS);
    return 0;
}
