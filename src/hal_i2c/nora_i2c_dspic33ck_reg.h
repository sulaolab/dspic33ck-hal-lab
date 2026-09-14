#ifndef NORA_I2C_DSPIC33CK_REG_H
#define NORA_I2C_DSPIC33CK_REG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Internal register helper layer for the classic dsPIC33CK I2C peripheral.
 *
 * This file intentionally uses 16-bit register pointers and bit masks instead
 * of XC-DSC bitfield structures such as I2C1CONLBITS.  The goal is to keep
 * compiler/DFP-specific details away from the public API.
 *
 * Unlike the dsPIC33AK "new" 32-bit I2C module, the CK peripheral is the
 * classic 16-bit module:
 *   - control split across I2CxCONL / I2CxCONH (no CON1/CON2)
 *   - a single I2CxSTAT status register (no STAT1/STAT2)
 *   - a single I2CxBRG baud generator (no LBRG/HBRG)
 *   - no bus-idle timeout (BITO) and no INTC interrupt-routing register
 *   - three separate interrupts: master (MI2Cx), slave (SI2Cx), collision
 *
 * Bit positions were checked against:
 *   Microchip.dsPIC33CK-MP_DFP.1.15.423
 *   xc-dsc/support/dsPIC33C/h/p33CK256MP508.h  (I2C1CONLBITS / I2C1STATBITS)
 *
 * Keep this file small.  Add only the bits actually used by the master driver.
 */

typedef struct {
    volatile uint16_t *CONL;   /* I2CxCONL: SEN/RSEN/PEN/RCEN/ACKEN/ACKDT/I2CEN */
    volatile uint16_t *CONH;   /* I2CxCONH: DHEN/AHEN/SBCDE/SDAHT/BOEN/SCIE/PCIE */
    volatile uint16_t *STAT;   /* I2CxSTAT: RBF/S/D_A/I2COV/IWCOL/BCL/TRSTAT/ACKSTAT */
    volatile uint16_t *BRG;    /* I2CxBRG: baud rate generator reload */
    volatile uint16_t *TRN;    /* I2CxTRN: transmit register */
    volatile uint16_t *RCV;    /* I2CxRCV: receive register */
    volatile uint16_t *ADD;    /* I2CxADD: slave own-address (unused by master) */
    volatile uint16_t *MSK;    /* I2CxMSK: slave address mask (unused by master) */
} nora_i2c_regs_t;

/* I2CxCONL bits (control low) */
#define NORA_I2C_CONL_SEN      (1u << 0)   /* I2CxCONLbits.SEN   (START request)    */
#define NORA_I2C_CONL_RSEN     (1u << 1)   /* I2CxCONLbits.RSEN  (repeated START)   */
#define NORA_I2C_CONL_PEN      (1u << 2)   /* I2CxCONLbits.PEN   (STOP request)     */
#define NORA_I2C_CONL_RCEN     (1u << 3)   /* I2CxCONLbits.RCEN  (receive enable)   */
#define NORA_I2C_CONL_ACKEN    (1u << 4)   /* I2CxCONLbits.ACKEN (ACK/NACK request) */
#define NORA_I2C_CONL_ACKDT    (1u << 5)   /* I2CxCONLbits.ACKDT (0=ACK, 1=NACK)    */
#define NORA_I2C_CONL_STREN    (1u << 6)   /* I2CxCONLbits.STREN (slave clock stretch) */
#define NORA_I2C_CONL_GCEN     (1u << 7)   /* I2CxCONLbits.GCEN  (general-call enable)  */
#define NORA_I2C_CONL_A10M     (1u << 10)  /* I2CxCONLbits.A10M  (10-bit own address)   */
#define NORA_I2C_CONL_STRICT   (1u << 11)  /* I2CxCONLbits.STRICT */
#define NORA_I2C_CONL_SCLREL   (1u << 12)  /* I2CxCONLbits.SCLREL (release clock)       */
#define NORA_I2C_CONL_I2CEN    (1u << 15)  /* I2CxCONLbits.I2CEN  (module enable, == AK ON) */

/* I2CxCONH bits (control high) */
#define NORA_I2C_CONH_DHEN     (1u << 0)   /* I2CxCONHbits.DHEN  (data hold, slave)     */
#define NORA_I2C_CONH_AHEN     (1u << 1)   /* I2CxCONHbits.AHEN  (address hold, slave)  */
#define NORA_I2C_CONH_SBCDE    (1u << 2)   /* I2CxCONHbits.SBCDE (start/stop int, slave) */
#define NORA_I2C_CONH_SDAHT    (1u << 3)   /* I2CxCONHbits.SDAHT (SDA hold time)        */
#define NORA_I2C_CONH_BOEN     (1u << 4)   /* I2CxCONHbits.BOEN  (buffer overwrite)     */
#define NORA_I2C_CONH_SCIE     (1u << 5)   /* I2CxCONHbits.SCIE  (START int, slave)     */
#define NORA_I2C_CONH_PCIE     (1u << 6)   /* I2CxCONHbits.PCIE  (STOP int, slave)      */

/* I2CxSTAT bits */
#define NORA_I2C_STAT_TBF      (1u << 0)   /* I2CxSTATbits.TBF     (transmit buffer full) */
#define NORA_I2C_STAT_RBF      (1u << 1)   /* I2CxSTATbits.RBF     (receive buffer full)  */
#define NORA_I2C_STAT_R_W      (1u << 2)   /* I2CxSTATbits.R_W     (slave: read/write)    */
#define NORA_I2C_STAT_S        (1u << 3)   /* I2CxSTATbits.S       (START detected)       */
#define NORA_I2C_STAT_P        (1u << 4)   /* I2CxSTATbits.P       (STOP detected)        */
#define NORA_I2C_STAT_D_A      (1u << 5)   /* I2CxSTATbits.D_A     (0=address, 1=data)    */
#define NORA_I2C_STAT_I2COV    (1u << 6)   /* I2CxSTATbits.I2COV   (receive overflow)     */
#define NORA_I2C_STAT_IWCOL    (1u << 7)   /* I2CxSTATbits.IWCOL   (write collision)      */
#define NORA_I2C_STAT_ADD10    (1u << 8)   /* I2CxSTATbits.ADD10                          */
#define NORA_I2C_STAT_GCSTAT   (1u << 9)   /* I2CxSTATbits.GCSTAT  (general call)         */
#define NORA_I2C_STAT_BCL      (1u << 10)  /* I2CxSTATbits.BCL     (bus collision)        */
#define NORA_I2C_STAT_ACKTIM   (1u << 13)  /* I2CxSTATbits.ACKTIM  (ACK time, slave)      */
#define NORA_I2C_STAT_TRSTAT   (1u << 14)  /* I2CxSTATbits.TRSTAT  (transmit in progress) */
#define NORA_I2C_STAT_ACKSTAT  (1u << 15)  /* I2CxSTATbits.ACKSTAT (0=ACK, 1=NACK recvd)  */

static inline void nora_i2c_reg_set(volatile uint16_t *reg, uint16_t mask)
{
    *reg = (uint16_t)(*reg | mask);
}

static inline void nora_i2c_reg_clear(volatile uint16_t *reg, uint16_t mask)
{
    *reg = (uint16_t)(*reg & (uint16_t)~mask);
}

static inline void nora_i2c_reg_write(volatile uint16_t *reg, uint16_t value)
{
    *reg = value;
}

static inline bool nora_i2c_reg_is_set(volatile uint16_t *reg, uint16_t mask)
{
    return ((*reg & mask) != 0u);
}

static inline void nora_i2c_reg_write_field(
    volatile uint16_t *reg,
    uint16_t mask,
    uint16_t value)
{
    *reg = (uint16_t)((*reg & (uint16_t)~mask) | (value & mask));
}

#endif /* NORA_I2C_DSPIC33CK_REG_H */
