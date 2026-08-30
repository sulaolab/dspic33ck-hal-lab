/*
 * i2c_probe.c -- see the header for what this classifies and why the buckets matter.
 *
 * No board-ownership guard: what used to be board-specific here is now either the
 * caller's struct or, for the pins, behind the board's own bus_init hook.
 */

#include "i2c_probe.h"

#include <stddef.h>
#include <stdint.h>

#include "console_out.h"
#include "nora_i2c_master.h"

#define PROBE_RX_LEN  2u

static nora_i2c_status_t probe_transact(const i2c_probe_t *probe,
                                            uint8_t rx[PROBE_RX_LEN])
{
    uint8_t tx[1];

    tx[0] = probe->reg_pointer;

    return nora_i2c_write_read(probe->inst, probe->addr7,
                                    tx, sizeof(tx), rx, PROBE_RX_LEN);
}

void i2c_probe_run(const i2c_probe_t *probe)
{
    uint8_t rx[PROBE_RX_LEN] = { 0u, 0u };
    nora_i2c_status_t st;

    if (probe == NULL) {
        return;
    }

    console_out_str("\nI2C probe: ");
    console_out_str((probe->where != NULL) ? probe->where : "(bus not described)");
    console_out_str("\n");

    if ((probe->bus_init != NULL) && !probe->bus_init()) {
        console_out_str("  FAIL (pin/HAL init)\n");
        return;
    }

    st = probe_transact(probe, rx);

    console_out_str("  target addr7=0x");
    console_out_hex16((uint16_t)probe->addr7);
    console_out_str(" reg=0x");
    console_out_hex16((uint16_t)probe->reg_pointer);
    console_out_str(" -> status=");
    console_out_str(nora_i2c_status_str(st));
    console_out_str(" rx=0x");
    console_out_hex16(rx[0]);
    console_out_hex16(rx[1]);
    console_out_str("\n");

    switch (st) {
    case NORA_I2C_OK:
        /* A real answer -- only happens with a device actually wired here. */
        console_out_str(
            "  I2C probe: OK -- a device answered (unexpected with nothing attached; "
            "if one IS wired, rx should read its ID)\n");
        break;
    case NORA_I2C_ERR_NACK:
        /* The expected, GOOD result with nothing on the bus: the module drove a real
         * START, clocked out the address, and correctly detected no ACK. The mechanism
         * works; there is simply no device to answer it. */
        console_out_str(
            "  I2C probe: PASS (mechanism) -- clean NACK, exactly what \"module works, "
            "nothing attached\" looks like\n");
        break;
    case NORA_I2C_ERR_TIMEOUT:
    case NORA_I2C_ERR_BUS:
    case NORA_I2C_ERR_COLLISION:
        /* Inconclusive, not a clean pass/fail -- see the header for why this is its own
         * bucket rather than being folded into NACK or FAIL. */
        console_out_str(
            "  I2C probe: INCONCLUSIVE -- timeout/bus/collision, not a clean NACK. "
            "Could be pin/fuse wiring or just an idle bus with no real device; needs a "
            "scope or a real device to settle.\n");
        break;
    default:
        console_out_str("  I2C probe: FAIL (see status above)\n");
        break;
    }
}

void i2c_probe_poll(const i2c_probe_t *probe, bool report)
{
    uint8_t rx[PROBE_RX_LEN] = { 0u, 0u };
    nora_i2c_status_t st;

    if (probe == NULL) {
        return;
    }

    /* Runs on every call, before the report gate: the steady bus activity is the point
     * (see the header). Only the print below is throttled. */
    st = probe_transact(probe, rx);

    if (!report) {
        return;
    }

    console_out_str("I2C poll: status=");
    console_out_str(nora_i2c_status_str(st));
    console_out_str("\n");
}
