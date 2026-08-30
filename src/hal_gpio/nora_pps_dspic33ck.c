/*
 * nora_pps_dspic33ck.c
 * ---------------
 * PPS routing implementation. See nora_pps.h for the layering and contract.
 * This is the CK sibling of dspic33ak_pps.c and follows the same structure:
 *   - output function code : _RPOUT_<sig>   (e.g. _RPOUT_SDO1)
 *   - output pin register  : _RP<nn>R       (RPORx bit-field alias)
 *   - input-select register: _<sig>R        (RPINRx bit-field alias)
 * A peripheral / RP the selected device header does not define is left out of
 * the relevant switch and the call returns false -- no per-device part-number
 * conditionals.
 */

#include "nora_pps.h"

#include <xc.h>
#include <stddef.h>
#include <stdint.h>

/* Physical remappable RP range on this classic-PPS device: ports B/C/D only. */
#define NORA_PPS_RP_MIN  32u
#define NORA_PPS_RP_MAX  79u

/* Virtual RP range (RPV0..RPV5). Padless; see the header. */
#define NORA_PPS_RPV_MIN 176u
#define NORA_PPS_RPV_MAX 181u


//===========================================================
// PPS lock gate (RPCON.IOLOCK)
//===========================================================
// Unlike dsPIC33A -- whose PAC leaves RPCON directly writable, so AK sets
// IOLOCK with a plain RPCONbits write -- the classic CK PPS block requires the
// __builtin_write_RPCON() unlock sequence to change IOLOCK. IOLOCK is bit 11
// (0x0800): write 0x0000 to unlock (IOLOCK = 0), 0x0800 to lock (IOLOCK = 1).
void nora_pps_unlock(void)
{
    __builtin_write_RPCON(0x0000);   /* IOLOCK = 0 : PPS writable  */
}

void nora_pps_lock(void)
{
    __builtin_write_RPCON(0x0800);   /* IOLOCK = 1 : PPS protected */
}


//===========================================================
// Local: peripheral output -> RPORx function code (_RPOUT_*)
//===========================================================
static bool nora_pps_get_output_code(nora_pps_output_t output, uint8_t *code)
{
    if (code == NULL)
    {
        return false;
    }

    switch (output)
    {
#ifdef _RPOUT_U1TX
    case NORA_PPS_OUTPUT_U1TX: *code = (uint8_t)_RPOUT_U1TX; return true;
#endif
#ifdef _RPOUT_U2TX
    case NORA_PPS_OUTPUT_U2TX: *code = (uint8_t)_RPOUT_U2TX; return true;
#endif
#ifdef _RPOUT_U3TX
    case NORA_PPS_OUTPUT_U3TX: *code = (uint8_t)_RPOUT_U3TX; return true;
#endif
#if defined(_RPOUT_SS1OUT)
    case NORA_PPS_OUTPUT_SS1:  *code = (uint8_t)_RPOUT_SS1OUT;  return true;
#elif defined(_RPOUT_SS1)
    // dsPIC33CK64MC105 (and siblings): same function, no OUT suffix.
    case NORA_PPS_OUTPUT_SS1:  *code = (uint8_t)_RPOUT_SS1;     return true;
#endif
#if defined(_RPOUT_SCK1OUT)
    case NORA_PPS_OUTPUT_SCK1: *code = (uint8_t)_RPOUT_SCK1OUT; return true;
#elif defined(_RPOUT_SCK1)
    case NORA_PPS_OUTPUT_SCK1: *code = (uint8_t)_RPOUT_SCK1;    return true;
#endif
#ifdef _RPOUT_SDO1
    case NORA_PPS_OUTPUT_SDO1: *code = (uint8_t)_RPOUT_SDO1; return true;
#endif
#if defined(_RPOUT_SS2OUT)
    case NORA_PPS_OUTPUT_SS2:  *code = (uint8_t)_RPOUT_SS2OUT;  return true;
#elif defined(_RPOUT_SS2)
    case NORA_PPS_OUTPUT_SS2:  *code = (uint8_t)_RPOUT_SS2;     return true;
#endif
#if defined(_RPOUT_SCK2OUT)
    case NORA_PPS_OUTPUT_SCK2: *code = (uint8_t)_RPOUT_SCK2OUT; return true;
#elif defined(_RPOUT_SCK2)
    case NORA_PPS_OUTPUT_SCK2: *code = (uint8_t)_RPOUT_SCK2;    return true;
#endif
#ifdef _RPOUT_SDO2
    case NORA_PPS_OUTPUT_SDO2: *code = (uint8_t)_RPOUT_SDO2; return true;
#endif
#ifdef _RPOUT_SS3OUT
    case NORA_PPS_OUTPUT_SS3:  *code = (uint8_t)_RPOUT_SS3OUT;  return true;
#endif
#ifdef _RPOUT_SCK3OUT
    case NORA_PPS_OUTPUT_SCK3: *code = (uint8_t)_RPOUT_SCK3OUT; return true;
#endif
#ifdef _RPOUT_SDO3
    case NORA_PPS_OUTPUT_SDO3: *code = (uint8_t)_RPOUT_SDO3; return true;
#endif
#if defined(_RPOUT_SS4OUT)
    case NORA_PPS_OUTPUT_SS4:  *code = (uint8_t)_RPOUT_SS4OUT;  return true;
#elif defined(_RPOUT_SS4)
    case NORA_PPS_OUTPUT_SS4:  *code = (uint8_t)_RPOUT_SS4;     return true;
#endif
#if defined(_RPOUT_SCK4OUT)
    case NORA_PPS_OUTPUT_SCK4: *code = (uint8_t)_RPOUT_SCK4OUT; return true;
#elif defined(_RPOUT_SCK4)
    case NORA_PPS_OUTPUT_SCK4: *code = (uint8_t)_RPOUT_SCK4;    return true;
#endif
#ifdef _RPOUT_SDO4
    case NORA_PPS_OUTPUT_SDO4: *code = (uint8_t)_RPOUT_SDO4; return true;
#endif
#ifdef _RPOUT_CLC1OUT
    case NORA_PPS_OUTPUT_CLC1: *code = (uint8_t)_RPOUT_CLC1OUT; return true;
#endif
#ifdef _RPOUT_CLC2OUT
    case NORA_PPS_OUTPUT_CLC2: *code = (uint8_t)_RPOUT_CLC2OUT; return true;
#endif
#ifdef _RPOUT_CLC3OUT
    case NORA_PPS_OUTPUT_CLC3: *code = (uint8_t)_RPOUT_CLC3OUT; return true;
#endif
#ifdef _RPOUT_CLC4OUT
    case NORA_PPS_OUTPUT_CLC4: *code = (uint8_t)_RPOUT_CLC4OUT; return true;
#endif
#ifdef _RPOUT_PWM4H
    case NORA_PPS_OUTPUT_PWM4H: *code = (uint8_t)_RPOUT_PWM4H; return true;
#endif
#ifdef _RPOUT_PWM4L
    case NORA_PPS_OUTPUT_PWM4L: *code = (uint8_t)_RPOUT_PWM4L; return true;
#endif
#ifdef _RPOUT_PWM1H
    case NORA_PPS_OUTPUT_PWM1H: *code = (uint8_t)_RPOUT_PWM1H; return true;
#endif
#ifdef _RPOUT_PWM2H
    case NORA_PPS_OUTPUT_PWM2H: *code = (uint8_t)_RPOUT_PWM2H; return true;
#endif
#ifdef _RPOUT_PWM3H
    case NORA_PPS_OUTPUT_PWM3H: *code = (uint8_t)_RPOUT_PWM3H; return true;
#endif
#ifdef _RPOUT_PWM5H
    case NORA_PPS_OUTPUT_PWM5H: *code = (uint8_t)_RPOUT_PWM5H; return true;
#endif
#ifdef _RPOUT_PWM5L
    case NORA_PPS_OUTPUT_PWM5L: *code = (uint8_t)_RPOUT_PWM5L; return true;
#endif
#ifdef _RPOUT_PWM6H
    case NORA_PPS_OUTPUT_PWM6H: *code = (uint8_t)_RPOUT_PWM6H; return true;
#endif
#ifdef _RPOUT_PWM6L
    case NORA_PPS_OUTPUT_PWM6L: *code = (uint8_t)_RPOUT_PWM6L; return true;
#endif
#ifdef _RPOUT_PWM7H
    case NORA_PPS_OUTPUT_PWM7H: *code = (uint8_t)_RPOUT_PWM7H; return true;
#endif
#ifdef _RPOUT_PWM7L
    case NORA_PPS_OUTPUT_PWM7L: *code = (uint8_t)_RPOUT_PWM7L; return true;
#endif
#ifdef _RPOUT_PWM8H
    case NORA_PPS_OUTPUT_PWM8H: *code = (uint8_t)_RPOUT_PWM8H; return true;
#endif
#ifdef _RPOUT_PWM8L
    case NORA_PPS_OUTPUT_PWM8L: *code = (uint8_t)_RPOUT_PWM8L; return true;
#endif
    /* PWM event outputs. CK silicon spells these PWMEA..PWMED; the neutral name
     * in nora_pps.h is the function, not either family's spelling. */
#ifdef _RPOUT_PWMEA
    case NORA_PPS_OUTPUT_PWM_EVENT_A: *code = (uint8_t)_RPOUT_PWMEA; return true;
#endif
#ifdef _RPOUT_PWMEB
    case NORA_PPS_OUTPUT_PWM_EVENT_B: *code = (uint8_t)_RPOUT_PWMEB; return true;
#endif
#ifdef _RPOUT_PWMEC
    case NORA_PPS_OUTPUT_PWM_EVENT_C: *code = (uint8_t)_RPOUT_PWMEC; return true;
#endif
#ifdef _RPOUT_PWMED
    case NORA_PPS_OUTPUT_PWM_EVENT_D: *code = (uint8_t)_RPOUT_PWMED; return true;
#endif
#ifdef _RPOUT_CMP1
    case NORA_PPS_OUTPUT_CMP1: *code = (uint8_t)_RPOUT_CMP1; return true;
#endif
#ifdef _RPOUT_CMP2
    case NORA_PPS_OUTPUT_CMP2: *code = (uint8_t)_RPOUT_CMP2; return true;
#endif
#ifdef _RPOUT_CMP3
    case NORA_PPS_OUTPUT_CMP3: *code = (uint8_t)_RPOUT_CMP3; return true;
#endif
    /* Reference clock output. CK256MP508 names it REFO1; CK64MC105 has ONE
     * reference clock and drops the digit (_RPOUT_REFO). Guarding only on
     * _RPOUT_REFO1 left this signal unroutable on MC105 -- silicon that has the
     * output, an enum value that names it, and a route call that returned false.
     * Same shape as the SS1OUT/SS1 pair above. */
#if defined(_RPOUT_REFO1)
    case NORA_PPS_OUTPUT_REFO1: *code = (uint8_t)_RPOUT_REFO1; return true;
#elif defined(_RPOUT_REFO)
    case NORA_PPS_OUTPUT_REFO1: *code = (uint8_t)_RPOUT_REFO;  return true;
#endif
#ifdef _RPOUT_REFO2
    case NORA_PPS_OUTPUT_REFO2: *code = (uint8_t)_RPOUT_REFO2; return true;
#endif
#ifdef _RPOUT_CAN1TX
    case NORA_PPS_OUTPUT_CAN1TX: *code = (uint8_t)_RPOUT_CAN1TX; return true;
#endif
#ifdef _RPOUT_CAN2TX
    case NORA_PPS_OUTPUT_CAN2TX: *code = (uint8_t)_RPOUT_CAN2TX; return true;
#endif
    /* CLC5..CLC10 exist on dsPIC33AK512MPS512, not on any CK part here; the
     * #ifdefs compile them out rather than needing a family split. */
#ifdef _RPOUT_CLC5OUT
    case NORA_PPS_OUTPUT_CLC5:  *code = (uint8_t)_RPOUT_CLC5OUT;  return true;
#endif
#ifdef _RPOUT_CLC6OUT
    case NORA_PPS_OUTPUT_CLC6:  *code = (uint8_t)_RPOUT_CLC6OUT;  return true;
#endif
#ifdef _RPOUT_CLC7OUT
    case NORA_PPS_OUTPUT_CLC7:  *code = (uint8_t)_RPOUT_CLC7OUT;  return true;
#endif
#ifdef _RPOUT_CLC8OUT
    case NORA_PPS_OUTPUT_CLC8:  *code = (uint8_t)_RPOUT_CLC8OUT;  return true;
#endif
#ifdef _RPOUT_CLC9OUT
    case NORA_PPS_OUTPUT_CLC9:  *code = (uint8_t)_RPOUT_CLC9OUT;  return true;
#endif
#ifdef _RPOUT_CLC10OUT
    case NORA_PPS_OUTPUT_CLC10: *code = (uint8_t)_RPOUT_CLC10OUT; return true;
#endif
#ifdef _RPOUT_CMP4
    case NORA_PPS_OUTPUT_CMP4: *code = (uint8_t)_RPOUT_CMP4; return true;
#endif
#ifdef _RPOUT_CMP5
    case NORA_PPS_OUTPUT_CMP5: *code = (uint8_t)_RPOUT_CMP5; return true;
#endif
#ifdef _RPOUT_CMP6
    case NORA_PPS_OUTPUT_CMP6: *code = (uint8_t)_RPOUT_CMP6; return true;
#endif
#ifdef _RPOUT_CMP7
    case NORA_PPS_OUTPUT_CMP7: *code = (uint8_t)_RPOUT_CMP7; return true;
#endif
#ifdef _RPOUT_CMP8
    case NORA_PPS_OUTPUT_CMP8: *code = (uint8_t)_RPOUT_CMP8; return true;
#endif
    default:
        break;
    }
    return false;   /* peripheral output not available on this device */
}


//===========================================================
// Local: write the output function code onto an RP pin's _RPnnR
//===========================================================
// _RPnnR is a bit-field alias (assignment only; no address / no formula). Every
// physical remappable RP on this device (RP32..RP79, ports B/C/D) is listed,
// each #ifdef-guarded so only the device's real registers compile. Ports A/E
// have no RPn; the RPV virtual outputs are excluded (this API is typed for
// physical GPIO RPs, matching the AK sibling). Flat switch by design -- a switch
// (not a formula) is the safe canonical form and mirrors dspic33ak_pps.c.
static bool nora_pps_write_output_rp(nora_gpio_rp_t rp, uint8_t code)
{
    switch (rp)
    {
#ifdef _RP32R
    case 32u: _RP32R = code; return true;
#endif
#ifdef _RP33R
    case 33u: _RP33R = code; return true;
#endif
#ifdef _RP34R
    case 34u: _RP34R = code; return true;
#endif
#ifdef _RP35R
    case 35u: _RP35R = code; return true;
#endif
#ifdef _RP36R
    case 36u: _RP36R = code; return true;
#endif
#ifdef _RP37R
    case 37u: _RP37R = code; return true;
#endif
#ifdef _RP38R
    case 38u: _RP38R = code; return true;
#endif
#ifdef _RP39R
    case 39u: _RP39R = code; return true;
#endif
#ifdef _RP40R
    case 40u: _RP40R = code; return true;
#endif
#ifdef _RP41R
    case 41u: _RP41R = code; return true;
#endif
#ifdef _RP42R
    case 42u: _RP42R = code; return true;
#endif
#ifdef _RP43R
    case 43u: _RP43R = code; return true;
#endif
#ifdef _RP44R
    case 44u: _RP44R = code; return true;
#endif
#ifdef _RP45R
    case 45u: _RP45R = code; return true;
#endif
#ifdef _RP46R
    case 46u: _RP46R = code; return true;
#endif
#ifdef _RP47R
    case 47u: _RP47R = code; return true;
#endif
#ifdef _RP48R
    case 48u: _RP48R = code; return true;
#endif
#ifdef _RP49R
    case 49u: _RP49R = code; return true;
#endif
#ifdef _RP50R
    case 50u: _RP50R = code; return true;
#endif
#ifdef _RP51R
    case 51u: _RP51R = code; return true;
#endif
#ifdef _RP52R
    case 52u: _RP52R = code; return true;
#endif
#ifdef _RP53R
    case 53u: _RP53R = code; return true;
#endif
#ifdef _RP54R
    case 54u: _RP54R = code; return true;
#endif
#ifdef _RP55R
    case 55u: _RP55R = code; return true;
#endif
#ifdef _RP56R
    case 56u: _RP56R = code; return true;
#endif
#ifdef _RP57R
    case 57u: _RP57R = code; return true;
#endif
#ifdef _RP58R
    case 58u: _RP58R = code; return true;
#endif
#ifdef _RP59R
    case 59u: _RP59R = code; return true;
#endif
#ifdef _RP60R
    case 60u: _RP60R = code; return true;
#endif
#ifdef _RP61R
    case 61u: _RP61R = code; return true;
#endif
#ifdef _RP62R
    case 62u: _RP62R = code; return true;
#endif
#ifdef _RP63R
    case 63u: _RP63R = code; return true;
#endif
#ifdef _RP64R
    case 64u: _RP64R = code; return true;
#endif
#ifdef _RP65R
    case 65u: _RP65R = code; return true;
#endif
#ifdef _RP66R
    case 66u: _RP66R = code; return true;
#endif
#ifdef _RP67R
    case 67u: _RP67R = code; return true;
#endif
#ifdef _RP68R
    case 68u: _RP68R = code; return true;
#endif
#ifdef _RP69R
    case 69u: _RP69R = code; return true;
#endif
#ifdef _RP70R
    case 70u: _RP70R = code; return true;
#endif
#ifdef _RP71R
    case 71u: _RP71R = code; return true;
#endif
#ifdef _RP72R
    case 72u: _RP72R = code; return true;
#endif
#ifdef _RP73R
    case 73u: _RP73R = code; return true;
#endif
#ifdef _RP74R
    case 74u: _RP74R = code; return true;
#endif
#ifdef _RP75R
    case 75u: _RP75R = code; return true;
#endif
#ifdef _RP76R
    case 76u: _RP76R = code; return true;
#endif
#ifdef _RP77R
    case 77u: _RP77R = code; return true;
#endif
#ifdef _RP78R
    case 78u: _RP78R = code; return true;
#endif
#ifdef _RP79R
    case 79u: _RP79R = code; return true;
#endif
    /* Virtual pins RPV0..RPV5 (RP176..RP181): padless on-chip routing endpoints.
     * Same _RPnnR aliases, same 6-bit code, so they belong in the same switch --
     * the only difference is that no GPIO configuration precedes them. */
#ifdef _RP176R
    case 176u: _RP176R = code; return true;
#endif
#ifdef _RP177R
    case 177u: _RP177R = code; return true;
#endif
#ifdef _RP178R
    case 178u: _RP178R = code; return true;
#endif
#ifdef _RP179R
    case 179u: _RP179R = code; return true;
#endif
#ifdef _RP180R
    case 180u: _RP180R = code; return true;
#endif
#ifdef _RP181R
    case 181u: _RP181R = code; return true;
#endif
    default:
        break;
    }
    return false;   /* no output PPS register for this RP on this device */
}


//===========================================================
// Local: read back an RP pin's _RPnnR output function code
//===========================================================
// Reading is the one direction that does NOT mirror the write switch's shape, and the
// reason is measured. _RPnnR is a readable bit-field alias, so a 48-case read switch is
// the obvious mirror -- but on CK64MC105/EV88G73A with --gc-sections on it cost 326
// bytes of flash (2026-08-03), because each case compiles to its own load/return and
// nothing can be discarded once find_output_rp() is referenced at all. A table sweep is
// a few instructions plus two bytes of const per pin.
//
// What keeps that safe is that the table is built from the SAME per-RP #ifdef list as
// the write switch: an entry exists only if that pin's register does, so the two cannot
// disagree about which pins the device has -- which is the point of centralising the map
// here instead of letting drivers keep private copies.
//
// Physical pins only: the reverse lookup answers "which pad", and including RPV0..5
// would let it report a padless pin as the answer.
#define NORA_PPS_RPOR_FIELD_MASK   (0x3Fu)   /* RPnR is a 6-bit output-select */
#define NORA_PPS_RPOR_FIELDS_PER   (2u)      /* two RPnR fields per 16-bit RPORx */

/*
 * RPn -> (RPORx index, field position) for every PHYSICAL remappable pin this device
 * has. Position is affine in the SLOT, not in RPn -- and on CK64MC105 the pins are
 * non-contiguous above RP61, so slot != rp - 32 there. Encoding the slot explicitly
 * is what makes that safe: the value comes from the DFP header via _RPnnR_POSITION /
 * the RPORx the alias lives in, so there is no arithmetic to get wrong.
 */
typedef struct
{
    uint8_t rp;     /* the RP number                      */
    uint8_t slot;   /* index of its 6-bit field in the bank */
} nora_pps_rpor_entry_t;

static const nora_pps_rpor_entry_t s_pps_rpor[] = {
#define NORA_PPS_RPOR_ENTRY(n, s)  { (uint8_t)(n), (uint8_t)(s) },
#ifdef _RP32R
    NORA_PPS_RPOR_ENTRY(32u,  0u)
#endif
#ifdef _RP33R
    NORA_PPS_RPOR_ENTRY(33u,  1u)
#endif
#ifdef _RP34R
    NORA_PPS_RPOR_ENTRY(34u,  2u)
#endif
#ifdef _RP35R
    NORA_PPS_RPOR_ENTRY(35u,  3u)
#endif
#ifdef _RP36R
    NORA_PPS_RPOR_ENTRY(36u,  4u)
#endif
#ifdef _RP37R
    NORA_PPS_RPOR_ENTRY(37u,  5u)
#endif
#ifdef _RP38R
    NORA_PPS_RPOR_ENTRY(38u,  6u)
#endif
#ifdef _RP39R
    NORA_PPS_RPOR_ENTRY(39u,  7u)
#endif
#ifdef _RP40R
    NORA_PPS_RPOR_ENTRY(40u,  8u)
#endif
#ifdef _RP41R
    NORA_PPS_RPOR_ENTRY(41u,  9u)
#endif
#ifdef _RP42R
    NORA_PPS_RPOR_ENTRY(42u, 10u)
#endif
#ifdef _RP43R
    NORA_PPS_RPOR_ENTRY(43u, 11u)
#endif
#ifdef _RP44R
    NORA_PPS_RPOR_ENTRY(44u, 12u)
#endif
#ifdef _RP45R
    NORA_PPS_RPOR_ENTRY(45u, 13u)
#endif
#ifdef _RP46R
    NORA_PPS_RPOR_ENTRY(46u, 14u)
#endif
#ifdef _RP47R
    NORA_PPS_RPOR_ENTRY(47u, 15u)
#endif
#ifdef _RP48R
    NORA_PPS_RPOR_ENTRY(48u, 16u)
#endif
#ifdef _RP49R
    NORA_PPS_RPOR_ENTRY(49u, 17u)
#endif
#ifdef _RP50R
    NORA_PPS_RPOR_ENTRY(50u, 18u)
#endif
#ifdef _RP51R
    NORA_PPS_RPOR_ENTRY(51u, 19u)
#endif
#ifdef _RP52R
    NORA_PPS_RPOR_ENTRY(52u, 20u)
#endif
#ifdef _RP53R
    NORA_PPS_RPOR_ENTRY(53u, 21u)
#endif
#ifdef _RP54R
    NORA_PPS_RPOR_ENTRY(54u, 22u)
#endif
#ifdef _RP55R
    NORA_PPS_RPOR_ENTRY(55u, 23u)
#endif
#ifdef _RP56R
    NORA_PPS_RPOR_ENTRY(56u, 24u)
#endif
#ifdef _RP57R
    NORA_PPS_RPOR_ENTRY(57u, 25u)
#endif
#ifdef _RP58R
    NORA_PPS_RPOR_ENTRY(58u, 26u)
#endif
#ifdef _RP59R
    NORA_PPS_RPOR_ENTRY(59u, 27u)
#endif
#ifdef _RP60R
    NORA_PPS_RPOR_ENTRY(60u, 28u)
#endif
#ifdef _RP61R
    NORA_PPS_RPOR_ENTRY(61u, 29u)
#endif
    /* From here the two supported parts diverge: MP508 continues contiguously with
     * RP62.., while MC105 has no RP62..64 and resumes at RP65 in the NEXT slot. Each
     * entry states its own slot, so both descriptions are just data. */
#ifdef _RP62R
    NORA_PPS_RPOR_ENTRY(62u, 30u)
#endif
#ifdef _RP63R
    NORA_PPS_RPOR_ENTRY(63u, 31u)
#endif
#ifdef _RP64R
    NORA_PPS_RPOR_ENTRY(64u, 32u)
#endif
#if defined(_RP65R) && defined(_RP64R)
    NORA_PPS_RPOR_ENTRY(65u, 33u)   /* MP508: contiguous after RP64 */
#elif defined(_RP65R)
    NORA_PPS_RPOR_ENTRY(65u, 30u)   /* MC105: RPOR15 low field      */
#endif
#ifdef _RP66R
    NORA_PPS_RPOR_ENTRY(66u, 34u)
#endif
#ifdef _RP67R
    NORA_PPS_RPOR_ENTRY(67u, 35u)
#endif
#ifdef _RP68R
    NORA_PPS_RPOR_ENTRY(68u, 36u)
#endif
#ifdef _RP69R
    NORA_PPS_RPOR_ENTRY(69u, 37u)
#endif
#ifdef _RP70R
    NORA_PPS_RPOR_ENTRY(70u, 38u)
#endif
#ifdef _RP71R
    NORA_PPS_RPOR_ENTRY(71u, 39u)
#endif
#if defined(_RP72R) && defined(_RP71R)
    NORA_PPS_RPOR_ENTRY(72u, 40u)   /* MP508: contiguous            */
#elif defined(_RP72R)
    NORA_PPS_RPOR_ENTRY(72u, 31u)   /* MC105: RPOR15 high field     */
#endif
#ifdef _RP73R
    NORA_PPS_RPOR_ENTRY(73u, 41u)
#endif
#if defined(_RP74R) && defined(_RP73R)
    NORA_PPS_RPOR_ENTRY(74u, 42u)   /* MP508: contiguous            */
#elif defined(_RP74R)
    NORA_PPS_RPOR_ENTRY(74u, 32u)   /* MC105: RPOR16 low field      */
#endif
#ifdef _RP75R
    NORA_PPS_RPOR_ENTRY(75u, 43u)
#endif
#ifdef _RP76R
    NORA_PPS_RPOR_ENTRY(76u, 44u)
#endif
#if defined(_RP77R) && defined(_RP76R)
    NORA_PPS_RPOR_ENTRY(77u, 45u)   /* MP508: contiguous            */
#elif defined(_RP77R)
    NORA_PPS_RPOR_ENTRY(77u, 33u)   /* MC105: RPOR16 high field     */
#endif
#ifdef _RP78R
    NORA_PPS_RPOR_ENTRY(78u, 46u)
#endif
#ifdef _RP79R
    NORA_PPS_RPOR_ENTRY(79u, 47u)
#endif
#undef NORA_PPS_RPOR_ENTRY
};

#define NORA_PPS_RPOR_COUNT \
    (sizeof(s_pps_rpor) / sizeof(s_pps_rpor[0]))

/* Slot i lives in (&RPOR0)[i / 2], field (i odd) ? [13:8] : [5:0]. Affine in the
 * SLOT, which is exactly what the table above supplies. */
static uint8_t nora_pps_read_slot(uint8_t slot)
{
    const volatile uint16_t *reg = (&RPOR0) + (slot / NORA_PPS_RPOR_FIELDS_PER);
    const uint16_t           pos = ((slot & 1u) != 0u) ? 8u : 0u;

    return (uint8_t)((*reg >> pos) & NORA_PPS_RPOR_FIELD_MASK);
}


bool nora_pps_route_output(nora_pps_output_t output, nora_gpio_rp_t rp)
{
    uint8_t code;
    bool ok;

    if (!nora_pps_get_output_code(output, &code))
    {
        return false;
    }

    nora_pps_unlock();
    ok = nora_pps_write_output_rp(rp, code);
    nora_pps_lock();
    return ok;
}


bool nora_pps_find_output_rp(nora_pps_output_t output, nora_gpio_rp_t *rp)
{
    uint8_t want;
    uint32_t i;

    if (rp == NULL)
    {
        return false;
    }
    if (!nora_pps_get_output_code(output, &want))
    {
        return false;   /* peripheral output not available on this device */
    }

    /* Sweep the device's physical remappable pins in table order. The table contains
     * exactly the pins that exist, so there are no gaps to know about and no pin is
     * visited that has no register. Read-only (no IOLOCK), and only ever called at
     * bring-up -- no hot path. */
    for (i = 0u; i < (uint32_t)NORA_PPS_RPOR_COUNT; ++i)
    {
        if (nora_pps_read_slot(s_pps_rpor[i].slot) == want)
        {
            *rp = (nora_gpio_rp_t)s_pps_rpor[i].rp;
            return true;
        }
    }
    return false;   /* no physical pin currently carries this output */
}


bool nora_pps_route_input(nora_pps_input_t input, nora_gpio_rp_t rp)
{
    bool ok = true;

    /* Reject any RP that is neither a physical remappable pin nor a virtual one
     * before writing. RPINRx input-selects take the RP NUMBER, and the virtual
     * numbers are legal values there (that is how a padless output reaches another
     * peripheral's input) -- so the range check admits both bands and nothing
     * between or beyond them. */
    if (((rp < NORA_PPS_RP_MIN)  || (rp > NORA_PPS_RP_MAX)) &&
        ((rp < NORA_PPS_RPV_MIN) || (rp > NORA_PPS_RPV_MAX)))
    {
        return false;
    }

    nora_pps_unlock();

    switch (input)
    {
#ifdef _U1RXR
    case NORA_PPS_INPUT_U1RX: _U1RXR = rp; break;
#endif
#ifdef _U2RXR
    case NORA_PPS_INPUT_U2RX: _U2RXR = rp; break;
#endif
#ifdef _U3RXR
    case NORA_PPS_INPUT_U3RX: _U3RXR = rp; break;
#endif
#ifdef _SS1R
    case NORA_PPS_INPUT_SS1:  _SS1R  = rp; break;
#endif
#ifdef _SCK1R
    case NORA_PPS_INPUT_SCK1: _SCK1R = rp; break;
#endif
#ifdef _SDI1R
    case NORA_PPS_INPUT_SDI1: _SDI1R = rp; break;
#endif
#ifdef _SS2R
    case NORA_PPS_INPUT_SS2:  _SS2R  = rp; break;
#endif
#ifdef _SCK2R
    case NORA_PPS_INPUT_SCK2: _SCK2R = rp; break;
#endif
#ifdef _SDI2R
    case NORA_PPS_INPUT_SDI2: _SDI2R = rp; break;
#endif
#ifdef _SS3R
    case NORA_PPS_INPUT_SS3:  _SS3R  = rp; break;
#endif
#ifdef _SCK3R
    case NORA_PPS_INPUT_SCK3: _SCK3R = rp; break;
#endif
#ifdef _SDI3R
    case NORA_PPS_INPUT_SDI3: _SDI3R = rp; break;
#endif
#ifdef _SS4R
    case NORA_PPS_INPUT_SS4:  _SS4R  = rp; break;
#endif
#ifdef _SCK4R
    case NORA_PPS_INPUT_SCK4: _SCK4R = rp; break;
#endif
#ifdef _SDI4R
    case NORA_PPS_INPUT_SDI4: _SDI4R = rp; break;
#endif
#ifdef _CLCINAR
    case NORA_PPS_INPUT_CLCINA: _CLCINAR = rp; break;
#endif
#ifdef _CLCINBR
    case NORA_PPS_INPUT_CLCINB: _CLCINBR = rp; break;
#endif
#ifdef _CLCINCR
    case NORA_PPS_INPUT_CLCINC: _CLCINCR = rp; break;
#endif
#ifdef _CLCINDR
    case NORA_PPS_INPUT_CLCIND: _CLCINDR = rp; break;
#endif
#ifdef _REFI1R
    case NORA_PPS_INPUT_REFI1: _REFI1R = rp; break;
#endif
#ifdef _INT1R
    case NORA_PPS_INPUT_INT1: _INT1R = rp; break;
#endif
#ifdef _INT2R
    case NORA_PPS_INPUT_INT2: _INT2R = rp; break;
#endif
#ifdef _INT3R
    case NORA_PPS_INPUT_INT3: _INT3R = rp; break;
#endif
#ifdef _CAN1RXR
    case NORA_PPS_INPUT_CAN1RX: _CAN1RXR = rp; break;
#endif
#ifdef _ICM1R
    case NORA_PPS_INPUT_ICM1: _ICM1R = rp; break;
#endif
#ifdef _ICM2R
    case NORA_PPS_INPUT_ICM2: _ICM2R = rp; break;
#endif
#ifdef _ICM3R
    case NORA_PPS_INPUT_ICM3: _ICM3R = rp; break;
#endif
#ifdef _ICM4R
    case NORA_PPS_INPUT_ICM4: _ICM4R = rp; break;
#endif
#ifdef _ICM5R
    case NORA_PPS_INPUT_ICM5: _ICM5R = rp; break;
#endif
#ifdef _ICM6R
    case NORA_PPS_INPUT_ICM6: _ICM6R = rp; break;
#endif
#ifdef _ICM7R
    case NORA_PPS_INPUT_ICM7: _ICM7R = rp; break;
#endif
#ifdef _ICM8R
    case NORA_PPS_INPUT_ICM8: _ICM8R = rp; break;
#endif
#ifdef _ICM9R
    case NORA_PPS_INPUT_ICM9: _ICM9R = rp; break;
#endif
    /* AK-side members of families already covered above. No CK part defines
     * _INT4R or _CLCINER..._CLCINJR, so these compile out here. */
#ifdef _INT4R
    case NORA_PPS_INPUT_INT4: _INT4R = rp; break;
#endif
#ifdef _CLCINER
    case NORA_PPS_INPUT_CLCINE: _CLCINER = rp; break;
#endif
#ifdef _CLCINFR
    case NORA_PPS_INPUT_CLCINF: _CLCINFR = rp; break;
#endif
#ifdef _CLCINGR
    case NORA_PPS_INPUT_CLCING: _CLCINGR = rp; break;
#endif
#ifdef _CLCINHR
    case NORA_PPS_INPUT_CLCINH: _CLCINHR = rp; break;
#endif
#ifdef _CLCINIR
    case NORA_PPS_INPUT_CLCINI: _CLCINIR = rp; break;
#endif
#ifdef _CLCINJR
    case NORA_PPS_INPUT_CLCINJ: _CLCINJR = rp; break;
#endif
#ifdef _REFI2R
    case NORA_PPS_INPUT_REFI2: _REFI2R = rp; break;
#endif
#ifdef _CAN2RXR
    case NORA_PPS_INPUT_CAN2RX: _CAN2RXR = rp; break;
#endif
    default:
        ok = false;   /* peripheral input not available on this device */
        break;
    }

    nora_pps_lock();
    return ok;
}


bool nora_pinmux_route_input(nora_pps_input_t function,
                                  nora_gpio_rp_t rp)
{
    if (!nora_gpio_rp_config_digital_input(rp))
    {
        return false;
    }

    return nora_pps_route_input(function, rp);
}


bool nora_pinmux_route_output(nora_pps_output_t function,
                                   nora_gpio_rp_t rp,
                                   bool initial_high)
{
    if (!nora_gpio_rp_config_digital_output(rp, initial_high))
    {
        return false;
    }

    return nora_pps_route_output(function, rp);
}
