#ifndef NORA_PPS_H
#define NORA_PPS_H

/*
 * nora_pps.h
 * ---------------
 * Peripheral Pin Select (PPS) routing for dsPIC33CK -- the companion to the
 * GPIO HAL (same hal_gpio family). hal_gpio owns the pin's ELECTRICAL attributes
 * (TRIS/LAT/ANSEL/pull/OD); this module owns the SIGNAL ROUTING (which RP pin a
 * peripheral input reads from / a peripheral output drives), i.e. the RPINRx
 * (input-select) and RPORx (output _RPnnR) registers.
 *
 * This is the CK sibling of the AK PPS HAL and mirrors it deliberately (same
 * API, same enum shape, same self-bracketed IOLOCK contract) so AK and CK read
 * as one family. Only the silicon-forced differences live in the .c:
 *   - CK is the classic PPS block: remappable pins are RP32..RP79 (ports B/C/D
 *     only, RB0 = RP32); ports A/E have no RPn. The RP<->pin map lives in
 *     nora_gpio_dspic33ck.c.
 *   - CK requires the __builtin_write_RPCON() unlock sequence to change IOLOCK
 *     (unlike AK, whose PAC leaves RPCON directly writable). See the .c.
 *
 * Why a thin PPS layer:
 *   Board code otherwise writes device SFRs directly -- e.g.
 *       _RP66R = _RPOUT_SDO1;   // SDO1 out on RP66 (what is 66? what is the code?)
 *       _SDI1R = 66;            // SDI1 in from RP66
 *   which leaks the RPORx/RPINRx map and the raw function codes into the board.
 *   These two calls hide that:
 *       nora_pps_route_output(NORA_PPS_OUTPUT_SDO1, 66u);
 *       nora_pps_route_input (NORA_PPS_INPUT_SDI1,  66u);
 *   leaving only "which signal" + "which RP" at the call site.
 *
 * Device adaptation: the implementation keys off the XC SFR/constant macros
 * (_RPOUT_xxx, _RPnnR, _<sig>R) with #ifdef, so a peripheral or RP the selected
 * device does not define is simply unroutable (the call returns false) -- no
 * per-device part-number conditionals here.
 */

#include <stdbool.h>

#include "nora_gpio.h"   /* nora_gpio_rp_t */

/*
 * VIRTUAL RP PINS (RPV0..RPV5 == RP176..RP181).
 *
 * These are remappable PPS endpoints with NO PAD: a peripheral output routed to one
 * is readable by another peripheral's PPS input and by nothing else. They are how a
 * signal is fed from one on-chip block to another without spending a package pin --
 * the CLC frame-sync generator in hal_spi_i2s_tdm uses RPV0 to hand the SPI's FRMSYNC
 * marker to a CLC input.
 *
 * They are deliberately NOT nora_gpio_pin_t values: they have no TRIS, no LAT
 * and no ANSEL, so nothing in the GPIO half of this family applies to them. They are
 * nora_gpio_rp_t values, because routing is the one thing they DO have -- which
 * is why the two route_*() calls below take them and the gpio config calls do not.
 *
 * Availability is per device (checked via _RP176R etc. in the .c); a virtual pin the
 * device does not define is simply unroutable and the call returns false.
 *
 * HOW MANY IS A SILICON COUNT, like NORA_GPIO_RP_MAX. This family has six
 * (RP176..RP181) on every part here; the dsPIC33AK family has sixteen
 * (RPV0..RPV15 == RP129..RP144 on dsPIC33AK512MPS512, none at all on
 * dsPIC33AK128MC106). So the numbers behind these names are NOT portable and were
 * never going to be -- but the NAMES are, and code that stays within RPV0..RPV5
 * compiles and routes on both families. Beyond RPV5 is AK-only.
 */
#define NORA_PPS_RP_VIRTUAL_FIRST   ((nora_gpio_rp_t)176u)
#define NORA_PPS_RPV0               ((nora_gpio_rp_t)176u)
#define NORA_PPS_RPV1               ((nora_gpio_rp_t)177u)
#define NORA_PPS_RPV2               ((nora_gpio_rp_t)178u)
#define NORA_PPS_RPV3               ((nora_gpio_rp_t)179u)
#define NORA_PPS_RPV4               ((nora_gpio_rp_t)180u)
#define NORA_PPS_RPV5               ((nora_gpio_rp_t)181u)

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PPS OUTPUT functions (a peripheral output driven onto an RP pin).
 *
 * The enum is the CK/AK source-compatibility union and is kept TEXTUALLY
 * IDENTICAL to the same enum in the dsPIC33AK NORA HAL, so a board or driver
 * that names a signal compiles on both families. A name may be accepted by this
 * header even when the selected CK device has no matching PPS output; in that
 * case pps_route_output() returns false and performs no write. Keeping the
 * logical signal name available lets board code report "not supported on this
 * target" at run time instead of needing a preprocessor split merely to compile.
 *
 * MEMBERSHIP RULE: for every peripheral family the enum names, it carries every
 * instance either family's device headers define. It is NOT every PPS-capable
 * signal on either part -- OCM, SENT, QEI, BISS and PTGTRG are absent as whole
 * families. Adding a signal here means adding it to the AK header too.
 *
 * The implementation maps supported names to their _RPOUT_x values, so the
 * device header remains the authority for availability -- and the two families'
 * differing spellings for one function (see PWM_EVENT below) are resolved in
 * the backend, not here.
 */
typedef enum
{
    NORA_PPS_OUTPUT_U1TX,
    NORA_PPS_OUTPUT_U2TX,
    NORA_PPS_OUTPUT_U3TX,

    NORA_PPS_OUTPUT_SS1,
    NORA_PPS_OUTPUT_SCK1,
    NORA_PPS_OUTPUT_SDO1,

    NORA_PPS_OUTPUT_SS2,
    NORA_PPS_OUTPUT_SCK2,
    NORA_PPS_OUTPUT_SDO2,

    NORA_PPS_OUTPUT_SS3,
    NORA_PPS_OUTPUT_SCK3,
    NORA_PPS_OUTPUT_SDO3,

    NORA_PPS_OUTPUT_CLC1,
    NORA_PPS_OUTPUT_CLC2,
    NORA_PPS_OUTPUT_CLC3,
    NORA_PPS_OUTPUT_CLC4,

    NORA_PPS_OUTPUT_PWM4H,
    NORA_PPS_OUTPUT_PWM4L,

    /*
     * PWM EVENT outputs. The two families name this same signal differently in
     * silicon -- CK calls it PWMEA..PWMED (_RPOUT_PWMEA), AK calls it
     * PEVTA..PEVTD (_RPOUT_PEVTA) -- so neither spelling can be the neutral one
     * without leaking one family's vocabulary into a header shared by both. The
     * name here is the FUNCTION; each backend maps it to whatever its device
     * header calls it.
     */
    NORA_PPS_OUTPUT_PWM_EVENT_A,
    NORA_PPS_OUTPUT_PWM_EVENT_B,
    NORA_PPS_OUTPUT_PWM_EVENT_C,
    NORA_PPS_OUTPUT_PWM_EVENT_D,

    NORA_PPS_OUTPUT_CMP1,
    NORA_PPS_OUTPUT_CMP2,
    NORA_PPS_OUTPUT_CMP3,

    NORA_PPS_OUTPUT_REFO1,

    NORA_PPS_OUTPUT_CAN1TX,

    /*
     * The rest of the union: signals present in the OTHER family's silicon (or
     * on a larger part of either) for a peripheral family already named above.
     * Appended rather than interleaved so the values above do not move.
     *
     * The rule that decides membership: for every peripheral family this enum
     * already covers, carry every instance either family's device headers
     * define. It is deliberately not "every PPS-capable signal on both
     * families" -- OCM, SENT, QEI, BISS and PTGTRG are absent from the enum
     * entirely, and stay absent until something needs them.
     */
    NORA_PPS_OUTPUT_SS4,
    NORA_PPS_OUTPUT_SCK4,
    NORA_PPS_OUTPUT_SDO4,
    NORA_PPS_OUTPUT_PWM1H,
    NORA_PPS_OUTPUT_PWM2H,
    NORA_PPS_OUTPUT_PWM3H,
    NORA_PPS_OUTPUT_PWM5H,
    NORA_PPS_OUTPUT_PWM5L,
    NORA_PPS_OUTPUT_PWM6H,
    NORA_PPS_OUTPUT_PWM6L,
    NORA_PPS_OUTPUT_PWM7H,
    NORA_PPS_OUTPUT_PWM7L,
    NORA_PPS_OUTPUT_PWM8H,
    NORA_PPS_OUTPUT_PWM8L,

    /* CLC5..CLC10: dsPIC33AK512MPS512 has ten CLC output selects. */
    NORA_PPS_OUTPUT_CLC5,
    NORA_PPS_OUTPUT_CLC6,
    NORA_PPS_OUTPUT_CLC7,
    NORA_PPS_OUTPUT_CLC8,
    NORA_PPS_OUTPUT_CLC9,
    NORA_PPS_OUTPUT_CLC10,

    /* CMP4..CMP8: dsPIC33AK512MPS512 has eight comparator outputs. */
    NORA_PPS_OUTPUT_CMP4,
    NORA_PPS_OUTPUT_CMP5,
    NORA_PPS_OUTPUT_CMP6,
    NORA_PPS_OUTPUT_CMP7,
    NORA_PPS_OUTPUT_CMP8,

    /* Second reference clock output: both AK parts define _RPOUT_REFO2. */
    NORA_PPS_OUTPUT_REFO2,

    /* Second CAN: dsPIC33AK512MPS512 defines _RPOUT_CAN2TX. */
    NORA_PPS_OUTPUT_CAN2TX
} nora_pps_output_t;

/*
 * PPS INPUT functions (a peripheral input fed from an RP pin). Like the output
 * enum above, this is the CK/AK source-compatibility union and is kept
 * TEXTUALLY IDENTICAL to its dsPIC33AK counterpart: an unsupported target
 * signal is rejected by pps_route_input() with false and no register write.
 * Each supported value maps to its RPINRx input-select register (a _<sig>R
 * bit-field alias; assignment takes the RP number directly).
 *
 * ICM1..ICM9 are the SCCP/MCCP Input Capture inputs (-> RPINR2..6). Route a pin
 * there to feed a CCP channel's Input Capture (AK's hal_ccp_input_capture; no
 * CK counterpart yet, and CK parts define only _ICM1R.._ICM4R or _ICM1R.._ICM9R
 * depending on the part).
 */
typedef enum
{
    NORA_PPS_INPUT_U1RX,
    NORA_PPS_INPUT_U2RX,
    NORA_PPS_INPUT_U3RX,

    NORA_PPS_INPUT_SS1,
    NORA_PPS_INPUT_SCK1,
    NORA_PPS_INPUT_SDI1,

    NORA_PPS_INPUT_SS2,
    NORA_PPS_INPUT_SCK2,
    NORA_PPS_INPUT_SDI2,

    NORA_PPS_INPUT_SS3,
    NORA_PPS_INPUT_SCK3,
    NORA_PPS_INPUT_SDI3,

    NORA_PPS_INPUT_CLCINA,
    NORA_PPS_INPUT_CLCINB,
    NORA_PPS_INPUT_CLCINC,
    NORA_PPS_INPUT_CLCIND,

    NORA_PPS_INPUT_INT1,
    NORA_PPS_INPUT_INT2,
    NORA_PPS_INPUT_INT3,

    NORA_PPS_INPUT_CAN1RX,

    /* The rest of the union -- same membership rule as the output enum above;
     * appended so the values before this point do not move. */
    NORA_PPS_INPUT_SS4,
    NORA_PPS_INPUT_SCK4,
    NORA_PPS_INPUT_SDI4,
    NORA_PPS_INPUT_REFI1,
    NORA_PPS_INPUT_ICM1,
    NORA_PPS_INPUT_ICM2,
    NORA_PPS_INPUT_ICM3,
    NORA_PPS_INPUT_ICM4,
    NORA_PPS_INPUT_ICM5,
    NORA_PPS_INPUT_ICM6,
    NORA_PPS_INPUT_ICM7,
    NORA_PPS_INPUT_ICM8,
    NORA_PPS_INPUT_ICM9,

    /* Fourth external interrupt: both AK parts define _INT4R; no CK part does. */
    NORA_PPS_INPUT_INT4,

    /* CLCINE..CLCINJ: dsPIC33AK512MPS512 has ten CLC input selects. */
    NORA_PPS_INPUT_CLCINE,
    NORA_PPS_INPUT_CLCINF,
    NORA_PPS_INPUT_CLCING,
    NORA_PPS_INPUT_CLCINH,
    NORA_PPS_INPUT_CLCINI,
    NORA_PPS_INPUT_CLCINJ,

    /* Second reference clock input: both AK parts define _REFI2R. No CK part
     * defines even _REFI1R, so both REFI values are AK-side here. */
    NORA_PPS_INPUT_REFI2,

    /* Second CAN: dsPIC33AK512MPS512 defines _CAN2RXR. */
    NORA_PPS_INPUT_CAN2RX
} nora_pps_input_t;

/*
 * PPS lock gate (RPCON.IOLOCK). unlock() makes the PPS map writable, lock()
 * protects it. route_*() below bracket their own writes; expose these only for
 * direct-SFR PPS writers or a single window around a batch.
 */
void nora_pps_unlock(void);   /* IOLOCK = 0 : PPS registers writable  */
void nora_pps_lock(void);     /* IOLOCK = 1 : PPS registers protected */

/*
 * Route a peripheral OUTPUT onto an RP pin (writes the RP's _RPnnR with the
 * peripheral's output function code). Self-brackets IOLOCK. Returns false if the
 * peripheral output is not available on this device, or the RP pin has no output
 * PPS register here (range/encoding contract only -- not a board-bonding check;
 * configure the GPIO output first via nora_gpio_rp_config_digital_output()).
 *
 * rp may be a VIRTUAL pin (NORA_PPS_RPV0..5): routing an output to one sends
 * the signal on-chip only, and there is no GPIO configuration step to do first.
 */
bool nora_pps_route_output(nora_pps_output_t output, nora_gpio_rp_t rp);

/*
 * Route a peripheral INPUT to read from an RP pin (writes the peripheral's RPINRx
 * input-select with the RP number). Self-brackets IOLOCK. Returns false if the
 * peripheral input is not available on this device, OR if rp is neither a physical
 * remappable pin nor a virtual one on this device (rejected before any register
 * write). For a physical pin, configure the GPIO input first via
 * nora_gpio_rp_config_digital_input(); for a virtual pin there is nothing to
 * configure -- the source is whatever output was routed onto it.
 */
bool nora_pps_route_input(nora_pps_input_t input, nora_gpio_rp_t rp);

/*
 * Combined pinmux helpers, matching the AK PPS contract.
 *
 * They first apply the GPIO digital configuration and only then write PPS. This
 * makes the common board-code sequence atomic at the API level and preserves the
 * glitch-aware output order used by gpio_rp_config_digital_output(). Virtual RPV
 * endpoints are deliberately rejected here because they are PPS-only and cannot
 * be configured as GPIO; route_*() remains the API for virtual routing.
 */
bool nora_pinmux_route_input(nora_pps_input_t function,
                                  nora_gpio_rp_t rp);
bool nora_pinmux_route_output(nora_pps_output_t function,
                                   nora_gpio_rp_t rp,
                                   bool initial_high);

/*
 * REVERSE LOOKUP: which physical RP pin currently drives this peripheral output?
 *
 * Scans the device's output PPS registers for the output's function code and reports
 * the first PHYSICAL pin carrying it (virtual pins are skipped -- a caller asking
 * "which pad is this on" does not want RPV0, and callers that care about the virtual
 * side put the signal there themselves and so already know). Returns false if the
 * output is unavailable on this device or no pin currently carries it.
 *
 * WHY THIS IS IN THE HAL. It answers "where did the board route this", which a
 * peripheral driver otherwise cannot ask without reimplementing the RPORx bank
 * layout -- and that layout is the thing most easily got wrong, because RPn is NOT an
 * affine function of the register slot on every part (CK64MC105's remappable pins are
 * non-contiguous above RP61, so slot-arithmetic silently addresses unrelated SFR
 * space). hal_spi_i2s_tdm's CLC frame-sync generator had exactly that bug, from
 * exactly that cause, before this call existed. Keeping the map in one place -- the
 * flat per-RP switch this file already maintains for writing -- means there is only
 * one copy to be right.
 *
 * Does not write anything, so it does not touch IOLOCK.
 */
bool nora_pps_find_output_rp(nora_pps_output_t output, nora_gpio_rp_t *rp);

#ifdef __cplusplus
}
#endif

#endif /* NORA_PPS_H */
