#ifndef I2C_PROBE_H
#define I2C_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#include "nora_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * i2c_probe.h -- does the I2C module itself work, with or without a device attached?
 *
 * WHAT IT ANSWERS, AND WHY THAT IS WORTH A MODULE
 * ----------------------------------------------
 * With nothing on the bus there is no external pull-up and no device to answer, so a
 * failed transaction proves nothing by itself. This runs one real write-read and
 * classifies what the MODULE did:
 *
 *   OK           something answered (only happens with a device actually wired)
 *   ERR_NACK     PASS of the mechanism: a real START, the address clocked out, and no
 *                ACK correctly detected. Exactly what "module works, nothing attached"
 *                looks like.
 *   TIMEOUT /    INCONCLUSIVE, reported as its own bucket rather than folded into NACK
 *   BUS /        or a bare FAIL: could be pin/fuse misconfiguration, could equally be
 *   COLLISION    an idle bus with no device and no bus capacitance. Collapsing distinct
 *                outcomes into one verdict is how a diagnosis goes wrong later -- the
 *                same reason the DMA self-test separates "did not transfer" from
 *                "transferred, data wrong" (docs/ck_hardware_notes.md).
 *
 * WAS boards/ev88g73a/ev88g73a_i2c1_probe.{c,h}
 * --------------------------------------------
 * Measured before moving it: its board content was FOUR VALUES (the instance, the two
 * RP numbers, the target address, the bus rate) plus nineteen calls into the board's own
 * UART functions -- no board logic. The values are the struct below, the output now goes
 * through app/console_out.h, and the two RP numbers never crossed the seam at all: pin
 * direction, analog-off and pull-ups are board work and stayed on the board, behind the
 * bus_init hook.
 */

typedef struct {
    nora_i2c_instance_t inst;

    /* Who to poke. A device's ID register is the usual choice: harmless to read, and it
     * has a known expected value if a device really is attached. */
    uint8_t addr7;
    uint8_t reg_pointer;

    /*
     * Bring the bus up: pins, pull-ups, HAL init. Board-owned, because that is the one
     * part of this that knows a pin number. Returning false is reported as
     * "FAIL (pin/HAL init)" and nothing is attempted on the bus.
     *
     * NULL means the bus is already up (a codec driver brought it up, say) -- the probe
     * then only transacts.
     */
    bool (*bus_init)(void);

    /*
     * One line of human context for the report, e.g.
     * "ASDA1=RP56(RC8) ASCL1=RP57(RC9), 400 kHz, no codec attached".
     *
     * The probe cannot know which pins a fuse or the board wiring selected, and a report
     * that does not say which wires it drove is a report nobody can act on.
     */
    const char *where;
} i2c_probe_t;

/*
 * Bring the bus up (if the hook is set) and run one transaction with the full
 * explanatory write-up. Boot-time call.
 */
void i2c_probe_run(const i2c_probe_t *probe);

/*
 * Re-run just the transaction. Call once per main-loop iteration: the transaction runs
 * on EVERY call, deliberately, because a scope on the bus needs a steady
 * START/ADDR/NACK to trigger on and that must not follow the console print cadence.
 * Only the one-line result is gated by `report`.
 *
 * Does NOT re-init: the pin/HAL configuration stays exactly what i2c_probe_run()
 * already verified once.
 */
void i2c_probe_poll(const i2c_probe_t *probe, bool report);

#ifdef __cplusplus
}
#endif

#endif /* I2C_PROBE_H */
