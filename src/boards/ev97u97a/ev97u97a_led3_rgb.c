/* EV97U97A audio port, adapted from the DM330030 board implementation. */
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

#ifndef DSPIC33CK_BOARD_EV97U97A
#error "boards/ev97u97a/ev97u97a_led3_rgb.c is EV97U97A-owned. Build it only in the CK1024MP710_EV97U97A configuration -- if it reached another one, fix the <item ex=...> exclusions in firmware.X/nbproject/configurations.xml."
#endif

#include "ev97u97a_led3_rgb.h"

#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "ev97u97a_pins.h"
#include "nora_gpio.h"
#if EV97U97A_RGB_USE_HARDWARE_PWM
#include "nora_pwm.h"
#endif

#define LED_ON  1
#define LED_OFF 0

#define INPUT  1
#define OUTPUT 0

#if !EV97U97A_RGB_USE_HARDWARE_PWM
struct LED {
    volatile uint16_t intensity;
    uint16_t modulation_count;
    unsigned int next_state;
    void (*pin_control)(unsigned int state);
};

static void ev97u97a_led3_red_pin_control(unsigned int state);
static void ev97u97a_led3_green_pin_control(unsigned int state);
static void ev97u97a_led3_blue_pin_control(unsigned int state);

static struct LED red = {0, 0, LED_OFF, &ev97u97a_led3_red_pin_control};
static struct LED green = {0, 0, LED_OFF, &ev97u97a_led3_green_pin_control};
static struct LED blue = {0, 0, LED_OFF, &ev97u97a_led3_blue_pin_control};
#else
static volatile uint16_t red;
static volatile uint16_t green;
static volatile uint16_t blue;
#endif

#define DEFAULT_INTENSITY 0x2000

#define EV97U97A_RGB_PWM_HZ          50000UL
#define EV97U97A_RGB_PWM_IRQ_PRIORITY 2u

#if EV97U97A_RGB_USE_HARDWARE_PWM
static uint16_t s_hw_period;

#if EV97U97A_RGB_PWM_DIAGNOSTIC_MODE == 1

#define EV97U97A_PWM_FORCE_PAD_HIGH(n)       \
    do {                                     \
        PG##n##IOCONLbits.OSYNC = 0x1u;      \
        PG##n##IOCONLbits.OVRDAT = 0x3u;    \
        PG##n##IOCONLbits.OVRENL = 1u;       \
        PG##n##IOCONLbits.OVRENH = 1u;       \
    } while (0)
#endif

#if EV97U97A_RGB_PWM_DIAGNOSTIC_MODE != 2
static uint16_t ev97u97a_led3_hw_duty(uint16_t intensity)
{
    return (uint16_t)(((uint32_t)intensity * s_hw_period + 127u) / 255u);
}
#endif
#endif

void ev97u97a_led3_red_set_intensity(uint16_t new_intensity)
{
#if !EV97U97A_RGB_USE_HARDWARE_PWM
    red.intensity = new_intensity >> 8;
#elif EV97U97A_RGB_PWM_DIAGNOSTIC_MODE == 2
    (void)new_intensity;
#else
    red = new_intensity >> 8;
    nora_pwm_generator_set_duty(NORA_PWM_GEN_2, ev97u97a_led3_hw_duty(red));
#endif
}

void ev97u97a_led3_red_on(void)
{
    ev97u97a_led3_red_set_intensity(DEFAULT_INTENSITY);
}

void ev97u97a_led3_red_off(void)
{
    ev97u97a_led3_red_set_intensity(0);
}

void ev97u97a_led3_red_toggle(void)
{
#if !EV97U97A_RGB_USE_HARDWARE_PWM
	if(red.intensity == 0)
#else
    if (red == 0u)
#endif
    {
        ev97u97a_led3_red_set_intensity(DEFAULT_INTENSITY);
    }
    else
    {
        ev97u97a_led3_red_set_intensity(0);
    }
}

#if !EV97U97A_RGB_USE_HARDWARE_PWM
static void ev97u97a_led3_red_pin_control(unsigned int state)
{

    (void)nora_gpio_write(EV97U97A_RGB_RED_PIN, state != 0u);
}
#endif

void ev97u97a_led3_green_set_intensity(uint16_t new_intensity)
{
#if !EV97U97A_RGB_USE_HARDWARE_PWM
    green.intensity = new_intensity >> 8;
#elif EV97U97A_RGB_PWM_DIAGNOSTIC_MODE == 2
    (void)new_intensity;
#else
    green = new_intensity >> 8;
    nora_pwm_generator_set_duty(NORA_PWM_GEN_3, ev97u97a_led3_hw_duty(green));
#endif
}

void ev97u97a_led3_green_on(void)
{
    ev97u97a_led3_green_set_intensity(DEFAULT_INTENSITY);
}

void ev97u97a_led3_green_off(void)
{
    ev97u97a_led3_green_set_intensity(0);
}

void ev97u97a_led3_green_toggle(void)
{
#if !EV97U97A_RGB_USE_HARDWARE_PWM
	if(green.intensity == 0)
#else
    if (green == 0u)
#endif
    {
        ev97u97a_led3_green_set_intensity(DEFAULT_INTENSITY);
    }
    else
    {
        ev97u97a_led3_green_set_intensity(0);
    }
}

#if !EV97U97A_RGB_USE_HARDWARE_PWM
static void ev97u97a_led3_green_pin_control(unsigned int state)
{

    (void)nora_gpio_write(EV97U97A_RGB_GREEN_PIN, state != 0u);
}
#endif

void ev97u97a_led3_blue_set_intensity(uint16_t new_intensity)
{
#if !EV97U97A_RGB_USE_HARDWARE_PWM
    blue.intensity = new_intensity >> 8;
#elif EV97U97A_RGB_PWM_DIAGNOSTIC_MODE == 2
    (void)new_intensity;
#else
    blue = new_intensity >> 8;
    nora_pwm_generator_set_duty(NORA_PWM_GEN_4, ev97u97a_led3_hw_duty(blue));
#endif
}

void ev97u97a_led3_blue_on(void)
{
    ev97u97a_led3_blue_set_intensity(DEFAULT_INTENSITY);
}

void ev97u97a_led3_blue_off(void)
{
    ev97u97a_led3_blue_set_intensity(0);
}

void ev97u97a_led3_blue_toggle(void)
{
#if !EV97U97A_RGB_USE_HARDWARE_PWM
	if(blue.intensity == 0)
#else
    if (blue == 0u)
#endif
    {
        ev97u97a_led3_blue_set_intensity(DEFAULT_INTENSITY);
    }
    else
    {
        ev97u97a_led3_blue_set_intensity(0);
    }
}

#if !EV97U97A_RGB_USE_HARDWARE_PWM
static void ev97u97a_led3_blue_pin_control(unsigned int state)
{

    (void)nora_gpio_write(EV97U97A_RGB_BLUE_PIN, state != 0u);
}
#endif

void ev97u97a_led3_rgb_on(void)
{
    ev97u97a_led3_red_on();
    ev97u97a_led3_blue_on();
    ev97u97a_led3_green_on();
}

void ev97u97a_led3_rgb_off(void)
{
    ev97u97a_led3_red_off();
    ev97u97a_led3_blue_off();
    ev97u97a_led3_green_off();
}

void ev97u97a_led3_rgb_toggle(void)
{
    ev97u97a_led3_red_toggle();
    ev97u97a_led3_blue_toggle();
    ev97u97a_led3_green_toggle();
}

void ev97u97a_led3_rgb_set_color(uint16_t red, uint16_t green, uint16_t blue)
{
    ev97u97a_led3_red_set_intensity(red);
    ev97u97a_led3_green_set_intensity(green);
    ev97u97a_led3_blue_set_intensity(blue);
}

static uint8_t ev97u97a_led3_percent_to_intensity(uint8_t percent)
{
    if (percent > 100u) {
        percent = 100u;
    }

    return (uint8_t)(((uint16_t)percent * 255u + 50u) / 100u);
}

static uint8_t ev97u97a_led3_intensity_to_percent(uint16_t intensity)
{
    if (intensity > 255u) {
        intensity = 255u;
    }

    return (uint8_t)((intensity * 100u + 127u) / 255u);
}

void ev97u97a_led3_rgb_set_percent(uint8_t red_percent,
                                    uint8_t green_percent,
                                    uint8_t blue_percent)
{
    ev97u97a_led3_rgb_set_color(
        (uint16_t)ev97u97a_led3_percent_to_intensity(red_percent) << 8,
        (uint16_t)ev97u97a_led3_percent_to_intensity(green_percent) << 8,
        (uint16_t)ev97u97a_led3_percent_to_intensity(blue_percent) << 8);
}

void ev97u97a_led3_rgb_get_percent(uint8_t *red_percent,
                                    uint8_t *green_percent,
                                    uint8_t *blue_percent)
{
    if (red_percent != NULL) {
#if !EV97U97A_RGB_USE_HARDWARE_PWM
        *red_percent = ev97u97a_led3_intensity_to_percent(red.intensity);
#else
        *red_percent = ev97u97a_led3_intensity_to_percent(red);
#endif
    }
    if (green_percent != NULL) {
#if !EV97U97A_RGB_USE_HARDWARE_PWM
        *green_percent = ev97u97a_led3_intensity_to_percent(green.intensity);
#else
        *green_percent = ev97u97a_led3_intensity_to_percent(green);
#endif
    }
    if (blue_percent != NULL) {
#if !EV97U97A_RGB_USE_HARDWARE_PWM
        *blue_percent = ev97u97a_led3_intensity_to_percent(blue.intensity);
#else
        *blue_percent = ev97u97a_led3_intensity_to_percent(blue);
#endif
    }
}

//https://www.embeddedrelated.com/showarticle/107.php
#define FULL_SCALE_MODULATION_COUNT 285

bool ev97u97a_led3_rgb_init(uint32_t fcy_hz)
{
#if EV97U97A_RGB_USE_HARDWARE_PWM
    const uint32_t period = fcy_hz / EV97U97A_RGB_PWM_HZ;
    const nora_pwm_generator_cfg_t cfg = {
        .period = (uint16_t)period,
        .duty_init = 0u,
        .pen_h = true,
        .pen_l = false,
        .low_active = false,
        .output_mode = NORA_PWM_MODE_INDEPENDENT,
        .is_module_master = false,
    };

    if ((fcy_hz == 0u) || ((fcy_hz % EV97U97A_RGB_PWM_HZ) != 0u) ||
        (period == 0u) || (period > 65536UL) ||
        !nora_pwm_module_init(NORA_PWM_MCLK_STANDARD)) {
        return false;
    }

    s_hw_period = (uint16_t)period;

    if (!nora_pwm_generator_init(NORA_PWM_GEN_2, &cfg) ||
        !nora_pwm_generator_init(NORA_PWM_GEN_3, &cfg) ||
        !nora_pwm_generator_init(NORA_PWM_GEN_4, &cfg)) {
        return false;
    }

#if EV97U97A_RGB_PWM_DIAGNOSTIC_MODE == 1
    EV97U97A_PWM_FORCE_PAD_HIGH(2);
    EV97U97A_PWM_FORCE_PAD_HIGH(3);
    EV97U97A_PWM_FORCE_PAD_HIGH(4);
#elif EV97U97A_RGB_PWM_DIAGNOSTIC_MODE == 2

    nora_pwm_generator_set_duty(NORA_PWM_GEN_2, s_hw_period / 2u);
    nora_pwm_generator_set_duty(NORA_PWM_GEN_3, s_hw_period / 2u);
    nora_pwm_generator_set_duty(NORA_PWM_GEN_4, s_hw_period / 2u);
#endif
    return true;
#else
#if defined(CCP2CON1L) && defined(CCP2CON1H) && defined(CCP2CON2L) && \
    defined(CCP2CON2H) && defined(CCP2CON3H) && defined(CCP2TMRL) && \
    defined(CCP2PRL) && defined(_CCT2IF) && defined(_CCT2IE) && \
    defined(_CCT2IP)
    const uint32_t counts = fcy_hz / EV97U97A_RGB_PWM_HZ;

    if ((fcy_hz == 0u) ||
        ((fcy_hz % EV97U97A_RGB_PWM_HZ) != 0u) ||
        (counts == 0u) ||
        (counts > 65536UL)) {
        return false;
    }

#if defined(_CCP2MD)
    _CCP2MD = 0;
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

    CCP2CON1Lbits.CCSEL   = 0u;
    CCP2CON1Lbits.MOD     = 0b0000;
    CCP2CON1Lbits.T32     = 0u;
    CCP2CON1Lbits.TMRPS   = 0b00;
    CCP2CON1Lbits.CLKSEL  = 0b000;
    CCP2CON1Lbits.CCPSIDL = 0u;

    _CCT2IP = EV97U97A_RGB_PWM_IRQ_PRIORITY;
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

void ev97u97a_led3_rgb_print_hw_pwm_diagnostic(void)
{
#if EV97U97A_RGB_USE_HARDWARE_PWM
#define EV97U97A_PWM_DUMP_GENERATOR(n)                                       \
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
            (unsigned int)EV97U97A_RGB_PWM_DIAGNOSTIC_MODE,
            (unsigned int)red, (unsigned int)green, (unsigned int)blue,
            (unsigned int)s_hw_period);
    EV97U97A_PWM_DUMP_GENERATOR(2);
    EV97U97A_PWM_DUMP_GENERATOR(3);
    EV97U97A_PWM_DUMP_GENERATOR(4);
#undef EV97U97A_PWM_DUMP_GENERATOR
#else
    printf("PWM RGB: software backend selected; PG dump not applicable\n");
#endif
}

#if EV97U97A_RGB_USE_HARDWARE_PWM && (EV97U97A_RGB_PWM_DIAGNOSTIC_MODE == 1)
#undef EV97U97A_PWM_FORCE_PAD_HIGH
#endif

#if EV97U97A_RGB_USE_HARDWARE_PWM

void ev97u97a_led3_rgb_tick(void)
{
}
#else
static void ModulateLEDNextStateSet(struct LED *led)
{

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

void ev97u97a_led3_rgb_tick(void)
{
    SetLEDState(&red);
    SetLEDState(&blue);
    SetLEDState(&green);

    ModulateLEDNextStateSet(&red);
    ModulateLEDNextStateSet(&blue);
    ModulateLEDNextStateSet(&green);
}

void __attribute__((__interrupt__, auto_psv)) _CCT2Interrupt(void)
{
    _CCT2IF = 0;
    ev97u97a_led3_rgb_tick();
}
#endif
