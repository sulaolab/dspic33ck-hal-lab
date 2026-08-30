# -*- coding: utf-8 -*-
"""Emit the dB -> mantissa table the CK firmware compiles, for a FIXED output shift.

GENERATED FILE -- do not hand-edit the output (src/app/dsp/gain_db_tables.h).

WHY A TABLE AND NOT A FORMULA
-----------------------------
`gain = 10^(dB/20)` needs a logarithm, and this part has no FPU and no business
calling `powf` (sonora's `db_to_lin()` does exactly that, once at init, because it
has a float unit and the room for libm).  The conversion therefore happens HERE,
at build time, and the target compiles integers -- the same rule
`gen_avas_type_ty_ck_tables.py` follows for the AVAS coefficients.

The table is indexed rather than macro-pasted because the value is ALSO settable at
run time (`*ti` / `*to`), and a run-time selector cannot be a preprocessor paste.
One table therefore serves both the compile-time default and the console.

THE DECOMPOSITION, AND WHY IT CHANGED
-------------------------------------
The first version of this table split the gain into an OCTAVE COUNT and a mantissa
in [1, 2), on the reasoning that the octave would be free: the hot path's exact
scale ends in a `<< 1`, so `<< (1 + n)` looked like the same instruction with a
different immediate.

That is true of the algebra and false of the code, and the image proved it: with
`n` a run-time field, `<< (1 + n)` is not a shift instruction but a general
variable 32-bit shift -- `subr`, a branch, `lsr`, `sl`, `sl`, `ior` -- and it also
turns the clamp bounds into run-time values, which the block ISR then SPILLS to the
frame and re-loads every sample.  Seven instructions, plus two reloads, per sample,
for the one line the design had priced at zero.

So the shift is now a COMPILE-TIME CONSTANT and the mantissa carries the whole
gain:

    y = ((x * m) >> 16) << SHIFT,   m = round(gain * 2^(16 - SHIFT))

With SHIFT = 4 that is one constant shift, two immediate compares for the clamp,
and no spill.  The price is mantissa resolution, and it is a price worth paying:

  * one LSB is 0.0148 dB at the WORST point of the grid (-24.0 dB, where the
    mantissa is smallest at 258) and 0.002 dB at unity, against a grid of 0.5 dB
    and an ear that found the value by listening.  The knob is not less precise in
    any sense a listener can reach.
  * the range is set by the mantissa fitting uint16: +24.08 dB, which is why
    SHIFT = 4 and not 3 -- SHIFT = 3 would cap at +18.06 dB and lose the +24 dB the
    grid already published, and SHIFT = 5 would double the error to buy +30 dB that
    nothing has asked for.

UNITY, AND THE FOUR BITS
------------------------
At 0.0 dB the mantissa is exactly 2^12, so `y = (x >> 4) << 4` = x with the low FOUR
bits cleared, where the octave form cleared one.  Those bits are pad, not audio: the
codec word is 24-bit left-justified in the 32-bit slot, so the low eight bits of a
sample carry nothing, and `gain_ctrl` already clears bit 0 at unity for exactly this
reason.  The firmware additionally SKIPS the whole stage when the mantissa is unity
(a per-block branch, not a per-sample one), so in the shipped configuration those
four bits are not touched at all -- but the identity is still proved for the scaled
path, because a `*ti 0.0` must not be a different signal from a `*ti` that was never
typed.

THE GRID IS 0.5 dB, ON PURPOSE
------------------------------
This gain exists to compensate an analog stage that was turned down, and it is found
by listening; 0.5 dB steps are already finer than that process can resolve, and a
coarse grid keeps the table small (194 bytes now that the octave column is gone).  A
console request in tenths of a dB is SNAPPED to this grid and the realised value is
printed back, the same way `gain_ctrl_mute_set()` snaps a ramp_ms to an available
curve and says which one it picked.

    python tools/gen_gain_db_tables.py [out.h]
    (default out: src/app/dsp/gain_db_tables.h)
"""
import math
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "src/app/dsp/gain_db_tables.h"

# The grid, in HALF-decibels, inclusive.  +-24 dB is not a limit of the arithmetic
# but of what this knob is for: 18.7 dB is the measured headroom of the quiet input
# this exists to lift, so +24 already covers
# every value that can be reached without clipping the source, and -24 covers the
# other direction by symmetry.  It IS, however, close to the limit of the mantissa:
# see SHIFT below.
HALF_MIN = -48          # -24.0 dB
HALF_MAX = +48          # +24.0 dB

# The fixed output shift, and therefore the mantissa's scale: m = gain * 2^(16-SHIFT).
# 4 is the smallest value that still expresses +24.0 dB in a uint16 (needs 64917 of
# 65535), and the smallest is the best one because the error is 2^-(16-SHIFT).
SHIFT = 4
MANT_ONE = 1 << (16 - SHIFT)        # 4096 = unity


def decompose(db):
    """dB -> mantissa for the hot path's fixed-shift scale.

    Returns the integer the firmware uses, not a float gain, so that this function
    IS the definition the host check compares C against.
    """
    m = int(round(10.0 ** (db / 20.0) * MANT_ONE))

    # Both ends are the design's business, not a runtime concern: the grid is fixed
    # here, so a violation is a generator bug and belongs in an assert.
    assert 1 <= m <= 0xFFFF, (db, m)

    return m


def realised_db(m):
    """What the integer actually means, in dB -- for the comment column."""
    return 20.0 * math.log10(m / float(MANT_ONE))


def main(dest=None):
    dest = dest or OUT
    halves = list(range(HALF_MIN, HALF_MAX + 1))
    rows = [(h, decompose(h * 0.5)) for h in halves]

    worst = max(abs(realised_db(m) - h * 0.5) for (h, m) in rows)

    out = []
    w = out.append
    w("/* =========================================================================")
    w(" * dB -> Q%d mantissa table for the CK audio chain's PRE_GAIN / POST_GAIN --"
      % (16 - SHIFT))
    w(" * GENERATED by tools/gen_gain_db_tables.py, DO NOT EDIT")
    w(" *")
    w(" * Grid: %.1f dB to %+.1f dB in 0.5 dB steps (%d entries, %d bytes of flash)."
      % (HALF_MIN * 0.5, HALF_MAX * 0.5, len(rows), len(rows) * 2))
    w(" * Worst realisation error over the whole grid: %.6f dB (at %+.1f dB, where the"
      % (worst, min(rows, key=lambda r: r[1])[0] * 0.5))
    w(" * mantissa is smallest); %.6f dB at unity.  The grid is 0.5 dB, so this is"
      % abs(realised_db(MANT_ONE + 1) - 0.0))
    w(" * three decimal orders below anything the knob can express.")
    w(" *")
    w(" * ONE mantissa per entry, consumed by gain_db.h's fixed-shift scale:")
    w(" *     t = x * m / 2^16      (two native 16x16 multiplies)")
    w(" *     y = t << %d            (saturating -- a boost CAN overflow)" % SHIFT)
    w(" * so 0.0 dB is mantissa 0x%04X = 2^%d and reproduces x apart from its low %d"
      % (MANT_ONE, 16 - SHIFT, SHIFT))
    w(" * bits, which are pad and not audio (24-bit word left-justified in a 32-bit")
    w(" * slot) -- and the firmware skips the stage entirely at unity anyway.")
    w(" *")
    w(" * The shift is a CONSTANT on purpose: as a run-time field it cost seven")
    w(" * instructions a sample and spilled the clamp bounds.  See the generator's")
    w(" * header comment.")
    w(" * ========================================================================= */")
    w("")
    w("#ifndef GAIN_DB_TABLES_H")
    w("#define GAIN_DB_TABLES_H")
    w("")
    w("#include <stdint.h>")
    w("")
    w("/* The grid, in half-decibels, inclusive.  Index 0 is GAIN_DB_HALF_MIN. */")
    w("#define GAIN_DB_HALF_MIN   (%d)      /* %+.1f dB */" % (HALF_MIN, HALF_MIN * 0.5))
    w("#define GAIN_DB_HALF_MAX   (%d)      /* %+.1f dB */" % (HALF_MAX, HALF_MAX * 0.5))
    w("#define GAIN_DB_ENTRIES    (%d)" % len(rows))
    w("")
    w("/* The FIXED output shift, and the mantissa that means unity.  Both are used in")
    w(" * arithmetic and in the clamp bounds, so they are one definition, here. */")
    w("#define GAIN_DB_SHIFT      (%d)" % SHIFT)
    w("#define GAIN_DB_MANT_ONE   (0x%04Xu)   /* 2^%d -- exactly 0.0 dB */"
      % (MANT_ONE, 16 - SHIFT))
    w("")
    w("/* Half-decibels -> table index, valid for GAIN_DB_HALF_MIN..GAIN_DB_HALF_MAX.")
    w(" * A constant expression, so the compile-time default costs no code at all. */")
    w("#define GAIN_DB_INDEX_OF_HALF(h)  ((h) - (GAIN_DB_HALF_MIN))")
    w("")
    w("/* Tenths of a dB -> half-decibels, as a CONSTANT expression for the config")
    w(" * default.  Requires a multiple of 5 (app_config.h enforces it with an")
    w(" * #error); the run-time path snaps instead, see gain_db_from_x10(). */")
    w("#define GAIN_DB_HALF_OF_X10(x10)  ((x10) / 5)")
    w("")
    w("/* The gain, as the multiplier the hot path uses: Q%d, unity 0x%04X. */"
      % (16 - SHIFT, MANT_ONE))
    w("static const uint16_t gain_db_mant[GAIN_DB_ENTRIES] = {")
    for (h, m) in rows:
        w("    0x%04Xu,   /* %+6.1f dB  ->  %8.5f x   (realised %+9.6f dB) */"
          % (m, h * 0.5, m / float(MANT_ONE), realised_db(m)))
    w("};")
    w("")
    w("#endif /* GAIN_DB_TABLES_H */")

    # LF, matching gen_avas_type_ty_ck_tables.py: .gitattributes normalises the repo to
    # LF and checks out CRLF, so writing CRLF here would commit a CRLF blob.
    with open(dest, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")

    print("wrote %s (%d entries, shift %d, worst error %.6f dB)"
          % (dest, len(rows), SHIFT, worst))


if __name__ == "__main__":
    main()
