#include "nora_i2c.h"
#include "nora_i2c_dspic33ck_device.h"
#include "nora_i2c_dspic33ck_reg.h"
#include "nora_i2c_dspic33ck_internal.h"

/* --------------------------------------------------------------------------
 * Shared helpers used by both the master and slave engines.
 *
 * The resolution helpers (inst_is_valid / get_regs / calc_brg) are pure. The
 * only module state here is the per-instance role (set by the master and slave
 * engines on init/deinit) behind nora_i2c_is_initialized().
 * -------------------------------------------------------------------------- */

/*
 * Fixed internal delay term used by the CK I2CxBRG formula, in nanoseconds.
 * The classic reference-manual value sits around 110-130 ns; 130 ns is used
 * here as a conservative default. This is the parameter most likely to need
 * bench validation -- measure SCL with a scope -- if a target rate reads
 * slightly off.
 */
#define NORA_I2C_BRG_TDELAY_NS   130u

/* --------------------------------------------------------------------------
 * Status -> short name. See the header for why this lives with the enum.
 * -------------------------------------------------------------------------- */
const char *nora_i2c_status_str(nora_i2c_status_t status)
{
    switch (status) {
    case NORA_I2C_OK:                  return "OK";
    case NORA_I2C_ERR_INVALID_ARG:     return "ERR_INVALID_ARG";
    case NORA_I2C_ERR_NOT_PRESENT:     return "ERR_NOT_PRESENT";
    case NORA_I2C_ERR_NOT_INITIALIZED: return "ERR_NOT_INITIALIZED";
    case NORA_I2C_ERR_BUSY:            return "ERR_BUSY";
    case NORA_I2C_ERR_TIMEOUT:         return "ERR_TIMEOUT";
    case NORA_I2C_ERR_NACK:            return "ERR_NACK";
    case NORA_I2C_ERR_BUS:             return "ERR_BUS";
    case NORA_I2C_ERR_COLLISION:       return "ERR_COLLISION";
    case NORA_I2C_ERR_UNSUPPORTED:     return "ERR_UNSUPPORTED";
    case NORA_I2C_ERR_SEQUENCE:        return "ERR_SEQUENCE";
    default:                                return "?";
    }
}

/* --------------------------------------------------------------------------
 * Validate instance number
 * -------------------------------------------------------------------------- */
bool nora_i2c_inst_is_valid(nora_i2c_instance_t inst)
{
    return ((unsigned)inst < (unsigned)NORA_I2C_INST_COUNT);
}

/* --------------------------------------------------------------------------
 * Resolve instance to register table
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_get_regs(
    nora_i2c_instance_t inst,
    const nora_i2c_regs_t **regs)
{
    const nora_i2c_device_t *dev;

    if (regs == 0) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    if (!nora_i2c_inst_is_valid(inst)) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    dev = nora_i2c_get_device(inst);
    if (dev == 0) {
        return NORA_I2C_ERR_NOT_PRESENT;
    }

    *regs = &dev->regs;
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Calculate I2CxBRG reload value (CK classic single-register form)
 * -------------------------------------------------------------------------- */
uint16_t nora_i2c_calc_brg(uint32_t fcy_hz, uint32_t bus_hz)
{
    uint64_t base;
    uint64_t delay;
    uint64_t brg;

    if (bus_hz == 0u || fcy_hz == 0u) {
        return 1u;
    }

    /*
     * I2CxBRG = (Fcy / Fscl - Fcy * TDELAY) - 1
     *
     * Fcy * TDELAY is evaluated as (Fcy * TDELAY_NS) / 1e9 in 64-bit integer
     * arithmetic to avoid floating point and uint32_t overflow.
     */
    base = (uint64_t)fcy_hz / (uint64_t)bus_hz;
    delay = ((uint64_t)fcy_hz * (uint64_t)NORA_I2C_BRG_TDELAY_NS)
            / 1000000000ull;

    if (base <= delay + 1ull) {
        return 1u;
    }

    brg = base - delay - 1ull;
    if (brg > 0xFFFFull) {
        brg = 0xFFFFull;
    }

    return (uint16_t)brg;
}

/* --------------------------------------------------------------------------
 * Check whether I2C instance exists on the selected device
 * -------------------------------------------------------------------------- */
bool nora_i2c_is_present(nora_i2c_instance_t inst)
{
    return nora_i2c_instance_is_present(inst);
}

/* --------------------------------------------------------------------------
 * Shared role / lifecycle state
 * -------------------------------------------------------------------------- */
static nora_i2c_role_t g_role[NORA_I2C_INST_COUNT];

void nora_i2c_set_role(nora_i2c_instance_t inst,
                            nora_i2c_role_t role)
{
    if (nora_i2c_inst_is_valid(inst)) {
        g_role[inst] = role;
    }
}

nora_i2c_role_t nora_i2c_get_role(nora_i2c_instance_t inst)
{
    if (!nora_i2c_inst_is_valid(inst)) {
        return NORA_I2C_ROLE_NONE;
    }
    return g_role[inst];
}

/* --------------------------------------------------------------------------
 * Initialized query (true once init'd as either master or slave)
 * -------------------------------------------------------------------------- */
bool nora_i2c_is_initialized(nora_i2c_instance_t inst)
{
    return (nora_i2c_get_role(inst) != NORA_I2C_ROLE_NONE);
}
