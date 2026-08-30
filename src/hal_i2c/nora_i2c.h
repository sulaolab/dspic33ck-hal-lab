#ifndef NORA_I2C_H
#define NORA_I2C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small, readable dsPIC33CK I2C driver - common types and lifecycle.
 *
 * API shape intentionally follows the dsPIC33AK I2C HAL where practical, and the
 * names now sit in the shared NORA namespace (silicon side tagged _dspic33ck).
 * It targets the classic 16-bit CK I2C peripheral (I2CxCONL/CONH/STAT/BRG)
 * instead of the dsPIC33A 32-bit module. Both the bus-master and the slave
 * (client) roles are ported.
 *
 * This header carries only what the master and slave roles share:
 *   - the instance and status enumerations,
 *   - the millisecond-tick callback type,
 *   - presence / initialized queries and deinit.
 *
 * For the bus-master API include nora_i2c_master.h; for the slave API
 * include nora_i2c_slave.h. A program may include either or both. This
 * header intentionally does not expose XC-DSC/DFP bitfield types.
 */

typedef enum {
    NORA_I2C_INST_1 = 0,
    NORA_I2C_INST_2,
    NORA_I2C_INST_3,
    NORA_I2C_INST_COUNT
} nora_i2c_instance_t;

typedef enum {
    NORA_I2C_OK = 0,
    NORA_I2C_ERR_INVALID_ARG,
    NORA_I2C_ERR_NOT_PRESENT,
    NORA_I2C_ERR_NOT_INITIALIZED,
    NORA_I2C_ERR_BUSY,
    NORA_I2C_ERR_TIMEOUT,
    NORA_I2C_ERR_NACK,
    NORA_I2C_ERR_BUS,
    NORA_I2C_ERR_COLLISION,
    NORA_I2C_ERR_UNSUPPORTED,
    NORA_I2C_ERR_SEQUENCE
} nora_i2c_status_t;

/*
 * The status as a short name, e.g. "ERR_NACK".
 *
 * Lives with the enum rather than with a caller because every caller that reports
 * an I2C result wants the same eleven strings: the version this replaces was a
 * static table inside the EV88G73A I2C probe, invisible to anyone else, while
 * wm8904.c printed the same statuses as bare integers. A switch here also stops
 * compiling silently when a status is added, which a caller's private copy does not.
 *
 * Never NULL: an unrecognised value returns "?" so a diagnostic path cannot fault
 * on it.
 */
const char *nora_i2c_status_str(nora_i2c_status_t status);

typedef uint32_t (*nora_i2c_get_ms_fn)(void);

/* Shared lifecycle / query API -------------------------------------------- */

/*
 * Deinitialize the selected I2C instance -- the MASTER role.
 *
 * It sits in this shared header because the peripheral is one thing and the
 * name is older than the two-role split, but it is the counterpart of
 * nora_i2c_init() only. An instance brought up with nora_i2c_slave_init() is
 * released by nora_i2c_slave_deinit(); calling this on one answers
 * NORA_I2C_ERR_NOT_INITIALIZED even though nora_i2c_is_initialized() below
 * reports true for it, because that query covers either role and this call
 * does not.
 *
 * If deinit recovers a stale pending transaction, it may return the recovery
 * status while still forcing the peripheral off and clearing HAL state.
 */
nora_i2c_status_t nora_i2c_deinit(
    nora_i2c_instance_t inst);

bool nora_i2c_is_present(
    nora_i2c_instance_t inst);

/*
 * Set the MASTER (MI2Cx) CPU interrupt priority for the selected I2C instance.
 *
 * This configures the master line only, and the name is older than that fact:
 * the CK classic peripheral exposes separate master (MI2Cx), slave (SI2Cx) and
 * bus-collision (I2CxBC) interrupts, so there is no single "I2C priority". The
 * application owns the master vector.
 *
 * Slave (SI2Cx) priority is nora_i2c_slave_set_interrupt_priority(), and the
 * slave vector is defined by this backend rather than by the application -- both
 * are described in nora_i2c_slave.h.
 */
nora_i2c_status_t nora_i2c_set_interrupt_priority(
    nora_i2c_instance_t inst,
    uint8_t priority);

/*
 * True once the instance has been initialized in EITHER role. Use
 * nora_i2c_slave_is_active() to tell the roles apart -- and note that
 * nora_i2c_deinit() above releases only the master one.
 */
bool nora_i2c_is_initialized(
    nora_i2c_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* NORA_I2C_H */
