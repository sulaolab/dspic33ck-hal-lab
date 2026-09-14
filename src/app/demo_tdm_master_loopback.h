#ifndef DEMO_TDM_MASTER_LOOPBACK_H
#define DEMO_TDM_MASTER_LOOPBACK_H

/*
 * Codec-less SPI/I2S/TDM MASTER loopback demo (Stage 3 HW smoke test).
 *
 * Compile-time gated by DEMO_ENABLE_TDM_MASTER_LOOPBACK (OFF by default) so the baseline
 * demo keeps building/running without it. When enabled, dsPIC33CK SPI1 is the
 * clock master: it self-generates BCLK (SCK), a 50%-duty FS (via CLC1, exercising
 * the Stage-2 fs_clc path), and SDO, running a TDM8 / 32-bit stream through the
 * transport HAL with RX/TX ping-pong DMA and a ramp-generating block callback.
 *
 * Wiring, on whichever four pins the board hands it: scope BCLK / FS / SDO, and jumper
 * SDO->SDI to loop the data back so the callback sees its own output one block later.
 *
 * IT USED TO NAME FOUR RP NUMBERS ITSELF -- DM330030's mikroBUS-A ones (RP72/66/70/71),
 * inside app/, with its own configure_pins that duplicated what that board's file already
 * did in the other direction. Two consequences, both bad, and one is why they are gone:
 *
 *   - a module shared between boards owned ONE board's pinout, so this demo could only
 *     ever run on that board -- while the board with actual hardware (EV88G73A) has the
 *     same transport and had its own private copy of this exerciser, since deleted.
 *   - the copy existed because that board's routing function was client-only, so the
 *     demo could not call it. It takes a role now (dm330030_tdm_pins_init), which is
 *     what let the duplicate go.
 *
 * So the pinout arrives through demo_tdm_master_loopback_port_t, exactly as
 * app/wm8904_audio.h takes wm8904_audio_port_t: the demo knows the TDM config and the
 * block callback, the board knows the pins. app/ now contains no RP number for any board.
 *
 * This is the first MASTER smoke test: it verifies self-clocked BCLK/FS cadence
 * and the CLC 50%-FS on a scope, and the fs_clc HW assumptions.
 */

#include <stdbool.h>

#include "nora_spi_i2s_tdm.h"   /* nora_spi_i2s_tdm_clock_role_t */

/*
 * What this demo needs from a board. One hook, deliberately the same signature the
 * transport HAL's own port asks for (configure_pins(role)), because that is all it is:
 * the demo forwards it, adding only the check that the role really is MASTER.
 *
 *   configure_pins  route BCLK/FS/SDO/SDI for the role. False => this board cannot take
 *                   that role, and start() aborts with a message saying so.
 *   wiring          the pins, as text, so the status line says which wires to scope.
 *                   A demo that does not say what to probe is a demo nobody can use.
 */
typedef struct {
    bool (*configure_pins)(nora_spi_i2s_tdm_clock_role_t role);
    const char *wiring;
} demo_tdm_master_loopback_port_t;

/* configure + open(MASTER) + start SPI1. `port` must outlive the demo (a static const in
 * the board's main.c); a NULL port, or a NULL configure_pins, is reported and does
 * nothing -- the transport is never opened with no pins routed. */
void demo_tdm_master_loopback_start(const demo_tdm_master_loopback_port_t *port);

void demo_tdm_master_loopback_poll(void);    /* print block/load status (call from main loop) */

#endif /* DEMO_TDM_MASTER_LOOPBACK_H */
