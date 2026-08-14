#ifndef NORA_I2C_SLAVE_H
#define NORA_I2C_SLAVE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "nora_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dsPIC33CK I2C slave (device/client) driver interface.
 *
 * Include this header to answer as an I2C slave. The shared instance / status
 * types live in nora_i2c.h (included above); the bus-master role is in
 * nora_i2c_master.h. A program may include either or both.
 *
 * The slave is callback-driven and interrupt-based. Unlike the dsPIC33A "new"
 * I2C module (which aggregates address / data / STOP into one event interrupt
 * routed through the INTC register), the classic CK peripheral has a single
 * dedicated slave interrupt (SI2Cx) that already fires on the address match, on
 * every received or transmitted data byte and - with PCIE set - on STOP. No
 * interrupt-routing register is involved.
 *
 * This driver defines the _SI2CxInterrupt vectors for the I2C instances that
 * exist on the target (in the device layer); each clears its flag and calls
 * nora_i2c_slave_event_irq(inst). The delegate is also exported so an
 * integration that owns the vector itself, or a host-side unit test, can drive
 * the same service routine.
 *
 * Scope: 7-bit addressing only. 10-bit and general-call are not handled yet.
 */

typedef struct {
    uint8_t addr7;          /* 7-bit own address (right-justified, e.g. 0x55) */
    uint8_t addr_mask;      /* I2CxMSK low 7 bits; 0 = exact match            */
    bool    clock_stretch;  /* STREN: hold SCL low to give callbacks time     */

    /* Address phase: the master addressed us. is_read is true when the master
     * wants to read from us (it will clock bytes out of on_tx_byte), false
     * when it will write to us (bytes arrive at on_rx_byte). May be NULL. */
    void (*on_addr_match)(bool is_read);

    /* Master-write: one received data byte. May be NULL (byte is dropped). */
    void (*on_rx_byte)(uint8_t b);

    /* Master-read: return the next byte to transmit. If NULL, 0xFF is sent. */
    uint8_t (*on_tx_byte)(void);

    /* STOP (or bus-release) ended the transaction. May be NULL. */
    void (*on_stop)(void);
} nora_i2c_slave_config_t;

/*
 * Set the CPU interrupt priority of the SLAVE (SI2Cx) line.
 *
 * WHY THIS IS A SEPARATE CALL FROM THE MASTER ONE. The classic CK peripheral has
 * three independent interrupts (MI2Cx, SI2Cx, I2CxBC), so there is no single
 * "I2C priority" to set: nora_i2c_set_interrupt_priority() writes _MI2CxIP and
 * nothing else. Before this call existed the slave line simply ran at its reset
 * priority with no way to change it, and pointing a caller at the master setter
 * looked like it worked.
 *
 * Call it BEFORE nora_i2c_slave_init() -- init enables the line, and raising the
 * priority of an already-enabled interrupt is a race the HAL does not need to
 * take. Valid range 0..7; 0 leaves the source masked by CPU priority rules.
 *
 * Returns NORA_I2C_ERR_INVALID_ARG for an out-of-range instance or priority,
 * NORA_I2C_ERR_NOT_PRESENT for an instance this device does not have, and
 * NORA_I2C_ERR_UNSUPPORTED if the selected part exposes no _SI2CxIP symbol for
 * it -- never OK for a priority that was not written.
 */
nora_i2c_status_t nora_i2c_slave_set_interrupt_priority(
    nora_i2c_instance_t inst,
    uint8_t priority);

/*
 * Configure the instance as a slave at config->addr7 (right-justified 7-bit;
 * above 0x7F is NORA_I2C_ERR_INVALID_ARG) and enable it.
 *
 * An instance that is live as a master must be released with nora_i2c_deinit()
 * first, otherwise this returns NORA_I2C_ERR_BUSY. Re-initializing an instance
 * that is already a slave is allowed.
 */
nora_i2c_status_t nora_i2c_slave_init(
    nora_i2c_instance_t inst,
    const nora_i2c_slave_config_t *config);

/* Disable the slave: turn the peripheral off, mask its interrupt, drop state. */
nora_i2c_status_t nora_i2c_slave_deinit(
    nora_i2c_instance_t inst);

/* True once nora_i2c_slave_init() has configured this instance. */
bool nora_i2c_slave_is_active(nora_i2c_instance_t inst);

/*
 * Slave interrupt delegate.
 *
 *   _SI2CxInterrupt -> nora_i2c_slave_event_irq(inst)
 *
 * Clears the hardware slave-interrupt flag and services whatever the classic
 * I2CxSTAT register reports (address match, received byte, transmit-continue,
 * STOP). The device layer wires the real vectors to this function; it is public
 * so a custom vector or a host-side test can call it directly.
 */
void nora_i2c_slave_event_irq(nora_i2c_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* NORA_I2C_SLAVE_H */
