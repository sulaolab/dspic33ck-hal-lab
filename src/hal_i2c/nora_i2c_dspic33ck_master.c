#include "nora_i2c_master.h"
#include "nora_i2c_dspic33ck_device.h"
#include "nora_i2c_dspic33ck_reg.h"
#include "nora_i2c_dspic33ck_internal.h"

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

typedef enum {
    NORA_I2C_PENDING_NONE = 0,
    NORA_I2C_PENDING_MASTER_WRITE
} nora_i2c_pending_state_t;

static uint32_t g_timeout_ms[NORA_I2C_INST_COUNT];
static nora_i2c_get_ms_fn g_get_ms[NORA_I2C_INST_COUNT];
static bool g_initialized[NORA_I2C_INST_COUNT];
static nora_i2c_pending_state_t g_pending_state[NORA_I2C_INST_COUNT];
static uint32_t g_pending_start_ms[NORA_I2C_INST_COUNT];
static uint32_t g_pending_timeout_ms[NORA_I2C_INST_COUNT];

/*
 * CK reformulation note (vs the AK source this was ported from):
 *
 * The dsPIC33A "new" I2C module exposes a second status word (STAT2) with
 * STARTE / STOPE / NACKE / HSTACT / ERR event bits. The CK classic module has
 * none of these. Completion and error state are therefore derived directly
 * from the request bits auto-clearing in I2CxCONL (SEN/RSEN/PEN/ACKEN) and from
 * I2CxSTAT (ACKSTAT for NACK, BCL/IWCOL/I2COV for bus faults). There is also no
 * "host active" bit, so the master-transmit byte sequence relies on TBF/TRSTAT.
 */

/* --------------------------------------------------------------------------
 * Local helper prototypes
 * -------------------------------------------------------------------------- */
static bool addr7_is_valid(uint8_t addr7);
static nora_i2c_status_t require_initialized(
    nora_i2c_instance_t inst,
    const nora_i2c_regs_t **regs);
static nora_i2c_status_t check_initialized(nora_i2c_instance_t inst);
static bool timeout_enabled(nora_i2c_instance_t inst);
static uint32_t timeout_start_ms(nora_i2c_instance_t inst);
static bool timeout_expired(nora_i2c_instance_t inst, uint32_t start_ms);
static bool pending_timeout_enabled(nora_i2c_instance_t inst);
static bool pending_timeout_expired(nora_i2c_instance_t inst);
static void pending_clear(nora_i2c_instance_t inst);
static void pending_set(
    nora_i2c_instance_t inst,
    nora_i2c_pending_state_t state);
static nora_i2c_status_t recover_stale_pending_if_needed(
    nora_i2c_instance_t inst);
static nora_i2c_status_t begin_independent_transaction(
    nora_i2c_instance_t inst);
static nora_i2c_status_t write_no_stop_sequence(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    const uint8_t *tx,
    size_t tx_len);
static nora_i2c_status_t read_after_restart_sequence(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    uint8_t *rx,
    size_t rx_len);
static nora_i2c_status_t stop_pending_sequence(
    nora_i2c_instance_t inst);
static nora_i2c_status_t abort_pending_sequence(
    nora_i2c_instance_t inst,
    nora_i2c_status_t original_status);
static void clear_transfer_status(const nora_i2c_regs_t *r);
static bool idle_request_bits_clear(const nora_i2c_regs_t *r);
static nora_i2c_status_t check_bus_fault(nora_i2c_instance_t inst);
static nora_i2c_status_t wait_until(
    nora_i2c_instance_t inst,
    bool (*done_fn)(nora_i2c_instance_t),
    bool check_nack);
static bool write_byte_ready(nora_i2c_instance_t inst);
static bool write_byte_done(nora_i2c_instance_t inst);
static bool ack_done(nora_i2c_instance_t inst);
static bool stop_fully_done(nora_i2c_instance_t inst);
static nora_i2c_status_t stop_quiet(nora_i2c_instance_t inst);
static nora_i2c_status_t start_blocking(nora_i2c_instance_t inst);
static nora_i2c_status_t restart_blocking(nora_i2c_instance_t inst);
static nora_i2c_status_t write_byte_blocking(
    nora_i2c_instance_t inst,
    uint8_t data);
static nora_i2c_status_t read_byte_blocking(
    nora_i2c_instance_t inst,
    uint8_t *data,
    bool ack);
static nora_i2c_status_t send_address_blocking(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    bool read);

/* ========================================================================== */
/* 1. Normal blocking API                                                     */
/* ========================================================================== */

/* --------------------------------------------------------------------------
 * Initialize I2C instance
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_init(
    nora_i2c_instance_t inst,
    const nora_i2c_config_t *config)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st;
    uint16_t brg;

    if (!nora_i2c_inst_is_valid(inst)) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    if (config == 0 || config->fcy_hz == 0u || config->bus_hz == 0u) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = nora_i2c_get_regs(inst, &r);
    if (st != NORA_I2C_OK) {
        return st;
    }

    /*
     * Refuse to re-purpose an instance that is live in the other role. Without
     * this, configuring a running slave as a master rewrote CONL/CONH and took
     * the role, but left the slave engine's own state active and _SI2CxIE
     * ENABLED -- a master-configured peripheral with an armed slave vector,
     * while nora_i2c_slave_is_active() still answered true. Call
     * nora_i2c_slave_deinit() first. Re-initializing the same role stays
     * allowed: bring-up code relies on it.
     */
    if (nora_i2c_get_role(inst) == NORA_I2C_ROLE_SLAVE) {
        return NORA_I2C_ERR_BUSY;
    }

    /* Start from a known disabled state; enable after configuration is complete. */
    nora_i2c_reg_write(r->CONL, 0u);
    nora_i2c_reg_write(r->CONH, 0u);
    clear_transfer_status(r);

    brg = nora_i2c_calc_brg(config->fcy_hz, config->bus_hz);
    nora_i2c_reg_write(r->BRG, brg);

    g_timeout_ms[inst] = config->timeout_ms;
    g_get_ms[inst] = config->get_ms;
    g_pending_timeout_ms[inst] = config->pending_timeout_ms;
    pending_clear(inst);
    g_initialized[inst] = true;
    nora_i2c_set_role(inst, NORA_I2C_ROLE_MASTER);

    nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_I2CEN);

    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Deinitialize I2C instance
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_deinit(
    nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st;

    if (!nora_i2c_inst_is_valid(inst)) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = nora_i2c_get_regs(inst, &r);
    if (st != NORA_I2C_OK) {
        return st;
    }

    st = recover_stale_pending_if_needed(inst);
    if (st != NORA_I2C_OK &&
        st != NORA_I2C_ERR_TIMEOUT &&
        st != NORA_I2C_ERR_BUS &&
        st != NORA_I2C_ERR_COLLISION) {
        return st;
    }

    if (st == NORA_I2C_OK &&
        g_pending_state[inst] == NORA_I2C_PENDING_NONE) {
        if (nora_i2c_reg_is_set(r->STAT, NORA_I2C_STAT_TRSTAT) ||
            !idle_request_bits_clear(r)) {
            return NORA_I2C_ERR_BUSY;
        }
    }

    /*
     * If a pending transaction is still latched, deinit is the explicit
     * recovery path.  Force the peripheral and HAL state off instead of
     * leaving the caller with no way out when pending timeout is disabled.
     */
    nora_i2c_reg_clear(r->CONL, NORA_I2C_CONL_I2CEN);

    g_timeout_ms[inst] = 0u;
    g_get_ms[inst] = 0;
    g_pending_timeout_ms[inst] = 0u;
    pending_clear(inst);
    g_initialized[inst] = false;
    nora_i2c_set_role(inst, NORA_I2C_ROLE_NONE);

    return st;
}

/* --------------------------------------------------------------------------
 * Update bus speed on an initialized idle instance
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_set_bus_speed(
    nora_i2c_instance_t inst,
    uint32_t fcy_hz,
    uint32_t bus_hz)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st;
    uint16_t brg;
    bool was_on;

    if (!nora_i2c_inst_is_valid(inst)) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    if (fcy_hz == 0u || bus_hz == 0u) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = require_initialized(inst, &r);
    if (st != NORA_I2C_OK) {
        return st;
    }

    /*
     * Do not run stale pending recovery here.  A pending no-STOP transaction
     * simply blocks the speed change; recovery stays the responsibility of the
     * read-after-restart / stop / deinit paths.
     */
    if (g_pending_state[inst] != NORA_I2C_PENDING_NONE) {
        return NORA_I2C_ERR_BUSY;
    }

    /*
     * Reject the speed change while a byte transfer is in progress or any
     * START / repeated-START / STOP / receive / ACK request bit is still set,
     * mirroring the idle check used by deinit. The CK classic module has no
     * HSTACT "host active" bit, so TRSTAT + the CONL request bits stand in.
     */
    if (nora_i2c_reg_is_set(r->STAT, NORA_I2C_STAT_TRSTAT) ||
        !idle_request_bits_clear(r)) {
        return NORA_I2C_ERR_BUSY;
    }

    brg = nora_i2c_calc_brg(fcy_hz, bus_hz);

    /*
     * Safe-side update: turn the peripheral off while BRG changes, then restore
     * the previous ON state.
     */
    was_on = nora_i2c_reg_is_set(r->CONL, NORA_I2C_CONL_I2CEN);
    if (was_on) {
        nora_i2c_reg_clear(r->CONL, NORA_I2C_CONL_I2CEN);
    }

    nora_i2c_reg_write(r->BRG, brg);

    if (was_on) {
        nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_I2CEN);
    }

    return NORA_I2C_OK;
}

/* nora_i2c_is_present() and nora_i2c_is_initialized() are shared;
 * see nora_i2c_dspic33ck_common.c. */

/* --------------------------------------------------------------------------
 * Blocking write transaction
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_write(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    const uint8_t *tx,
    size_t tx_len)
{
    nora_i2c_status_t st;

    if (!addr7_is_valid(addr7) || (tx_len != 0u && tx == 0)) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = begin_independent_transaction(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    st = write_no_stop_sequence(inst, addr7, tx, tx_len);
    if (st != NORA_I2C_OK) {
        return st;
    }

    return stop_pending_sequence(inst);
}

/* --------------------------------------------------------------------------
 * Blocking read transaction
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_read(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    uint8_t *rx,
    size_t rx_len)
{
    nora_i2c_status_t st;
    size_t i;

    if (!addr7_is_valid(addr7) || rx == 0 || rx_len == 0u) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = begin_independent_transaction(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    st = start_blocking(inst);
    if (st != NORA_I2C_OK) {
        (void)stop_quiet(inst);
        return st;
    }

    st = send_address_blocking(inst, addr7, true);
    if (st != NORA_I2C_OK) {
        (void)stop_quiet(inst);
        return st;
    }

    for (i = 0u; i < rx_len; i++) {
        bool ack = ((i + 1u) < rx_len);
        st = read_byte_blocking(inst, &rx[i], ack);
        if (st != NORA_I2C_OK) {
            (void)stop_quiet(inst);
            return st;
        }
    }

    return stop_quiet(inst);
}

/* --------------------------------------------------------------------------
 * Blocking write-read transaction with repeated START
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_write_read(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len)
{
    nora_i2c_status_t st;

    if (!addr7_is_valid(addr7) ||
        (tx_len != 0u && tx == 0) || rx == 0 || rx_len == 0u) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = begin_independent_transaction(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    st = write_no_stop_sequence(inst, addr7, tx, tx_len);
    if (st != NORA_I2C_OK) {
        return st;
    }

    return read_after_restart_sequence(inst, addr7, rx, rx_len);
}

/* --------------------------------------------------------------------------
 * Blocking master write transaction without STOP
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_master_write_no_stop(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    const uint8_t *tx,
    size_t tx_len)
{
    nora_i2c_status_t st;

    if (!addr7_is_valid(addr7) || (tx_len != 0u && tx == 0)) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = begin_independent_transaction(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    return write_no_stop_sequence(inst, addr7, tx, tx_len);
}

/* --------------------------------------------------------------------------
 * Blocking master read after repeated START from a pending write
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_master_read_after_restart(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    uint8_t *rx,
    size_t rx_len)
{
    nora_i2c_status_t st;

    if (!addr7_is_valid(addr7) || rx == 0 || rx_len == 0u) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = recover_stale_pending_if_needed(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    if (g_pending_state[inst] != NORA_I2C_PENDING_MASTER_WRITE) {
        return NORA_I2C_ERR_SEQUENCE;
    }

    return read_after_restart_sequence(inst, addr7, rx, rx_len);
}

/* --------------------------------------------------------------------------
 * Blocking master STOP for an explicit pending transaction end
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_master_stop(
    nora_i2c_instance_t inst)
{
    nora_i2c_status_t st;

    st = recover_stale_pending_if_needed(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    if (g_pending_state[inst] == NORA_I2C_PENDING_NONE) {
        return NORA_I2C_ERR_SEQUENCE;
    }

    return stop_pending_sequence(inst);
}

/* ========================================================================== */
/* 2. Low-level global API                                                    */
/* ========================================================================== */

/* --------------------------------------------------------------------------
 * Issue START condition
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_ll_start_issue(
    nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st = require_initialized(inst, &r);

    if (st != NORA_I2C_OK) {
        return st;
    }

    clear_transfer_status(r);
    nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_SEN);
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Check START busy flag
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_start_busy(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return nora_i2c_reg_is_set(r->CONL, NORA_I2C_CONL_SEN);
}

/* --------------------------------------------------------------------------
 * Check START done status
 *
 * The CK classic module clears CONL.SEN in hardware once the START condition
 * has completed. STAT.S is set alongside; SEN clearing is the definitive
 * completion signal for the request bit.
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_start_done(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return !nora_i2c_reg_is_set(r->CONL, NORA_I2C_CONL_SEN);
}

/* --------------------------------------------------------------------------
 * Issue repeated START condition
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_ll_restart_issue(
    nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st = require_initialized(inst, &r);

    if (st != NORA_I2C_OK) {
        return st;
    }

    /*
     * A repeated START is a START condition generated without a preceding STOP.
     * Use RSEN so this low-level API maps directly to the CK repeated-START
     * request bit; the hardware clears RSEN on completion.
     */
    nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_RSEN);
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Check repeated START busy flag
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_restart_busy(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return nora_i2c_reg_is_set(r->CONL, NORA_I2C_CONL_RSEN);
}

/* --------------------------------------------------------------------------
 * Check repeated START done status
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_restart_done(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return !nora_i2c_reg_is_set(r->CONL, NORA_I2C_CONL_RSEN);
}

/* --------------------------------------------------------------------------
 * Issue STOP condition
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_ll_stop_issue(
    nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st = require_initialized(inst, &r);

    if (st != NORA_I2C_OK) {
        return st;
    }

    nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_PEN);
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Check STOP busy flag
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_stop_busy(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return nora_i2c_reg_is_set(r->CONL, NORA_I2C_CONL_PEN);
}

/* --------------------------------------------------------------------------
 * Check STOP done status
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_stop_done(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return !nora_i2c_reg_is_set(r->CONL, NORA_I2C_CONL_PEN);
}

/* --------------------------------------------------------------------------
 * Issue one-byte write
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_ll_write_byte_issue(
    nora_i2c_instance_t inst,
    uint8_t data)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st = require_initialized(inst, &r);

    if (st != NORA_I2C_OK) {
        return st;
    }

    if (nora_i2c_reg_is_set(r->STAT, NORA_I2C_STAT_IWCOL)) {
        nora_i2c_reg_clear(r->STAT, NORA_I2C_STAT_IWCOL);
    }

    nora_i2c_reg_write(r->TRN, (uint16_t)data);
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Check one-byte write busy flag
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_write_byte_busy(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return nora_i2c_reg_is_set(r->STAT, NORA_I2C_STAT_TRSTAT);
}

/* --------------------------------------------------------------------------
 * Check ACK result after one-byte write
 *
 * On the CK classic module the received ACK/NACK is reported by STAT.ACKSTAT
 * (0 = ACK, 1 = NACK). The AK source used the STAT2.NACKE event bit, which does
 * not exist here.
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_write_byte_acked(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return !nora_i2c_reg_is_set(r->STAT, NORA_I2C_STAT_ACKSTAT);
}

/* --------------------------------------------------------------------------
 * Issue one-byte read
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_ll_read_byte_issue(
    nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st = require_initialized(inst, &r);

    if (st != NORA_I2C_OK) {
        return st;
    }

    nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_RCEN);
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Check receive buffer ready
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_read_byte_ready(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return nora_i2c_reg_is_set(r->STAT, NORA_I2C_STAT_RBF);
}

/* --------------------------------------------------------------------------
 * Get received byte
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_ll_read_byte_get(
    nora_i2c_instance_t inst,
    uint8_t *data)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st;

    if (data == 0) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = nora_i2c_get_regs(inst, &r);
    if (st != NORA_I2C_OK) {
        return st;
    }

    *data = (uint8_t)(*r->RCV & 0xFFu);
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Issue ACK or NACK after read
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_ll_ack_issue(
    nora_i2c_instance_t inst,
    bool ack)
{
    const nora_i2c_regs_t *r;
    nora_i2c_status_t st = require_initialized(inst, &r);

    if (st != NORA_I2C_OK) {
        return st;
    }

    if (ack) {
        nora_i2c_reg_clear(r->CONL, NORA_I2C_CONL_ACKDT);
    } else {
        nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_ACKDT);
    }

    nora_i2c_reg_set(r->CONL, NORA_I2C_CONL_ACKEN);
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Check ACK/NACK issue busy flag
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_ack_busy(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return nora_i2c_reg_is_set(r->CONL, NORA_I2C_CONL_ACKEN);
}

/* --------------------------------------------------------------------------
 * Check I2C error status
 *
 * The CK classic module has no aggregate STAT2.ERR bit; bus faults are derived
 * from the collision (BCL / IWCOL) and receive-overflow (I2COV) status bits.
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_has_error(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return true;
    }

    return nora_i2c_reg_is_set(r->STAT,
                                    NORA_I2C_STAT_BCL |
                                    NORA_I2C_STAT_IWCOL |
                                    NORA_I2C_STAT_I2COV);
}

/* --------------------------------------------------------------------------
 * Check NACK status
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_has_nack(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return true;
    }

    return nora_i2c_reg_is_set(r->STAT, NORA_I2C_STAT_ACKSTAT);
}

/* --------------------------------------------------------------------------
 * Check bus collision status
 * -------------------------------------------------------------------------- */
bool nora_i2c_ll_has_collision(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return true;
    }

    return nora_i2c_reg_is_set(r->STAT,
                                    NORA_I2C_STAT_IWCOL |
                                    NORA_I2C_STAT_BCL);
}

/* ========================================================================== */
/* 3. Static local helper functions                                           */
/* ========================================================================== */

/* --------------------------------------------------------------------------
 * Validate a 7-bit slave address
 *
 * The address is right-justified 7-bit: send_address_blocking() forms the wire
 * byte as (addr7 << 1) | R/W, so bit 7 of the argument has nowhere to go.
 * Rejecting it catches the ordinary mistake rather than an exotic one -- a
 * caller passing the already-shifted 8-bit address a datasheet also prints
 * (WM8904: 7-bit 0x1A, also documented as 0x34) would otherwise address
 * 0x34 << 1 = 0x68, a legal byte for a different device, and get a silent wrong
 * target or an ERR_NACK that reads as a wiring fault. nora_i2c_slave_init()
 * already range-checks its own addr7 the same way.
 * -------------------------------------------------------------------------- */
static bool addr7_is_valid(uint8_t addr7)
{
    return (addr7 <= 0x7Fu);
}

/* --------------------------------------------------------------------------
 * Resolve registers and require initialized state
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t require_initialized(
    nora_i2c_instance_t inst,
    const nora_i2c_regs_t **regs)
{
    nora_i2c_status_t st;

    if (!nora_i2c_inst_is_valid(inst)) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    st = nora_i2c_get_regs(inst, regs);
    if (st != NORA_I2C_OK) {
        return st;
    }

    if (!g_initialized[inst]) {
        return NORA_I2C_ERR_NOT_INITIALIZED;
    }

    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Require initialized state without exposing register pointer to caller
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t check_initialized(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;
    return require_initialized(inst, &r);
}

/* --------------------------------------------------------------------------
 * Check whether timeout is enabled
 * -------------------------------------------------------------------------- */
static bool timeout_enabled(nora_i2c_instance_t inst)
{
    return nora_i2c_inst_is_valid(inst) &&
           (g_get_ms[inst] != 0) &&
           (g_timeout_ms[inst] != 0u);
}

/* --------------------------------------------------------------------------
 * Capture timeout start tick
 * -------------------------------------------------------------------------- */
static uint32_t timeout_start_ms(nora_i2c_instance_t inst)
{
    if (!timeout_enabled(inst)) {
        return 0u;
    }

    return g_get_ms[inst]();
}

/* --------------------------------------------------------------------------
 * Check timeout expiration
 * -------------------------------------------------------------------------- */
static bool timeout_expired(nora_i2c_instance_t inst, uint32_t start_ms)
{
    uint32_t now;

    if (!timeout_enabled(inst)) {
        return false;
    }

    now = g_get_ms[inst]();
    return ((uint32_t)(now - start_ms) >= g_timeout_ms[inst]);
}

/* --------------------------------------------------------------------------
 * Check whether pending transaction timeout is enabled
 * -------------------------------------------------------------------------- */
static bool pending_timeout_enabled(nora_i2c_instance_t inst)
{
    return nora_i2c_inst_is_valid(inst) &&
           (g_get_ms[inst] != 0) &&
           (g_pending_timeout_ms[inst] != 0u);
}

/* --------------------------------------------------------------------------
 * Check pending transaction timeout expiration
 * -------------------------------------------------------------------------- */
static bool pending_timeout_expired(nora_i2c_instance_t inst)
{
    uint32_t now;

    if (g_pending_state[inst] == NORA_I2C_PENDING_NONE) {
        return false;
    }

    if (!pending_timeout_enabled(inst)) {
        return false;
    }

    now = g_get_ms[inst]();
    return ((uint32_t)(now - g_pending_start_ms[inst]) >=
            g_pending_timeout_ms[inst]);
}

/* --------------------------------------------------------------------------
 * Clear pending transaction state
 * -------------------------------------------------------------------------- */
static void pending_clear(nora_i2c_instance_t inst)
{
    if (!nora_i2c_inst_is_valid(inst)) {
        return;
    }

    g_pending_state[inst] = NORA_I2C_PENDING_NONE;
    g_pending_start_ms[inst] = 0u;
}

/* --------------------------------------------------------------------------
 * Set pending transaction state
 * -------------------------------------------------------------------------- */
static void pending_set(
    nora_i2c_instance_t inst,
    nora_i2c_pending_state_t state)
{
    if (!nora_i2c_inst_is_valid(inst)) {
        return;
    }

    g_pending_state[inst] = state;
    g_pending_start_ms[inst] = pending_timeout_enabled(inst) ?
                               g_get_ms[inst]() :
                               0u;
}

/* --------------------------------------------------------------------------
 * Recover a stale no-STOP transaction if its pending timeout has elapsed
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t recover_stale_pending_if_needed(
    nora_i2c_instance_t inst)
{
    nora_i2c_status_t st;

    st = check_initialized(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    if (!pending_timeout_expired(inst)) {
        return NORA_I2C_OK;
    }

    st = stop_quiet(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    pending_clear(inst);
    return NORA_I2C_ERR_TIMEOUT;
}

/* --------------------------------------------------------------------------
 * Public API guard for starting a new non-pending transaction
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t begin_independent_transaction(
    nora_i2c_instance_t inst)
{
    nora_i2c_status_t st;

    /*
     * recover_stale_pending_if_needed() returns ERR_TIMEOUT when it had to
     * force a stale no-STOP transaction closed.  Propagate it: the caller
     * must observe that its previous transaction was recovered and retry,
     * rather than have a fresh transfer silently started in its place.
     */
    st = recover_stale_pending_if_needed(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    if (g_pending_state[inst] != NORA_I2C_PENDING_NONE) {
        return NORA_I2C_ERR_BUSY;
    }

    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Internal master write sequence that intentionally leaves STOP pending
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t write_no_stop_sequence(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    const uint8_t *tx,
    size_t tx_len)
{
    nora_i2c_status_t st;
    size_t i;

    st = start_blocking(inst);
    if (st != NORA_I2C_OK) {
        (void)stop_quiet(inst);
        return st;
    }

    st = send_address_blocking(inst, addr7, false);
    if (st != NORA_I2C_OK) {
        (void)stop_quiet(inst);
        return st;
    }

    for (i = 0u; i < tx_len; i++) {
        st = write_byte_blocking(inst, tx[i]);
        if (st != NORA_I2C_OK) {
            (void)stop_quiet(inst);
            return st;
        }
    }

    pending_set(inst, NORA_I2C_PENDING_MASTER_WRITE);
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Internal repeated START read sequence from a pending write
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t read_after_restart_sequence(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    uint8_t *rx,
    size_t rx_len)
{
    nora_i2c_status_t st;
    size_t i;

    st = restart_blocking(inst);
    if (st != NORA_I2C_OK) {
        return abort_pending_sequence(inst, st);
    }

    st = send_address_blocking(inst, addr7, true);
    if (st != NORA_I2C_OK) {
        return abort_pending_sequence(inst, st);
    }

    for (i = 0u; i < rx_len; i++) {
        bool ack = ((i + 1u) < rx_len);
        st = read_byte_blocking(inst, &rx[i], ack);
        if (st != NORA_I2C_OK) {
            return abort_pending_sequence(inst, st);
        }
    }

    return stop_pending_sequence(inst);
}

/* --------------------------------------------------------------------------
 * Internal STOP sequence for a known pending transaction
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t stop_pending_sequence(
    nora_i2c_instance_t inst)
{
    nora_i2c_status_t st;

    /*
     * Clear the pending state only once STOP has actually completed.  If STOP
     * fails the bus is not cleanly released, so keep the transaction pending:
     * the next public call is then rejected with ERR_BUSY (or recovered by the
     * pending timeout) instead of starting a new transfer on a stuck bus.
     */
    st = stop_quiet(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    pending_clear(inst);
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Try to STOP after a pending transaction error
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t abort_pending_sequence(
    nora_i2c_instance_t inst,
    nora_i2c_status_t original_status)
{
    nora_i2c_status_t stop_status;

    /*
     * Best-effort STOP after a failed transfer.  If STOP itself fails the bus
     * is not recovered, so surface the STOP error and keep the transaction
     * pending so the caller can tell that recovery did not complete.  Only on
     * a clean STOP do we clear the pending state and return the original
     * transfer error (e.g. ERR_NACK) as the diagnostic result.
     */
    stop_status = stop_quiet(inst);
    if (stop_status != NORA_I2C_OK) {
        return stop_status;
    }

    pending_clear(inst);
    return original_status;
}

/* --------------------------------------------------------------------------
 * Clear transfer-related status bits
 * -------------------------------------------------------------------------- */
static void clear_transfer_status(const nora_i2c_regs_t *r)
{
    /*
     * Keep status cleanup narrow.  The CK classic module holds its clearable
     * fault bits in the single STAT register; there is no STAT2.
     */
    nora_i2c_reg_clear(r->STAT,
                            NORA_I2C_STAT_IWCOL |
                            NORA_I2C_STAT_I2COV |
                            NORA_I2C_STAT_BCL);
}

/* --------------------------------------------------------------------------
 * True when no START / repeated-START / STOP / receive / ACK request is queued
 * -------------------------------------------------------------------------- */
static bool idle_request_bits_clear(const nora_i2c_regs_t *r)
{
    return !nora_i2c_reg_is_set(r->CONL,
                                     NORA_I2C_CONL_SEN |
                                     NORA_I2C_CONL_RSEN |
                                     NORA_I2C_CONL_PEN |
                                     NORA_I2C_CONL_RCEN |
                                     NORA_I2C_CONL_ACKEN);
}

/* --------------------------------------------------------------------------
 * Convert bus fault status to driver status
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t check_bus_fault(nora_i2c_instance_t inst)
{
    if (nora_i2c_ll_has_collision(inst)) {
        return NORA_I2C_ERR_COLLISION;
    }

    if (nora_i2c_ll_has_nack(inst)) {
        return NORA_I2C_ERR_NACK;
    }

    if (nora_i2c_ll_has_error(inst)) {
        return NORA_I2C_ERR_BUS;
    }

    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Wait for condition using optional timeout
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t wait_until(
    nora_i2c_instance_t inst,
    bool (*done_fn)(nora_i2c_instance_t),
    bool check_nack)
{
    uint32_t start_ms = timeout_start_ms(inst);
    nora_i2c_status_t st;

    while (!done_fn(inst)) {
        if (check_nack) {
            st = check_bus_fault(inst);
        } else {
            if (nora_i2c_ll_has_collision(inst)) {
                st = NORA_I2C_ERR_COLLISION;
            } else if (nora_i2c_ll_has_error(inst)) {
                st = NORA_I2C_ERR_BUS;
            } else {
                st = NORA_I2C_OK;
            }
        }

        if (st != NORA_I2C_OK) {
            return st;
        }

        if (timeout_expired(inst, start_ms)) {
            return NORA_I2C_ERR_TIMEOUT;
        }
    }

    if (nora_i2c_ll_has_collision(inst)) {
        return NORA_I2C_ERR_COLLISION;
    }

    if (nora_i2c_ll_has_error(inst)) {
        return NORA_I2C_ERR_BUS;
    }

    if (check_nack) {
        return check_bus_fault(inst);
    }

    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Adapter: write byte ready condition
 *
 * The transmit register can accept a new byte when the transmit buffer is not
 * full (TBF clear). This is the classic-module gate before writing I2CxTRN.
 * -------------------------------------------------------------------------- */
static bool write_byte_ready(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return !nora_i2c_reg_is_set(r->STAT, NORA_I2C_STAT_TBF);
}

/* --------------------------------------------------------------------------
 * Adapter: write byte done condition
 *
 * After I2CxTRN is written, TRSTAT stays set for the 8 data bits plus the ACK
 * clock and clears when the master byte transfer is complete.
 * -------------------------------------------------------------------------- */
static bool write_byte_done(nora_i2c_instance_t inst)
{
    return !nora_i2c_ll_write_byte_busy(inst);
}

/* --------------------------------------------------------------------------
 * Adapter: ACK issue done condition
 * -------------------------------------------------------------------------- */
static bool ack_done(nora_i2c_instance_t inst)
{
    return !nora_i2c_ll_ack_busy(inst);
}

/* --------------------------------------------------------------------------
 * Adapter: STOP fully completed condition
 *
 * A STOP is finished, and the bus released, once CONL.PEN returns to 0. The AK
 * source additionally gated on STAT2.STOPE; the CK classic module has no STOPE
 * event bit, so PEN clearing is the completion signal. Waiting for the request
 * bit to retire before starting the next transfer is the same behaviour the AK
 * comment credits with fixing slow-speed (100 kHz) STOP/START overlap.
 * -------------------------------------------------------------------------- */
static bool stop_fully_done(nora_i2c_instance_t inst)
{
    const nora_i2c_regs_t *r;

    if (nora_i2c_get_regs(inst, &r) != NORA_I2C_OK) {
        return false;
    }

    return !nora_i2c_reg_is_set(r->CONL, NORA_I2C_CONL_PEN);
}

/* --------------------------------------------------------------------------
 * Issue STOP and wait quietly
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t stop_quiet(nora_i2c_instance_t inst)
{
    nora_i2c_status_t st;

    st = nora_i2c_ll_stop_issue(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    return wait_until(inst, stop_fully_done, false);
}

/* --------------------------------------------------------------------------
 * Issue START and wait
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t start_blocking(nora_i2c_instance_t inst)
{
    nora_i2c_status_t st;

    st = nora_i2c_ll_start_issue(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    return wait_until(inst, nora_i2c_ll_start_done, false);
}

/* --------------------------------------------------------------------------
 * Issue repeated START and wait
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t restart_blocking(nora_i2c_instance_t inst)
{
    nora_i2c_status_t st;

    st = nora_i2c_ll_restart_issue(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    return wait_until(inst, nora_i2c_ll_restart_done, false);
}

/* --------------------------------------------------------------------------
 * Write one byte and wait
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t write_byte_blocking(
    nora_i2c_instance_t inst,
    uint8_t data)
{
    nora_i2c_status_t st;

    /*
     * Byte write sequence (CK classic master transmit):
     *   1. wait until the transmit buffer can accept a byte (TBF clear)
     *   2. write I2CxTRN
     *   3. wait until the byte transfer completes (TRSTAT clear)
     *   4. confirm the slave ACKed (ACKSTAT clear)
     *
     * The AK source also waited on STAT2.HSTACT ("host active") and STAT1.D_A;
     * neither maps to CK master transmit, where TBF/TRSTAT/ACKSTAT fully
     * describe the byte phase.
     */
    st = wait_until(inst, write_byte_ready, false);
    if (st != NORA_I2C_OK) {
        return st;
    }

    st = nora_i2c_ll_write_byte_issue(inst, data);
    if (st != NORA_I2C_OK) {
        return st;
    }

    st = wait_until(inst, write_byte_done, false);
    if (st != NORA_I2C_OK) {
        return st;
    }

    if (!nora_i2c_ll_write_byte_acked(inst)) {
        return NORA_I2C_ERR_NACK;
    }

    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Read one byte, issue ACK/NACK, and wait
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t read_byte_blocking(
    nora_i2c_instance_t inst,
    uint8_t *data,
    bool ack)
{
    nora_i2c_status_t st;

    st = nora_i2c_ll_read_byte_issue(inst);
    if (st != NORA_I2C_OK) {
        return st;
    }

    st = wait_until(inst, nora_i2c_ll_read_byte_ready, false);
    if (st != NORA_I2C_OK) {
        return st;
    }

    st = nora_i2c_ll_read_byte_get(inst, data);
    if (st != NORA_I2C_OK) {
        return st;
    }

    st = nora_i2c_ll_ack_issue(inst, ack);
    if (st != NORA_I2C_OK) {
        return st;
    }

    return wait_until(inst, ack_done, false);
}

/* --------------------------------------------------------------------------
 * Send 7-bit address with R/W bit
 * -------------------------------------------------------------------------- */
static nora_i2c_status_t send_address_blocking(
    nora_i2c_instance_t inst,
    uint8_t addr7,
    bool read)
{
    uint8_t addr_byte = (uint8_t)((addr7 << 1) | (read ? 1u : 0u));
    return write_byte_blocking(inst, addr_byte);
}

/* --------------------------------------------------------------------------
 * Interrupt helper API (reserved)
 *
 * Present so this HAL's compile-time surface matches the dsPIC33AK HAL's, where
 * these three are stubs with exactly this return value. Nothing here touches
 * MI2C/SI2C IE bits: a real implementation would, and would then have to define
 * which of the three masks maps onto which vector -- a contract neither family
 * has agreed yet. Until it does, refusing at run time is the honest answer.
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_irq_enable(
    nora_i2c_instance_t inst,
    uint32_t irq_mask)
{
    (void)inst;
    (void)irq_mask;
    return NORA_I2C_ERR_UNSUPPORTED;
}

nora_i2c_status_t nora_i2c_irq_disable(
    nora_i2c_instance_t inst,
    uint32_t irq_mask)
{
    (void)inst;
    (void)irq_mask;
    return NORA_I2C_ERR_UNSUPPORTED;
}

nora_i2c_status_t nora_i2c_irq_clear(
    nora_i2c_instance_t inst,
    uint32_t irq_mask)
{
    (void)inst;
    (void)irq_mask;
    return NORA_I2C_ERR_UNSUPPORTED;
}
