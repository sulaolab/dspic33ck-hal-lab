/*
 * Host harness: render N samples of avas_type_ty_ck and print them as decimal Q15,
 * one per line, so run_host_check.py can diff them against the numpy model.
 *
 * The point of this is narrow and worth stating: the model in
 * tools/avas_type_ty_fixed_model.py is where the design's accuracy was measured
 * (48.1 dB below the offline reference, -79.0 dBFS line-free floor).  Those
 * figures only describe the FIRMWARE if the firmware computes the same integers.
 * "Looks right on a scope" cannot distinguish a correct engine from one with a
 * wrong headroom shift, so the two are compared exactly instead.
 *
 * avas_synth_line_ck.c is compiled unmodified.  It contains no register access, no
 * chip header and no peripheral dependency, which is what makes that possible --
 * keep it that way.
 *
 * The GATE is not exercised here: the model has no gate, so the comparison runs
 * avas_line_ck_render_sample() (the engine proper).  The gate is a separate
 * one-pole with its own arithmetic, checked by run_host_check.py in closed form.
 */
#include <stdio.h>
#include <stdlib.h>

#include "avas_synth_line_ck.h"

int main(int argc, char **argv)
{
    static avas_line_ck_t s;   /* static: ~1 KB, and this mirrors the firmware */
    long n = (argc > 1) ? strtol(argv[1], NULL, 10) : 24000;
    long i;

    avas_line_ck_init(&s);

    for (i = 0; i < n; i++) {
        printf("%d\n", (int)avas_line_ck_render_sample(&s));
    }

    return 0;
}
