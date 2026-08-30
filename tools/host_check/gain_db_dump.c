/*
 * gain_db_dump.c -- host harness for src/app/dsp/gain_db.h.
 *
 * Two jobs, and they are different jobs:
 *
 *  1. SELF-PROOF, here in C, against a 64-bit reference. The target cannot afford an
 *     int64 multiply (that is the whole reason gain_db.h exists in the form it does --
 *     ___muldi3, ~88 cycles a sample), but the HOST can, so the 16x16 reconstruction is
 *     checked against the obvious `(int64_t)x * m >> 16 << SHIFT` with saturation, for
 *     every one of the table's 97 entries. The failure modes of this kind of rewrite --
 *     a sign lost on an arithmetic shift, a clamp bound off by one, a low word treated
 *     as signed -- all produce audio that still sounds like audio, so an exact
 *     comparison is the only instrument that separates them from correct code.
 *
 *     It now also proves something the fixed-shift form ADDED: the clamp is decided on
 *     t's HIGH WORD against two 16-bit immediates, and the reference decides it on the
 *     full 64-bit product. Those two agree exactly only because `INT32_MAX >> SHIFT` is a
 *     whole multiple of 65536 -- an argument that is easy to make on paper and worth
 *     having checked either side of every boundary by machine.
 *
 *  2. A CHECKSUM per entry, which run_gain_db_check.py recomputes from the same model
 *     that generated the table. That is what ties the firmware to the generator: the
 *     table could be internally consistent and still not be 10^(dB/20).
 *
 * Plus the one claim worth proving exhaustively rather than by sampling: at 0.0 dB the
 * scale is the IDENTITY on every audio bit, for all 2^32 inputs. That premise is what
 * makes the stage safe to leave in the path, and "we tried some samples" is not the same
 * statement. With the fixed shift it is the identity apart from the low GAIN_DB_SHIFT
 * bits, which are pad and not audio (24-bit word left-justified in a 32-bit slot) -- and
 * the firmware skips the stage entirely at unity anyway, so the proof covers the path a
 * `*ti 0.0` takes rather than the one the shipped configuration takes.
 *
 *     gcc -std=c99 -O2 -Wall -Wextra -I src/app/dsp tools/host_check/gain_db_dump.c
 *
 * Prints one line per table entry:  idx half mant checksum nsat
 * then "IDENTITY ok" (or a diagnosis), then "SELFCHECK ok".
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gain_db.h"

/* ------------------------------------------------------------------------- *
 * The 64-bit reference. This is the DEFINITION the firmware's two 16x16
 * multiplies are required to reproduce, written the way it would be written if
 * width were free -- deliberately NOT sharing a line of code with the thing it
 * checks, and in particular deciding saturation on the FULL product rather than
 * on a high word.
 * ------------------------------------------------------------------------- */
static int32_t ref_scale(int32_t x, uint16_t mant, int *saturated)
{
    const int64_t prod = ((int64_t)x * (int64_t)mant) >> 16;   /* floor, exact */
    const int64_t y    = prod << GAIN_DB_SHIFT;

    *saturated = 0;
    if (y > (int64_t)INT32_MAX) { *saturated = 1; return INT32_MAX; }
    if (y < (int64_t)INT32_MIN) { *saturated = 1; return INT32_MIN; }

    return (int32_t)y;
}

/* ------------------------------------------------------------------------- *
 * The sample list, defined by a formula so that run_gain_db_check.py can walk
 * exactly the same one without either side sending 6 million numbers over a pipe.
 *
 * STRIDE is 65521, the largest prime below 65536 and therefore coprime with it:
 * 65536 steps of it visit every possible LOW WORD exactly once. That matters here
 * specifically -- the low word is the operand fed to __builtin_muluu as unsigned,
 * and "the low half was treated as signed" is the bug this arrangement cannot miss.
 * ------------------------------------------------------------------------- */
#define STRIDE   65521u
#define SEED     0x01234567u
#define NSTRIDE  65536u
#define NEDGE    8192           /* exhaustive +-NEDGE around each clamp boundary */

static int32_t stride_sample(uint32_t k)
{
    return (int32_t)(SEED + (k * STRIDE));    /* wraps mod 2^32, which is the point */
}

/* Where x crosses the clamp, derived the same way on both sides: t = x*m/2^16 must
 * stay within +-2^(31-SHIFT), so x is about that bound scaled back up by 2^16/m. */
static int32_t clamp_edge_x(uint16_t mant)
{
    const int64_t tmax = (int64_t)(INT32_MAX >> GAIN_DB_SHIFT);
    const int64_t x    = (tmax << 16) / (int64_t)mant;

    /* At heavy attenuation the crossing is off the end of int32 -- there is no x that
     * can clamp. Park the sweep at the top of the range instead of overflowing. */
    return (x > (int64_t)INT32_MAX) ? INT32_MAX : (int32_t)x;
}

static uint32_t checksum_step(uint32_t acc, int32_t y)
{
    /* FNV-1a over the four bytes, chosen only because it is short to restate in
     * python and mixes every bit -- nothing here needs a cryptographic hash. */
    uint32_t v = (uint32_t)y;
    int      b;

    for (b = 0; b < 4; b++) {
        acc ^= (v & 0xFFu);
        acc *= 16777619u;
        v  >>= 8;
    }

    return acc;
}

static int failures = 0;

static void check_one(int32_t x, uint16_t mant, int idx)
{
    uint16_t nsat_c = 0u;
    int      nsat_r = 0;
    int32_t  yc = gain_db_scale(x, mant, &nsat_c);
    int32_t  yr = ref_scale(x, mant, &nsat_r);

    if ((yc != yr) || ((nsat_c != 0u) != (nsat_r != 0))) {
        if (failures < 10) {
            printf("MISMATCH idx=%d mant=0x%04X x=%ld  fw=%ld(sat %u)"
                   "  ref=%ld(sat %d)\n",
                   idx, (unsigned)mant, (long)x,
                   (long)yc, (unsigned)nsat_c, (long)yr, nsat_r);
        }
        failures++;
    }
}

int main(void)
{
    int idx;

    for (idx = 0; idx < GAIN_DB_ENTRIES; idx++) {
        const uint16_t mant = gain_db_mant[idx];
        const int      half = idx + GAIN_DB_HALF_MIN;
        const int32_t  edge = clamp_edge_x(mant);
        uint32_t       acc  = 2166136261u;    /* FNV-1a offset basis */
        uint16_t       nsat = 0u;
        uint32_t       k;
        int32_t        d;

        for (k = 0u; k < NSTRIDE; k++) {
            const int32_t x = stride_sample(k);
            int32_t       y;

            check_one(x, mant, idx);
            y   = gain_db_scale(x, mant, &nsat);
            acc = checksum_step(acc, y);
        }

        /* Exhaustive either side of both clamp boundaries. Sampling cannot be trusted
         * here: an off-by-one bound is invisible everywhere except within one count
         * of it, and that is exactly where the loudest samples land -- and it is the
         * high-word clamp's only opportunity to differ from the 64-bit one. */
        for (d = -NEDGE; d <= NEDGE; d++) {
            const int64_t xp = (int64_t)edge + d;
            const int64_t xn = -(int64_t)edge + d;

            if ((xp >= INT32_MIN) && (xp <= INT32_MAX)) {
                check_one((int32_t)xp, mant, idx);
            }
            if ((xn >= INT32_MIN) && (xn <= INT32_MAX)) {
                check_one((int32_t)xn, mant, idx);
            }
        }

        check_one(INT32_MIN,     mant, idx);
        check_one(INT32_MIN + 1, mant, idx);
        check_one(-1,            mant, idx);
        check_one(0,             mant, idx);
        check_one(1,             mant, idx);
        check_one(INT32_MAX - 1, mant, idx);
        check_one(INT32_MAX,     mant, idx);

        printf("%d %d %u %u %u\n", idx, half, (unsigned)mant,
               (unsigned)acc, (unsigned)nsat);
    }

    /* --------------------------------------------------------------------- *
     * 0.0 dB is the identity, for all 2^32 inputs. Exhaustive on purpose: it is
     * cheap enough on a host that sampling it would be a choice to know less.
     *
     * "Identity" means every bit that carries audio. The low GAIN_DB_SHIFT bits
     * are dropped -- the codec word is 24-bit left-justified in the 32-bit slot,
     * so the low eight bits are pad, and the gain stage downstream already drops
     * bit 0 at unity for the same reason (gain_ctrl.h, consequence 3). The
     * fixed-shift form widened that from one bit to four; both are inside the pad.
     * --------------------------------------------------------------------- */
    {
        const int      unity_idx = GAIN_DB_INDEX_OF_HALF(0);
        const uint16_t mant      = gain_db_mant[unity_idx];
        const uint32_t keep      = ~(uint32_t)((1u << GAIN_DB_SHIFT) - 1u);
        uint16_t       nsat      = 0u;
        uint32_t       u         = 0u;
        long           bad       = 0;

        if (mant != GAIN_DB_MANT_ONE) {
            printf("IDENTITY FAIL: 0.0 dB is mant=0x%04X, not 0x%04X\n",
                   (unsigned)mant, (unsigned)GAIN_DB_MANT_ONE);
            bad++;
        }

        do {
            const int32_t x = (int32_t)u;

            if (gain_db_scale(x, mant, &nsat) != (int32_t)(u & keep)) {
                if (bad < 5) {
                    printf("IDENTITY FAIL at x=%ld\n", (long)x);
                }
                bad++;
            }
            u++;
        } while (u != 0u);

        if (nsat != 0u) {
            printf("IDENTITY FAIL: %u samples saturated at 0.0 dB\n", (unsigned)nsat);
            bad++;
        }

        if (bad == 0) {
            printf("IDENTITY ok (all 2^32 inputs, low %d bits dropped)\n",
                   (int)GAIN_DB_SHIFT);
        } else {
            printf("IDENTITY failed\n");
            failures++;
        }
    }

    if (failures != 0) {
        printf("SELFCHECK FAILED: %d mismatches\n", failures);
        return 1;
    }

    printf("SELFCHECK ok\n");

    return 0;
}
