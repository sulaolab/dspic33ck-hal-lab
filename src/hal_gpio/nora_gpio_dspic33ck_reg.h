#ifndef NORA_GPIO_REG_H
#define NORA_GPIO_REG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Internal register helper layer for the CK GPIO HAL.
 *
 * dsPIC33CK GPIO SFRs are 16-bit in the DFP. Keep this layer separate from the
 * public API so AK-style driver bodies can still be mirrored with CK-width
 * register access underneath.
 */

static inline void nora_gpio_reg_set(volatile uint16_t *reg, uint16_t mask)
{
    *reg = (uint16_t)(*reg | mask);
}

static inline void nora_gpio_reg_clear(volatile uint16_t *reg, uint16_t mask)
{
    *reg = (uint16_t)(*reg & (uint16_t)~mask);
}

static inline void nora_gpio_reg_toggle(volatile uint16_t *reg, uint16_t mask)
{
    *reg = (uint16_t)(*reg ^ mask);
}

static inline bool nora_gpio_reg_is_set(volatile uint16_t *reg, uint16_t mask)
{
    return ((*reg & mask) != 0u);
}

#endif /* NORA_GPIO_REG_H */
