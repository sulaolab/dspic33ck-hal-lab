#ifndef WM8904_PORT_H
#define WM8904_PORT_H

/*
 * wm8904_port.h -- dspic33ck-hal-lab port shim for the WM8904 driver.
 *
 * The WM8904 register driver (wm8904.c/.h/_def.h) is ported verbatim from the
 * upstream dsPIC33AK firmware. That project pulls its timing helpers, trace output and
 * audio-format switches from the app layer (app_specific_config_defs.h,
 * timer_app.h, ...). This lab has no such app layer, so this single header
 * supplies the small set of symbols wm8904.c depends on, keeping the driver
 * body a near-verbatim copy.
 *
 * Everything here is a LAB/board-level choice, not a WM8904 fact:
 *   - timing (delay_ms/delay_us/GetTicks),
 *   - the audio-interface framing the codec is configured for.
 * The register facts stay in wm8904_def.h.
 */

#include <stdint.h>
#include <stdio.h>

/*
 * GetTicks() comes from the app layer, exactly as it does upstream (that project's
 * timer_app.h, named in the note above). Until 2026-08-03 this file DEFINED it instead --
 * a static inline over nora_tick_timer_get_ms() -- which made a codec driver's port
 * shim the only place in the tree that could tell the time, and left it returning 0 in
 * every profile that did not happen to start the timer. app/timer_app.h is now that home,
 * on both this tree and sonora's, so this line is the whole of what is needed here.
 */
#include "timer_app.h"              /* GetTicks() */

/* --- Fcy for libpic30 __delay_* (device-max operating point; see the board bring-up file) --- */
#ifndef FCY
#define FCY 100000000UL
#endif
#include <libpic30.h>               /* __delay_ms / __delay_us (needs FCY above) */

/* --- Timing helpers expected by wm8904.c ---------------------------------- */
#define delay_ms(ms)   __delay_ms((double)(ms))
#define delay_us(us)   __delay_us((double)(us))

/* GetTicks() is declared by timer_app.h, included above. It is cosmetic in this driver --
 * the DC-servo and write-sequencer waits are bounded by the delay_ms loops here, not by
 * elapsed time -- so a tick that was not running could never hang wm8904.c; it only made
 * the TRACE timestamps read 0. Every profile's bring-up starts it now. */

/* --- Audio-interface framing this lab configures the WM8904 for -----------
 * Passthrough (ADC->DAC) demo runs TDM8 / 32-bit / no 1-bit delay, matching the
 * dsPIC SPI slave transport (8 slots). These mirror the upstream APP_* switches so
 * the driver body compiles unchanged. Override with -D before this header if a
 * future demo needs I2S. wm8904.c's #if RESOLVED_TRANSPORT_SLOTS_PER_FRAME/
 * DATA_DELAY_BITS checks were renamed to these two directly (they are the same
 * value under a different name, not a separate concept -- see wm8904.c). */
#ifndef APP_USE_I2S_FORMAT
#define APP_USE_I2S_FORMAT   0
#endif
#ifndef APP_USE_1_BIT_DELAY
#define APP_USE_1_BIT_DELAY  0
#endif
#ifndef APP_SLOTS_PER_FS
#define APP_SLOTS_PER_FS     8      /* TDM8 */
#endif

/* --- Board-specific analog choices the upstream driver's newer config path added ---
 * Both are explicit rather than left undefined-as-0: an undefined macro in an #if is
 * silently 0 in C, which happens to be the value wanted here, but "happens to" is not
 * a reason to omit it -- see docs/ck_silicon_findings.md's closing note on comments
 * (and here, absent macros) that disagree with what the code actually depends on.
 *
 *   RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK : this lab has no board with the upstream
 *     reference PCB's dual RED/BLUE input jacks: 0 selects the BLUE (IN2) input path,
 *     matching every WM8904 config this repo has run before this driver update.
 *   RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED  : the passthrough demo is line-in -> line-out,
 *     not a microphone path, so the MIC_BIAS supply stays off.
 * Neither is a WM8904 fact; both are this lab's own board choice, same as the rest of
 * this file. Override with -D before this header if a future demo needs otherwise. */
#ifndef RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK
#define RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK  0
#endif
#ifndef RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED
#define RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED   0
#endif

/* RESOLVED_BOARD_USE_CMSIS_I2C is deliberately left UNDEFINED, not defined to 0: wm8904.c
 * uses it in a plain #if (undefined -> 0 in C, no -Wundef in this build), matching how the
 * driver's own CMSIS branches were already inert here before this update (the previous
 * version's equivalent guard, ENA_CMSIS_I2C, was also never defined). */

/* The upstream project uses COMPILEASSERT() for its format sanity checks. Map it to C's
 * _Static_assert so the same checks hold here. */
#ifndef COMPILEASSERT
#define COMPILEASSERT(expr)  _Static_assert((expr), "WM8904 compile-time assertion failed")
#endif

/* --- TRACE: the driver's bring-up narration, and what it costs -------------
 * wm8904.c prints through TRACE, which upstream is plain printf(). On a 64 KB part that
 * one line is not small. Measured on the EV88G73A image, two -Full builds of the same
 * commit with the same pinned -BuildId (2026-08-07): 64,284 B -> 54,780 B, i.e. 96% -> 82%
 * of flash, and 2,148 B of free flash became 11,652 B. The 9,504 B splits as:
 *
 *   4,788 B  printf machinery -- the whole of libc99-elf.a, which left the link entirely:
 *            __printf_core / __fmt_d,o,s,u,x / __pop_int,ptr,float / __cvt_u / atoi /
 *            putc, and through __cvt_u and __pop_float the 64-bit divide-by-10 and the
 *            float-to-double conversion that no line in this tree asked for. (The 64-bit
 *            divmoddi3/muldi3/umuldi3 STAYED -- those are nora_clock_dspic33ck.c's.)
 *   3,192 B  the format strings themselves, in .const. Const sits in program flash at
 *            3 bytes per 2 data bytes, so a string costs 1.5x what it reads.
 *   1,296 B  the call sites in this file -- 50 varargs frames of argument marshalling.
 *     228 B  the stdio write hook in nora_uart_dspic33ck.o / uart_platform_stdio.o, .dinit
 *            and link padding.
 *
 * wm8904.c was the ONLY caller of printf in that image -- every other line the console
 * prints goes through uart_platform/console_out.h, which has no format machinery at all.
 * So the codec driver alone was paying for stdio, in an image sitting at 96% of flash.
 *
 * NOTE the semantics of the OFF path: ((void)0) does NOT evaluate its arguments, so a
 * side effect inside a TRACE argument would be silently lost. All 50 sites were audited
 * for this -- the only call in any argument is GetTicks(), a pure counter read. Keep it
 * that way: put nothing in a TRACE argument that has to happen.
 *
 * TRACE is therefore compiled out by default, with the messages left in the source.
 * Build with -Define ENA_WM8904_TRACE=1 to get the narration back for bring-up; that
 * build pays for printf exactly as it did before. -Define changes need -Full, or the old
 * objects are silently relinked and the switch appears not to work.
 *
 * Same shape as hal_spi_i2s_tdm's ENA_TDM_DBG, which gates that layer's printf for the
 * same reason. If the messages are ever wanted in a production image, point TRACE at
 * console_out_str/console_out_u32/console_out_hex16 HERE -- one place, without touching
 * the 50 call sites in the driver body. That is a different trade (it keeps the 3,192 B
 * of strings and drops only the 4,788 B), which is why it is not what this does.
 */
#ifndef ENA_WM8904_TRACE
#define ENA_WM8904_TRACE  0
#endif
#if (ENA_WM8904_TRACE != 0) && (ENA_WM8904_TRACE != 1)
#error "ENA_WM8904_TRACE must be 0 or 1 -- a bare -DENA_WM8904_TRACE with no value is refused rather than guessed at."
#endif
#if ENA_WM8904_TRACE
#define TRACE  printf
#else
#define TRACE(...)  ((void)0)
#endif

#endif /* WM8904_PORT_H */
