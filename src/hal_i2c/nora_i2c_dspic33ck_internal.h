#ifndef NORA_I2C_DSPIC33CK_INTERNAL_H
#define NORA_I2C_DSPIC33CK_INTERNAL_H

#include "nora_i2c.h"
#include "nora_i2c_dspic33ck_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal shared helpers used by both the master and slave engines.
 *
 * Pure resolution helpers (no module state):
 *   - inst_is_valid : range-check the instance enum
 *   - get_regs      : resolve an instance to its register pointer table
 *   - calc_brg      : compute the CK I2CxBRG reload value from fcy/bus rates
 *
 * Plus the shared per-instance role/lifecycle state (set_role / get_role and
 * the public is_initialized), which the master and slave engines update so a
 * single query reflects either role.
 */

bool nora_i2c_inst_is_valid(nora_i2c_instance_t inst);

nora_i2c_status_t nora_i2c_get_regs(
    nora_i2c_instance_t inst,
    const nora_i2c_regs_t **regs);

/*
 * Compute the classic CK I2CxBRG reload value.
 *
 * Reference-manual formula (dsPIC33/PIC24 FRM, I2C):
 *   I2CxBRG = (Fcy / Fscl - Fcy * TDELAY) - 1
 * where TDELAY is the fixed internal SDA/SCL delay (~130 ns). The result is
 * clamped to a minimum of 1 so a mis-scaled request never writes 0 (which would
 * give an undefined SCL period).
 */
uint16_t nora_i2c_calc_brg(uint32_t fcy_hz, uint32_t bus_hz);

typedef enum {
    NORA_I2C_ROLE_NONE = 0,
    NORA_I2C_ROLE_MASTER,
    NORA_I2C_ROLE_SLAVE
} nora_i2c_role_t;

void nora_i2c_set_role(nora_i2c_instance_t inst,
                            nora_i2c_role_t role);
nora_i2c_role_t nora_i2c_get_role(nora_i2c_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* NORA_I2C_DSPIC33CK_INTERNAL_H */
