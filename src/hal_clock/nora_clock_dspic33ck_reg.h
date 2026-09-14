#ifndef NORA_CLOCK_DSPIC33CK_REG_H
#define NORA_CLOCK_DSPIC33CK_REG_H

#include <stdint.h>
#include <stdbool.h>

#include "nora_clock.h"
#include "nora_clock_dspic33ck.h"

/*
 * dsPIC33CK oscillator register layer: the only file that touches OSCCON, CLKDIV,
 * PLLFBD and PLLDIV.
 *
 * Owns the OSWEN switch sequence, the LOCK wait, and the timeout budget. Callers pass
 * already-encoded NOSC values and already-solved divider fields -- logical source names
 * and board policy do not belong here, and neither does any decision about WHETHER a
 * write is allowed. Preflight lives above this layer, because a refusal must happen
 * before anything here is called.
 *
 * READING IS ONE PASS, ON PURPOSE
 *   Every question about the current clock is answered from a single capture, so a
 *   caller cannot compose an answer from two reads taken either side of a switch and
 *   get a state the hardware was never in. That is a weaker promise than atomicity and
 *   the contract says so: no interrupt is disabled here.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* NOSC / COSC oscillator selection encodings (OSCCON<10:8> and <14:12>). */
#define NORA_CLOCK_DSPIC33CK_NOSC_FRC      (0x0u)   /* Fast RC                  */
#define NORA_CLOCK_DSPIC33CK_NOSC_FRCPLL   (0x1u)   /* Fast RC through the PLL  */
#define NORA_CLOCK_DSPIC33CK_NOSC_PRI      (0x2u)   /* Primary (XT/HS/EC)       */
#define NORA_CLOCK_DSPIC33CK_NOSC_PRIPLL   (0x3u)   /* Primary through the PLL  */
#define NORA_CLOCK_DSPIC33CK_NOSC_LPRC     (0x5u)   /* Low-power RC             */
#define NORA_CLOCK_DSPIC33CK_NOSC_BFRC     (0x6u)   /* Backup Fast RC           */
#define NORA_CLOCK_DSPIC33CK_NOSC_FRCDIVN  (0x7u)   /* FRC divided by FRCDIV    */

/*
 * One capture, decoded. Field values are as the registers hold them: PLLPRE and the two
 * POSTDIVs are already divisors (the silicon stores them that way), FRCDIV is a code and
 * needs nora_clock_device_dspic33ck_frcdiv_divisor().
 */
typedef struct {
    uint16_t cosc;       /* OSCCON.COSC  -- what the system runs on NOW          */
    uint16_t nosc;       /* OSCCON.NOSC  -- what was last requested              */
    bool     oswen;      /* a switch is in progress                              */
    bool     lock;       /* the PLL is locked                                    */
    bool     cf;         /* clock failure was detected                           */
    bool     clklock;    /* clock and PLL configuration is locked against writes */
    uint16_t pllpre;     /* CLKDIV.PLLPRE   -- divisor                           */
    uint16_t frcdiv;     /* CLKDIV.FRCDIV   -- code, not a divisor               */
    uint16_t pllfbdiv;   /* PLLFBD.PLLFBDIV -- multiplier                        */
    uint16_t post1div;   /* PLLDIV.POST1DIV -- divisor                           */
    uint16_t post2div;   /* PLLDIV.POST2DIV -- divisor                           */
} nora_clock_dspic33ck_fields_t;

/*
 * Read the four oscillator words once and hand back the raw words, the decoded fields,
 * or both. Either pointer may be NULL. Reads only.
 */
void nora_clock_dspic33ck_reg_capture(
    nora_clock_dspic33ck_capture_t *raw,
    nora_clock_dspic33ck_fields_t *fields);

typedef struct {
    uint16_t feedback_div;   /* PLLFBD.PLLFBDIV   (12-bit)         */
    uint16_t pre_div;        /* CLKDIV.PLLPRE     (6-bit divider)  */
    uint16_t post_div1;      /* PLLDIV.POST1DIV   (3-bit divider)  */
    uint16_t post_div2;      /* PLLDIV.POST2DIV   (3-bit divider)  */
} nora_clock_dspic33ck_reg_pll_config_t;

/*
 * Program the PLL divider chain (CLKDIV.PLLPRE, PLLFBD, PLLDIV POST1/POST2).
 *
 * Does NOT switch the clock. That separation is the contract's
 * (nora_clock_pll_configure() "DOES NOT SWITCH"), and on this silicon it is also a
 * hardware requirement: these fields must not be written while the PLL they describe is
 * driving the system clock, so the caller has to have established that it is not.
 */
nora_clock_status_t nora_clock_dspic33ck_reg_pll_program(
    const nora_clock_dspic33ck_reg_pll_config_t *config);

/*
 * Request a switch to the given NOSC encoding via OSCCON.OSWEN, wait for OSWEN to
 * clear, and when wait_lock is set also wait for OSCCON.LOCK.
 *
 * Both waits are bounded by an internal poll budget rather than a time, because the
 * frequency the loop runs at is the very thing being changed. On NORA_CLOCK_ERR_TIMEOUT
 * *diag says which wait gave up -- a source that never started and a PLL that never
 * locked are the same status and different problems. *diag is written on success too
 * (NONE), so the caller never has to remember to clear it.
 */
nora_clock_status_t nora_clock_dspic33ck_reg_switch(
    uint16_t nosc,
    bool wait_lock,
    uint16_t *diag);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CLOCK_DSPIC33CK_REG_H */
