#include <xc.h>
#include "nora_i2c_dspic33ck_device.h"
#include "nora_i2c_slave.h"

/*
 * Device/instance mapping layer.
 *
 * This is the only place that should know about I2C1CONL/I2C2CONL/I2C3CONL
 * symbol names.  The rest of the driver uses only the register pointer table
 * returned from nora_i2c_get_device().
 *
 * The CK256MP508 has three I2C instances (I2C1..I2C3). The ADD/MSK slave
 * registers are wired in for completeness but are unused by the master engine.
 */

static const nora_i2c_device_t g_i2c_devices[NORA_I2C_INST_COUNT] = {
#if defined(I2C1CONL)
    [NORA_I2C_INST_1] = {
        .present = true,
        .regs = {
            .CONL = &I2C1CONL,
            .CONH = &I2C1CONH,
            .STAT = &I2C1STAT,
            .BRG  = &I2C1BRG,
            .TRN  = &I2C1TRN,
            .RCV  = &I2C1RCV,
            .ADD  = &I2C1ADD,
            .MSK  = &I2C1MSK,
        },
    },
#else
    [NORA_I2C_INST_1] = { .present = false },
#endif

#if defined(I2C2CONL)
    [NORA_I2C_INST_2] = {
        .present = true,
        .regs = {
            .CONL = &I2C2CONL,
            .CONH = &I2C2CONH,
            .STAT = &I2C2STAT,
            .BRG  = &I2C2BRG,
            .TRN  = &I2C2TRN,
            .RCV  = &I2C2RCV,
            .ADD  = &I2C2ADD,
            .MSK  = &I2C2MSK,
        },
    },
#else
    [NORA_I2C_INST_2] = { .present = false },
#endif

#if defined(I2C3CONL)
    [NORA_I2C_INST_3] = {
        .present = true,
        .regs = {
            .CONL = &I2C3CONL,
            .CONH = &I2C3CONH,
            .STAT = &I2C3STAT,
            .BRG  = &I2C3BRG,
            .TRN  = &I2C3TRN,
            .RCV  = &I2C3RCV,
            .ADD  = &I2C3ADD,
            .MSK  = &I2C3MSK,
        },
    },
#else
    [NORA_I2C_INST_3] = { .present = false },
#endif
};

const nora_i2c_device_t *nora_i2c_get_device(
    nora_i2c_instance_t inst)
{
    if ((unsigned)inst >= (unsigned)NORA_I2C_INST_COUNT) {
        return 0;
    }

    if (!g_i2c_devices[inst].present) {
        return 0;
    }

    return &g_i2c_devices[inst];
}

bool nora_i2c_instance_is_present(nora_i2c_instance_t inst)
{
    return (nora_i2c_get_device(inst) != 0);
}

nora_i2c_status_t nora_i2c_set_interrupt_priority(
    nora_i2c_instance_t inst,
    uint8_t priority)
{
    if ((unsigned)inst >= (unsigned)NORA_I2C_INST_COUNT) {
        return NORA_I2C_ERR_INVALID_ARG;
    }
    if (priority > 7u) {
        return NORA_I2C_ERR_INVALID_ARG;
    }
    if (nora_i2c_get_device(inst) == 0) {
        return NORA_I2C_ERR_NOT_PRESENT;
    }

    /*
     * MASTER (MI2Cx) PRIORITY ONLY, and the name is the reason this comment is
     * here: the CK classic peripheral has three separate interrupts -- master
     * (MI2Cx), slave (SI2Cx) and bus collision (I2CxBC) -- so "the I2C priority"
     * is not one thing. This call writes _MI2CxIP; the master vector is the
     * application's. The slave line has its own priority setter
     * (nora_i2c_slave_set_interrupt_priority()) and its vector is defined by
     * this backend, both described in nora_i2c_slave.h.
     */
    switch (inst) {
    case NORA_I2C_INST_1:
#if defined(_MI2C1IP)
        _MI2C1IP = priority;
        return NORA_I2C_OK;
#else
        break;
#endif
    case NORA_I2C_INST_2:
#if defined(_MI2C2IP)
        _MI2C2IP = priority;
        return NORA_I2C_OK;
#else
        break;
#endif
    case NORA_I2C_INST_3:
#if defined(_MI2C3IP)
        _MI2C3IP = priority;
        return NORA_I2C_OK;
#else
        break;
#endif
    default:
        break;
    }

    return NORA_I2C_ERR_UNSUPPORTED;
}

/* --------------------------------------------------------------------------
 * Slave (SI2Cx) interrupt control
 *
 * These confine the device-specific _SI2CxIF / _SI2CxIE / _SI2CxIP symbols to
 * the device layer, mirroring how nora_i2c_set_interrupt_priority() handles
 * _MI2CxIP. The slave-flag registers are scattered across the interrupt banks
 * on this part (SI2C1 -> IFS1/IEC1, SI2C2 -> IFS2/IEC2, SI2C3 -> IFS8/IEC8),
 * which is exactly why the engine goes through these accessors instead of a
 * bitmask.
 * -------------------------------------------------------------------------- */
void nora_i2c_slave_irq_enable(nora_i2c_instance_t inst)
{
    switch (inst) {
    case NORA_I2C_INST_1:
#if defined(_SI2C1IE)
        _SI2C1IE = 1;
#endif
        break;
    case NORA_I2C_INST_2:
#if defined(_SI2C2IE)
        _SI2C2IE = 1;
#endif
        break;
    case NORA_I2C_INST_3:
#if defined(_SI2C3IE)
        _SI2C3IE = 1;
#endif
        break;
    default:
        break;
    }
}

void nora_i2c_slave_irq_disable(nora_i2c_instance_t inst)
{
    switch (inst) {
    case NORA_I2C_INST_1:
#if defined(_SI2C1IE)
        _SI2C1IE = 0;
#endif
        break;
    case NORA_I2C_INST_2:
#if defined(_SI2C2IE)
        _SI2C2IE = 0;
#endif
        break;
    case NORA_I2C_INST_3:
#if defined(_SI2C3IE)
        _SI2C3IE = 0;
#endif
        break;
    default:
        break;
    }
}

bool nora_i2c_slave_irq_set_priority(nora_i2c_instance_t inst, uint8_t priority)
{
    /*
     * Returns false when this part has no _SI2CxIP symbol for the instance, so
     * the public wrapper can answer NORA_I2C_ERR_UNSUPPORTED instead of claiming
     * a priority was programmed. The enable/disable/clear helpers above are void
     * because a missing enable bit means the interrupt cannot fire at all, while
     * a missing priority bit would otherwise leave a silent reset-value default.
     */
    switch (inst) {
    case NORA_I2C_INST_1:
#if defined(_SI2C1IP)
        _SI2C1IP = priority;
        return true;
#else
        break;
#endif
    case NORA_I2C_INST_2:
#if defined(_SI2C2IP)
        _SI2C2IP = priority;
        return true;
#else
        break;
#endif
    case NORA_I2C_INST_3:
#if defined(_SI2C3IP)
        _SI2C3IP = priority;
        return true;
#else
        break;
#endif
    default:
        break;
    }

    return false;
}

void nora_i2c_slave_irq_clear(nora_i2c_instance_t inst)
{
    switch (inst) {
    case NORA_I2C_INST_1:
#if defined(_SI2C1IF)
        _SI2C1IF = 0;
#endif
        break;
    case NORA_I2C_INST_2:
#if defined(_SI2C2IF)
        _SI2C2IF = 0;
#endif
        break;
    case NORA_I2C_INST_3:
#if defined(_SI2C3IF)
        _SI2C3IF = 0;
#endif
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * Slave interrupt vectors
 *
 * Each vector delegates to the portable slave engine, which clears the flag and
 * services I2CxSTAT. Guarded per-instance so the driver only defines vectors
 * for I2C instances the target actually has.
 * -------------------------------------------------------------------------- */
#if defined(_SI2C1IF)
void __attribute__((interrupt, no_auto_psv)) _SI2C1Interrupt(void)
{
    nora_i2c_slave_event_irq(NORA_I2C_INST_1);
}
#endif

#if defined(_SI2C2IF)
void __attribute__((interrupt, no_auto_psv)) _SI2C2Interrupt(void)
{
    nora_i2c_slave_event_irq(NORA_I2C_INST_2);
}
#endif

#if defined(_SI2C3IF)
void __attribute__((interrupt, no_auto_psv)) _SI2C3Interrupt(void)
{
    nora_i2c_slave_event_irq(NORA_I2C_INST_3);
}
#endif
