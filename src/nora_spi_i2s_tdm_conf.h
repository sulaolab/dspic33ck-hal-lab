#ifndef NORA_SPI_I2S_TDM_CONF_H
#define NORA_SPI_I2S_TDM_CONF_H

//===========================================================
// nora_spi_i2s_tdm_conf.h_example  --  PROJECT CONFIG (dspic33ck-hal-lab, SPI1 TDM8 slave-capable)
//
// This is the publishable EXAMPLE for the SPI/I2S/TDM HAL's compile-time config. The
// HAL ships only this template; the PROJECT supplies the real header (bring-your-own-
// config, like CMSIS RTE_Device.h / FreeRTOSConfig.h / lwipopts.h). To use the HAL:
//
//   1. Copy this file to your project, renamed nora_spi_i2s_tdm_conf.h.
//   2. Put it on the compiler include path (the HAL core does
//      #include "nora_spi_i2s_tdm_conf.h"). Keep it OUT of this folder so it
//      shadows nothing -- e.g. in your project's src/.
//   3. Edit the values below for your board.
//
// The `.h_example` extension means this template is never picked up by that include.
//
// This is the SOLE, self-contained HAL config entry: the core translation units include
// ONLY this header for config and it has NO app-layer dependency (it never includes
// app_specific_config_*). Everything the HAL needs lives here as plain literals -- so
// hal_spi_i2s_tdm/ stands alone (given a supplied conf.h) and is publishable as-is.
//
// Dependency direction: app code MAY read these macros (app -> HAL); the HAL MUST NOT
// read app config (HAL -> app is forbidden). If your project also derives these values
// in its own config, keep the two as independent owners and assert their CONSISTENCY on
// the APP side (compare your APP_* against the NORA_TDM_* here and #error on a
// mismatch). The HAL does not police the app, and vice versa.
//
// Each setting is -D-overridable (#ifndef-guarded). This template defaults to the
// SAFEST generic config: a single SPI1 TDM8 stream, no second codec. Override with -D
// or by editing below for I2S (2 slots), a different block size, a second SPI, etc.
//
// Compile-time integration settings:
//   NORA_TDM_SLOTS_PER_FS   slots per frame-sync: TDM8 = 8, I2S = 2.
//   NORA_TDM_BLOCK_FRAMES   frames per ping/pong half (DMA block size).
//   NORA_TDM_USE_SPI2      1 = SPI2 Audio transport is part of this build.
// (Sample rate is NOT a setting here -- the transport is rate-agnostic; the product's
// supported-rate policy lives in the app layer, not the HAL.)
// The core's static DMA ping-pong buffers are sized 2 * SLOTS_PER_FS *
// BLOCK_FRAMES, and configure() rejects a config_t whose slots_per_fs /
// block_frames do not match these compile-time values.
//===========================================================

// --- HAL geometry / topology (literals; -D wins) ---
#ifndef NORA_TDM_SLOTS_PER_FS
#define NORA_TDM_SLOTS_PER_FS    8     // TDM8 (I2S = 2)
#endif
#ifndef NORA_TDM_BLOCK_FRAMES
#define NORA_TDM_BLOCK_FRAMES    32    // frames per ping/pong half
#endif
#ifndef NORA_TDM_USE_SPI2
#define NORA_TDM_USE_SPI2        0     // single SPI Audio transport by default
#endif

//===========================================================
// DMA channel allocation (single source of truth for the SPI<->DMA binding)
//
// Each SPI Audio instance owns one RX + one TX DMA channel. These numbers land in the HAL
// core's s_spi_legs[] table, and each RX channel is bound by a compile-time assert to the
// core's LITERAL _DMA<n>Interrupt vector name. So the leg table follows an edit here, but the
// vector does NOT: remapping an RX channel FAILS THE BUILD (assert) until the vector name in
// nora_spi_i2s_tdm_dspic33ck.c is changed to match by hand. Each is -D overridable. Maintain
// a chip-wide map by hand: the HAL cannot see other subsystems' DMA usage. Two legs given the
// same channel are caught the same way (the assert, or a redefined vector on an RX collision).
//===========================================================
#ifndef NORA_TDM_SPI1_RX_DMA
#define NORA_TDM_SPI1_RX_DMA   0
#endif
#ifndef NORA_TDM_SPI1_TX_DMA
#define NORA_TDM_SPI1_TX_DMA   1
#endif
#ifndef NORA_TDM_SPI2_RX_DMA
#define NORA_TDM_SPI2_RX_DMA   2
#endif
#ifndef NORA_TDM_SPI2_TX_DMA
#define NORA_TDM_SPI2_TX_DMA   3
#endif


//===========================================================
// Sync-domain seeds (co-clocked grouping).
//
// Legs that share one BCLK/FS carry the SAME domain id. It is ORTHOGONAL to the clock
// role: a domain holds at most one CLOCK_MASTER and any number of slaves riding its
// clock, and two legs on independent clocks belong to different domains even if both
// are masters. It is also orthogonal to BLOCK_REF, which names the leg whose RX block
// defines the block boundary for the singleton reporters.
//
// These are SEEDS only: the value lands in each leg at build time, and
// nora_spi_i2s_tdm_configure_system() overwrites it with the caller's topology
// table, which is the runtime source of truth. A build that never calls
// configure_system() keeps these values, so a single-leg project needs no topology code.
//
// Range is 0..31 because start_all_domains() dedups and rolls back through a 32-bit
// domain mask. Coverage matches the DMA-channel macros above (SPI1/SPI2): a project
// adding a third leg supplies its channels and its domain the same way.
//===========================================================
#ifndef NORA_TDM_SPI1_SYNC_DOMAIN
#define NORA_TDM_SPI1_SYNC_DOMAIN   0
#endif
#ifndef NORA_TDM_SPI2_SYNC_DOMAIN
#define NORA_TDM_SPI2_SYNC_DOMAIN   0   // default second leg is a FOLLOWER on SPI1's clock
#endif


//===========================================================
// DMA interrupt-vector ownership.
//   1 (default) : TURNKEY -- the HAL DEFINES the _DMA<rx>Interrupt vectors itself.
//   0           : the HAL defines NO vectors. The integrator owns the IVT and calls
//                 nora_spi_i2s_tdm_inst_rx_isr(spiN()) from their own
//                 _DMA<rx>Interrupt for each instance's RX channel.
//===========================================================
#ifndef NORA_TDM_DEFINE_DMA_VECTORS
#define NORA_TDM_DEFINE_DMA_VECTORS   1
#endif


//===========================================================
// Engine-wide TDMsum occupancy profiler (nora_spi_i2s_tdm_tdmsum_*).
//   1 (default) : the RX-block ISR brackets itself with the sumprof enter/exit hooks, so
//                 _tdmsum_get() reports peak busy time summed over ALL legs in one common
//                 window. This is how a multi-leg engine's true CPU occupancy is measured
//                 (the per-leg monitor cannot see legs overlapping each other).
//   0           : the hooks, their state and the three _tdmsum_* entry points are not
//                 compiled at all. Saves both ROM and per-block ISR cycles. The per-leg
//                 load monitor (nora_spi_i2s_tdm_inst_get_load()) is unaffected.
//
// Set this to 0 in a single-leg project, or any project that never calls _tdmsum_get():
// the hooks are NOT free just because nothing reads the result. They run on every RX block
// whenever the high-res timer is initialized, and a consumer that leaves them at 1 without
// calling _tdmsum_get() pays for a measurement it never looks at.
//
// Note for THIS family: NORA_TDM_ISR_TIMING_SAMPLE_LOG2 above samples the per-leg monitor
// (default 1 block in 16), but a UNION occupancy cannot be sampled -- a window whose peak
// fell in an unmeasured block would read low. So the sumprof hooks time EVERY block, and
// their per-block cost is NOT reduced by that knob. On a single-leg build there is also
// nothing to union, which is the case for setting this to 0.
//===========================================================
#ifndef NORA_TDM_SUMPROF
#define NORA_TDM_SUMPROF   1
#endif


//===========================================================
// ISR load-monitor sampling policy.
//
// A 48 kHz / 32-frame TDM block calls the RX DMA ISR about 1500 times per
// second.  Measuring every invocation costs two coherent 32-bit timer reads
// and several volatile 32-bit updates *inside the thing being measured*.
//
// Keep the DMA/SPI fault monitors exact on every block; this knob applies only
// to execution-time min/max/last statistics.  The default 4 means one timing
// sample per 2^4 = 16 blocks (~94 samples/s for the default geometry).  Thus
// TDM load values are deliberately a sampled peak, not a proof that every
// block was observed.  Set to 0 while investigating a rare timing failure to
// restore the former every-block measurement, at the expected ISR cost.
//
// It is -D overridable so a product can select its own diagnostic/performance
// tradeoff without changing the transport source.
//===========================================================
#ifndef NORA_TDM_ISR_TIMING_SAMPLE_LOG2
#define NORA_TDM_ISR_TIMING_SAMPLE_LOG2   4
#endif

#if (NORA_TDM_ISR_TIMING_SAMPLE_LOG2 < 0) || (NORA_TDM_ISR_TIMING_SAMPLE_LOG2 > 7)
#error "NORA_TDM_ISR_TIMING_SAMPLE_LOG2 must be in 0..7 (period must fit the 8-bit ISR counter)."
#endif


//===========================================================
// Instance count + physical assignment.
//
// conf.h no longer OWNS the topology. There was a NORA_TDM_INSTANCE_LIST X-macro
// here -- one row per leg, naming the physical SPI, both DMA channels, the block-timing
// role, the sync domain and the geometry -- and the core generated its leg enum,
// per-instance buffers, s_spi_legs[] table and _DMAnInterrupt vectors from it. The core
// now writes all four out directly in C, keyed off the per-leg knobs ABOVE
// (NORA_TDM_USE_SPI2, NORA_TDM_SPIn_RX/TX_DMA, NORA_TDM_SPIn_SYNC_DOMAIN)
// and the stream geometry (NORA_TDM_SLOTS_PER_FS / _BLOCK_FRAMES). So:
//
//   - this file supplies per-leg VALUES; the core owns the SHAPE (how many legs, which
//     leg is the block-timing reference, which vector belongs to which leg);
//   - adding a leg is a core edit (four explicit blocks) plus its channel/domain macros
//     here -- it is no longer a one-row change in this file;
//   - the block-timing reference is leg 0 by construction in the core, not a role column
//     here. It is NOT a clock master: the clock role (master/slave) is per-instance in
//     config_t.clock_role, and the sync domain is a third independent axis (leg 0 defines
//     the block, clock_role says who drives the clock, sync-domain says who rides the SAME
//     clock).
//   - per-leg geometry is still possible (each leg's Rx_<name>/Tx_<name> is sized
//     2*slots*blk from that leg's own values), but both default legs take the stream-wide
//     macros above.
//
// The per-leg sync-domain macros above remain build-time SEEDS only; configure_system()
// commits each leg's domain from the caller's topology table at runtime.
//===========================================================


#if (NORA_TDM_SLOTS_PER_FS <= 0)
#error "NORA_TDM_SLOTS_PER_FS must be positive."
#endif

#if (NORA_TDM_SLOTS_PER_FS > 255)
#error "NORA_TDM_SLOTS_PER_FS must fit in uint8_t."
#endif

/*
 * FS cadence must be FRMCNT-encodable, checked HERE so an unframeable geometry stops the
 * build instead of reaching hw.c and disabling framing at runtime.
 *
 * FRMCNT counts SERIAL WIRE WORDS as log2(N) with N <= 32, and a 32-bit audio slot is TWO
 * 16-bit wire words (the wire is MODE16 -- see audit defect 5), so the cadence is
 * 2 * SLOTS_PER_FS wire words and SLOTS_PER_FS must be a power of two no greater than 16.
 * TDM32 was representable under the old MODE32 wire and is not anymore.
 */
#if (NORA_TDM_SLOTS_PER_FS > 16)
#error "NORA_TDM_SLOTS_PER_FS > 16 cannot be framed: 2 wire words per 32-bit slot would need FRMCNT > 32."
#endif

#if ((NORA_TDM_SLOTS_PER_FS & (NORA_TDM_SLOTS_PER_FS - 1)) != 0)
#error "NORA_TDM_SLOTS_PER_FS must be a power of two -- FRMCNT encodes the FS cadence as log2(N)."
#endif

#if (NORA_TDM_BLOCK_FRAMES <= 0)
#error "NORA_TDM_BLOCK_FRAMES must be positive."
#endif

#if (NORA_TDM_BLOCK_FRAMES > 65535)
#error "NORA_TDM_BLOCK_FRAMES must fit in uint16_t."
#endif

#if ((NORA_TDM_USE_SPI2 != 0) && (NORA_TDM_USE_SPI2 != 1))
#error "NORA_TDM_USE_SPI2 must be 0 or 1."
#endif

#if ((NORA_TDM_SUMPROF != 0) && (NORA_TDM_SUMPROF != 1))
#error "NORA_TDM_SUMPROF must be 0 or 1."
#endif

/*
 * Domain ids must fit the 32-bit mask start_all_domains() uses to dedup the domains it
 * saw and to roll back only the ones it started. Checked for BOTH seeds unconditionally,
 * not just the one the current NORA_TDM_USE_SPI2 selects: a guard that only fires in
 * the configuration nobody builds is worth less than one that always fires.
 */
#if ((NORA_TDM_SPI1_SYNC_DOMAIN) < 0) || ((NORA_TDM_SPI1_SYNC_DOMAIN) >= 32)
#error "NORA_TDM_SPI1_SYNC_DOMAIN must be in 0..31."
#endif

#if ((NORA_TDM_SPI2_SYNC_DOMAIN) < 0) || ((NORA_TDM_SPI2_SYNC_DOMAIN) >= 32)
#error "NORA_TDM_SPI2_SYNC_DOMAIN must be in 0..31."
#endif

#if (NORA_TDM_SLOTS_PER_FS > (2147483647 / (2 * NORA_TDM_BLOCK_FRAMES)))
#error "SPI/I2S/TDM DMA buffer geometry overflows the static buffer element count."
#endif

#endif // NORA_SPI_I2S_TDM_CONF_H
