/*******************************************************************************
Copyright 2016 Microchip Technology Inc. (www.microchip.com)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*******************************************************************************/

/*
 * Modified by SulaoLab, 2026 (Apache-2.0 section 4(b)). Changes to Microchip's
 * original: renamed into this board's namespace (was bsp/led3_rgb.c), the three
 * pin writes go through the GPIO HAL instead of LATx/TRISx, ANSEL is owned per
 * pin, the ownership assert below was added, and two defects that were only
 * correct at one clock point were fixed. See the notes further down for why.
 */

#ifndef DSPIC33CK_BOARD_DM330030
#error "boards/dm330030/dm330030_led3_rgb.c is DM330030-owned. Build it only in the CK256MP508_DM330030 configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "dm330030_led3_rgb.h"

#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * The three pin writes below go through the GPIO HAL rather than LATx/TRISx, for
 * the reason spelled out in dm330030_io.h: dm330030_board.c's
 * dm330030_user_io_pins_init() already configures DM330030_RGB_{RED,GREEN,BLUE}_PIN, so a
 * second place asserting direction makes two owners of one pin. As written before, each
 * pin_control() rewrote TRIS on every call -- and this one is called from the 50 kHz PWM
 * tick, so it was doing that thousands of times a second.
 *
 * The CK256MP508 pin-function table lists the RGB pins as RE13/RE14/RE15 only,
 * without an RP/PPS alias, so a conventional hardware PWM output cannot reach
 * this LED without a board change. Do not infer that from the `RE` spelling:
 * dsPIC33AK devices use port and RP names as two aliases for the same physical
 * pin; this is a specific CK-pin-table fact. SCCP2 is nevertheless a
 * useful hardware timebase: its 50 kHz timer event advances the three software
 * PWM accumulators below. At the blue end of the POT ramp the newly introduced
 * red/green terms can be as small as 2/285. A 4 kHz PDM carrier put those terms
 * at about 28 Hz, visibly flashing green against the steady blue. At 50 kHz the
 * same first non-zero term is about 350 Hz. This approaches Sonora's 96 kHz
 * hardware-PWM carrier without assigning its interrupt-level CPU cost, while the 1 ms application clock and
 * its clients remain completely untouched.
 */
#include "dm330030_pins.h"
#include "nora_gpio.h"
#if DM330030_RGB_USE_HARDWARE_PWM
#include "nora_pwm.h"
#endif

/* This module uses 3 dimmable LEDs (red, green, blue).  It uses them to
 * implement one color changing LED.
 */

#define LED_ON  1
#define LED_OFF 0

#define INPUT  1
#define OUTPUT 0

#if !DM330030_RGB_USE_HARDWARE_PWM
struct LED {
    volatile uint16_t intensity;
    uint16_t modulation_count;
    unsigned int next_state;
    void (*pin_control)(unsigned int state);
};

static void dm330030_led3_red_pin_control(unsigned int state);
static void dm330030_led3_green_pin_control(unsigned int state);
static void dm330030_led3_blue_pin_control(unsigned int state);

static struct LED red = {0, 0, LED_OFF, &dm330030_led3_red_pin_control};
static struct LED green = {0, 0, LED_OFF, &dm330030_led3_green_pin_control};
static struct LED blue = {0, 0, LED_OFF, &dm330030_led3_blue_pin_control};
#else
static volatile uint16_t red;
static volatile uint16_t green;
static volatile uint16_t blue;
#endif

#define DEFAULT_INTENSITY 0x2000

#define DM330030_RGB_PWM_HZ          50000UL
#define DM330030_RGB_PWM_IRQ_PRIORITY 2u

#if DM330030_RGB_USE_HARDWARE_PWM
static uint16_t s_hw_period;

#if DM330030_RGB_PWM_DIAGNOSTIC_MODE == 1
/* Override is below the PWM generator and above its waveform logic: this is a
 * direct electrical test of the output pads, not another PWM configuration. */
#define DM330030_PWM_FORCE_PAD_HIGH(n)       \
    do {                                     \
        PG##n##IOCONLbits.OSYNC = 0x1u;      \
        PG##n##IOCONLbits.OVRDAT = 0x3u;    \
        PG##n##IOCONLbits.OVRENL = 1u;       \
        PG##n##IOCONLbits.OVRENH = 1u;       \
    } while (0)
#endif

#if DM330030_RGB_PWM_DIAGNOSTIC_MODE != 2
static uint16_t dm330030_led3_hw_duty(uint16_t intensity)
{
    return (uint16_t)(((uint32_t)intensity * s_hw_period + 127u) / 255u);
}
#endif
#endif

void dm330030_led3_red_set_intensity(uint16_t new_intensity)
{  
#if !DM330030_RGB_USE_HARDWARE_PWM
    red.intensity = new_intensity >> 8;
#elif DM330030_RGB_PWM_DIAGNOSTIC_MODE == 2
    (void)new_intensity; /* 50% waveform is deliberately fixed for the scope. */
#else
    red = new_intensity >> 8;
    nora_pwm_generator_set_duty(NORA_PWM_GEN_1, dm330030_led3_hw_duty(red));
#endif
}

void dm330030_led3_red_on(void)
{
    dm330030_led3_red_set_intensity(DEFAULT_INTENSITY);
}

void dm330030_led3_red_off(void)
{
    dm330030_led3_red_set_intensity(0);
}

void dm330030_led3_red_toggle(void)
{
#if !DM330030_RGB_USE_HARDWARE_PWM
	if(red.intensity == 0)
#else
    if (red == 0u)
#endif
    {
        dm330030_led3_red_set_intensity(DEFAULT_INTENSITY);
    }
    else
    {
        dm330030_led3_red_set_intensity(0);
    }
}

#if !DM330030_RGB_USE_HARDWARE_PWM
static void dm330030_led3_red_pin_control(unsigned int state)
{
    /* Direction is dm330030_board.c's; this only drives the level. */
    (void)nora_gpio_write(DM330030_RGB_RED_PIN, state != 0u);
}
#endif

void dm330030_led3_green_set_intensity(uint16_t new_intensity)
{  
#if !DM330030_RGB_USE_HARDWARE_PWM
    green.intensity = new_intensity >> 8;
#elif DM330030_RGB_PWM_DIAGNOSTIC_MODE == 2
    (void)new_intensity; /* 50% waveform is deliberately fixed for the scope. */
#else
    green = new_intensity >> 8;
    nora_pwm_generator_set_duty(NORA_PWM_GEN_2, dm330030_led3_hw_duty(green));
#endif
}

void dm330030_led3_green_on(void)
{
    dm330030_led3_green_set_intensity(DEFAULT_INTENSITY);
}

void dm330030_led3_green_off(void)
{
    dm330030_led3_green_set_intensity(0);
}

void dm330030_led3_green_toggle(void)
{
#if !DM330030_RGB_USE_HARDWARE_PWM
	if(green.intensity == 0)
#else
    if (green == 0u)
#endif
    {
        dm330030_led3_green_set_intensity(DEFAULT_INTENSITY);
    }
    else
    {
        dm330030_led3_green_set_intensity(0);
    }
}

#if !DM330030_RGB_USE_HARDWARE_PWM
static void dm330030_led3_green_pin_control(unsigned int state)
{
    /* Direction is dm330030_board.c's; this only drives the level. */
    (void)nora_gpio_write(DM330030_RGB_GREEN_PIN, state != 0u);
}
#endif

void dm330030_led3_blue_set_intensity(uint16_t new_intensity)
{      
#if !DM330030_RGB_USE_HARDWARE_PWM
    blue.intensity = new_intensity >> 8;
#elif DM330030_RGB_PWM_DIAGNOSTIC_MODE == 2
    (void)new_intensity; /* 50% waveform is deliberately fixed for the scope. */
#else
    blue = new_intensity >> 8;
    nora_pwm_generator_set_duty(NORA_PWM_GEN_3, dm330030_led3_hw_duty(blue));
#endif
}

void dm330030_led3_blue_on(void)
{
    dm330030_led3_blue_set_intensity(DEFAULT_INTENSITY);
}

void dm330030_led3_blue_off(void)
{
    dm330030_led3_blue_set_intensity(0);
}

void dm330030_led3_blue_toggle(void)
{
#if !DM330030_RGB_USE_HARDWARE_PWM
	if(blue.intensity == 0)
#else
    if (blue == 0u)
#endif
    {
        dm330030_led3_blue_set_intensity(DEFAULT_INTENSITY);
    }
    else
    {
        dm330030_led3_blue_set_intensity(0);
    }
}

#if !DM330030_RGB_USE_HARDWARE_PWM
static void dm330030_led3_blue_pin_control(unsigned int state)
{
    /* Direction is dm330030_board.c's; this only drives the level. */
    (void)nora_gpio_write(DM330030_RGB_BLUE_PIN, state != 0u);
}
#endif

void dm330030_led3_rgb_on(void)
{
    dm330030_led3_red_on();
    dm330030_led3_blue_on();
    dm330030_led3_green_on();  
}

void dm330030_led3_rgb_off(void)
{
    dm330030_led3_red_off();
    dm330030_led3_blue_off();
    dm330030_led3_green_off();  
}

void dm330030_led3_rgb_toggle(void)
{
    dm330030_led3_red_toggle();
    dm330030_led3_blue_toggle();
    dm330030_led3_green_toggle();  
}

void dm330030_led3_rgb_set_color(uint16_t red, uint16_t green, uint16_t blue)
{
    dm330030_led3_red_set_intensity(red);
    dm330030_led3_green_set_intensity(green);
    dm330030_led3_blue_set_intensity(blue);
}

static uint8_t dm330030_led3_percent_to_intensity(uint8_t percent)
{
    if (percent > 100u) {
        percent = 100u;
    }

    /* Round rather than truncate: 100% must reach 255 and a stated 1% survives. */
    return (uint8_t)(((uint16_t)percent * 255u + 50u) / 100u);
}

static uint8_t dm330030_led3_intensity_to_percent(uint16_t intensity)
{
    if (intensity > 255u) {
        intensity = 255u;
    }

    return (uint8_t)((intensity * 100u + 127u) / 255u);
}

void dm330030_led3_rgb_set_percent(uint8_t red_percent,
                                    uint8_t green_percent,
                                    uint8_t blue_percent)
{
    dm330030_led3_rgb_set_color(
        (uint16_t)dm330030_led3_percent_to_intensity(red_percent) << 8,
        (uint16_t)dm330030_led3_percent_to_intensity(green_percent) << 8,
        (uint16_t)dm330030_led3_percent_to_intensity(blue_percent) << 8);
}

void dm330030_led3_rgb_get_percent(uint8_t *red_percent,
                                    uint8_t *green_percent,
                                    uint8_t *blue_percent)
{
    if (red_percent != NULL) {
#if !DM330030_RGB_USE_HARDWARE_PWM
        *red_percent = dm330030_led3_intensity_to_percent(red.intensity);
#else
        *red_percent = dm330030_led3_intensity_to_percent(red);
#endif
    }
    if (green_percent != NULL) {
#if !DM330030_RGB_USE_HARDWARE_PWM
        *green_percent = dm330030_led3_intensity_to_percent(green.intensity);
#else
        *green_percent = dm330030_led3_intensity_to_percent(green);
#endif
    }
    if (blue_percent != NULL) {
#if !DM330030_RGB_USE_HARDWARE_PWM
        *blue_percent = dm330030_led3_intensity_to_percent(blue.intensity);
#else
        *blue_percent = dm330030_led3_intensity_to_percent(blue);
#endif
    }
}


//https://www.embeddedrelated.com/showarticle/107.php
#define FULL_SCALE_MODULATION_COUNT 285

/*
 * SCCP2's timer event is deliberately lower priority than the TDM DMA block
 * ISR (priority 4). It cannot extend the measured audio block execution time
 * or make an audio deadline late; a pending RGB update simply runs afterwards.
 * CCP1 remains the high-resolution audio-load counter, and no other profile
 * builds this board-owned source file.
 */
bool dm330030_led3_rgb_init(uint32_t fcy_hz)
{
#if DM330030_RGB_USE_HARDWARE_PWM
    const uint32_t period = fcy_hz / DM330030_RGB_PWM_HZ;
    const nora_pwm_generator_cfg_t cfg = {
        .period = (uint16_t)period,
        .duty_init = 0u,
        .pen_h = true,
        .pen_l = false,
        .low_active = false,
        .output_mode = NORA_PWM_MODE_INDEPENDENT,
        .is_module_master = false,
    };

    if ((fcy_hz == 0u) || ((fcy_hz % DM330030_RGB_PWM_HZ) != 0u) ||
        (period == 0u) || (period > 65536UL) ||
        !nora_pwm_module_init(NORA_PWM_MCLK_STANDARD)) {
        return false;
    }

    s_hw_period = (uint16_t)period;

    if (!nora_pwm_generator_init(NORA_PWM_GEN_1, &cfg) ||
        !nora_pwm_generator_init(NORA_PWM_GEN_2, &cfg) ||
        !nora_pwm_generator_init(NORA_PWM_GEN_3, &cfg)) {
        return false;
    }

#if DM330030_RGB_PWM_DIAGNOSTIC_MODE == 1
    DM330030_PWM_FORCE_PAD_HIGH(1);
    DM330030_PWM_FORCE_PAD_HIGH(2);
    DM330030_PWM_FORCE_PAD_HIGH(3);
#elif DM330030_RGB_PWM_DIAGNOSTIC_MODE == 2
    /* RB14/RB12/RB10 are the same PWM1H/2H/3H pads used by the J2 bridge. */
    nora_pwm_generator_set_duty(NORA_PWM_GEN_1, s_hw_period / 2u);
    nora_pwm_generator_set_duty(NORA_PWM_GEN_2, s_hw_period / 2u);
    nora_pwm_generator_set_duty(NORA_PWM_GEN_3, s_hw_period / 2u);
#endif
    return true;
#else
#if defined(CCP2CON1L) && defined(CCP2CON1H) && defined(CCP2CON2L) && \
    defined(CCP2CON2H) && defined(CCP2CON3H) && defined(CCP2TMRL) && \
    defined(CCP2PRL) && defined(_CCT2IF) && defined(_CCT2IE) && \
    defined(_CCT2IP)
    const uint32_t counts = fcy_hz / DM330030_RGB_PWM_HZ;

    if ((fcy_hz == 0u) ||
        ((fcy_hz % DM330030_RGB_PWM_HZ) != 0u) ||
        (counts == 0u) ||
        (counts > 65536UL)) {
        return false;
    }

#if defined(_CCP2MD)
    _CCP2MD = 0;                 /* enable the unused SCCP2 peripheral */
#endif

    _CCT2IE = 0;
    CCP2CON1Lbits.CCPON = 0;
    _CCT2IF = 0;

    CCP2CON1L = 0u;
    CCP2CON1H = 0u;
    CCP2CON2L = 0u;
    CCP2CON2H = 0u;
    CCP2CON3H = 0u;
    CCP2TMRL = 0u;
    CCP2PRL = (uint16_t)(counts - 1u);

    CCP2CON1Lbits.CCSEL   = 0u;    /* timer/output-compare mode */
    CCP2CON1Lbits.MOD     = 0b0000;/* plain periodic timer       */
    CCP2CON1Lbits.T32     = 0u;    /* 16-bit is sufficient here  */
    CCP2CON1Lbits.TMRPS   = 0b00;  /* Fcy / 1                    */
    CCP2CON1Lbits.CLKSEL  = 0b000; /* peripheral clock = Fcy     */
    CCP2CON1Lbits.CCPSIDL = 0u;    /* keep PWM active in Idle    */

    _CCT2IP = DM330030_RGB_PWM_IRQ_PRIORITY;
    _CCT2IF = 0;
    _CCT2IE = 1;
    CCP2CON1Lbits.CCPON = 1;

    return true;
#else
    (void)fcy_hz;
    return false;
#endif
#endif
}

void dm330030_led3_rgb_print_hw_pwm_diagnostic(void)
{
#if DM330030_RGB_USE_HARDWARE_PWM
#define DM330030_PWM_DUMP_GENERATOR(n)                                       \
    printf("PWM PG" #n " A: CONL=%04X CONH=%04X STAT=%04X IOCONL=%04X "    \
            "IOCONH=%04X EVTL=%04X EVTH=%04X\n",                              \
           (unsigned int)PG##n##CONL, (unsigned int)PG##n##CONH,              \
           (unsigned int)PG##n##STAT, (unsigned int)PG##n##IOCONL,            \
           (unsigned int)PG##n##IOCONH, (unsigned int)PG##n##EVTL,            \
           (unsigned int)PG##n##EVTH);                                       \
    printf("PWM PG" #n " B: FPCIL=%04X FPCIH=%04X CLPCIL=%04X CLPCIH=%04X "\
            "FFPCIL=%04X FFPCIH=%04X SPCIL=%04X SPCIH=%04X\n",                \
           (unsigned int)PG##n##FPCIL, (unsigned int)PG##n##FPCIH,            \
           (unsigned int)PG##n##CLPCIL, (unsigned int)PG##n##CLPCIH,          \
           (unsigned int)PG##n##FFPCIL, (unsigned int)PG##n##FFPCIH,          \
           (unsigned int)PG##n##SPCIL, (unsigned int)PG##n##SPCIH);           \
    printf("PWM PG" #n " C: LEBL=%04X LEBH=%04X PHASE=%04X DC=%04X "       \
            "DCA=%04X PER=%04X TRIGA=%04X TRIGB=%04X TRIGC=%04X\n",           \
           (unsigned int)PG##n##LEBL, (unsigned int)PG##n##LEBH,              \
           (unsigned int)PG##n##PHASE, (unsigned int)PG##n##DC,               \
           (unsigned int)PG##n##DCA, (unsigned int)PG##n##PER,                \
           (unsigned int)PG##n##TRIGA, (unsigned int)PG##n##TRIGB,            \
           (unsigned int)PG##n##TRIGC)

    printf("PWM RGB: PMD1=%04X PCLKCON=%04X MPER=%04X MDC=%04X "
            "TRISB=%04X LATB=%04X PORTB=%04X RPOR5=%04X RPOR6=%04X "
            "RPOR7=%04X RPCON=%04X diag_mode=%u rgb=(%u,%u,%u) period=%u\n",
            (unsigned int)PMD1, (unsigned int)PCLKCON, (unsigned int)MPER,
            (unsigned int)MDC, (unsigned int)TRISB, (unsigned int)LATB,
            (unsigned int)PORTB, (unsigned int)RPOR5, (unsigned int)RPOR6,
            (unsigned int)RPOR7, (unsigned int)RPCON,
            (unsigned int)DM330030_RGB_PWM_DIAGNOSTIC_MODE,
            (unsigned int)red, (unsigned int)green, (unsigned int)blue,
            (unsigned int)s_hw_period);
    DM330030_PWM_DUMP_GENERATOR(1);
    DM330030_PWM_DUMP_GENERATOR(2);
    DM330030_PWM_DUMP_GENERATOR(3);
#undef DM330030_PWM_DUMP_GENERATOR
#else
    printf("PWM RGB: software backend selected; PG dump not applicable\n");
#endif
}

#if DM330030_RGB_USE_HARDWARE_PWM && (DM330030_RGB_PWM_DIAGNOSTIC_MODE == 1)
#undef DM330030_PWM_FORCE_PAD_HIGH
#endif

#if DM330030_RGB_USE_HARDWARE_PWM
/* Kept as a benign fallback target for the legacy 1 ms registry.  A PG-backed
 * RGB engine has no per-tick work. */
void dm330030_led3_rgb_tick(void)
{
}
#else
static void ModulateLEDNextStateSet(struct LED *led)
{
    /*
     * Do not quantise low values. The inherited OOB code turned 1..9 into
     * off and 10..17 into 18, creating visible blue->white and white->red
     * steps. At 4 kHz the POT indicator's dimmest normal level (12) is a
     * 168 Hz pulse train; deliberately requested levels below that remain
     * exact for calibration rather than being silently brightened or removed.
     */
    led->modulation_count += led->intensity;
    if(led->modulation_count < FULL_SCALE_MODULATION_COUNT)
    {
        led->next_state = LED_OFF;
    }
    else
    {
        led->next_state = LED_ON;
        led->modulation_count -= FULL_SCALE_MODULATION_COUNT;
    }
}

static void SetLEDState(struct LED *led)
{
    led->pin_control(led->next_state);
}

void dm330030_led3_rgb_tick(void)
{
    SetLEDState(&red);
    SetLEDState(&blue);
    SetLEDState(&green);
    
    ModulateLEDNextStateSet(&red);
    ModulateLEDNextStateSet(&blue);
    ModulateLEDNextStateSet(&green);
}

/* CCP2 Timer Event -- generated at 4 kHz by dm330030_led3_rgb_init(). */
void __attribute__((__interrupt__, auto_psv)) _CCT2Interrupt(void)
{
    _CCT2IF = 0;
    dm330030_led3_rgb_tick();
}
#endif /* DM330030_RGB_USE_HARDWARE_PWM */


