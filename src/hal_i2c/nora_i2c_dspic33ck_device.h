#ifndef NORA_I2C_DSPIC33CK_DEVICE_H
#define NORA_I2C_DSPIC33CK_DEVICE_H

#include <stdbool.h>
#include "nora_i2c.h"
#include "nora_i2c_dspic33ck_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;
    nora_i2c_regs_t regs;
} nora_i2c_device_t;

const nora_i2c_device_t *nora_i2c_get_device(
    nora_i2c_instance_t inst);

bool nora_i2c_instance_is_present(nora_i2c_instance_t inst);

/*
 * Slave (SI2Cx) interrupt control.
 *
 * The classic CK I2C peripheral exposes a dedicated slave interrupt separate
 * from the master (MI2Cx) one. As with nora_i2c_set_interrupt_priority(),
 * the device-specific _SI2CxIF / _SI2CxIE symbols are confined to the device
 * layer; the slave engine drives these helpers instead of touching the DFP
 * symbols directly. Unknown / absent instances are a no-op. This layer also
 * defines the _SI2CxInterrupt vectors, which call nora_i2c_slave_event_irq().
 */
void nora_i2c_slave_irq_enable(nora_i2c_instance_t inst);
void nora_i2c_slave_irq_disable(nora_i2c_instance_t inst);
void nora_i2c_slave_irq_clear(nora_i2c_instance_t inst);

/*
 * Program _SI2CxIP. Returns false for an unknown instance or for a part with no
 * such symbol -- unlike the three above, this one reports, because the public
 * nora_i2c_slave_set_interrupt_priority() must not answer OK for a priority it
 * did not write (the line would keep running at its reset priority, which is
 * exactly the defect this call was added to remove).
 */
bool nora_i2c_slave_irq_set_priority(nora_i2c_instance_t inst, uint8_t priority);

#ifdef __cplusplus
}
#endif

#endif /* NORA_I2C_DSPIC33CK_DEVICE_H */
