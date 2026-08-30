#ifndef NORA_I2C_MASTER_H
#define NORA_I2C_MASTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "nora_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dsPIC33CK I2C master driver interface.
 *
 * Include this header to use the I2C peripheral as a bus master. The shared
 * instance / status types live in nora_i2c.h (included above).
 *
 * Design policy (mirrors the AK HAL):
 *   - The normal user-facing API is blocking and easy to use.
 *   - Low-level functions separate "issue" and "status check" so advanced
 *     users can replace polling waits with interrupt flags or RTOS waits.
 *   - This header intentionally does not expose XC-DSC/DFP bitfield types.
 */

/*
 * bus_hz is a REQUEST, not a guarantee. It is converted to the single I2CxBRG
 * reload value, so the achievable rates are quantized, and a request the
 * divider cannot express is CLAMPED rather than refused -- init() and
 * set_bus_speed() still answer NORA_I2C_OK. Only fcy_hz == 0 and bus_hz == 0
 * are rejected.
 *
 * Achieved rate = Fcy / (BRG + 1 + Fcy * TDELAY), where TDELAY is the module's
 * fixed internal SDA/SCL delay, taken here as a 130 ns estimate. If a target
 * rate matters, measure SCL with a scope; 130 ns is the term most likely to
 * need bench validation.
 */
typedef struct {
    uint32_t fcy_hz;
    uint32_t bus_hz;
    uint32_t timeout_ms;

    /*
     * Optional millisecond tick callback for timeout handling.
     * If get_ms is NULL, timeout handling is disabled.
     * If timeout_ms is 0, timeout handling is also disabled.
     *
     * pending_timeout_ms is independent from timeout_ms. If non-zero, a
     * no-STOP transaction left pending is recovered on the next public
     * transaction API call after this timeout has elapsed.
     */
    nora_i2c_get_ms_fn get_ms;
    uint32_t pending_timeout_ms;
} nora_i2c_config_t;

/* Normal blocking API -----------------------------------------------------
 *
 * addr7 is the RIGHT-JUSTIFIED 7-bit address (WM8904 = 0x1A, not the 0x34 its
 * datasheet also prints): the driver forms the wire byte as (addr7 << 1) | R/W.
 * Anything above 0x7F is NORA_I2C_ERR_INVALID_ARG rather than a silently
 * truncated address that would reach a different device.
 *
 * Buffer arguments are asymmetric, deliberately: a write may pass tx_len == 0
 * with tx == NULL, which sends address + STOP only and is how a bus scan probes
 * for a device, whereas a read of zero bytes has no meaning on the wire and
 * rx == NULL or rx_len == 0 is NORA_I2C_ERR_INVALID_ARG.
 *
 * An instance that is live as a slave must be released with
 * nora_i2c_slave_deinit() before nora_i2c_init(); otherwise init returns
 * NORA_I2C_ERR_BUSY. Re-initializing an instance that is already a master is
 * allowed.
 */

nora_i2c_status_t nora_i2c_init(
    nora_i2c_instance_t inst,
    const nora_i2c_config_t *config);

/*
 * Update the I2C bus speed for an initialized idle instance.
 *
 * Returns NORA_I2C_ERR_BUSY if the host state machine is active or a
 * no-STOP transaction is pending.
 */
nora_i2c_status_t nora_i2c_set_bus_speed(
    nora_i2c_instance_t inst,
    uint32_t fcy_hz,
    uint32_t bus_hz);

nora_i2c_status_t nora_i2c_write(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    const uint8_t *tx,
    size_t tx_len);

nora_i2c_status_t nora_i2c_read(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    uint8_t *rx,
    size_t rx_len);

nora_i2c_status_t nora_i2c_write_read(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len);

/* Master transaction API --------------------------------------------------
 * These functions expose the STOP-pending sequence needed by CMSIS-Driver
 * I2C xfer_pending style transfers.
 */

nora_i2c_status_t nora_i2c_master_write_no_stop(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    const uint8_t *tx,
    size_t tx_len);

nora_i2c_status_t nora_i2c_master_read_after_restart(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    uint8_t *rx,
    size_t rx_len);

nora_i2c_status_t nora_i2c_master_stop(
    nora_i2c_instance_t inst);

/* Low-level primitive API -------------------------------------------------
 * These functions are intentionally small. The blocking API above is built
 * from these issue/check operations.
 *
 * The pending transaction guard is applied to normal blocking and master
 * transaction APIs. Low-level primitive APIs do not enforce pending-state
 * sequencing.
 */

nora_i2c_status_t nora_i2c_ll_start_issue(
    nora_i2c_instance_t inst);
bool nora_i2c_ll_start_busy(nora_i2c_instance_t inst);
bool nora_i2c_ll_start_done(nora_i2c_instance_t inst);

/*
 * Repeated START primitive.
 *
 * A repeated START is a START condition generated without a preceding STOP.
 * The implementation uses the CK RSEN request bit and confirms completion when
 * the hardware clears RSEN.
 */
nora_i2c_status_t nora_i2c_ll_restart_issue(
    nora_i2c_instance_t inst);
bool nora_i2c_ll_restart_busy(nora_i2c_instance_t inst);
bool nora_i2c_ll_restart_done(nora_i2c_instance_t inst);

nora_i2c_status_t nora_i2c_ll_stop_issue(
    nora_i2c_instance_t inst);
bool nora_i2c_ll_stop_busy(nora_i2c_instance_t inst);
bool nora_i2c_ll_stop_done(nora_i2c_instance_t inst);

nora_i2c_status_t nora_i2c_ll_write_byte_issue(
    nora_i2c_instance_t inst,
    uint8_t data);
bool nora_i2c_ll_write_byte_busy(nora_i2c_instance_t inst);
bool nora_i2c_ll_write_byte_acked(nora_i2c_instance_t inst);

nora_i2c_status_t nora_i2c_ll_read_byte_issue(
    nora_i2c_instance_t inst);
bool nora_i2c_ll_read_byte_ready(nora_i2c_instance_t inst);
nora_i2c_status_t nora_i2c_ll_read_byte_get(
    nora_i2c_instance_t inst,
    uint8_t *data);

nora_i2c_status_t nora_i2c_ll_ack_issue(
    nora_i2c_instance_t inst,
    bool ack);
bool nora_i2c_ll_ack_busy(nora_i2c_instance_t inst);

bool nora_i2c_ll_has_error(nora_i2c_instance_t inst);
bool nora_i2c_ll_has_nack(nora_i2c_instance_t inst);
bool nora_i2c_ll_has_collision(nora_i2c_instance_t inst);

/* Interrupt helper API ----------------------------------------------------
 * This is deliberately small. The driver does not force an interrupt-driven
 * transfer engine. Users may call these helpers from their own ISR design.
 *
 * Current sandbox implementation returns NORA_I2C_ERR_UNSUPPORTED.
 * These functions are reserved for a future small interrupt helper layer.
 *
 * Declared here so the compile boundary matches the dsPIC33AK HAL exactly: AK
 * declares the same three functions with the same stub bodies, so portable code
 * that calls them compiles on both families and is refused at run time on both.
 * Without these declarations that code would fail to BUILD here only -- a
 * difference with no capability behind it. The backend's own per-instance slave
 * IRQ helpers (nora_i2c_slave_irq_* in nora_i2c_dspic33ck_device.h) are a
 * separate, silicon-specific thing and are not what these are.
 */

#define NORA_I2C_IRQ_TRANSFER_DONE   (1u << 0)
#define NORA_I2C_IRQ_ERROR           (1u << 1)
#define NORA_I2C_IRQ_BUS_COLLISION   (1u << 2)
#define NORA_I2C_IRQ_ALL             (NORA_I2C_IRQ_TRANSFER_DONE | \
                                           NORA_I2C_IRQ_ERROR | \
                                           NORA_I2C_IRQ_BUS_COLLISION)

nora_i2c_status_t nora_i2c_irq_enable(
    nora_i2c_instance_t inst,
    uint32_t irq_mask);

nora_i2c_status_t nora_i2c_irq_disable(
    nora_i2c_instance_t inst,
    uint32_t irq_mask);

nora_i2c_status_t nora_i2c_irq_clear(
    nora_i2c_instance_t inst,
    uint32_t irq_mask);

#ifdef __cplusplus
}
#endif

#endif /* NORA_I2C_MASTER_H */
