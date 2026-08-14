/*
 * Low-level DMA HAL (dsPIC33CK) - implementation.
 *
 * CK sibling of dspic33ak_dma.c: same structure and flow (global init, channel
 * config/enable, flag clear, status/src read, ping-pong half query). The
 * register layer targets the CK DMA controller (DMACHn / DMAINTn / DMASRCn /
 * DMADSTn / DMACNTn, DMACON). Bit masks come from nora_dma_dspic33ck_reg.h.
 *
 * No dependency on SPI, audio, PWM, DSP, printf, or application code.
 */

#include <xc.h>

#include "nora_dma.h"
#include "nora_dma_dspic33ck_reg.h"
/* Supplies the `_hot` inlines. Every out-of-line function at the bottom of this
 * file is literally a call to its inline twin, so there is one implementation of
 * each operation and the two forms cannot drift apart. */
#include "nora_dma_dspic33ck_fast.h"

/* ---------------------------------------------------------------------------
 * Private: per-channel register mapping
 *
 * DMACHn/SRCn/DSTn/CNTn/INTn are uniform 16-bit SFRs; the channel number is
 * collapsed into an index. Higher channels are guarded so this builds on CK
 * parts with fewer DMA channels.
 * ------------------------------------------------------------------------- */

typedef struct {
    volatile uint16_t *CH;
    volatile uint16_t *SRC;
    volatile uint16_t *DST;
    volatile uint16_t *CNT;
    volatile uint16_t *INT;
} dma_ch_regs_t;

static const dma_ch_regs_t s_dma_ch[] = {
    { &DMACH0, &DMASRC0, &DMADST0, &DMACNT0, &DMAINT0 },
    { &DMACH1, &DMASRC1, &DMADST1, &DMACNT1, &DMAINT1 },
#if defined(_DMA2IF)
    { &DMACH2, &DMASRC2, &DMADST2, &DMACNT2, &DMAINT2 },
#endif
#if defined(_DMA3IF)
    { &DMACH3, &DMASRC3, &DMADST3, &DMACNT3, &DMAINT3 },
#endif
};

#define DMA_CH_COUNT  (sizeof(s_dma_ch) / sizeof(s_dma_ch[0]))

static const dma_ch_regs_t *dma_regs(nora_dma_channel_t ch)
{
    if ((unsigned int)ch >= DMA_CH_COUNT) {
        return (const dma_ch_regs_t *)0;
    }
    return &s_dma_ch[(unsigned int)ch];
}

/* ---------------------------------------------------------------------------
 * Private: logical trigger -> DMAINTn.CHSEL
 *
 * CHSEL is a DEDICATED DMA trigger-source encoding and is NOT the CPU interrupt
 * vector number. Transcribed from "TABLE 10-1: DMA CHANNEL TRIGGER SOURCES":
 * dsPIC33CK64MC105 DS70005399D page 175, dsPIC33CK256MP508 DS70005349 section
 * 10.4 -- the "SPIx Receiver" / "SPIx Transmitter" rows. Both parts agree on
 * SPI1/SPI2; only MP508 has SPI3.
 *
 * Do NOT put the IRQ number here. SPI1RX is IRQ 9 (IFS0[9]) and SPI1TX is IRQ 10,
 * but CHSEL 9 and 10 select SI2C1 and MI2C1 -- so using the IRQ number arms the
 * channel against an I2C peripheral that is never enabled. Every register then
 * reads exactly as configured and the transport reports success, yet not one
 * element is ever transferred: no DMA trigger, so the TX FIFO stays empty, so a
 * framed master never shifts and emits no BCLK. That was the SPI/TDM transport's
 * actual first-silicon failure on both devices. This mapper is why a consumer no
 * longer transcribes the table at all: it names the peripheral leg it wants and
 * the wrong-number failure mode stops being reachable from outside this file.
 *
 * The table is per device, not per family, so the mapper is also the refusal
 * point: SPI3's codes are Reserved on MC105, and a channel armed against a
 * Reserved code is exactly the silent no-transfer failure above.
 * ------------------------------------------------------------------------- */

#define DMA_CHSEL_NONE      (0x07u)   /* NVM Write Complete -- see below       */
#define DMA_CHSEL_SPI1_RX   (0x02u)
#define DMA_CHSEL_SPI1_TX   (0x03u)
#define DMA_CHSEL_SPI2_RX   (0x10u)
#define DMA_CHSEL_SPI2_TX   (0x11u)
#define DMA_CHSEL_SPI3_RX   (0x62u)   /* MP508 only (Reserved on MC105)        */
#define DMA_CHSEL_SPI3_TX   (0x63u)

/* Returns false if this device has no such trigger, leaving *chsel untouched.
 *
 * NORA_DMA_TRIGGER_NONE maps to "NVM Write Complete" (Table 10-1, 07h). The
 * select field always names something, so a software-triggered channel needs a
 * source that is quiet during its window rather than a leftover value; NVM write
 * completion is the quiet choice here because nothing in this HAL programs flash.
 * That makes TRIGGER_NONE quiet by CONSTRUCTION, not by hardware: an application
 * that self-programs flash while such a channel is armed would trigger it, and
 * must pick a different quiet code -- which is precisely the per-device knowledge
 * that belongs in this mapper and not in the consumer. */
static bool dma_trigger_to_chsel(nora_dma_trigger_t trigger, uint16_t *chsel)
{
    switch (trigger) {
    case NORA_DMA_TRIGGER_NONE:    *chsel = DMA_CHSEL_NONE;    return true;
    case NORA_DMA_TRIGGER_SPI1_RX: *chsel = DMA_CHSEL_SPI1_RX; return true;
    case NORA_DMA_TRIGGER_SPI1_TX: *chsel = DMA_CHSEL_SPI1_TX; return true;
    case NORA_DMA_TRIGGER_SPI2_RX: *chsel = DMA_CHSEL_SPI2_RX; return true;
    case NORA_DMA_TRIGGER_SPI2_TX: *chsel = DMA_CHSEL_SPI2_TX; return true;
#if defined(SPI3CON1L)
    case NORA_DMA_TRIGGER_SPI3_RX: *chsel = DMA_CHSEL_SPI3_RX; return true;
    case NORA_DMA_TRIGGER_SPI3_TX: *chsel = DMA_CHSEL_SPI3_TX; return true;
#endif
    default: return false;
    }
}

/* ---------------------------------------------------------------------------
 * Private: interrupt Flag/Enable/Priority (do not channel-index)
 * ------------------------------------------------------------------------- */

static void dma_irq_clear_flag(nora_dma_channel_t ch)
{
    switch (ch) {
    case NORA_DMA_CHANNEL_0: _DMA0IF = 0; break;
    case NORA_DMA_CHANNEL_1: _DMA1IF = 0; break;
#if defined(_DMA2IF)
    case NORA_DMA_CHANNEL_2: _DMA2IF = 0; break;
#endif
#if defined(_DMA3IF)
    case NORA_DMA_CHANNEL_3: _DMA3IF = 0; break;
#endif
    default: break;
    }
}

/* Each arm stores a literal so the write is one bset.b / bclr.b on the shared IECx word --
 * see nora_dma_irq_restore_hot() in nora_dma_dspic33ck_fast.h for why the runtime
 * value is not good enough. */
static void dma_irq_enable_bit(nora_dma_channel_t ch, bool enable)
{
    if (enable) {
        switch (ch) {
        case NORA_DMA_CHANNEL_0: _DMA0IE = 1u; break;
        case NORA_DMA_CHANNEL_1: _DMA1IE = 1u; break;
#if defined(_DMA2IF)
        case NORA_DMA_CHANNEL_2: _DMA2IE = 1u; break;
#endif
#if defined(_DMA3IF)
        case NORA_DMA_CHANNEL_3: _DMA3IE = 1u; break;
#endif
        default: break;
        }
    } else {
        switch (ch) {
        case NORA_DMA_CHANNEL_0: _DMA0IE = 0u; break;
        case NORA_DMA_CHANNEL_1: _DMA1IE = 0u; break;
#if defined(_DMA2IF)
        case NORA_DMA_CHANNEL_2: _DMA2IE = 0u; break;
#endif
#if defined(_DMA3IF)
        case NORA_DMA_CHANNEL_3: _DMA3IE = 0u; break;
#endif
        default: break;
        }
    }
}

static bool dma_irq_is_enabled_bit(nora_dma_channel_t ch)
{
    switch (ch) {
    case NORA_DMA_CHANNEL_0: return (_DMA0IE != 0u);
    case NORA_DMA_CHANNEL_1: return (_DMA1IE != 0u);
#if defined(_DMA2IF)
    case NORA_DMA_CHANNEL_2: return (_DMA2IE != 0u);
#endif
#if defined(_DMA3IF)
    case NORA_DMA_CHANNEL_3: return (_DMA3IE != 0u);
#endif
    default: return false;
    }
}

static void dma_irq_set_priority(nora_dma_channel_t ch, uint8_t prio)
{
    switch (ch) {
    case NORA_DMA_CHANNEL_0: _DMA0IP = prio; break;
    case NORA_DMA_CHANNEL_1: _DMA1IP = prio; break;
#if defined(_DMA2IF)
    case NORA_DMA_CHANNEL_2: _DMA2IP = prio; break;
#endif
#if defined(_DMA3IF)
    case NORA_DMA_CHANNEL_3: _DMA3IP = prio; break;
#endif
    default: break;
    }
}

/* ---------------------------------------------------------------------------
 * Global
 * ------------------------------------------------------------------------- */

/*
 * DMAL/DMAH address-limit window (data sheet "Typical Setup" step 2).
 *
 * These are NOT optional. Both registers reset to 0x0000, and HIGHIF fires on any
 * access "higher than DMAH", so a freshly reset controller permits nothing: the very
 * first element of any transfer terminates the transaction (DMACNTn has already
 * decremented -- the limit check is explicitly NOT made before the access,
 * DS70005399D page 174 DMAINTn Note 2, so this is not MPU-style pre-access
 * protection), sets DMAINTn.HIGHIF and clears CHEN. The channel then sits with correct-looking
 * SRC/DST/CNT/CHSEL and never moves again, which reads as "the trigger never fired"
 * rather than "the transfer was refused".
 *
 * The default window is fully permissive on purpose. DMAL/DMAH only police Data Space
 * ABOVE the SFR range, and the hardware keeps its own fixed check against the top of
 * data RAM regardless of DMAH -- so 0x0000/0xFFFF means "no extra software fence,
 * hardware still catches genuinely out-of-RAM addresses". It is also device-independent:
 * the RAM top differs per part (MC105 0x1000-0x2FFF, MP508 larger), and a HAL that
 * hardcoded one part's top would silently forbid the upper RAM of another.
 *
 * Override either bound to fence a region off from DMA (the data sheet's stated purpose
 * for these registers) -- note that any DMA buffer then has to live inside the window.
 */
#ifndef NORA_DMA_ADDR_LIMIT_LOW
#define NORA_DMA_ADDR_LIMIT_LOW    (0x0000u)
#endif
#ifndef NORA_DMA_ADDR_LIMIT_HIGH
#define NORA_DMA_ADDR_LIMIT_HIGH   (0xFFFFu)
#endif

void nora_dma_global_init(void)
{
    /* Give DMA priority over the CPU for SRAM (bus-matrix arbitration).
     *
     * By default the CPU X/Y buses outrank DMA on SRAM. An audio SPI issues a
     * DMA request every element with only one pending request slot, so a
     * CPU-delayed grant becomes DMAINTn.OVRUNIF -- a dropped sample. Raising DMA
     * RAM priority prevents that overrun; SFR arbitration is unchanged. This is
     * the CK equivalent of AK's BMXINITPR.DMAPR (here it is MSTRPR.DMAPR).
     *
     * Note this is BUS PRIORITY, not an address window; the address window is
     * DMAL/DMAH, programmed just below. (An earlier version of this comment claimed
     * CK had no address window and that the reset value already covered RAM. Both
     * halves were wrong -- see the NORA_DMA_ADDR_LIMIT_* note above.) */
    MSTRPRbits.DMAPR = 1u;

    /* Address-limit window. Must precede any channel enable: with the reset value
     * every transfer is terminated on its first element. */
    DMAL = (uint16_t)NORA_DMA_ADDR_LIMIT_LOW;
    DMAH = (uint16_t)NORA_DMA_ADDR_LIMIT_HIGH;

    /* Turn the controller on. Safe to call more than once. */
    nora_dma_reg_set16(&DMACON, NORA_DMA_DSPIC33CK_CON_DMAEN);
}

bool nora_dma_global_is_ready(void)
{
    /* Controller enabled, DMA holds SRAM bus priority, AND the address-limit window
     * admits something (all three set by init).
     *
     * DMAH is part of "ready" because DMAH == 0 is the reset state, in which every
     * channel arms successfully and then terminates its first element. Failing here
     * turns that into a refusal at config time -- a channel that cannot transfer
     * should not report a successful start. */
    return nora_dma_reg_is_set16(&DMACON, NORA_DMA_DSPIC33CK_CON_DMAEN)
           && (MSTRPRbits.DMAPR != 0u)
           && (DMAH != 0u);
}

/* ---------------------------------------------------------------------------
 * Per channel
 * ------------------------------------------------------------------------- */

bool nora_dma_channel_config(nora_dma_channel_t ch, const nora_dma_channel_cfg_t *cfg)
{
    const dma_ch_regs_t *r = dma_regs(ch);
    uint16_t chsel;
    bool reload;

    if ((r == 0) || (cfg == 0)) {
        return false;
    }
    if (!nora_dma_global_is_ready()) {
        return false;
    }

    /* Reject out-of-range enums / priority rather than masking silently. */
    if (cfg->size > NORA_DMA_SIZE_WORD) {
        return false;
    }
    /* 32-bit elements are not available on the 16-bit core. */
    if (cfg->size == NORA_DMA_SIZE_WORD) {
        return false;
    }
    if (cfg->src_mode > NORA_DMA_ADDR_DECREMENT) {
        return false;
    }
    if (cfg->dst_mode > NORA_DMA_ADDR_DECREMENT) {
        return false;
    }
    if (cfg->tr_mode > NORA_DMA_TRMODE_REPEAT_CONTINUOUS) {
        return false;
    }
    if ((cfg->count == 0u) || (cfg->count > UINT16_MAX)) {
        /* DMACNTn == 0 would leave the channel with nothing to do; in Repeated
         * modes it is also the value the controller reloads to. The public API
         * accepts a 32-bit count for AK parity, but CK's DMACNTn is 16-bit and
         * must reject an unrepresentable request instead of truncating it. */
        return false;
    }
    if (cfg->irq_priority_set && (cfg->irq_priority > 7u)) {
        return false;
    }
    /* Resolve the trigger BEFORE the first register write, so a trigger this
     * device does not implement is a refusal and not a half-programmed channel.
     * On MC105 that is SPI3: its select codes are Reserved there, and a channel
     * armed against a Reserved code takes triggers from nothing while every
     * register reads back exactly as written. */
    if (!dma_trigger_to_chsel(cfg->trigger, &chsel)) {
        return false;
    }

    /* Mask this channel's CPU IRQ before reconfiguring (re-config safety). */
    dma_irq_enable_bit(ch, false);

    /* Known-disabled start; wipe channel config + status before programming. At
     * config time a full DMAINTn clear is fine (CHSEL/HALFEN are rewritten just
     * below). */
    *r->CH  = 0u;
    *r->INT = 0u;
    dma_irq_clear_flag(ch);

    /* Addresses (16-bit) and element count.
     *
     * DMACNTn is the transfer COUNT, not count-1: the DMA FRM (DS30009742C, One-Shot
     * mode) states its reset value is 0001h and that a single transfer decrements it
     * to 0000h. Writing count-1 here silently performed one transfer too few -- for a
     * ping-pong audio buffer that is a permanent one-element phase slip rather than an
     * obvious failure, so it is worth stating the source. Measured on hardware: with
     * count = 4 the old code moved exactly 3 elements. */
    *r->SRC = (uint16_t)(uintptr_t)cfg->src;
    *r->DST = (uint16_t)(uintptr_t)cfg->dst;
    *r->CNT = (uint16_t)cfg->count;

    /* DMACHn fields (CHEN intentionally left 0). */
    nora_dma_reg_write_field16(r->CH, NORA_DMA_DSPIC33CK_CH_SAMODE_MASK,
                                    NORA_DMA_DSPIC33CK_CH_SAMODE_POS, (uint16_t)cfg->src_mode);
    nora_dma_reg_write_field16(r->CH, NORA_DMA_DSPIC33CK_CH_DAMODE_MASK,
                                    NORA_DMA_DSPIC33CK_CH_DAMODE_POS, (uint16_t)cfg->dst_mode);
    nora_dma_reg_write_field16(r->CH, NORA_DMA_DSPIC33CK_CH_TRMODE_MASK,
                                    NORA_DMA_DSPIC33CK_CH_TRMODE_POS, (uint16_t)cfg->tr_mode);

    /* SIZE: CK bit is 0 = 16-bit word, 1 = byte. */
    if (cfg->size == NORA_DMA_SIZE_BYTE) {
        nora_dma_reg_set16(r->CH, NORA_DMA_DSPIC33CK_CH_SIZE);
    } else {
        nora_dma_reg_clear16(r->CH, NORA_DMA_DSPIC33CK_CH_SIZE);
    }

    /* Single RELOAD (AK's RELOADS/RELOADD/RELOADC collapse to it).
     *
     * The collapse only ever decides DMASRCn/DMADSTn: DMACNTn is reloaded in the
     * Repeated modes regardless of this bit (DS70005399D page 173, DMACHn Note 2),
     * which is what lets Repeated One-Shot ping-pong indefinitely. So reload_count
     * is already true of the hardware there, and what the bit still buys a
     * streaming consumer is the ADDRESS reload. */
    reload = (cfg->reload_count || cfg->reload_src || cfg->reload_dst);
    if (reload) {
        nora_dma_reg_set16(r->CH, NORA_DMA_DSPIC33CK_CH_RELOAD);
    }

    /* DMAINTn: trigger select (CHSEL = DMA trigger-source ID from the device's
     * Table 10-1; NOT the CPU IRQ number -- resolved by dma_trigger_to_chsel()
     * above, which is the only place that transcription lives) + HALF enable.
     * done_int_en has no register bit on CK (DONE fires via _DMAnIE). */
    nora_dma_reg_write_field16(r->INT, NORA_DMA_DSPIC33CK_INT_CHSEL_MASK,
                                    NORA_DMA_DSPIC33CK_INT_CHSEL_POS, chsel);
    if (cfg->half_int_en) {
        nora_dma_reg_set16(r->INT, NORA_DMA_DSPIC33CK_INT_HALFEN);
    }

    /* Clear stale status flags (keep CHSEL/HALFEN) + pending CPU flag before
     * (re-)enabling the IRQ. */
    nora_dma_reg_clear16(r->INT, NORA_DMA_DSPIC33CK_INT_STATUS_MASK);
    dma_irq_clear_flag(ch);

    if (cfg->irq_priority_set) {
        dma_irq_set_priority(ch, cfg->irq_priority);
    }
    dma_irq_enable_bit(ch, cfg->irq_enable);

    return true;
}

bool nora_dma_channel_enable(nora_dma_channel_t ch, bool enable)
{
    const dma_ch_regs_t *r = dma_regs(ch);

    if (r == 0) {
        return false;
    }
    if (enable) {
        if (!nora_dma_global_is_ready()) {
            return false;
        }
        nora_dma_reg_set16(r->CH, NORA_DMA_DSPIC33CK_CH_CHEN);
    } else {
        nora_dma_reg_clear16(r->CH, NORA_DMA_DSPIC33CK_CH_CHEN);
    }
    return true;
}

void nora_dma_irq_enable(nora_dma_channel_t ch, bool enable)
{
    dma_irq_enable_bit(ch, enable);
}

bool nora_dma_irq_is_enabled(nora_dma_channel_t ch)
{
    return dma_irq_is_enabled_bit(ch);
}

void nora_dma_software_trigger(nora_dma_channel_t ch)
{
    const dma_ch_regs_t *r = dma_regs(ch);

    if (r == 0) {
        return;
    }
    nora_dma_reg_set16(r->CH, NORA_DMA_DSPIC33CK_CH_CHREQ);
}

bool nora_dma_request_pending(nora_dma_channel_t ch)
{
    const dma_ch_regs_t *r = dma_regs(ch);

    if (r == 0) {
        return false;
    }
    return nora_dma_reg_is_set16(r->CH, NORA_DMA_DSPIC33CK_CH_CHREQ);
}

/* The contract's count is 32-bit; DMACNTn is 16-bit, so this widens. The widening
 * is safe in both directions here: channel_config() already refuses a count above
 * UINT16_MAX, so no value this function can return was truncated on the way in. */
uint32_t nora_dma_read_count(nora_dma_channel_t ch)
{
    const dma_ch_regs_t *r = dma_regs(ch);

    if (r == 0) {
        return 0u;
    }
    return (uint32_t)*r->CNT;
}

void nora_dma_clear_status(nora_dma_channel_t ch)
{
    const dma_ch_regs_t *r = dma_regs(ch);

    if (r == 0) {
        return;
    }
    /* Clear only the status flags; keep CHSEL / HALFEN in DMAINTn. */
    nora_dma_reg_clear16(r->INT, NORA_DMA_DSPIC33CK_INT_STATUS_MASK);
}

void nora_dma_clear_irq_flag(nora_dma_channel_t ch)
{
    dma_irq_clear_flag(ch);
}

nora_dma_status_t nora_dma_read_status(nora_dma_channel_t ch)
{
    const dma_ch_regs_t *r = dma_regs(ch);

    if (r == 0) {
        return 0u;
    }
    return (nora_dma_status_t)*r->INT;
}

/* ---------------------------------------------------------------------------
 * Out-of-line twins of the `_hot` inlines
 *
 * Each is a call to its inline twin in nora_dma_dspic33ck_fast.h, so there is one
 * implementation of every operation. These exist so that a consumer which is NOT
 * on a measured ISR path -- a self-test, a console command, a bring-up sequence --
 * never has to include a header full of SFRs to ask a DMA question.
 * ------------------------------------------------------------------------- */

bool nora_dma_irq_disable_save(nora_dma_channel_t ch)
{
    return nora_dma_irq_disable_save_hot(ch);
}

void nora_dma_irq_restore(nora_dma_channel_t ch, bool was_enabled)
{
    nora_dma_irq_restore_hot(ch, was_enabled);
}

uint32_t nora_dma_read_src(nora_dma_channel_t ch)
{
    return nora_dma_read_src_hot(ch);
}

nora_dma_status_t nora_dma_isr_snapshot(nora_dma_channel_t ch)
{
    return nora_dma_isr_snapshot_hot(ch);
}

nora_dma_half_t nora_dma_half_from_status(nora_dma_status_t status)
{
    return nora_dma_half_from_status_hot(status);
}

bool nora_dma_status_has_half_done_conflict(nora_dma_status_t status)
{
    return nora_dma_status_has_half_done_conflict_hot(status);
}

bool nora_dma_status_has_overrun(nora_dma_status_t status)
{
    return nora_dma_status_has_overrun_hot(status);
}

bool nora_dma_status_has_completed_half(nora_dma_status_t status)
{
    return nora_dma_status_has_completed_half_hot(status);
}

bool nora_dma_status_has_completed(nora_dma_status_t status)
{
    return nora_dma_status_has_completed_hot(status);
}
