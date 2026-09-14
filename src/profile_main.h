#ifndef PROFILE_MAIN_H
#define PROFILE_MAIN_H

/*
 * profile_main.h -- the contract between src/main.c and a board profile.
 *
 * WHY THIS EXISTS
 * ---------------
 * There were two main() functions, one per board, both buried in
 * boards/<board>/. Two costs: whoever opens the repo has no obvious entry
 * point, and the genuinely-common shape of bring-up was invisible because it was
 * written out twice in different words.
 *
 * The shape IS common. Both boards ran the same four phases:
 *
 *   1  bring-up      clock, pins, console            (order is load-bearing:
 *                                                     baud comes from the clock)
 *   2  report        banner / operating point
 *   3  start         whichever exercisers are enabled at build time
 *   4  forever       poll the exercisers, paced by a timer
 *
 * These are the same devices doing much the same job, so that is not a
 * coincidence -- and where a future board's sequence genuinely differs, the
 * difference now has to be stated as a hook rather than hidden inside a private
 * main().
 *
 * WHAT IS DELIBERATELY *NOT* HERE
 * -------------------------------
 * No board detection, no function-pointer table, no registration. Exactly one
 * profile is compiled into any image -- the mutual exclusion is already enforced
 * by build.ps1's source list and the profiles' own #error guards -- so these are
 * plain extern symbols resolved at link time. A dispatch mechanism would add a
 * layer that can never have more than one entry.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Phase 1. Clock, port defaults, console pins, console. Must leave the console
 * usable, because phase 2 prints through it.
 */
void profile_bring_up(void);

/*
 * Phase 2. Announce what came up -- build ID, clock, reset cause, whatever this
 * board can report. Runs once, after phase 1.
 */
void profile_report(void);

/*
 * Phase 3. Start the exercisers this image was built with. Runs once.
 * A profile with nothing to start supplies an empty body.
 */
void profile_start(void);

/*
 * Phase 4, called once per main-loop iteration.
 *
 * `report` is true on the iterations where periodic status should be PRINTED.
 * The distinction matters and is why this is a parameter rather than something
 * the profile works out for itself: polling has to stay at the loop rate (button
 * sampling, scope-trigger transactions) while printing is throttled far slower,
 * and conflating the two makes a console-readability change silently degrade
 * button response.
 */
void profile_poll(bool report);

/*
 * Phase 4's pacing. Blocks until the next iteration is due, and is where a
 * profile may service anything that must not wait for the next loop (the
 * EV88G73A console drains its UART RX here -- polling only between iterations
 * would overrun the hardware FIFO).
 *
 * Returns true on the iterations where profile_poll() should be asked to print.
 * Owning the cadence here keeps it next to the timer that defines it.
 */
bool profile_wait_next_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_MAIN_H */
