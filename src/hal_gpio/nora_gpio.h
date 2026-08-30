#ifndef NORA_GPIO_H
#define NORA_GPIO_H

/*
 * Small GPIO HAL for dsPIC33CK devices.
 *
 * The API intentionally mirrors the dsPIC33AK GPIO HAL shape and now shares its
 * NORA namespace: the contract header carries no chip name, and the
 * silicon-specific backend files carry the _dspic33ck tag. A common AK/CK facade is
 * still deferred until the surface proves stable.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    NORA_GPIO_PORT_A = 0,
    NORA_GPIO_PORT_B = 1,
    NORA_GPIO_PORT_C = 2,
    NORA_GPIO_PORT_D = 3,
    NORA_GPIO_PORT_E = 4,
    NORA_GPIO_PORT_F = 5,
    NORA_GPIO_PORT_G = 6,
    NORA_GPIO_PORT_H = 7
} nora_gpio_port_t;

typedef uint16_t nora_gpio_pin_t;

#define NORA_GPIO_PIN(port, bit) \
    ((nora_gpio_pin_t)((((uint16_t)(port)) << 4) | ((uint16_t)(bit) & 0x0Fu)))

#define NORA_GPIO_PIN_PORT(pin) ((uint8_t)(((uint16_t)(pin) >> 4) & 0x07u))
#define NORA_GPIO_PIN_BIT(pin)  ((uint8_t)((uint16_t)(pin) & 0x0Fu))

typedef enum
{
    NORA_GPIO_DIR_INPUT  = 0,
    NORA_GPIO_DIR_OUTPUT = 1
} nora_gpio_dir_t;

typedef enum
{
    NORA_GPIO_PULL_NONE = 0,
    NORA_GPIO_PULL_UP,
    NORA_GPIO_PULL_DOWN
} nora_gpio_pull_t;

typedef struct
{
    nora_gpio_dir_t  dir;
    nora_gpio_pull_t pull;
    bool                  analog;
    bool                  open_drain;
    bool                  initial_high;
} nora_gpio_config_t;

typedef int8_t nora_gpio_level_t;

#define NORA_GPIO_LEVEL_ERROR ((nora_gpio_level_t)-1)
#define NORA_GPIO_LEVEL_LOW   ((nora_gpio_level_t)0)
#define NORA_GPIO_LEVEL_HIGH  ((nora_gpio_level_t)1)

bool nora_gpio_set_direction(nora_gpio_pin_t pin, nora_gpio_dir_t dir);
bool nora_gpio_set_pull(nora_gpio_pin_t pin, nora_gpio_pull_t pull);
bool nora_gpio_set_analog(nora_gpio_pin_t pin, bool analog);
bool nora_gpio_set_open_drain(nora_gpio_pin_t pin, bool enable);
bool nora_gpio_config(nora_gpio_pin_t pin, const nora_gpio_config_t *config);

bool nora_gpio_config_digital_input(nora_gpio_pin_t pin);
bool nora_gpio_config_digital_output(nora_gpio_pin_t pin, bool initial_high);

bool nora_gpio_write(nora_gpio_pin_t pin, bool high);
bool nora_gpio_set(nora_gpio_pin_t pin);
bool nora_gpio_clear(nora_gpio_pin_t pin);
bool nora_gpio_toggle(nora_gpio_pin_t pin);

nora_gpio_level_t nora_gpio_read(nora_gpio_pin_t pin);
nora_gpio_level_t nora_gpio_read_output(nora_gpio_pin_t pin);

/*
 * RP-first API.
 *
 * CK uses the classic flat map RPn = 16*(port+1) + bit, so only ports B/C/D are
 * remappable: RB0 = RP32 .. RD15 = RP79 (48 physical pins). This differs from
 * the AK packed-pin+1 encoding, but the public API stays AK-shaped and the CK
 * conversion is handled in the implementation. Port A/E have no RPn, and the
 * virtual RPV0-5 outputs are not exposed through this GPIO-typed API (mirroring
 * the AK HAL's treatment of its own RPV range).
 */
typedef uint8_t nora_gpio_rp_t;

#define NORA_GPIO_RP_MIN  (32u)   /* RB0 */
#define NORA_GPIO_RP_MAX  (79u)   /* RD15 (last physical remappable pin) */

/*
 * These two spell the RP number as nora_gpio_rp_t, which is what the AK public
 * API declares and what every other RP entry point below already takes. They
 * used to say bare uint8_t in the name of byte-for-byte AK compatibility, but
 * the typedef IS uint8_t on both targets, so naming it is compatible at the ABI
 * and identical at the source -- and it stops these two being the only place in
 * the tree that bypasses the type the boards' *_pins.h cast to.
 */
bool nora_gpio_pin_from_rp(nora_gpio_rp_t rp, nora_gpio_pin_t *pin);
bool nora_gpio_rp_from_pin(nora_gpio_pin_t pin, nora_gpio_rp_t *rp);

bool nora_gpio_rp_config(nora_gpio_rp_t rp,
                              const nora_gpio_config_t *config);

bool nora_gpio_rp_set_direction(nora_gpio_rp_t rp, nora_gpio_dir_t dir);
bool nora_gpio_rp_set_pull(nora_gpio_rp_t rp, nora_gpio_pull_t pull);
bool nora_gpio_rp_set_analog(nora_gpio_rp_t rp, bool analog);
bool nora_gpio_rp_set_open_drain(nora_gpio_rp_t rp, bool enable);

bool nora_gpio_rp_config_digital_input(nora_gpio_rp_t rp);
bool nora_gpio_rp_config_digital_output(nora_gpio_rp_t rp, bool initial_high);

bool nora_gpio_rp_set(nora_gpio_rp_t rp);
bool nora_gpio_rp_clear(nora_gpio_rp_t rp);
bool nora_gpio_rp_toggle(nora_gpio_rp_t rp);
bool nora_gpio_rp_write(nora_gpio_rp_t rp, bool high);

nora_gpio_level_t nora_gpio_rp_read(nora_gpio_rp_t rp);
nora_gpio_level_t nora_gpio_rp_read_output(nora_gpio_rp_t rp);

#ifdef __cplusplus
}
#endif

#endif /* NORA_GPIO_H */
