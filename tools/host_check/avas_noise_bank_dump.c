/*
 * Host harness for the L3 noise bank: render N samples of avas_noise_bank_ck.c and
 * print them, one integer per line, so run_noise_check.py can require them to be
 * bit-identical to the generator's reference.
 *
 * GUSTS ARE OFF here, and that is the point rather than a shortcut.  The gust drive is
 * a PRNG drawn once per band per control block; making two languages agree on it proves
 * nothing about the filter, and the modulation's actual specification is a standard
 * deviation in dB -- which the generator measures separately (1.56 dB against the
 * model's 1.5 dB).  What CAN be proved bit for bit is the part an ear cannot audit: the
 * source tilt, twelve SVFs, and the level.  So that is what this proves.
 */
#include <stdio.h>
#include <stdlib.h>

#include "avas_noise_bank_ck.c"

int main(int argc, char **argv)
{
    avas_noise_bank_ck_t bank;
    long n = (argc > 1) ? strtol(argv[1], NULL, 10) : 4096L;
    long i;

    avas_noise_bank_ck_init(&bank);
    for (i = 0; i < n; i++) {
        printf("%d\n", (int)avas_noise_bank_ck_sample(&bank));
    }
    return 0;
}
