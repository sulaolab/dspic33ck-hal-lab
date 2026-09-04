#ifndef _WM8904_H_
#define	_WM8904_H_


//===========================================================
// INCLUDES
//===========================================================

#include <stdbool.h>
#include <stdint.h>
//===========================================================
// Definition
//===========================================================


//===========================================================
// Enum & Struct typedef
//===========================================================

/*
 * Declick research one-shot restart-strategy bitmask for mute/restart pop reduction.
 * 0 == baseline == the shipping behavior below;
 * this lab never arms a mask (nothing here calls wm8904_set_pending_declick()), so the
 * WSEQ shutdown + manual startup sequence is always what runs. Kept rather than stripped
 * so the complete strategy set remains available. Bits may be OR-combined.
 */
typedef enum {
    WM8904_DECLICK_NONE            = 0x00u,  // shipping default: WSEQ shutdown + manual startup (see below)
    WM8904_DECLICK_ORDERED_SHUTDN  = 0x01u,  // C: Table 42-ordered HP disable during shutdown
    WM8904_DECLICK_WARM_SERVO      = 0x02u,  // B: skip R0 SW-reset + DCS_TRIG_DAC_WR retained-servo restore
    WM8904_DECLICK_SOFT_UNMUTE     = 0x04u,  // D: ramped HPOUT analog unmute
    WM8904_DECLICK_WRITE_SEQUENCER = 0x08u,  // A: force vendor WSEQ shutdown (now also the default; redundant)
    WM8904_DECLICK_SOFT_SHUTDOWN   = 0x10u,  // E: ramp HPOUT gain DOWN before mute/shutdown (measured no-op)
    WM8904_DECLICK_WSEQ_STARTUP    = 0x20u,  // F: vendor WSEQ startup for the analog bring-up (startup pop)
    WM8904_DECLICK_LEGACY_QUENCH   = 0x40u,  // regression: force the OLD quench shutdown (pre-declick default)
} wm8904_declick_mask_t;

/*
 * SHUTDOWN discharge policy:
 *   default (mask NONE) = vendor Control Write Sequencer shutdown (Table 89), which does the full ordered
 *   VMID/charge-pump/bias power-down and suppresses the shutdown pop (~47 dB vs the old quench) and, by
 *   leaving the chip fully discharged, also cuts the following startup pop. WSEQ needs SYSCLK; it falls back
 *   to the quench on timeout. LEGACY_QUENCH forces the old behavior for A/B regression.
 */


//===========================================================
// Variables
//===========================================================


//===========================================================
// Function Prototype
//===========================================================

/* Configure the standard-rate codec path and verify all I2C writes/readbacks. */
extern bool wm8904_init( uint8_t inst, bool master_cfg );
extern void wm8904_init_96k_adc_only( uint8_t inst, bool master_cfg );
extern void wm8904_init_96k_dac_only( uint8_t inst, bool master_cfg );
extern void wm8904_shutdown( uint8_t inst );
extern void wm8904_dump_reg( uint8_t inst );
/*
 * Analog output mute/unmute (HPOUT), and whether the codec CONFIRMED it.
 *
 * Returns false when the requested state could not be verified: an out-of-range inst, an
 * I2C write that failed, or a readback that disagreed with what was written. The value is
 * this driver's sticky per-instance I/O health (the same thing wm8904_init() returns), so
 * an earlier failure on this instance also reads as unverified -- which is the safe
 * direction: once I2C on this codec has failed, no later write can be trusted either.
 *
 * IT IS NOT COSMETIC. *ts mutes the codec before halting TDM/DMA precisely so that
 * debugger console traffic cannot reach HPOUT while a drag-and-drop programming cycle
 * resets the target, and buildtools/README.md gates the HEX copy on that reply. A void
 * return let an I2C failure be reported as a successful mute, i.e. a green light for the
 * one hazard the command exists to prevent.
 *
 * A HPOUT readback intentionally masks WM8904_HPOUT_VU before comparing (it is a
 * volume-update trigger that does not read back), so a good mute write does NOT read as
 * unverified. See wm8904_verify_write_readback().
 */
extern bool wm8904_set_analog_output_mute( uint8_t inst, bool mute );

/* Raw register access for interactive experiments. `inst` is the I2C instance. Thin
 * public wrappers over the internal verified write / read. */
extern void     wm8904_reg_write( uint8_t inst, uint8_t reg, uint16_t data );
extern uint16_t wm8904_reg_read( uint8_t inst, uint8_t reg );

/*
 * Distinguish a genuine, separate slave codec on `inst` from the already-
 * configured TDM master codec that a bridged MikroBUS-A/B I2C bus aliases onto
 * this instance. Returns true only when a WM8904 responds on `inst` AND it is
 * NOT in master mode (AUDIO_INTERFACE_1.BCLK_DIR clear).
 * Call AFTER the master codec (e.g. MikroA) has been initialised: a genuine
 * independent slave reads BCLK_DIR=0, while the master codec seen through a
 * bridge reads BCLK_DIR=1. Lets the caller skip B init on bridged boards
 * without a build switch, and without false-skipping a real dual-codec board.
 */
extern bool wm8904_is_distinct_slave( uint8_t inst );

/*
 * Select the sample rate applied to `inst` on its NEXT (re)configuration.
 * Stores the request only; the caller must re-init the codec for it to take effect.
 * Returns false for an instance out of range or an fs outside the supported standard
 * menu: 8/11.025/12/16/22.05/24/32/44.1/48 kHz. The 44.1 kHz family uses the codec FLL;
 * the 48 kHz family is FLL-less. Default (never called) is 48 kHz.
 */
extern bool wm8904_set_rate_hz( uint8_t inst, uint32_t fs_hz );

/* Currently-selected sample rate for `inst` (default 48000 until wm8904_set_rate_hz changes it). */
extern uint32_t wm8904_get_rate_hz( uint8_t inst );

/*
 * Declick research (one-shot): arm/read the restart-strategy bitmask consumed by the NEXT codec
 * (re)configure. `mask` is a bitwise-OR of wm8904_declick_mask_t. Set to WM8904_DECLICK_NONE (0)
 * for baseline. The mask persists until explicitly changed. Nothing in this lab calls
 * wm8904_set_pending_declick(), so it stays NONE and the shipping sequence always runs;
 * kept for the complete restart-strategy interface.
 */
extern void    wm8904_set_pending_declick( uint8_t mask );
extern uint8_t wm8904_get_pending_declick( void );

/* True once a full STARTUP DC-servo run on `inst` has captured offset values usable by WARM_SERVO. */
extern bool    wm8904_declick_servo_captured( uint8_t inst );

#endif //!_WM8904_H_
