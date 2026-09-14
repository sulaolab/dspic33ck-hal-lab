#ifndef TIMER_APP_H
#define TIMER_APP_H

/*
 * timer_app.{c,h} -- THE RUNNING TIME, and the one owner of the Timer1 vector.
 *
 * GetTicks() is a free-running millisecond counter since bring-up. Anything in this
 * tree that means "every N ms" is expressed against it:
 *
 *     static uint32_t last;
 *     if ((uint32_t)(GetTicks() - last) >= INTERVAL_MS) {
 *         last = GetTicks();
 *         ... do the thing ...
 *     }
 *
 * The cast-and-subtract is not decoration. It is what makes the test correct across the
 * 32-bit millisecond rollover (~49.7 days): the DIFFERENCE stays right when the counter
 * wraps, whereas `GetTicks() >= last + INTERVAL_MS` stops firing for one interval and
 * `GetTicks() - last` on mixed types can be promoted to something signed. Use the form
 * above verbatim. It keeps elapsed-time scheduling correct across the tick wrap.
 *
 * WHY IT IS HERE AND NOT IN A DRIVER. Until 2026-08-03 the only GetTicks() in this tree
 * was a static inline inside chip_drivers/wm8904_port.h -- a codec driver's porting shim.
 * So "what time is it" was reachable only by including a codec, was compiled out of every
 * profile without one, and returned 0 unless some demo happened to have started the timer.
 * Time is an application service; it now has an application-level home, and the codec shim
 * includes this instead of declaring its own.
 *
 * WHY THIS FILE OWNS _T1Interrupt. hal_timer/nora_tick_timer.h leaves the vector to
 * the application on purpose (the HAL cannot know what else wants a 1 ms edge). It used to
 * be claimed by app/timer_1ms.c, the vendor BSP compatibility layer, which is excluded from
 * the EV88G73A configuration -- so on that board the vector belonged to nobody and the tick
 * could not run at all. One vector, one owner, present in every profile; timer_1ms.c's
 * TICK_HANDLER registry is now serviced through timer_app_set_tick_hook() instead of
 * carrying the vector with it.
 *
 * NOT PROVIDED HERE: delay_ms()/delay_us(), which sonora's timer_app does provide.
 * chip_drivers/wm8904_port.h already defines delay_ms as a MACRO over libpic30's
 * __delay_ms(), and a function declaration plus a function-like macro of the same name is
 * an include-order trap, not a convergence. The blocking waits in wm8904.c are bounded by
 * that macro and are deliberately left alone; this file is about the running time.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the 1 ms tick. Two entry points rather than one config struct, because WHICH
 * CLOCK FEEDS TIMER1 IS A BOARD DECISION WITH A REASON, and the reason differs:
 *
 *   _from_fcy(fcy_hz)  Timer1 counts the peripheral clock (Fp = Fcy), so the tick is
 *                      only 1 ms if the system clock is at the operating point the
 *                      caller states. That coupling is a FEATURE where the tick paces a
 *                      visible signal: EV88G73A's LED cycle is a live check that Fcy is
 *                      on target (a half-speed clock shows up as a half-speed blink),
 *                      and reading Fcy back from the Clock HAL instead would self-correct
 *                      and hide exactly that fault.
 *   _from_frc(frc_hz)  Timer1 counts the FRC directly, so the tick survives a wrong or
 *                      fallen-back system clock. DM330030's inherited demo is clocked
 *                      this way and stays so.
 *
 * Both return false if the divisor cannot be made exact or the timer is absent; nothing
 * silently runs at the wrong rate. Call once, from profile_bring_up().
 *
 * That "cannot be made exact" is a real refusal and not just a description: the HAL
 * underneath was rounding to the nearest count and returning OK until 2026-08-03, so this
 * paragraph was a claim the implementation did not honour. It does now
 * (NORA_TICK_TIMER_ERR_INEXACT_PERIOD). Both current boards divide exactly --
 * 100 MHz/8/1000 = 12500 and 8 MHz/1/1000 = 8000 -- so no behaviour changed here; what
 * changed is that a future clock cannot quietly make every millisecond in the tree wrong.
 */
bool timer_app_start_from_fcy(uint32_t fcy_hz);
bool timer_app_start_from_frc(uint32_t frc_hz);

/* Milliseconds since the tick started; 0 before it has. */
uint32_t GetTicks(void);

/* False before timer_app_start_*() succeeds -- i.e. GetTicks() is standing still. */
bool timer_app_running(void);

/*
 * One optional per-millisecond callback, invoked from _T1Interrupt after the tick has
 * advanced. Exists for app/timer_1ms.c's vendor TICK_HANDLER registry (the RGB soft-PWM
 * and button sampler on DM330030 are clients of it); the hook is what lets that file keep
 * its registry without owning the vector.
 *
 * ISR CONTEXT, and at TIMER_APP_IRQ_PRIORITY -- see the note on it in the .c. Keep it
 * short; pass NULL to detach.
 */
typedef void (*timer_app_tick_hook_t)(void);
void timer_app_set_tick_hook(timer_app_tick_hook_t hook);

#ifdef __cplusplus
}
#endif

#endif /* TIMER_APP_H */
