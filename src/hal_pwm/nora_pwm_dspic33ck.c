/* SPDX-License-Identifier: MIT-0 */

/* dsPIC33CK implementation of the small NORA PWM (PG) HAL. */

#include <xc.h>
#include <stddef.h>

#include "nora_pwm.h"

static bool s_module_initialized;
static nora_pwm_mclk_source_t s_module_source;
static bool s_generator_ready[NORA_PWM_GEN_COUNT];
static uint16_t s_generator_period[NORA_PWM_GEN_COUNT];

#define NORA_PWM_DEFINE_GENERATOR(n)                                          \
static void nora_pwm_pg##n##_configure(const nora_pwm_generator_cfg_t *cfg)    \
{                                                                              \
    PG##n##CONL = 0u;                                                          \
    PG##n##CONH = 0u;                                                          \
    PG##n##IOCONL = 0u;                                                        \
    PG##n##IOCONH = 0u;                                                        \
    PG##n##CONLbits.CLKSEL = 0x1u; /* module master clock */                  \
    /* PMOD=Independent requires the matching Dual PWM timing mode: DC drives
     * PWMxH and TRIGB drives PWMxL.  MODSEL=000 is single-output Independent
     * Edge mode, where a configured second output otherwise remains low. */  \
    PG##n##CONLbits.MODSEL =                                                  \
        (cfg->output_mode == NORA_PWM_MODE_INDEPENDENT) ? 0x2u : 0x0u;         \
    PG##n##CONHbits.SOCS = 0u;    /* self-triggered */                        \
    PG##n##CONHbits.UPDMOD = 0u;  /* update at local EOC */                    \
    PG##n##PER = (uint16_t)(cfg->period - 1u);                                 \
    /* In Independent output mode, DC controls PWMxH while TRIGB controls
     * PWMxL.  This compact HAL deliberately presents one duty per generator,
     * so drive the two pads identically.  It matters on DM330030, where the
     * J2 RGB bridge is connected to PWM1L/2L/3L. */                           \
    PG##n##DC = cfg->duty_init;                                                \
    PG##n##TRIGB = cfg->duty_init;                                             \
    PG##n##IOCONHbits.PMOD = (uint16_t)cfg->output_mode;                       \
    PG##n##IOCONHbits.POLH = cfg->low_active ? 1u : 0u;                        \
    PG##n##IOCONHbits.POLL = cfg->low_active ? 1u : 0u;                        \
    PG##n##IOCONHbits.PENH = cfg->pen_h ? 1u : 0u;                             \
    PG##n##IOCONHbits.PENL = cfg->pen_l ? 1u : 0u;                             \
    PG##n##CONHbits.MSTEN = cfg->is_module_master ? 1u : 0u;                  \
    /* PER/DC live in buffered data registers.  Start the generator, then     \
     * request their first transfer; without UPDREQ the reset-time active      \
     * values remain zero and the pads stay low despite a plausible PGxDC \
     * readback. */                                                           \
    PG##n##CONLbits.ON = 1u;                                                   \
    PG##n##STATbits.UPDREQ = 1u;                                               \
}                                                                              \
static void nora_pwm_pg##n##_set_duty(uint16_t duty)                           \
{                                                                              \
    PG##n##DC = duty;                                                          \
    PG##n##TRIGB = duty;                                                       \
    /* UPDMOD=0 applies a requested update at the next local SOC. */          \
    PG##n##STATbits.UPDREQ = 1u;                                               \
}

NORA_PWM_DEFINE_GENERATOR(1)
NORA_PWM_DEFINE_GENERATOR(2)
NORA_PWM_DEFINE_GENERATOR(3)
#if defined(PG4CONL)
NORA_PWM_DEFINE_GENERATOR(4)
#endif

bool nora_pwm_module_init(nora_pwm_mclk_source_t source)
{
    if (source != NORA_PWM_MCLK_STANDARD) {
        return false;
    }
    if (s_module_initialized) {
        return s_module_source == source;
    }

    /*
     * PWM is a separately gateable peripheral on CK.  Its reset state happens
     * to be enabled on the current MP508, but clearing PWMMD here is required
     * by this HAL's bring-up contract: writing PG registers while the module is
     * gated produces a perfectly plausible software configuration with no
     * physical waveform.
     */
#if defined(_PWMMD)
    _PWMMD = 0u;
#endif

    /* CLKSEL=01 on each PG selects FOSC directly (not Fcy). */
    PCLKCONbits.MCLKSEL = 0u;
    PCLKCONbits.DIVSEL = 0u;
    MPER = 0u;
    MDC = 0u;

    s_module_source = source;
    s_module_initialized = true;
    return true;
}

bool nora_pwm_generator_is_present(nora_pwm_generator_id_t id)
{
#if !defined(PG4CONL)
    if (id == NORA_PWM_GEN_4) return false;
#endif
    return (unsigned)id < (unsigned)NORA_PWM_GEN_COUNT;
}

bool nora_pwm_generator_init(nora_pwm_generator_id_t id,
                             const nora_pwm_generator_cfg_t *cfg)
{
    if ((!s_module_initialized) || (cfg == NULL) ||
        !nora_pwm_generator_is_present(id) || (cfg->period == 0u) ||
        (cfg->duty_init > cfg->period) ||
        ((unsigned)cfg->output_mode > (unsigned)NORA_PWM_MODE_PUSH_PULL)) {
        return false;
    }

    switch (id) {
    case NORA_PWM_GEN_1: nora_pwm_pg1_configure(cfg); break;
    case NORA_PWM_GEN_2: nora_pwm_pg2_configure(cfg); break;
    case NORA_PWM_GEN_3: nora_pwm_pg3_configure(cfg); break;
#if defined(PG4CONL)
    case NORA_PWM_GEN_4: nora_pwm_pg4_configure(cfg); break;
#endif
    default: return false;
    }

    s_generator_ready[id] = true;
    s_generator_period[id] = cfg->period;
    return true;
}

void nora_pwm_generator_set_duty(nora_pwm_generator_id_t id, uint16_t duty)
{
    if (!nora_pwm_generator_is_present(id) || !s_generator_ready[id]) {
        return;
    }
    if (duty > s_generator_period[id]) {
        duty = s_generator_period[id];
    }

    switch (id) {
    case NORA_PWM_GEN_1: nora_pwm_pg1_set_duty(duty); break;
    case NORA_PWM_GEN_2: nora_pwm_pg2_set_duty(duty); break;
    case NORA_PWM_GEN_3: nora_pwm_pg3_set_duty(duty); break;
#if defined(PG4CONL)
    case NORA_PWM_GEN_4: nora_pwm_pg4_set_duty(duty); break;
#endif
    default: break;
    }
}

/* Output override / run control, shared shape with the AK backend. */
#define NORA_PWM_DEFINE_OVERRIDE(n)                                            \
static void nora_pwm_pg##n##_override(bool enable, bool high, bool synchronous)\
{                                                                              \
    uint16_t value = PG##n##IOCONL;                                            \
    value &= (uint16_t)~(_PG##n##IOCONL_OSYNC_MASK | _PG##n##IOCONL_OVRDAT_MASK |\
                         _PG##n##IOCONL_OVRENH_MASK | _PG##n##IOCONL_OVRENL_MASK);\
    /* OSYNC 00 = next SOC, 01 = immediate. */                                 \
    if (!synchronous) value |= (uint16_t)(1u << _PG##n##IOCONL_OSYNC_POSITION);\
    if (high) value |= _PG##n##IOCONL_OVRDAT_MASK;                             \
    if (enable) value |= _PG##n##IOCONL_OVRENH_MASK | _PG##n##IOCONL_OVRENL_MASK;\
    PG##n##IOCONL = value;                                                     \
}                                                                              \
static void nora_pwm_pg##n##_run(bool on)                                      \
{                                                                              \
    PG##n##CONLbits.ON = on ? 1u : 0u;                                         \
}

NORA_PWM_DEFINE_OVERRIDE(1)
NORA_PWM_DEFINE_OVERRIDE(2)
NORA_PWM_DEFINE_OVERRIDE(3)
#if defined(PG4CONL)
NORA_PWM_DEFINE_OVERRIDE(4)
#endif

bool nora_pwm_generator_override(nora_pwm_generator_id_t id, bool enable,
                                 bool high, bool synchronous)
{
    if (!nora_pwm_generator_is_present(id) || !s_generator_ready[id]) return false;
    switch (id) {
    case NORA_PWM_GEN_1: nora_pwm_pg1_override(enable, high, synchronous); return true;
    case NORA_PWM_GEN_2: nora_pwm_pg2_override(enable, high, synchronous); return true;
    case NORA_PWM_GEN_3: nora_pwm_pg3_override(enable, high, synchronous); return true;
#if defined(PG4CONL)
    case NORA_PWM_GEN_4: nora_pwm_pg4_override(enable, high, synchronous); return true;
#endif
    default: return false;
    }
}

bool nora_pwm_generator_run(nora_pwm_generator_id_t id, bool on)
{
    if (!nora_pwm_generator_is_present(id) || !s_generator_ready[id]) return false;
    switch (id) {
    case NORA_PWM_GEN_1: nora_pwm_pg1_run(on); return true;
    case NORA_PWM_GEN_2: nora_pwm_pg2_run(on); return true;
    case NORA_PWM_GEN_3: nora_pwm_pg3_run(on); return true;
#if defined(PG4CONL)
    case NORA_PWM_GEN_4: nora_pwm_pg4_run(on); return true;
#endif
    default: return false;
    }
}
