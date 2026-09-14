/*
 * main.c -- the entry point, for every board this repo builds.
 *
 * There used to be two of these, one per profile, each buried in
 * boards/<board>/. Now there is one, here, where anyone opening the repo will
 * find it; the per-board parts are the five hooks in profile_main.h, and exactly
 * one profile is linked into any image.
 *
 * The loop below is deliberately the whole of it. If a board needs something
 * that does not fit these phases, the honest move is to add a hook and say so in
 * profile_main.h -- not to grow a special case here, which is how the two
 * private main()s drifted apart in the first place.
 */

#include <stdbool.h>

#include "profile_main.h"

int main(void)
{
    /* Order is load-bearing: the console's baud divisor is derived from the
     * clock, so bring-up precedes anything that prints. */
    profile_bring_up();
    profile_report();
    profile_start();

    for (;;) {
        /*
         * Pace first, then poll. The wait is what defines the loop rate and it
         * returns whether this iteration is a reporting one, so the decision to
         * print lives with the timer that sets the cadence rather than being
         * recomputed downstream.
         */
        bool report = profile_wait_next_tick();

        profile_poll(report);
    }
}
