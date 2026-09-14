#include <xc.h>
#include <stddef.h>

#include "nora_gpio.h"
#include "nora_gpio_dspic33ck_reg.h"

typedef struct {
    volatile uint16_t *ansel;
    volatile uint16_t *tris;
    volatile uint16_t *port;
    volatile uint16_t *lat;
    volatile uint16_t *odc;
    volatile uint16_t *cnpu;
    volatile uint16_t *cnpd;
} nora_gpio_port_regs_t;

#define GPIO_PORT_ROW(letter) { &ANSEL##letter, &TRIS##letter, &PORT##letter, &LAT##letter, &ODC##letter, &CNPU##letter, &CNPD##letter }
#define GPIO_PORT_EMPTY       { NULL, NULL, NULL, NULL, NULL, NULL, NULL }

static const nora_gpio_port_regs_t gpio_ports[] = {
#if defined(ANSELA) && defined(TRISA) && defined(PORTA) && defined(LATA)
    GPIO_PORT_ROW(A),
#else
    GPIO_PORT_EMPTY,
#endif
#if defined(ANSELB) && defined(TRISB) && defined(PORTB) && defined(LATB)
    GPIO_PORT_ROW(B),
#else
    GPIO_PORT_EMPTY,
#endif
#if defined(ANSELC) && defined(TRISC) && defined(PORTC) && defined(LATC)
    GPIO_PORT_ROW(C),
#else
    GPIO_PORT_EMPTY,
#endif
#if defined(ANSELD) && defined(TRISD) && defined(PORTD) && defined(LATD)
    GPIO_PORT_ROW(D),
#else
    GPIO_PORT_EMPTY,
#endif
#if defined(ANSELE) && defined(TRISE) && defined(PORTE) && defined(LATE)
    GPIO_PORT_ROW(E),
#else
    GPIO_PORT_EMPTY,
#endif
    GPIO_PORT_EMPTY,
    GPIO_PORT_EMPTY,
    GPIO_PORT_EMPTY
};

static const nora_gpio_port_regs_t *gpio_get_port(nora_gpio_pin_t pin)
{
    uint8_t port = NORA_GPIO_PIN_PORT(pin);

    if (port >= (sizeof(gpio_ports) / sizeof(gpio_ports[0]))) {
        return NULL;
    }
    if (gpio_ports[port].tris == NULL) {
        return NULL;
    }

    return &gpio_ports[port];
}

static uint16_t gpio_pin_mask(nora_gpio_pin_t pin)
{
    return (uint16_t)(1u << NORA_GPIO_PIN_BIT(pin));
}

bool nora_gpio_set_direction(nora_gpio_pin_t pin, nora_gpio_dir_t dir)
{
    const nora_gpio_port_regs_t *p = gpio_get_port(pin);
    uint16_t mask = gpio_pin_mask(pin);

    if (p == NULL) {
        return false;
    }

    if (dir == NORA_GPIO_DIR_INPUT) {
        nora_gpio_reg_set(p->tris, mask);
    } else {
        nora_gpio_reg_clear(p->tris, mask);
    }

    return true;
}

bool nora_gpio_set_pull(nora_gpio_pin_t pin, nora_gpio_pull_t pull)
{
    const nora_gpio_port_regs_t *p = gpio_get_port(pin);
    uint16_t mask = gpio_pin_mask(pin);

    if (p == NULL) {
        return false;
    }

    nora_gpio_reg_clear(p->cnpu, mask);
    nora_gpio_reg_clear(p->cnpd, mask);

    if (pull == NORA_GPIO_PULL_UP) {
        nora_gpio_reg_set(p->cnpu, mask);
    } else if (pull == NORA_GPIO_PULL_DOWN) {
        nora_gpio_reg_set(p->cnpd, mask);
    }

    return true;
}

bool nora_gpio_set_analog(nora_gpio_pin_t pin, bool analog)
{
    const nora_gpio_port_regs_t *p = gpio_get_port(pin);
    uint16_t mask = gpio_pin_mask(pin);

    if (p == NULL) {
        return false;
    }

    if (analog) {
        nora_gpio_reg_set(p->ansel, mask);
    } else {
        nora_gpio_reg_clear(p->ansel, mask);
    }

    return true;
}

bool nora_gpio_set_open_drain(nora_gpio_pin_t pin, bool enable)
{
    const nora_gpio_port_regs_t *p = gpio_get_port(pin);
    uint16_t mask = gpio_pin_mask(pin);

    if (p == NULL) {
        return false;
    }

    if (enable) {
        nora_gpio_reg_set(p->odc, mask);
    } else {
        nora_gpio_reg_clear(p->odc, mask);
    }

    return true;
}

bool nora_gpio_config(nora_gpio_pin_t pin, const nora_gpio_config_t *config)
{
    if (config == NULL) {
        return false;
    }

    if (!nora_gpio_set_analog(pin, config->analog)) {
        return false;
    }
    if (!nora_gpio_set_pull(pin, config->pull)) {
        return false;
    }
    if (!nora_gpio_set_open_drain(pin, config->open_drain)) {
        return false;
    }

    if (config->dir == NORA_GPIO_DIR_OUTPUT) {
        if (!nora_gpio_write(pin, config->initial_high)) {
            return false;
        }
    }

    return nora_gpio_set_direction(pin, config->dir);
}

bool nora_gpio_config_digital_input(nora_gpio_pin_t pin)
{
    static const nora_gpio_config_t cfg = {
        .dir = NORA_GPIO_DIR_INPUT,
        .pull = NORA_GPIO_PULL_NONE,
        .analog = false,
        .open_drain = false,
        .initial_high = false
    };

    return nora_gpio_config(pin, &cfg);
}

bool nora_gpio_config_digital_output(nora_gpio_pin_t pin, bool initial_high)
{
    nora_gpio_config_t cfg = {
        .dir = NORA_GPIO_DIR_OUTPUT,
        .pull = NORA_GPIO_PULL_NONE,
        .analog = false,
        .open_drain = false,
        .initial_high = initial_high
    };

    return nora_gpio_config(pin, &cfg);
}

bool nora_gpio_write(nora_gpio_pin_t pin, bool high)
{
    const nora_gpio_port_regs_t *p = gpio_get_port(pin);
    uint16_t mask = gpio_pin_mask(pin);

    if (p == NULL) {
        return false;
    }

    if (high) {
        nora_gpio_reg_set(p->lat, mask);
    } else {
        nora_gpio_reg_clear(p->lat, mask);
    }

    return true;
}

bool nora_gpio_set(nora_gpio_pin_t pin)
{
    return nora_gpio_write(pin, true);
}

bool nora_gpio_clear(nora_gpio_pin_t pin)
{
    return nora_gpio_write(pin, false);
}

bool nora_gpio_toggle(nora_gpio_pin_t pin)
{
    const nora_gpio_port_regs_t *p = gpio_get_port(pin);

    if (p == NULL) {
        return false;
    }

    nora_gpio_reg_toggle(p->lat, gpio_pin_mask(pin));
    return true;
}

nora_gpio_level_t nora_gpio_read(nora_gpio_pin_t pin)
{
    const nora_gpio_port_regs_t *p = gpio_get_port(pin);

    if (p == NULL) {
        return NORA_GPIO_LEVEL_ERROR;
    }

    return nora_gpio_reg_is_set(p->port, gpio_pin_mask(pin)) ?
        NORA_GPIO_LEVEL_HIGH : NORA_GPIO_LEVEL_LOW;
}

nora_gpio_level_t nora_gpio_read_output(nora_gpio_pin_t pin)
{
    const nora_gpio_port_regs_t *p = gpio_get_port(pin);

    if (p == NULL) {
        return NORA_GPIO_LEVEL_ERROR;
    }

    return nora_gpio_reg_is_set(p->lat, gpio_pin_mask(pin)) ?
        NORA_GPIO_LEVEL_HIGH : NORA_GPIO_LEVEL_LOW;
}

bool nora_gpio_pin_from_rp(nora_gpio_rp_t rp, nora_gpio_pin_t *pin)
{
    if (pin == NULL) {
        return false;
    }

    if ((rp >= 32u) && (rp <= 47u)) {
        *pin = NORA_GPIO_PIN(NORA_GPIO_PORT_B, (uint8_t)(rp - 32u));
        return true;
    }
    if ((rp >= 48u) && (rp <= 63u)) {
        *pin = NORA_GPIO_PIN(NORA_GPIO_PORT_C, (uint8_t)(rp - 48u));
        return true;
    }
    if ((rp >= 64u) && (rp <= 79u)) {
        *pin = NORA_GPIO_PIN(NORA_GPIO_PORT_D, (uint8_t)(rp - 64u));
        return true;
    }

    /*
     * Virtual RPs RPV0-5 (RP176-181) are internal PPS-only pins with no GPIO
     * port pad; they are intentionally NOT mapped here (mirroring the AK HAL,
     * which excludes its RPV range from the GPIO-typed RP API). Port E is a real
     * GPIO port but has no remappable RPn, so it is reached by pin name only.
     */
    return false;
}

bool nora_gpio_rp_from_pin(nora_gpio_pin_t pin, nora_gpio_rp_t *rp)
{
    uint8_t port;
    uint8_t bit;

    if (rp == NULL) {
        return false;
    }

    port = NORA_GPIO_PIN_PORT(pin);
    bit = NORA_GPIO_PIN_BIT(pin);

    if (port == NORA_GPIO_PORT_B) {
        *rp = (uint8_t)(32u + bit);
        return true;
    }
    if (port == NORA_GPIO_PORT_C) {
        *rp = (uint8_t)(48u + bit);
        return true;
    }
    if (port == NORA_GPIO_PORT_D) {
        *rp = (uint8_t)(64u + bit);
        return true;
    }

    /* Only ports B/C/D are remappable (RP32-79). Port A/E have no RPn; the
     * virtual RPV0-5 outputs are not exposed through this GPIO-typed API. */
    return false;
}

bool nora_gpio_rp_config(nora_gpio_rp_t rp,
                              const nora_gpio_config_t *config)
{
    nora_gpio_pin_t pin;
    return nora_gpio_pin_from_rp(rp, &pin) && nora_gpio_config(pin, config);
}

bool nora_gpio_rp_set_direction(nora_gpio_rp_t rp, nora_gpio_dir_t dir)
{
    nora_gpio_pin_t pin;
    return nora_gpio_pin_from_rp(rp, &pin) && nora_gpio_set_direction(pin, dir);
}

bool nora_gpio_rp_set_pull(nora_gpio_rp_t rp, nora_gpio_pull_t pull)
{
    nora_gpio_pin_t pin;
    return nora_gpio_pin_from_rp(rp, &pin) && nora_gpio_set_pull(pin, pull);
}

bool nora_gpio_rp_set_analog(nora_gpio_rp_t rp, bool analog)
{
    nora_gpio_pin_t pin;
    return nora_gpio_pin_from_rp(rp, &pin) && nora_gpio_set_analog(pin, analog);
}

bool nora_gpio_rp_set_open_drain(nora_gpio_rp_t rp, bool enable)
{
    nora_gpio_pin_t pin;
    return nora_gpio_pin_from_rp(rp, &pin) && nora_gpio_set_open_drain(pin, enable);
}

bool nora_gpio_rp_config_digital_input(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;
    return nora_gpio_pin_from_rp(rp, &pin) && nora_gpio_config_digital_input(pin);
}

bool nora_gpio_rp_config_digital_output(nora_gpio_rp_t rp, bool initial_high)
{
    nora_gpio_pin_t pin;
    return nora_gpio_pin_from_rp(rp, &pin) && nora_gpio_config_digital_output(pin, initial_high);
}

bool nora_gpio_rp_set(nora_gpio_rp_t rp)
{
    return nora_gpio_rp_write(rp, true);
}

bool nora_gpio_rp_clear(nora_gpio_rp_t rp)
{
    return nora_gpio_rp_write(rp, false);
}

bool nora_gpio_rp_toggle(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;
    return nora_gpio_pin_from_rp(rp, &pin) && nora_gpio_toggle(pin);
}

bool nora_gpio_rp_write(nora_gpio_rp_t rp, bool high)
{
    nora_gpio_pin_t pin;
    return nora_gpio_pin_from_rp(rp, &pin) && nora_gpio_write(pin, high);
}

nora_gpio_level_t nora_gpio_rp_read(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin)) {
        return NORA_GPIO_LEVEL_ERROR;
    }
    return nora_gpio_read(pin);
}

nora_gpio_level_t nora_gpio_rp_read_output(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin)) {
        return NORA_GPIO_LEVEL_ERROR;
    }
    return nora_gpio_read_output(pin);
}
