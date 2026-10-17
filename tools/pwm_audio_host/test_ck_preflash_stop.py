"""Run CK *ts, codec stop, common PWM core and board stop with mocked HW.

The real function bodies are extracted unchanged. No COM/hardware access.
"""
from pathlib import Path
import subprocess
import tempfile

from run_pwm_audio_host_test import find_cc
from test_ck_pwm_bench import function, ROOT, BOARD, HAL


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nora_pwm_audio_backend.h"
#define EV08P02A_PWM_AUDIO 1
#define WM8904_AUDIO_USE_SYSTEM_START 0
#define EV08P02A_PWM_SLEEP_PIN 1
#define NORA_DMA_CHANNEL_2 2
#define NORA_PWM_GEN_4 4
enum { APP_CONSOLE_OK, APP_CONSOLE_ERR_BAD_PARM_LEN };
enum { WM8904_AUDIO_STOP_NONE, WM8904_AUDIO_STOP_MUTE_VERIFIED,
       WM8904_AUDIO_STOP_MUTE_UNVERIFIED };
typedef struct { char name, kind; uint8_t data_len; int status; } app_console_msg_t;
typedef struct { void (*output_stop)(void); } mock_port_t;
typedef struct { const mock_port_t *port; unsigned i2c_inst_legacy; } mock_config_t;
typedef struct { unsigned unused; } nora_spi_i2s_tdm_inst_t;
static const mock_config_t *s_cfg;
static bool s_started, s_button_prev, s_hw_up, s_released;
static unsigned s_stop_state, s_mute_request, s_mute_applied;
static unsigned s_rx_observe_seen, s_rx_observe_seq, s_rx_peak[2];
static unsigned s_rx_slot_mask, s_rx_activity, s_output_activity, s_pre_sat, s_post_sat;
static bool sleep_high, dma_on, irq_on, pg_on, override_low, mute_ok;
static char events[128], output[1024];
static unsigned reports;
static void event(char v) { size_t n = strlen(events); assert(n+1 < sizeof events); events[n]=v; }
static bool nora_gpio_clear(unsigned pin) { assert(pin==1); sleep_high=false; event('S'); return true; }
static bool nora_dma_channel_enable(unsigned ch, bool on) { assert(ch==2 && !on); dma_on=false; event('D'); return true; }
static void nora_dma_irq_enable(unsigned ch, bool on) { assert(ch==2 && !on); irq_on=false; event('I'); }
static bool nora_pwm_generator_override(unsigned pg, bool enable, bool h, bool l)
{ assert(pg==4 && enable && !h && !l); override_low=true; event('O'); return true; }
static bool nora_pwm_generator_run(unsigned pg, bool on) { assert(pg==4 && !on); pg_on=false; event('P'); return true; }
static bool wm8904_set_analog_output_mute(unsigned i2c, bool on)
{ assert(i2c==1 && on); assert(!sleep_high && !dma_on && !pg_on); event('M'); return mute_ok; }
static nora_spi_i2s_tdm_inst_t *nora_spi_i2s_tdm_spi1(void)
{ static nora_spi_i2s_tdm_inst_t inst; return &inst; }
static bool nora_spi_i2s_tdm_inst_stop(nora_spi_i2s_tdm_inst_t *inst)
{ assert(inst); assert(!sleep_high && !dma_on && !pg_on); event('T'); return true; }
static bool nora_spi_i2s_tdm_close(void) { event('C'); return true; }
static void report_transport_error(const char *p) { (void)p; assert(false); }
static void console_out_str(const char *p)
{ assert(strlen(output)+strlen(p) < sizeof output); strcat(output,p); }
static void wm8904_audio_stop_report(void) { ++reports; }
static void ev08p02a_pwm_audio_report(void) { ++reports; }
nora_pwm_audio_result_t nora_pwm_audio_backend_init(uint8_t ch, nora_pwm_audio_modulator_t *m)
{ assert(ch==1); return nora_pwm_audio_modulator_setup(m,1042,2) ? NORA_PWM_AUDIO_OK : NORA_PWM_AUDIO_FAULT; }
nora_pwm_audio_result_t nora_pwm_audio_backend_start(void)
{ s_hw_up=s_released=sleep_high=dma_on=irq_on=pg_on=true; override_low=false; return NORA_PWM_AUDIO_OK; }
'''

CHECKS = r'''
static void clear_trace(void) { memset(events,0,sizeof events); memset(output,0,sizeof output); }
static void started(void)
{
    const nora_pwm_audio_config_t cfg = {48000,32,1};
    assert(nora_pwm_audio_init(&cfg)==NORA_PWM_AUDIO_OK);
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    s_started=true; clear_trace();
}
static void stopped(void)
{
    static const int32_t pcm[32] = {0};
    const nora_pwm_audio_block_t block = {pcm,NULL};
    assert(nora_pwm_audio_write(&block)==NORA_PWM_AUDIO_NOT_READY);
    assert(!s_hw_up && !s_released);
    assert(!sleep_high && !dma_on && !irq_on && !pg_on && override_low);
}
int main(void)
{
    app_console_msg_t msg = {.name='s', .kind='*'};
    const mock_port_t port = {ev08p02a_pwm_audio_stop};
    const mock_config_t cfg = {&port,1};
    /* PWM-only lab start, no CODEC configuration. */
    s_cfg=NULL; started(); transport_stop(&msg); stopped();
    assert(strcmp(events,"SDIOPTC")==0);
    assert(s_stop_state==WM8904_AUDIO_STOP_MUTE_UNVERIFIED);
    assert(!strstr(output,"analog mute verified"));
    /* A repeated *ts still stops and closes without re-enabling any output. */
    clear_trace(); transport_stop(&msg); stopped(); assert(strcmp(events,"STC")==0);
    /* Ordinary playback, including duplicate output_stop via the codec port. */
    s_cfg=&cfg; mute_ok=true; started(); transport_stop(&msg); stopped();
    assert(strcmp(events,"SDIOPMSTC")==0);
    assert(strstr(output,"analog mute verified, TDM/DMA halted"));
    /* I2C mute/readback failure must not keep the speaker PWM running. */
    mute_ok=false; started(); transport_stop(&msg); stopped();
    assert(strcmp(events,"SDIOPMSTC")==0);
    assert(!strstr(output,"analog mute verified"));
    /* Query and malformed payload do not stop a playing stream. */
    started(); msg.kind='?'; reports=0; transport_stop(&msg);
    assert(pg_on && dma_on && sleep_high && !events[0] && reports==2);
    msg.kind='*'; msg.data_len=1; transport_stop(&msg);
    assert(msg.status==APP_CONSOLE_ERR_BAD_PARM_LEN && pg_on && !events[0]);
    msg.data_len=0; transport_stop(&msg); stopped();
    puts("PASS *ts: PWM-only / repeated / codec / mute-failure / query / bad payload");
    return 0;
}
'''


def main():
    backend = (BOARD / 'ev08p02a_pwm_audio_hal.c').read_text(encoding='utf-8')
    audio = (ROOT / 'src/app/wm8904_audio.c').read_text(encoding='utf-8')
    app = (BOARD / 'main.c').read_text(encoding='utf-8')
    # Extract the exact *ts branch into a small command harness.
    handler = function(app, 'ev08p02a_transport_console_onmsg')
    branch = handler[handler.index("    if (msg->name == 's')"):]
    depth = 1
    end = branch.index('{') + 1
    while depth:
        depth += (branch[end] == '{') - (branch[end] == '}')
        end += 1
    functions = function(backend, 'nora_pwm_audio_backend_stop')
    functions += '\n' + function(backend, 'ev08p02a_pwm_audio_stop')
    functions += '\n' + function(audio, 'wm8904_audio_stop')
    functions += '\nstatic void transport_stop(app_console_msg_t *msg) {\n' + branch[:end] + '\n}\n'
    with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as temp:
        temp = Path(temp)
        (temp / 'nora_pwm_audio_conf.h').write_text('''
#define NORA_PWM_AUDIO_ENABLE 1
#define NORA_PWM_AUDIO_SAMPLE_RATE_HZ 48000u
#define NORA_PWM_AUDIO_FRAMES_PER_BLOCK 32u
#define NORA_PWM_AUDIO_MAX_CHANNELS 1u
#define NORA_PWM_AUDIO_FIFO_BLOCKS 4u
#define NORA_PWM_AUDIO_PRIME_BLOCKS 2u
#define NORA_PWM_AUDIO_DUTY_T uint16_t
''')
        source = temp / 'stop.c'
        source.write_text(PRELUDE + functions + CHECKS)
        exe = temp / 'stop.exe'
        subprocess.run([find_cc(None), '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(temp), '-I', str(HAL), str(source),
                        str(HAL / 'nora_pwm_audio.c'), str(HAL / 'nora_pwm_audio_modulator.c'),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
