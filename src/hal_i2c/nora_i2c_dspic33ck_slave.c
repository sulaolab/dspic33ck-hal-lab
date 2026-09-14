#include "nora_i2c_slave.h"
#include "nora_i2c_dspic33ck_device.h"
#include "nora_i2c_dspic33ck_reg.h"
#include "nora_i2c_dspic33ck_internal.h"

/* --------------------------------------------------------------------------
 * dsPIC33CK I2C slave engine (interrupt-driven, callback-based).
 *
 * CK reformulation note (vs the AK source this was ported from):
 *
 * The dsPIC33A "new" I2C module runs the client in a smart/FIFO mode with a
 * one-byte packet size (CON2.PSZ) and routes address / data / transmit-continue
 * / STOP onto a single event interrupt via the INTC register (CADDRIE / CDRXIE
 * / CDTXIE / CLTIE). The classic CK module has neither CON2 nor INTC: it is the
 * plain byte-at-a-time client, and its single slave interrupt (SI2Cx) already
 * fires on the address match, on every received/transmitted data byte and -
 * with CONH.PCIE set - on STOP. So the whole INTC routing block and the
 * dedicated RX/TX delegates collapse into one vector here.
 *
 * The single service routine polls I2CxSTAT and acts on whatever is pending;
 * reading I2CxRCV clears RBF, so a spurious re-entry is a harmless no-op.
 *
 * I2CxSTAT tells us what happened:
 *   RBF  - a byte (address or data) is in RCV
 *   D_A  - 0 = that byte was the address, 1 = data
 *   R_W  - at the address, 1 = master reads from us (we transmit)
 *   P    - STOP detected
 *   ACKSTAT - after a transmitted byte, 0 = master ACKed (wants more)
 * After handling a byte we set CONL.SCLREL to release SCL: the hardware holds
 * the clock after the address byte (and, when STREN is set, after each byte)
 * even in a simple slave, so releasing it is always required.
 *
 * Address acceptance and per-byte ACK are left to the hardware auto-ACK path
 * (CONH.AHEN / CONH.DHEN stay 0): this callback model always accepts, matching
 * the AK slave, so the ACK-hold machinery is intentionally unused.
 * -------------------------------------------------------------------------- */

static nora_i2c_slave_config_t g_cfg[NORA_I2C_INST_COUNT];
static bool                         g_active[NORA_I2C_INST_COUNT];
static bool                         g_reading[NORA_I2C_INST_COUNT];

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_slave_set_interrupt_priority(
    nora_i2c_instance_t inst,
    uint8_t priority)
{
    /* Same validation order as nora_i2c_set_interrupt_priority(), so the two
     * priority setters answer identically for a bad argument or a missing
     * instance and differ only in which line they program. */
    if ((unsigned)inst >= (unsigned)NORA_I2C_INST_COUNT) {
        return NORA_I2C_ERR_INVALID_ARG;
    }
    if (priority > 7u) {
        return NORA_I2C_ERR_INVALID_ARG;
    }
    if (!nora_i2c_instance_is_present(inst)) {
        return NORA_I2C_ERR_NOT_PRESENT;
    }

    if (!nora_i2c_slave_irq_set_priority(inst, priority)) {
        return NORA_I2C_ERR_UNSUPPORTED;
    }

    return NORA_I2C_OK;
}

nora_i2c_status_t nora_i2c_slave_init(
    nora_i2c_instance_t inst,
    const nora_i2c_slave_config_t *config)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st;

    if (config == 0 || config->addr7 > 0x7Fu) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = nora_i2c_get_regs(inst, &r);
    if (st != NORA_I2C_OK) {
        return st;
    }

    /* The slave engine needs the slave-only registers (ADD/MSK). An instance
     * whose device table entry only maps the master registers must not be
     * driven as a slave - that would dereference NULL. Reject it instead. */
    if (r->ADD == 0 || r->MSK == 0) {
        return NORA_I2C_ERR_NOT_PRESENT;
    }

    /*
     * The mirror of the guard in nora_i2c_init(): an instance live as a master
     * must be released with nora_i2c_deinit() before it can answer as a slave.
     * Taking it here would leave the master engine's initialized flag and its
     * pending-transaction state behind, so a later master call would act on a
     * peripheral this engine has reprogrammed. Re-initializing the slave role
     * itself stays allowed.
     */
    if (nora_i2c_get_role(inst) == NORA_I2C_ROLE_MASTER) {
        return NORA_I2C_ERR_BUSY;
    }

    /* Start from a known disabled state; enable after configuration is
     * complete. Classic client mode: 7-bit addressing (A10M = 0) and hardware
     * auto-ACK (AHEN/DHEN = 0), so a zeroed control pair is the baseline. */
    nora_i2c_reg_write(r->CONL, 0u);

    if (config->clock_stretch) {
        nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_STREN);
    }

    /* PCIE makes a STOP condition raise the slave interrupt. On the CK classic
     * module this bit lives in CONH (the AK module carried PCIE in CON1). */
    nora_i2c_reg_write(r->CONH, NORA_I2C_CONH_PCIE);

    /* 7-bit own address (right-justified in ADD<6:0>) and address mask. */
    nora_i2c_reg_write(r->ADD, (uint16_t)config->addr7);
    nora_i2c_reg_write(r->MSK, (uint16_t)config->addr_mask);

    /* Drop any stale receive-overflow latch. */
    nora_i2c_reg_clear(r->STAT, NORA_I2C_STAT_I2COV);

    g_cfg[inst]     = *config;
    g_reading[inst] = false;
    g_active[inst]  = true;
    nora_i2c_set_role(inst, NORA_I2C_ROLE_SLAVE);

    /* Enable the single slave (SI2Cx) interrupt. */
    nora_i2c_slave_irq_clear(inst);
    nora_i2c_slave_irq_enable(inst);

    /* Release SCL and turn the slave on. */
    nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_SCLREL);
    nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_I2CEN);

    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Deinit
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_slave_deinit(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st;

    st = nora_i2c_get_regs(inst, &r);
    if (st != NORA_I2C_OK) {
        return st;
    }

    /* As in slave_init: an instance without slave register mappings was never
     * (and cannot be) a slave, so there is nothing to tear down. */
    if (r->ADD == 0 || r->MSK == 0) {
        return NORA_I2C_ERR_NOT_PRESENT;
    }

    nora_i2c_slave_irq_disable(inst);

    nora_i2c_reg_clear(r->CONL, NORA_I2C_CONL_I2CEN);

    g_active[inst]  = false;
    g_reading[inst] = false;
    nora_i2c_set_role(inst, NORA_I2C_ROLE_NONE);

    return NORA_I2C_OK;
}

bool nora_i2c_slave_is_active(nora_i2c_instance_t inst)
{
    if (!nora_i2c_inst_is_valid(inst)) {
        return false;
    }
    return g_active[inst];
}

/* --------------------------------------------------------------------------
 * Shared service: poll I2CxSTAT and act on whatever is pending. Called from the
 * slave interrupt delegate; idempotent because reading RCV clears RBF.
 * -------------------------------------------------------------------------- */
static uint8_t next_tx_byte(nora_i2c_instance_t inst)
{
    if (g_cfg[inst].on_tx_byte != 0) {
        return g_cfg[inst].on_tx_byte();
    }
    return 0xFFu;
}

static void slave_service(nora_i2c_instance_t inst,
                          const nora_i2c_regs_t *r)
{
    uint16_t stat = *r->STAT;

    if ((stat & NORA_I2C_STAT_RBF) != 0u) {
        uint8_t b = (uint8_t)(*r->RCV & 0xFFu);     /* read clears RBF */

        if ((stat & NORA_I2C_STAT_D_A) == 0u) {
            /* Address byte: latch direction and notify. The hardware has
             * stretched SCL after the address; release it below. */
            bool is_read = ((stat & NORA_I2C_STAT_R_W) != 0u);
            g_reading[inst] = is_read;

            if (g_cfg[inst].on_addr_match != 0) {
                g_cfg[inst].on_addr_match(is_read);
            }
            if (is_read) {
                /* Master-read: load the first byte to transmit. */
                nora_i2c_reg_write(r->TRN, (uint16_t)next_tx_byte(inst));
            }
        } else if (!g_reading[inst]) {
            /* Master-write data byte. */
            if (g_cfg[inst].on_rx_byte != 0) {
                g_cfg[inst].on_rx_byte(b);
            }
        }

        nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_SCLREL);
    } else if (g_reading[inst]) {
        /* Master-read in progress: this interrupt is the falling edge of the
         * ACK/NACK after the byte we just transmitted. */
        if ((stat & NORA_I2C_STAT_ACKSTAT) == 0u) {
            /* ACK: the host wants more -> load the next byte and release. */
            nora_i2c_reg_write(r->TRN, (uint16_t)next_tx_byte(inst));
            nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_SCLREL);
        } else {
            /* NACK: the read is finished. Do not write TRN again; the module
             * stops stretching on its own and a STOP follows. */
            g_reading[inst] = false;
        }
    }

    if ((stat & NORA_I2C_STAT_P) != 0u) {
        /* STOP: end of transaction. */
        g_reading[inst] = false;
        if (g_cfg[inst].on_stop != 0) {
            g_cfg[inst].on_stop();
        }
    }

    /* Never let a receive-overflow latch wedge the slave. */
    if (nora_i2c_reg_is_set(r->STAT, NORA_I2C_STAT_I2COV)) {
        nora_i2c_reg_clear(r->STAT, NORA_I2C_STAT_I2COV);
    }
}

/* --------------------------------------------------------------------------
 * Interrupt delegate: clear the slave flag *before* servicing, so a new event
 * raised while servicing (the master can resume the moment we release SCLREL)
 * keeps its flag set and re-enters rather than being cleared away. The service
 * routine is idempotent, so a spurious re-entry is harmless.
 * -------------------------------------------------------------------------- */
void nora_i2c_slave_event_irq(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return;
    }
    nora_i2c_slave_irq_clear(inst);
    if (g_active[inst]) {
        slave_service(inst, r);
    }
}
