#ifndef NORA_DMA_H
#define NORA_DMA_H

#include <stdint.h>
#include <stdbool.h>

/*
 * nora_dma.h
 * Nora DMA HAL public interface.
 *
 * Portability scope:
 *   This interface minimizes application changes between Nora-supported
 *   dsPIC33AK and dsPIC33CK ports. It is not a universal, arbitrary-processor
 *   DMA HAL: channel inventory, trigger-to-hardware mapping, address-window
 *   rules, and register behavior remain properties of the selected Nora port
 *   and backend.
 *
 * This file is the portable contract and holds no processor SFRs: no <xc.h>, no
 * register header, no status bit macro, and no `static inline`. The measured
 * direct-SFR path lives in nora_dma_dspic33ck_fast.h, where every function is the
 * portable name plus `_hot`; including that header is a consumer's declaration
 * that it is on a measured ISR path. Every `_hot` function has its out-of-line
 * twin here, so a consumer that is not on such a path never sees a register.
 *
 * Design boundaries (intentional):
 *   - Knows nothing about SPI, audio, PWM, DSP, printf, or application code.
 *   - Contains NO ping-pong / block-streaming policy.
 *   - The caller owns the DMA buffers; this HAL only takes addresses.
 *   - No callback framework: DMA ISRs stay in the consumer modules, which own
 *     their _DMAnInterrupt() handlers.
 *
 * CK properties handled inside the backend (see nora_dma_dspic33ck.c), stated
 * because they decide this port's enumerator sets rather than the API shape:
 *   - CK has 4 channels (0..3), all 16-bit.
 *   - CK DOES have an address-limit window and it must be programmed before use
 *     -- see nora_dma_global_init().
 *   - Trigger select, HALF/DONE status and the half-done enable share one
 *     register per channel.
 *   - The element count register is the count itself (NOT count - 1) and is
 *     16-bit, so this port validates cfg.count against its own limit.
 *   - One reload control (AK's RELOADS/RELOADD/RELOADC collapse to it).
 *   - No independent DONE interrupt enable: DONE is gated only by the CPU IE.
 *   - The element-width field is 1 bit: 16-bit word or byte (no 32-bit element
 *     on the 16-bit core).
 */

/*
 * Logical Nora DMA channel identity. A port backend validates and maps this
 * identity to the DMA channel inventory of its processor; code outside a
 * backend must not treat these values as SFR indexes.
 *
 * Enumerator set is per family: CK has four channels where AK has eight.
 */
typedef enum {
    NORA_DMA_CHANNEL_0,
    NORA_DMA_CHANNEL_1,
    NORA_DMA_CHANNEL_2,
    NORA_DMA_CHANNEL_3,
} nora_dma_channel_t;

/*
 * DMA triggers currently needed on this port. These are logical peripheral
 * events, not hardware trigger-select register values. The selected Nora port
 * maps them to its device-specific trigger representation, and rejects a trigger
 * its device does not have (CK: SPI3 exists on MP508 and not on MC105, where the
 * same select code is Reserved).
 *
 * Enumerator set is per family: CK reaches SPI3 where AK reaches SPI4.
 *
 * NORA_DMA_TRIGGER_NONE means "no peripheral trigger intended": the channel is
 * meant to be driven by nora_dma_software_trigger() alone. It is not a
 * "don't care" -- the select field always names something, so a channel with no
 * peripheral attached needs a positively quiet choice rather than a leftover
 * one. Naming the intent here is what lets a software-triggered consumer move
 * between families, since the quiet code itself is per device.
 *
 * It is an intent, NOT a hardware guarantee, because the select field has no "no
 * trigger" encoding: each backend picks a source that is quiet in its own system.
 * On dsPIC33CK that source is NVM Write Complete, so such a channel is
 * software-only for exactly as long as nothing completes a flash write. A system
 * that self-programs flash while one is armed must pick a different quiet code --
 * see dma_trigger_to_chsel() in nora_dma_dspic33ck.c.
 */
typedef enum {
    NORA_DMA_TRIGGER_NONE,
    NORA_DMA_TRIGGER_SPI1_RX,
    NORA_DMA_TRIGGER_SPI1_TX,
    NORA_DMA_TRIGGER_SPI2_RX,
    NORA_DMA_TRIGGER_SPI2_TX,
    NORA_DMA_TRIGGER_SPI3_RX,
    NORA_DMA_TRIGGER_SPI3_TX,
} nora_dma_trigger_t;

/* A raw, backend-owned DMA status snapshot. Use the query functions below
 * rather than interpreting processor status bits in a consumer. */
typedef uint32_t nora_dma_status_t;

/* Transfer element width.
 * CK's width field is a single bit (16-bit word or byte). The enum keeps the AK
 * names/values for source parity; nora_dma_channel_config() maps HALFWORD to the
 * CK 16-bit word, BYTE to the CK byte, and REJECTS WORD (32-bit is not available
 * on the 16-bit core). */
typedef enum {
    NORA_DMA_SIZE_BYTE     = 0,   /* 1 byte                        */
    NORA_DMA_SIZE_HALFWORD = 1,   /* 16-bit                        */
    NORA_DMA_SIZE_WORD     = 2,   /* 32-bit: rejected on this port  */
} nora_dma_size_t;

/* Address behavior after each element. */
typedef enum {
    NORA_DMA_ADDR_FIXED     = 0,  /* unchanged         */
    NORA_DMA_ADDR_INCREMENT = 1,  /* increment by SIZE */
    NORA_DMA_ADDR_DECREMENT = 2,  /* decrement by SIZE */
} nora_dma_addr_mode_t;

/* Transfer/repeat mode. */
typedef enum {
    NORA_DMA_TRMODE_ONESHOT           = 0, /* One-Shot                 */
    NORA_DMA_TRMODE_REPEAT_ONESHOT    = 1, /* Repeated One-Shot (used) */
    NORA_DMA_TRMODE_CONTINUOUS        = 2, /* Continuous               */
    NORA_DMA_TRMODE_REPEAT_CONTINUOUS = 3, /* Repeated Continuous      */
} nora_dma_trmode_t;

/*
 * One channel's configuration. Field names and semantics are the portable
 * contract; the CK-specific collapses are noted per field.
 */
typedef struct {
    volatile void            *src;
    volatile void            *dst;
    /* Element count of one transfer/repeat -- the number of elements to move, NOT
     * an "elements - 1" register value.
     *
     * For a ping-pong buffer this is the count of the WHOLE ping+pong buffer, not
     * of one half. HALF fires at the midpoint of this count and DONE at its end,
     * which is what makes HALF -> first half and DONE -> second half
     * (nora_dma_half_t) mean what they say; passing one half's count would put the
     * HALF boundary in the middle of the ping half instead. Current users pass
     * ARRAY_SIZE() of the full ping+pong buffer.
     *
     * The contract is 32-bit in every family. This port's count register is
     * 16-bit, so nora_dma_channel_config() rejects values above its limit rather
     * than truncating them. */
    uint32_t                  count;

    nora_dma_addr_mode_t src_mode;
    nora_dma_addr_mode_t dst_mode;
    nora_dma_size_t      size;
    nora_dma_trmode_t    tr_mode;

    /* CK has a single reload control; these three fields are OR-collapsed
     * (reload is set if any is true). */
    bool                      reload_count;
    bool                      reload_src;
    bool                      reload_dst;

    bool                      half_int_en;
    /* CK has no independent DONE interrupt enable. DONE always contributes to the
     * channel interrupt when the CPU IE is set, so done_int_en cannot be masked
     * independently of HALF; it is accepted for API parity but writes no register
     * bit. */
    bool                      done_int_en;

    nora_dma_trigger_t        trigger;      /* logical peripheral trigger */

    /* CPU interrupt control.
     * irq_priority is written only when irq_priority_set is true, so a caller
     * can intentionally keep its port reset/default priority. */
    bool                      irq_priority_set;
    uint8_t                   irq_priority; /* 0..7, used iff irq_priority_set */
    bool                      irq_enable;
} nora_dma_channel_cfg_t;

/* Pure DMA ping-pong timing mechanism (NOT policy):
 * maps a backend status snapshot to which buffer half just completed.
 * DONE takes precedence over HALF, matching the current RX handler behavior.
 */
typedef enum {
    NORA_DMA_HALF_NONE   = 0,   /* neither HALF nor DONE set             */
    NORA_DMA_HALF_FIRST  = 1,   /* HALF: first half just filled/emptied  */
    NORA_DMA_HALF_SECOND = 2,   /* DONE: second half just filled/emptied */
} nora_dma_half_t;

/*
 * Pure predicates over a status snapshot. All are side-effect-free and take the
 * word, not the channel, so a caller that already snapshotted can classify it
 * without touching hardware again.
 *
 * Each has a `_hot` static-inline twin in the backend's *_fast.h for ISR use; see
 * the fast header for the naming rule.
 *
 * Which question to ask depends on how the channel was armed, and the two
 * "completed" predicates are NOT interchangeable:
 *
 *   - ping-pong (a repeating transfer whose halves are consumed alternately):
 *     ask has_completed_half(), then half_from_status() for which half it was.
 *   - single-shot (one transfer, armed once, waited on until it finishes):
 *     ask has_completed(). has_completed_half() is already true at the MIDPOINT
 *     of such a transfer, so spinning on it releases the caller while the second
 *     half is still being written -- and half_from_status() cannot express "the
 *     whole transfer" either, because a single-shot has no second half to name.
 */
bool nora_dma_status_has_half_done_conflict(nora_dma_status_t status);
bool nora_dma_status_has_overrun(nora_dma_status_t status);

/* A ping-pong half boundary was reached (HALF or DONE). Not the single-shot
 * question -- see the note above. */
bool nora_dma_status_has_completed_half(nora_dma_status_t status);

/* The transfer as a whole completed (DONE). This is the single-shot question,
 * and it is what a self-test that arms one full-block transfer and spins must
 * wait on. */
bool nora_dma_status_has_completed(nora_dma_status_t status);

/* ---- Global ---- */

/* Bring up the DMA controller: give DMA SRAM bus priority over the CPU (prevents
 * overrun/dropped samples on audio streams), program the address-limit window,
 * and enable the controller. Safe to call more than once.
 *
 * The address-window step is mandatory on this port, not a refinement: both limit
 * registers reset to 0x0000, and a channel enabled against that window faults on
 * its first element. Note this is not pre-access protection: the limit test is not
 * made before the access (DS70005399D page 174, DMAINTn Note 2), so the first
 * out-of-window access can happen and the transaction is terminated after it --
 * the channel then stops with correct-looking registers and streams nothing. Must
 * be called before any channel is configured. */
void nora_dma_global_init(void);

/* Returns true if the controller is on, DMA holds SRAM bus priority, AND the
 * address-limit window is programmed. Side-effect-free: no register writes, no
 * printf, no halt. */
bool nora_dma_global_is_ready(void);

/* ---- Per channel ---- */

/*
 * Invalid-channel handling convention across this API (ch outside the device's
 * channel inventory):
 *   - config / enable          return false (and write nothing).
 *   - void IRQ/status helpers  silently ignore the call (no register write).
 *   - read helpers             return 0.
 */

/* Configure a channel (SRC/DST/CNT, channel fields, trigger, IRQ
 * priority/enable). Leaves the channel DISABLED. Call
 * nora_dma_channel_enable(ch, true) to start.
 * Returns false (and writes NO channel register) if cfg is NULL, the channel
 * index is invalid, the DMA controller is not ready (nora_dma_global_init() must
 * have been called first), or cfg holds an out-of-range enum / IRQ priority --
 * including NORA_DMA_SIZE_WORD, a count above this port's register limit, or a
 * trigger this device does not implement. Returns true on success. Never calls
 * nora_dma_global_init() itself.
 * Re-config safe: masks the channel's CPU IRQ and clears stale status / pending
 * CPU interrupt flag before and after programming, so a stale interrupt or
 * leftover HALF/DONE status cannot disturb a stop -> re-config -> restart
 * cycle. */
bool nora_dma_channel_config(nora_dma_channel_t ch, const nora_dma_channel_cfg_t *cfg);

/* Start/stop the channel.
 * enable==true: returns false (writes nothing) if the channel index is invalid
 * or the DMA controller is not ready; otherwise starts the channel and returns
 * true.
 * enable==false: always disables (safe direction) and returns true, except for
 * an invalid channel index which returns false. */
bool nora_dma_channel_enable(nora_dma_channel_t ch, bool enable);

/* Request one transfer from software: exactly the effect the selected peripheral
 * trigger would have if it fired once. The channel must already be configured and
 * enabled. This is the only way to exercise a channel with no peripheral attached,
 * which is what makes "is the DMA controller itself working?" answerable
 * independently of the peripheral's event wiring. No-op for an invalid channel. */
void nora_dma_software_trigger(nora_dma_channel_t ch);

/* True while a software- or peripheral-requested transfer is still outstanding;
 * hardware clears the request when it is serviced. False for an invalid channel. */
bool nora_dma_request_pending(nora_dma_channel_t ch);

/* Elements still to move in the current transfer. Falling with no status flag set
 * is direct evidence the channel is being triggered and is moving data. Returns 0
 * for an invalid channel. The contract is uint32_t in every family; a backend whose
 * count register is narrower returns the widened value. */
uint32_t nora_dma_read_count(nora_dma_channel_t ch);

/* General IRQ control: set/clear the channel's CPU interrupt enable,
 * independently of the channel enable.
 * Needed by the TDM soft-stop path, which masks the DMA IRQ before stopping the
 * channel so the ISR cannot run during teardown. */
void nora_dma_irq_enable(nora_dma_channel_t ch, bool enable);

/* General IRQ control: read the channel's CPU interrupt enable;
 * false for an invalid channel.
 * Lets a caller save/restore the IE state around a brief mask without hardcoding
 * the channel's SFR (used by the TDM core's per-instance RX-IE guard). */
bool nora_dma_irq_is_enabled(nora_dma_channel_t ch);

/* Save/mask and restore the CPU interrupt enable around a brief critical
 * section. The returned value from nora_dma_irq_disable_save() is for the
 * paired nora_dma_irq_restore() call. */
bool nora_dma_irq_disable_save(nora_dma_channel_t ch);
void nora_dma_irq_restore(nora_dma_channel_t ch, bool was_enabled);

/* Clear channel status flags (the interrupt-flag bits only; trigger select and
 * the half-done enable are kept). */
void nora_dma_clear_status(nora_dma_channel_t ch);

/* Clear the channel's CPU interrupt flag. */
void nora_dma_clear_irq_flag(nora_dma_channel_t ch);

/* Read raw channel status. Use nora_dma_half_from_status() to interpret it. */
nora_dma_status_t nora_dma_read_status(nora_dma_channel_t ch);

/* Read the active source address. The TX-side ping-pong consumer compares this
 * against its own half-buffer address; that comparison remains consumer policy. */
uint32_t nora_dma_read_src(nora_dma_channel_t ch);

/* Interpret a raw backend status value as a ping-pong half indicator (pure mechanism). */
nora_dma_half_t nora_dma_half_from_status(nora_dma_status_t status);

/* Ordered ISR snapshot sequence (NOT a single atomic instruction): clear the CPU
 * interrupt flag, snapshot status, then clear status. Returns a raw status
 * snapshot. Operation order is backend-defined and currently preserves:
 * clear IRQ flag, read status, clear status.
 *
 * Order note: the CPU interrupt flag is cleared FIRST so that a HALF/DONE event
 * raised anywhere later in the sequence still re-asserts it and the ISR runs
 * again. That is all the ordering buys, and it is NOT race-free: clearing status
 * is a read-modify-write of the register that also carries the control fields, so
 * an event landing between the snapshot and the clear has its status flag cleared
 * along with the observed ones. The re-entered ISR then sees no flag at all, which
 * is why NORA_DMA_HALF_NONE must be rejected by the caller rather than mapped to a
 * buffer half. Do not rely on the second event "staying latched" in the status
 * word; only the CPU flag survives. */
nora_dma_status_t nora_dma_isr_snapshot(nora_dma_channel_t ch);

#endif /* NORA_DMA_H */
