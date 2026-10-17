"""Exercise actual CK duty refill/control code and SW0/LED code on a host.

No hardware access. Backend functions are extracted unchanged; portable HAL
sources are compiled unchanged. Simulated independent clocks include +/-0.3%.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from run_pwm_audio_host_test import find_cc

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'src/boards/ev08p02a'
HAL = ROOT / 'src/hal_pwm_audio'


def function(source, name):
    match = re.search(r'^(?:static\s+)?(?:inline\s+)?[\w]+\s+' + name + r'\s*\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise RuntimeError(f'missing function: {name}')
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "nora_pwm_audio_backend.h"
#include "ev08p02a_bench_controls.h"
#define EV08P02A_PWM_AUDIO 1
#define EV08P02A_ENABLE_ADC_POT 1
#define EV08P02A_PWM_CARRIER_FACTOR FACTOR
#define PWM_HALF 32u
#define PWM_DMA_HALF (32u * FACTOR)
#define PWM_DMA_WORDS (2u * PWM_DMA_HALF)
#define PWM_PERIOD (((200000000UL / (48000UL * FACTOR)) + 1u) & ~1u)
#define PWM_NEUTRAL (PWM_PERIOD/2u)
#define PWM_MIN_DUTY 2u
#define PWM_FRAC_BITS (FACTOR == 4 ? 4u : FACTOR == 2 ? 3u : 2u)
#define PWM_FRAC_ONE ((int16_t)(1 << PWM_FRAC_BITS))
static uint8_t s_ns_active = 2, s_ns_order = 2;
static int16_t s_ns_e1, s_ns_e2;
#define PWM_ELASTIC_WORDS 256u
#define PWM_ELASTIC_PRIME_WORDS 128u
#define PWM_ELASTIC_TARGET 112u
#define PWM_ELASTIC_DELTA_LIMIT 655
static volatile uint16_t s_dma_words[PWM_DMA_WORDS];
static uint16_t s_elastic_fifo[256], s_elastic_head, s_elastic_tail, s_elastic_phase_q16;
static int32_t s_elastic_level_filtered_q8;
static bool s_elastic_primed, s_core_primed;
static uint32_t s_core_taken_blocks;
static int16_t s_elastic_delta_q16;
static uint16_t s_elastic_level, s_elastic_min, s_elastic_max;
static uint16_t s_elastic_prefill_halves, s_elastic_underruns;
static uint16_t s_elastic_stretched_halves, s_elastic_compressed_halves;
static uint16_t s_duty_min, s_duty_max;
static bool s_lab_mode, fault_pin, mock_running, mock_neutral;
#define EV08P02A_PWM_FAULT_PIN 1
#define NORA_GPIO_LEVEL_HIGH 1
static int nora_gpio_read(int pin) { (void)pin; return fault_pin; }
static void ev08p02a_pwm_audio_stop(void) { mock_running = false; }
static bool ev08p02a_pwm_audio_start(void) { mock_running = true; return true; }
static void ev08p02a_pwm_audio_set_neutral(bool v) { mock_neutral = v; }
static bool s_periodic_report_enabled = true;
static uint8_t s_test_mode;
static uint8_t mock_test = 9;
static bool ev08p02a_pwm_audio_set_test_mode(uint8_t t) { if (t > 4u) return false; mock_test = t; return true; }
static uint8_t mock_ns = 9;
static int16_t mock_gain = 9999;
static bool ev08p02a_pwm_audio_set_ns_order(uint8_t o) { if (o > 2u) return false; mock_ns = o; return true; }
static bool ev08p02a_pwm_audio_set_gain_x10(int16_t g) { mock_gain = g; return g >= -240 && g <= 240; }
static void ev08p02a_pwm_audio_report(void) {}
static unsigned injected;
static void ev08p02a_pwm_audio_inject_stage_fault(void) { ++injected; }
static void wm8904_audio_mute_report(void) {}
static void ev08p02a_pot_report(void) {}
static unsigned pitch_reports, gain_reports;
static void wm8904_audio_avas_pitch_report(void) { ++pitch_reports; }
static void wm8904_audio_avas_gain_report(void) { ++gain_reports; }
static void console_out_str(const char *p) { (void)p; }
enum { APP_CONSOLE_OK, APP_CONSOLE_ERR_BAD_DATA, APP_CONSOLE_ERR_NOT_FOUND,
       APP_CONSOLE_ERR_UNSUPPORTED, APP_CONSOLE_ERR_BAD_PARM_LEN,
       APP_CONSOLE_ERR_OPERATION_FAILED };
enum { WM8904_AUDIO_AVAS_VOICE_TYPE_TY, WM8904_AUDIO_AVAS_VOICE_TYPE_LB };
typedef struct { char name, kind; uint8_t data_len, data[4]; int status; } app_console_msg_t;
static unsigned voice_keys;
static uint8_t voice_key;
static void wm8904_audio_avas_voice_key(uint8_t v) { ++voice_keys; voice_key = v; }
static int16_t pitch_step;
static void wm8904_audio_avas_pitch_step(int16_t v) { pitch_step = v; }
static int16_t gain_step_sum;
static unsigned gain_steps;
static void wm8904_audio_avas_gain_step(int16_t v) { ++gain_steps; gain_step_sum += v; }
static void ev08p02a_avas_pot_keys_took_over(void) {}
static void ev08p02a_avas_pot_rearm(void) {}
static uint32_t fake_tick;
static uint32_t GetTicks(void) { return fake_tick; }
static bool s_avas_on, fading, rate_ok = true;
static uint8_t sounding_voice;
static bool avas_is_sounding(void) { return s_avas_on || fading; }
static bool avas_voice_set(uint8_t v) { sounding_voice = v; return !fading; }
static void avas_enable_set(bool v) {
    if (!v) { s_avas_on = false; fading = true; }
    else if (rate_ok) s_avas_on = true;
}

nora_pwm_audio_result_t nora_pwm_audio_backend_init(uint8_t ch, nora_pwm_audio_modulator_t *m)
{ (void)ch; return nora_pwm_audio_modulator_setup(m, PWM_PERIOD << PWM_FRAC_BITS, 2u << PWM_FRAC_BITS) ? NORA_PWM_AUDIO_OK : NORA_PWM_AUDIO_FAULT; }
nora_pwm_audio_result_t nora_pwm_audio_backend_start(void) { return NORA_PWM_AUDIO_OK; }
void nora_pwm_audio_backend_stop(void) {}
'''

CHECKS = r'''
static void controls(void)
{
    /* Two complete TY/OFF/LB/OFF cycles, blocked starts keep their turn. */
    rate_ok = false; wm8904_audio_avas_button(); assert(!s_avas_on);
    rate_ok = true;
    for (unsigned cycle=0; cycle<2; ++cycle) {
        for (unsigned voice=0; voice<2; ++voice) {
            wm8904_audio_avas_button(); assert(s_avas_on && sounding_voice == voice);
            wm8904_audio_avas_button(); assert(!s_avas_on && fading);
            wm8904_audio_avas_button(); assert(!s_avas_on);
            fading = false;
        }
    }
    ev08p02a_transport_console_hotkey(0x1b);
    ev08p02a_transport_console_hotkey('[');
    ev08p02a_transport_console_hotkey('A');
    assert(pitch_step == 10 && voice_keys == 0);
    ev08p02a_transport_console_hotkey('A'); assert(voice_keys == 1 && voice_key == 1);
    ev08p02a_transport_console_hotkey(0x1b);
    ev08p02a_transport_console_hotkey('O');
    ev08p02a_transport_console_hotkey('B'); assert(pitch_step == -10 && voice_keys == 1);
    ev08p02a_transport_console_hotkey(0x1b);
    fake_tick = 251; ev08p02a_transport_console_hotkey('a');
    assert(voice_keys == 2 && voice_key == 0);
    /* AVAS level trim: '-' lowers, '=' and '+' raise, 1 dB a press. */
    assert(ev08p02a_transport_console_hotkey('-'));
    assert(gain_steps == 1 && gain_step_sum == -1);
    assert(ev08p02a_transport_console_hotkey('='));
    assert(ev08p02a_transport_console_hotkey('+'));
    assert(gain_steps == 3 && gain_step_sum == 1 && voice_keys == 2);
    ev08p02a_button_state_t b = {0};
    assert(ev08p02a_button_update(&b, true, 10) == EV08P02A_BUTTON_NONE);
    assert(ev08p02a_button_update(&b, true, 24) == EV08P02A_BUTTON_NONE);
    assert(ev08p02a_button_update(&b, true, 25) == EV08P02A_BUTTON_NONE);
    assert(ev08p02a_button_update(&b, false, 80) == EV08P02A_BUTTON_NONE);
    assert(ev08p02a_button_update(&b, false, 95) == EV08P02A_BUTTON_SHORT);
    assert(ev08p02a_button_update(&b, false, 100) == EV08P02A_BUTTON_NONE);
    assert(ev08p02a_button_update(&b, true, 200) == EV08P02A_BUTTON_NONE);
    assert(ev08p02a_button_update(&b, true, 215) == EV08P02A_BUTTON_NONE);
    assert(ev08p02a_button_update(&b, true, 1714) == EV08P02A_BUTTON_NONE);
    assert(ev08p02a_button_update(&b, true, 1715) == EV08P02A_BUTTON_LONG);
    assert(ev08p02a_button_update(&b, true, 3000) == EV08P02A_BUTTON_NONE);
    assert(ev08p02a_button_update(&b, false, 3100) == EV08P02A_BUTTON_NONE);
    assert(ev08p02a_button_update(&b, false, 3115) == EV08P02A_BUTTON_NONE);
    memset(&b, 0, sizeof b);
    ev08p02a_button_update(&b, true, UINT32_MAX-20u);
    ev08p02a_button_update(&b, true, UINT32_MAX-5u);
    assert(ev08p02a_button_update(&b, true, 1494u) == EV08P02A_BUTTON_LONG);
    ev08p02a_led_state_t led = {0};
    assert(ev08p02a_led_update(&led, false, 0));
    assert(!ev08p02a_led_update(&led, false, 500));
    assert(ev08p02a_led_update(&led, false, 2000));
    assert(ev08p02a_led_update(&led, true, 2050));
    assert(!ev08p02a_led_update(&led, true, 2150));
    assert(ev08p02a_led_update(&led, true, 5050));
    assert(ev08p02a_led_update(&led, false, UINT32_MAX-20u));
    assert(!ev08p02a_led_update(&led, false, 479u));

    fault_pin = false; s_lab_mode = false;
    assert(stage_fault() == !EV08P02A_PWM_SCOPE_ONLY);
    s_lab_mode = true; assert(!stage_fault());
    app_console_msg_t q = {.name='s', .kind='?', .data_len=0, .status=-1};
    ev08p02a_classic_console_onmsg(&q);
    assert(q.status == APP_CONSOLE_OK && pitch_reports == 1 && gain_reports == 1);
    app_console_msg_t m = {.name='p', .kind='*', .data_len=2, .data={0x54,1}};
    mock_running = false; s_lab_mode = false;
    ev08p02a_classic_console_onmsg(&m);
    assert(m.status == APP_CONSOLE_ERR_BAD_DATA && !mock_running && !s_lab_mode);
    m.data[0] = 0x55; m.data[1] = 2;
    ev08p02a_classic_console_onmsg(&m);
    assert(m.status == APP_CONSOLE_ERR_BAD_DATA && !mock_running);
    m.data[1] = 1; ev08p02a_classic_console_onmsg(&m);
    assert(m.status == APP_CONSOLE_OK && mock_running && s_lab_mode);
    m.data[1] = 0; ev08p02a_classic_console_onmsg(&m);
    assert(m.status == APP_CONSOLE_OK && !mock_running && !s_lab_mode);
    m.data_len = 1; m.data[0] = 2;
    ev08p02a_classic_console_onmsg(&m);
    assert(EV08P02A_PWM_SCOPE_ONLY ? mock_neutral : m.status == APP_CONSOLE_ERR_BAD_DATA);
    /* *cp5901 injects one stage fault; any other 59xx is refused untouched. */
    app_console_msg_t f = {.name='p', .kind='*', .data_len=2, .data={0x59,0}};
    ev08p02a_classic_console_onmsg(&f);
    assert(f.status == APP_CONSOLE_ERR_BAD_DATA && injected == 0);
    f.data[1] = 1; ev08p02a_classic_console_onmsg(&f);
    assert(f.status == APP_CONSOLE_OK && injected == 1);
}

static void shaper(void)
{
    /* A constant fractional target: every order keeps the mean exact, and
     * order 2 never strays more than 2 counts from it. */
    for (uint8_t order = 0; order <= 2; ++order) {
        int32_t sum = 0;
        int16_t target = (int16_t)((PWM_NEUTRAL << PWM_FRAC_BITS) + 5);
        s_ns_order = order; s_ns_active = 99; ns_take_order();
        for (unsigned i = 0; i < 4096; ++i) {
            uint16_t d = ns_quantize(target);
            assert(abs((int)d - (int)PWM_NEUTRAL) <= 2);
            sum += d;
        }
        if (order) {
            double mean = (double)sum / 4096.0;
            double want = (double)target / (double)PWM_FRAC_ONE;
            assert(mean > want - 0.01 && mean < want + 0.01);
        }
    }
    /* End clamp: the error clamp keeps feedback bounded. */
    s_ns_order = 2; s_ns_active = 99; ns_take_order();
    for (unsigned i = 0; i < 100; ++i) assert(ns_quantize(0) == PWM_MIN_DUTY);
    assert(s_ns_e1 <= PWM_FRAC_ONE && s_ns_e1 >= -PWM_FRAC_ONE);
    s_ns_order = 2; s_ns_active = 99; ns_take_order();
    app_console_msg_t m = {.name='p', .kind='*', .data_len=2, .data={0x56,1}};
    ev08p02a_classic_console_onmsg(&m); assert(m.status == APP_CONSOLE_OK && mock_ns == 1);
    m.data[1] = 3; ev08p02a_classic_console_onmsg(&m); assert(m.status == APP_CONSOLE_ERR_OPERATION_FAILED);
    m.data[0] = 0x57; m.data[1] = 0xF4; ev08p02a_classic_console_onmsg(&m);
    assert(m.status == APP_CONSOLE_OK && mock_gain == -120);
    m.data[1] = 0x0C; ev08p02a_classic_console_onmsg(&m); assert(mock_gain == 120);
    m.data[1] = 0x7F; ev08p02a_classic_console_onmsg(&m); assert(m.status == APP_CONSOLE_ERR_OPERATION_FAILED);
    m.data[0] = 0x58; m.data[1] = 1; ev08p02a_classic_console_onmsg(&m); assert(m.status == APP_CONSOLE_OK && mock_test == 1);
    m.data[1] = 5; ev08p02a_classic_console_onmsg(&m); assert(m.status == APP_CONSOLE_ERR_OPERATION_FAILED);
    /* Internal tone: 48-sample period, peak = span/4, centred on neutral. */
    {
        int lo = 1 << 30, hi = -(1 << 30);
        for (unsigned i = 0; i < 48; ++i) {
            int v = test_tone_q();
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        int amp = (int)(((PWM_NEUTRAL - PWM_MIN_DUTY) << PWM_FRAC_BITS) / 4u);
        int mid = (int)(PWM_NEUTRAL << PWM_FRAC_BITS);
        assert(abs(hi - mid - amp) <= 1 && abs(mid - lo - amp) <= 1);
    }
}

static void drift(double producer_step)
{
    nora_pwm_audio_config_t cfg = {48000u, 32u, 1u};
    nora_pwm_audio_status_t st;
    int32_t pcm[32];
    for (unsigned i=0; i<32; ++i) pcm[i] = (i&1) ? INT32_MAX : INT32_MIN;
    nora_pwm_audio_block_t block = {pcm, NULL};
    for (unsigned i=0; i<PWM_DMA_WORDS; ++i) s_dma_words[i] = 0u;
    s_elastic_head = s_elastic_tail = s_elastic_phase_q16 = 0;
    s_elastic_primed = s_core_primed = false;
    s_core_taken_blocks = 0;
    s_elastic_delta_q16 = 0;
    s_elastic_underruns = 0;
    s_elastic_min = 255; s_elastic_max = 0;
    s_elastic_level_filtered_q8 = 112*256;
    assert(nora_pwm_audio_init(&cfg) == NORA_PWM_AUDIO_OK);
    assert(nora_pwm_audio_start() == NORA_PWM_AUDIO_OK);
    double phase = 0;
    for (unsigned half=0; half<30000; ++half) {
        phase += producer_step;
        while (phase >= 1.0) {
            assert(nora_pwm_audio_write(&block) == NORA_PWM_AUDIO_OK);
            phase -= 1.0;
        }
        refill_half((half&1) ? PWM_DMA_HALF : 0u);
        if (s_elastic_primed) {
            for (unsigned i=0; i<PWM_HALF; ++i) {
                unsigned off = ((half&1) ? PWM_DMA_HALF : 0u) + i*FACTOR;
                assert(s_dma_words[off] >= 2u && s_dma_words[off] <= PWM_PERIOD-2u);
                for (unsigned repeat=1; repeat<FACTOR; ++repeat) /* shaped: |2e1-e2| <= 3 counts each side */
                    assert(abs((int)s_dma_words[off] - (int)s_dma_words[off+repeat]) <= 7);
            }
        }
    }
    nora_pwm_audio_get_status(&st);
    assert(!st.rejected_blocks && !st.underrun_blocks && !s_elastic_underruns);
    assert(s_elastic_level > 60u && s_elastic_level < 160u);
    /* Real output starvation returns a whole neutral DMA half and reprimes. */
    for (unsigned i=0; i<12; ++i) refill_half(0);
    assert(s_elastic_underruns && !s_elastic_primed);
    for (unsigned i=0; i<PWM_DMA_HALF; ++i) assert(s_dma_words[i] == PWM_NEUTRAL);
    nora_pwm_audio_stop();
}
int main(void)
{
    controls();
    shaper();
    drift(0.997); drift(1.0); drift(1.003);
    printf("PASS CK bench factor=%u scope=%u: gestures, LED, commands, shaper, drift, starvation\n", FACTOR, EV08P02A_PWM_SCOPE_ONLY);
    return 0;
}
'''


def main():
    cc = find_cc(None)
    backend = (BOARD / 'ev08p02a_pwm_audio_hal.c').read_text(encoding='utf-8')
    app = (BOARD / 'main.c').read_text(encoding='utf-8')
    functions = '\n'.join(function(backend, f) for f in (
        'fill_neutral', 'stage_fault', 'ns_quantize', 'test_tone_q', 'store_duty', 'ns_take_order', 'stage_core_block',
        'refill_half', 'ev08p02a_pwm_audio_lab_control'))
    functions += '\n' + function(app, 'ev08p02a_classic_console_onmsg')
    functions += '\n' + function(app, 'ev08p02a_transport_console_hotkey')
    audio = (ROOT / 'src/app/wm8904_audio.c').read_text(encoding='utf-8')
    functions += '\n' + function(audio, 'wm8904_audio_avas_button')
    with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as tmp:
        tmp = Path(tmp)
        (tmp / 'nora_pwm_audio_conf.h').write_text('''
#include <stdint.h>
#define NORA_PWM_AUDIO_ENABLE 1
#define NORA_PWM_AUDIO_SAMPLE_RATE_HZ 48000u
#define NORA_PWM_AUDIO_FRAMES_PER_BLOCK 32u
#define NORA_PWM_AUDIO_MAX_CHANNELS 1u
#define NORA_PWM_AUDIO_FIFO_BLOCKS 4u
#define NORA_PWM_AUDIO_PRIME_BLOCKS 2u
#define NORA_PWM_AUDIO_DUTY_T uint16_t
''')
        c = tmp / 'test.c'
        c.write_text(PRELUDE + functions + CHECKS)
        for factor in (1, 2, 4):
            for scope in (0, 1):
                exe = tmp / 'test.exe'
                command = [cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                           f'-DFACTOR={factor}', f'-DEV08P02A_PWM_SCOPE_ONLY={scope}',
                           '-I', str(tmp), '-I', str(HAL), '-I', str(BOARD), str(c),
                           str(HAL / 'nora_pwm_audio.c'),
                           str(HAL / 'nora_pwm_audio_modulator.c'), '-o', str(exe)]
                result = subprocess.run(command, capture_output=True, text=True)
                if result.returncode:
                    sys.exit(result.stdout + result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True)
                print(result.stdout.strip())
                if result.returncode:
                    sys.exit(result.stderr or f'case failed: {factor}/{scope}')
    print('ALL PASS')


if __name__ == '__main__':
    main()
