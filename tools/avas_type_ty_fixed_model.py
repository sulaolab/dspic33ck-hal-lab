# -*- coding: utf-8 -*-
"""Bit-accurate model of the FIXED-POINT line-model AVAS engine for dsPIC33CK.

TWO COEFFICIENT SETS, ONE MODEL -- which is the same finding the firmware rests
on: everything below is arithmetic that does
not know which vehicle it is rendering.  So this file is NOT copied for the
Type_LB; it is given a different table.  Which one comes from argv[1]:

  * an AK-format header (`avas_synth_type_ty_tables.h`) -- the Type_TY L1 set, 185 lines
    in 11 clusters, with the clustering and the 60 s peak already decided there.
  * a LINE LIST (`tools/L3_params_type_lb.txt`) -- the Type_LB L3 set, 264
    lines, where the clustering is done HERE from MAX_SPAN_HZ (there is no AK
    Type_LB table to inherit it from) and the 60 s peak is read out of the
    file's header, where its generator measured it by AK's own convention.

The file keeps its name because the build-time knobs, the host check and the cost
probe all spell it that way, and the owner's rename decision (design section 9.1)
was engine-only for exactly that reason.  tools/avas_type_lb_fixed_model.py is the
Type_LB's driver over this module, not a second model.

Why this exists
---------------
The shipped engine (`dspic33ak-audio-dsp-sonora`'s
`src/apps/classic/dsp/avas_synth_type_ty.c`) is float throughout and runs on a
dsPIC33AK, which has an FPU.  dsPIC33CK has none, so a CK port is a rewrite of
the arithmetic.  Every quality figure quoted for the AK engine (48.0 dB below
signal at D=32, -71.5 dBFS floor in the line-free bands) came from an offline
model, not from hardware; this reuses that model's metric code.

A fixed-point rewrite has failure modes the float version cannot have: a wrong
headroom shift, a table too coarse, an interpolation fraction truncated too far.
On a speaker those are indistinguishable from a wrong coefficient table.  So the
fixed-point design is measured HERE first, against the same reference, and only
then written in C.

What is modelled
----------------
Exactly the integer operations the C will execute, with numpy int64 carrying the
values and explicit masks/shifts emulating 16- and 32-bit registers:

  * phase        uint32, full cycle = 2**32.  Wrap IS the integer overflow, so
                 the float `audio_fast_wrap_0_to_2pi()` disappears.
  * sine         Q15 table, `TABBITS` address bits, optional linear
                 interpolation on `FRACBITS` of the phase remainder.  cos is
                 the same table read a quarter-cycle further along, which is an
                 exact integer index offset -- no second table.
  * amplitudes   Q15, scaled by A_SCALE = 32767 / max_k(cluster amp sum) so that
                 |Z_k| <= 32767 for every cluster (|sum a_j e^{i p_j}| <= sum a_j).
  * envelope     int32 whose TOP 16 BITS are the Q15 value.  Chosen so the C
                 reads the Q15 part as the high word -- free on a 16-bit core --
                 while the low 16 bits carry the interpolation slope's fraction.
  * carriers     two variants, see CARRIER below.

Usage
-----
    python tools/avas_type_ty_fixed_model.py [table.h] [seconds]

Environment overrides (all optional), so a sweep is a shell loop:
    TABBITS=9 FRACBITS=8 CARRIER=table TERMSHIFT=8 DEC=32
    ENVINTERP=rect|polar POLAR_CONV=exact|cordic POLAR_ABITS=15 POLAR_PBITS=32
    (see ENVI below; polar was rejected on the metric in docs section 16 and that
    rejection was WITHDRAWN on listening, so it is a shipping candidate again)
"""
import math
import os
import re
import sys

import numpy as np

HDR = (sys.argv[1] if len(sys.argv) > 1
       else "tools/host_check/ref/avas_synth_type_ty_tables.h")
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0

FS = 48000.0
N = int(round(SECONDS * FS))

# ---------------------------------------------------------------------------
# Table.  Two formats, and NEITHER is a copy of the coefficients: the AK header is
# the file the AK firmware compiles, and the line list is generated straight out of
# the analysis cache by tools/gen_avas_type_lb_lines.py.  Re-run either analysis and
# this follows.
#
# MAX_SPAN_HZ is read only by the line-list path, because it is a decision that the
# AK header has already made and baked into its cluster array.  It is the width no
# cluster may exceed, and it trades full-rate carriers for baseband bandwidth.
# ---------------------------------------------------------------------------
MAX_SPAN_HZ = float(os.environ.get("MAX_SPAN_HZ", 200.0))


def _load_ak_header(path):
    """The AK firmware's generated table: lines, clusters and the 60 s peak."""
    txt = open(path, encoding="utf-8").read()

    def _block(name):
        return re.search(r"s_ty_l1_%s\[[^\]]*\]\s*=\s*\{(.*?)\n\};" % name,
                         txt, re.S).group(1)

    lines = np.array([[float(v) for v in m] for m in re.findall(
        r"\{\s*([-\d.eE+]+)f,\s*([-\d.eE+]+)f,\s*([-\d.eE+]+)f\s*\}", _block("line"))])
    clus = [(float(a), int(b), int(c)) for a, b, c in re.findall(
        r"\{\s*([-\d.eE+]+)f,\s*(\d+)u,\s*(\d+)u\s*\}", _block("cluster"))]
    peak = float(re.search(r"AVAS_TY_L1_PEAK_ABS\s+\(([\d.eE+-]+)f\)", txt).group(1))
    return lines, clus, peak


def _load_line_list(path):
    """A `FRQ AMP PHA` list, clustered HERE by MAX_SPAN_HZ.

    The clustering rule is the AK generator's, reproduced rather than approximated:
    greedy contiguous runs of ascending
    frequency no wider than MAX_SPAN_HZ, carrier at the AMPLITUDE-WEIGHTED centroid
    so the smallest baseband offset lands on the strongest lines.  Contiguity is not
    cosmetic -- the firmware's clusters ARE runs of table entries, which is why it
    needs no per-line cluster index.

    The peak comes out of the file's header comment, where the generator measured it
    over 60 s of running time exactly as AK does.  It is not recomputed here: a 60 s
    render of 264 lines is half a minute of numpy, and every host-check run would
    pay it to arrive at a number that changes only when the line list does.
    """
    txt = open(path, encoding="utf-8").read()
    m = re.search(r"peak_abs\s*=\s*([\d.eE+-]+)", txt)
    if not m:
        raise SystemExit(
            "%s carries no `peak_abs=` in its header -- regenerate it with "
            "tools/gen_avas_type_lb_lines.py, which measures it over 60 s the way "
            "the AK generator does.  Without it the output "
            "level would be invented here instead of measured." % path)
    peak = float(m.group(1))
    lines = np.loadtxt(path)
    assert lines.ndim == 2 and lines.shape[1] == 3, "expected FRQ AMP PHA columns"
    lines = lines[np.argsort(lines[:, 0])]
    clus, first = [], 0
    for i in range(1, len(lines) + 1):
        if (i < len(lines)) and ((lines[i, 0] - lines[first, 0]) <= MAX_SPAN_HZ):
            continue
        seg = lines[first:i]
        clus.append((float((seg[:, 1] * seg[:, 0]).sum() / seg[:, 1].sum()),
                     first, i - first))
        first = i
    return lines, clus, peak


SOURCE_IS_LINE_LIST = HDR.lower().endswith(".txt")
lines, clus, PEAK_ABS = (_load_line_list(HDR) if SOURCE_IS_LINE_LIST
                         else _load_ak_header(HDR))

FRQ, AMP, PHA = lines[:, 0], lines[:, 1], lines[:, 2]
NLINE, K = len(lines), len(clus)
FC = np.array([c[0] for c in clus])
FIRST = np.array([c[1] for c in clus], dtype=np.int64)
COUNT = np.array([c[2] for c in clus], dtype=np.int64)
FC_PER_LINE = np.repeat(FC, COUNT)
assert FIRST[0] == 0 and int((FIRST + COUNT)[-1]) == NLINE, "clusters must tile the table"

# ---------------------------------------------------------------------------
# Fixed-point design parameters
#
# WHAT THE DEFAULTS BELOW ARE, because three of them have been overtaken by the
# firmware and the comments on them still say "what ships": they are the REFERENCE
# configuration -- rect, 32/32 phase, the wide envelope, every floor floored -- and
# that is deliberate.  The headline 48.1 dB / -79.0 dBFS is DEFINED at this point, so
# moving these defaults would silently redefine the figure every later section quotes.
#
# What the firmware ships is stated in ONE place, the generated header
# (src/app/dsp/avas_type_ty_ck_tables.h): polar, 16/16, ENVFRAC=0 with ENVROUND=1 since
# document sections 22 and 25.  tools/host_check/run_host_check.py reads all of them
# out of that header and sets them here before importing this module, which is why an
# argument-free host check certifies the shipped combination while an argument-free run
# of THIS file still prints the reference.  So read "what ships" below as "what shipped
# when the paragraph was written"; the document's section numbers in each one date it.
# ---------------------------------------------------------------------------
DEC = int(os.environ.get("DEC", 32))
TABBITS = int(os.environ.get("TABBITS", 9))        # 9 -> 512 entries -> 1 KB flash
# FRACBITS is 16 because that is the width at which the interpolation multiply's
# shift becomes FREE on a 16-bit core -- ">> 16" of a 32-bit product is "use the
# high word", so it costs no instruction at all, where ">> 8" costs a three
# instruction shift/shift/ior on the register pair.  It was measured against
# FRACBITS=8 and is IDENTICAL on every figure (48.1 dB, -78.2 dBFS at 2 s), so
# this is a free change, not a quality/cost trade.  0 still disables interpolation.
FRACBITS = int(os.environ.get("FRACBITS", 16))
CARRIER = os.environ.get("CARRIER", "table")       # "table" | "rotator"
TERMSHIFT = int(os.environ.get("TERMSHIFT", 4))    # per-carrier shift before summing y
# WHICH COORDINATES THE ENVELOPE IS INTERPOLATED IN.  This is the section 12
# "polar carriers" lever, and it is a QUALITY question before it is a cost one:
#
#   "rect"   what ships.  env_i/env_q are walked linearly towards the next Z_k, and
#            the carrier loop computes I*cos(theta) - Q*sin(theta): two interpolated
#            table lookups and two multiplies per carrier per sample.
#   "polar"  A_k * cos(theta_k + phi_k), with phi_k folded into the carrier's own
#            phase accumulator as a per-sample increment -- ONE lookup, ONE multiply
#            and one 32-bit envelope add instead of two.  Algebraically identical AT
#            the rebuild instants; the difference is entirely in what happens BETWEEN
#            them.  A straight line in (I,Q) passes through the origin, so a beat
#            null is reproduced; a straight line in (A,phi) walks the magnitude from
#            one endpoint to the other while the angle sweeps, so it goes AROUND the
#            origin and fills the null in.  That is the risk section 12 says to
#            measure here rather than assume, and this is the parameter that measures
#            it.  No frequency error is introduced either way, which is why the model
#            can answer it at all (unlike the 16-bit phase question).
ENVI = os.environ.get("ENVINTERP", "rect")         # "rect" | "polar"
assert ENVI in ("rect", "polar"), "ENVINTERP must be rect or polar"
# HOW (I,Q) -> (A,phi) IS COMPUTED.  Two conversions, and the difference between
# them is the difference between a bound and a firmware:
#
#   "exact"   numpy hypot/arctan2, rounded to the state widths and optionally to
#             POLAR_ABITS/POLAR_PBITS.  This is what section 16 measured, and it is
#             deliberately an UPPER BOUND on the scheme -- no conversion error at
#             all, so the 5 dB it loses is entirely the interpolation geometry.
#             Still the default, so every figure in that section reproduces.
#   "cordic"  the integer vectoring CORDIC the C actually runs, bit for bit (see
#             to_polar_cordic).  This is what tools/host_check/ proves the firmware
#             against, so it is the conversion that has to be modelled once polar
#             is a shipping candidate rather than a question.
#
# POLAR_ABITS/POLAR_PBITS apply to "exact" only: they exist to sweep how much
# precision the conversion needs (measured: 12 bits of angle is free), which is how
# CORDIC_N below was chosen instead of guessed.
POLAR_CONV = os.environ.get("POLAR_CONV", "exact")
assert POLAR_CONV in ("exact", "cordic"), "POLAR_CONV must be exact or cordic"
POLAR_ABITS = int(os.environ.get("POLAR_ABITS", 15))
POLAR_PBITS = int(os.environ.get("POLAR_PBITS", 32))
# CORDIC iterations.  Each resolves about one bit of angle, and the sweep in docs
# section 16 measured 12 bits of angle as free (10 costs 0.2 dB, 8 costs 2.2), so 12
# is measured rather than chosen for roundness.  It is also where the atan table runs
# out at a 16-bit phase: atan(2**-13) is 1.3 phase units and atan(2**-14) rounds to 1,
# i.e. further iterations would stop turning the vector.
CORDIC_N = int(os.environ.get("CORDIC_N", 12))
# How the 11 carrier terms are summed.  This is a WIDTH parameter, not a tuning
# knob, and it matches avas_type_ty_ck.c's AVAS_TYPE_TY_CK_ACC_MODE:
#
#   "int32"  the legacy arithmetic -- what shipped up to section 14, and still the
#            path for a core that cannot reach the DSP accumulator: every term
#            truncated by TERMSHIFT before it is summed, because 11 * 2**31 does not
#            fit an int32 accumulator.  TERMSHIFT stays in this model for exactly
#            that reason; a kept mode without its proof would be worse than none.
#   "acc40"  the dsPIC 40-bit DSP accumulator: mac/msc the untruncated products,
#            ONE shift at the end, and the store saturates to int16.  Strictly
#            more accurate -- TERMSHIFT disappears -- so it is NOT bit-equal to
#            "int32" and is not meant to be.
#
# The envelope loop is deliberately not parameterised: it never truncated a term
# (its bound fits int32), so all three C modes compute the same integers there.
ACC = os.environ.get("ACC", "int32")
assert ACC in ("int32", "acc40"), "ACC must be int32 or acc40"
ROTBITS = int(os.environ.get("ROTBITS", 14))       # CARRIER=rotator state precision
# ENVELOPE STATE WIDTH, which is section 12's V3 and the ONLY variant of the three
# whose question the model can still answer (a truncated envelope is not a frequency
# error, so the dB-below metric keeps its meaning).  The accumulator carries the Q15
# value in its top 16 bits with ENVFRAC fraction bits below it, i.e. the state is
# 16+ENVFRAC bits wide: 16 is what ships (the int32 register pair), 0 is V3 -- an
# int16 state, which is what deletes one word of the two per-carrier accumulates.
#
# Section 8's stated reason for choosing int32 lives exactly here and nowhere else:
# the slope is (target - now) >> DECSHIFT, so with no fraction bits every block whose
# envelope moves less than DEC Q15 LSBs has a slope that QUANTISES.  Note the
# asymmetry, because it decides what the failure sounds like: C's >> on a signed type
# is a floor, so a rising block truncates to 0 (frozen) while a falling one truncates
# to -1 (still moves).  That is a bias towards silence, not a symmetric rounding
# error, and it is why this is a defect question before it is a quality one.
ENVFRAC = int(os.environ.get("ENVFRAC", 16))
assert 0 <= ENVFRAC <= 16, "ENVFRAC is the fraction bits BELOW the Q15 value"
# WHETHER THE SLOPE IS ROUNDED OR FLOORED.  0 is what ships and what every figure in
# the document was taken with, so it stays the default.  1 adds DEC/2 before the
# shift, which is one `add #16` per cluster per REBUILD -- i.e. 11 instructions per 32
# samples, against the four per carrier per SAMPLE that ENVFRAC=0 removes.
#
# It exists because the floor is not a symmetric error at ENVFRAC=0: a block that
# needs to rise by less than DEC gets slope 0 and does not move, while one that needs
# to fall by less than DEC gets slope -1 and moves the full DEC downwards.  The state
# therefore settles BELOW its target rather than around it, and a systematic envelope
# offset multiplies the carrier into a spurious line at the cluster centroid -- which
# is coherent, so it costs far more on the metric than its size suggests.  Rounding
# makes that offset zero-mean without widening the state.  Untouched at ENVFRAC=16,
# where the fraction bits already make the floor's residue 2**-16 of an LSB.
ENVROUND = int(os.environ.get("ENVROUND", 0))
# WHETHER THE READ-OUT IS ROUNDED OR FLOORED, which is a DIFFERENT floor from
# ENVROUND's and the reason section 20 exists.  The carrier loop takes the Q15 value
# out of the accumulator with `>> ENVFRAC` (`(int16_t)(ei >> 16)` in the C), and that
# shift is also C's floor -- so at the SHIPPED width every envelope component is
# biased down by half an LSB on every sample, uniformly, on both I and Q.  A constant
# error on the envelope is not noise: it multiplies the carrier, so it lands as a
# spurious line at the cluster's own frequency, which is exactly the quantity
# section 19 measured as "max coherent offset".
#
# ENVROUND cannot touch this.  It rounds the SLOPE, whose LSB is 2**-ENVFRAC of a Q15
# count, so at ENVFRAC=16 it moves the state by 2**-12 of an LSB and the read-out
# floor is untouched -- which is why section 19's item 4 ("ENVROUND at the shipped
# 32-bit width") was measuring the wrong floor.  0 is what ships and what every
# figure before section 20 was taken with.
ENVOUTROUND = int(os.environ.get("ENVOUTROUND", 0))
# WHETHER THE TARGET IS ROUNDED OR FLOORED -- the THIRD floor, and the other half of
# the shipped bias.  eval_cluster() sums Q30 products and returns Q15, i.e. `acc >>
# 15` (mode 0/1) or `sac A, #0` (mode 2), and both truncate.  So every envelope
# target arrives half an LSB low before the interpolator has done anything, and that
# is on top of the read-out floor above: the two are why the measured I bias is
# ~-1.0 LSB rather than ~-0.5.
#
# This one is the cheap one to fix in the shipped ACC_MODE 2, where `sac.r` rounds in
# the same instruction; in modes 0/1 it is one add per component per cluster per
# rebuild, i.e. the 22 instructions per block section 19's item 4 attributed to the
# slope.
ENVTGTROUND = int(os.environ.get("ENVTGTROUND", 0))
# Phase accumulator WIDTHS, separately for the 11 full-rate carriers and the 185
# baseband oscillators.  32 is what the firmware ships.  16 is the experiment: on a
# 16-bit core a 16-bit accumulator is one add instead of add+addc, and the
# index/fraction split stops needing the two halves joined.  The cost is frequency
# RESOLUTION -- the step must be an integer, so it is fs/2**bits for a carrier and
# (fs/DEC)/2**bits for a baseband oscillator -- which is what this parameter exists
# to measure.  Fewer phase bits also leave fewer fraction bits for the
# interpolation; that is handled automatically below and was measured to be free.
PHBITS_CAR = int(os.environ.get("PHBITS_CAR", 32))
PHBITS_BB = int(os.environ.get("PHBITS_BB", 32))
assert PHBITS_CAR - TABBITS >= 1 and PHBITS_BB - TABBITS >= 1,     "phase must carry at least the table index plus one fraction bit"
assert DEC & (DEC - 1) == 0, "DEC must be a power of two (the slope divide is a shift)"
DECSHIFT = DEC.bit_length() - 1

TABN = 1 << TABBITS
Q15 = 32767
# |Z_k| <= sum of that cluster's amplitudes; scale so the worst cluster fills Q15.
CLUS_AMPSUM = np.array([AMP[f:f + n].sum() for _, f, n in clus])
A_SCALE = Q15 / CLUS_AMPSUM.max()
# y is accumulated in these scaled units; the measured 60 s peak maps to
# PEAK_ABS*A_SCALE, and the AK engine normalises that peak to 0.9 full scale.
#
# THAT GAIN CAN EXCEED UNITY, and the Type_LB is where it does.  The two
# quantities are independent: A_SCALE is set by the WORST CLUSTER's amplitude sum
# (the bound that keeps |Z_k| inside Q15) while the numerator is the peak the whole
# sum actually reaches.  Type_TY's 11 narrow clusters make those nearly equal (peak
# 0.3858 against a worst cluster sum of 0.3758, ratio 1.026), so its gain is 0.877
# and fits Q15 with room.  The Type_LB's few fat clusters do not: at MAX_SPAN_HZ
# = 200 the worst cluster sums to 1.1839 while the 264-line sum peaks at 0.6311, so
# the gain is 1.69 -- Q15 cannot hold it, and a wider cluster makes it worse.
#
# So the gain is carried as a SHIFT plus a Q15 mantissa, which is the same
# representation the pre-gain work settled on for the same reason (an octave is
# free on this core; only the mantissa costs a multiply).  OUT_SHIFT is 0 for every
# table whose gain is below unity -- Type_TY's arithmetic is therefore untouched,
# which is what keeps its host check bit-exact and its image byte-identical.
#
# What a gain above unity brings with it is the possibility of CLIPPING, because the
# normalisation targets a 60 s peak and quasi-periodic beating keeps growing past it
# (AK's own comment on the same number).  At OUT_SHIFT > 0 the product is therefore
# clamped rather than wrapped: a wrap is a full-scale click, and the engine would
# otherwise deliver one the first time the sum exceeded its measured peak by 11 %.
_OUT_GAIN_LIN = 0.9 * Q15 / (PEAK_ABS * A_SCALE)
OUT_SHIFT = 0
while OUT_SHIFT < 15 and int(round(_OUT_GAIN_LIN / 2.0 ** OUT_SHIFT * 32768.0)) > Q15:
    OUT_SHIFT += 1
OUT_GAIN_Q15 = int(round(_OUT_GAIN_LIN / 2.0 ** OUT_SHIFT * 32768.0))


def _i32(x):
    """Emulate a 32-bit signed register."""
    return ((np.asarray(x, dtype=np.int64) + 0x80000000) & 0xFFFFFFFF) - 0x80000000


def _wrap_unsigned(x, bits):
    """Emulate a uint accumulator of `bits` width."""
    return np.asarray(x, dtype=np.int64) & ((1 << bits) - 1)


def _wrap_signed(x, bits):
    """Emulate a signed register of `bits` width (the step tables can be negative)."""
    h = 1 << (bits - 1)
    return ((np.asarray(x, dtype=np.int64) + h) & ((1 << bits) - 1)) - h


def _sar(x, n):
    """Arithmetic right shift, i.e. C's >> on a signed type: floor, not trunc."""
    return np.asarray(x, dtype=np.int64) >> n


def _slope(diff):
    """(target - now) >> DECSHIFT: the envelope slope, floored or rounded."""
    if ENVROUND:
        diff = np.asarray(diff, dtype=np.int64) + (1 << (DECSHIFT - 1))
    return _sar(diff, DECSHIFT)


def _envout(x):
    """Take the Q15 value out of the envelope accumulator: `(int16_t)(ei >> 16)`.

    Floored, like the C's shift, unless ENVOUTROUND -- in which case half an LSB is
    added first.  The firmware does NOT spend an add per sample for that: it carries
    the half permanently in the accumulator's fraction bits (added once at reset and
    once per rebuild to the target), where the floor becomes round-to-nearest for
    free.  The two are the same integers, which is what host_check checks.
    """
    if ENVOUTROUND and ENVFRAC:
        x = np.asarray(x, dtype=np.int64) + (1 << (ENVFRAC - 1))
    return _sar(x, ENVFRAC)


def _env(x):
    """Emulate the envelope accumulator register: 16+ENVFRAC bits, signed.

    At the shipped ENVFRAC=16 this is exactly _i32(); at ENVFRAC=0 it is an int16,
    and the wrap is not decoration -- V3's whole risk is what the low end does.
    """
    return _wrap_signed(x, 16 + ENVFRAC)


# Q15 sine table, one full cycle.  Entry TABN is a duplicate of 0 so the
# interpolation's tab[idx+1] needs no mask -- 2 extra bytes buys an AND.
SINTAB = np.round(Q15 * np.sin(2.0 * np.pi * np.arange(TABN + 1) / TABN)).astype(np.int64)
SINTAB[TABN] = SINTAB[0]

# The firmware stores {value, delta-to-next} PAIRS rather than values, because a
# 32-bit pair is ONE mov.d where two values are two loads plus two sign extends
# plus a 32-bit subtract.  Same arithmetic, so the interpolation below is left
# alone: s0 + ((SINTAB[idx+1] - s0) * frac >> FRACBITS) with the difference read
# from flash instead of computed.  The delta has to fit int16, which it does with
# room to spare -- the largest step of a Q15 sine sampled TABN times is
# 2*pi*32767/TABN, i.e. 402 at TABN=512.
SINDELTA = np.diff(SINTAB)
assert np.abs(SINDELTA).max() <= 32767, "sine delta does not fit int16 at TABBITS=%d" % TABBITS

_IDXSH = 32 - TABBITS
_FRSH = 32 - TABBITS - FRACBITS


def _fracbits(phbits):
    """Fraction bits actually available: whatever the phase has left below the
    index, capped at FRACBITS.  A 16-bit phase with a 512-entry table leaves 7,
    and 7 was measured identical to 16 on every figure, so narrowing the phase
    costs nothing HERE -- only in frequency resolution."""
    return min(FRACBITS, phbits - TABBITS)


def sin_q15(ph, phbits=32):
    """Q15 sine of a phase of `phbits` width, already masked to that width."""
    ph = np.asarray(ph, dtype=np.int64)
    idx = ph >> (phbits - TABBITS)
    fb = _fracbits(phbits)
    if fb == 0:
        return SINTAB[idx]
    frac = (ph >> (phbits - TABBITS - fb)) & ((1 << fb) - 1)
    s0 = SINTAB[idx]
    return s0 + _sar((SINTAB[idx + 1] - s0) * frac, fb)


def cos_q15(ph, phbits=32):
    """A quarter cycle further along the same table -- an exact index offset."""
    return sin_q15(_wrap_unsigned(np.asarray(ph, dtype=np.int64)
                                 + (1 << (phbits - 2)), phbits), phbits)


# ---------------------------------------------------------------------------
# Flash-resident derived tables (all constant, so the C computes none of these)
# ---------------------------------------------------------------------------
AMP_Q15 = np.round(AMP * A_SCALE).astype(np.int64)
BB_STEP = _wrap_signed(np.round((FRQ - FC_PER_LINE) * DEC / FS * 2.0 ** PHBITS_BB),
                       PHBITS_BB)
CAR_STEP = _wrap_unsigned(np.round(FC / FS * 2.0 ** PHBITS_CAR), PHBITS_CAR)
BB_PHA0 = _wrap_unsigned(np.round(np.mod(PHA, 2.0 * np.pi) / (2.0 * np.pi)
                                  * 2.0 ** PHBITS_BB), PHBITS_BB)
# What the integer steps actually realise, so the pitch error is reported rather
# than inferred.  A carrier error shifts a whole cluster coherently; a baseband
# error changes how that cluster's lines beat against each other, which IS the sound.
CAR_F_REAL = CAR_STEP / 2.0 ** PHBITS_CAR * FS
BB_F_REAL = BB_STEP / 2.0 ** PHBITS_BB * FS / DEC
CAR_F_ERR = np.abs(CAR_F_REAL - FC)
BB_F_ERR = np.abs(BB_F_REAL - (FRQ - FC_PER_LINE))
# CORDIC constants for the polar conversion, in the CARRIER PHASE's own units --
# which is the point of doing it here: the angle comes out as a phase accumulator
# value, so there is no radians-to-phase conversion anywhere in the firmware, and the
# table has to be regenerated per phase width exactly as the step tables are.
#
# atan(2**-i) as a fraction of a turn.  The C reads these out of the generated header
# (gen_avas_type_ty_ck_tables.py imports them from here), so there is no second copy.
CORDIC_ATAN = np.array([int(round(math.atan(2.0 ** -i) / (2.0 * np.pi)
                                  * 2.0 ** PHBITS_CAR))
                        for i in range(CORDIC_N)], dtype=np.int64)
# The vectoring CORDIC lengthens the vector by a fixed factor, so the magnitude comes
# out as K*|Z| and one multiply per cluster per rebuild takes it back.  1/K in Q15,
# with the >> 14 in to_polar_cordic undoing the input's >> 1 at the same time.
CORDIC_K = float(np.prod([math.sqrt(1.0 + 4.0 ** -i) for i in range(CORDIC_N)]))
CORDIC_KINV_Q15 = int(round((1 << 15) / CORDIC_K))
# Half a turn, the pre-rotation that puts the vector in the right half plane.
CORDIC_HALF = 1 << (PHBITS_CAR - 1)

# Rotator increments, Q14 so that 1.0 is representable.
QROT = 1 << ROTBITS
ROT_C = np.round(QROT * np.cos(2.0 * np.pi * FC / FS)).astype(np.int64)
ROT_S = np.round(QROT * np.sin(2.0 * np.pi * FC / FS)).astype(np.int64)


# ---------------------------------------------------------------------------
# The engine
# ---------------------------------------------------------------------------
def eval_cluster(bb_phase):
    """Z_k for every cluster, and advance every baseband phase by one dec step.

    zi/zq accumulate the Q30 products and shift once, which is what a 40-bit
    dsPIC accumulator does for free.  |sum| <= 32767*32768 < 2**31, so a plain
    int32 accumulator is also safe if the DSP accumulator is not used.
    """
    c = cos_q15(bb_phase, PHBITS_BB)
    s = sin_q15(bb_phase, PHBITS_BB)
    # ENVTGTROUND is `sac.r` rather than `sac`: half an LSB before the Q30 -> Q15
    # shift.  The bound still holds -- (32767*32767 + 2**14) >> 15 = 32767 -- so the
    # "int16 out is lossless" argument in the C is unchanged by it.
    rnd = (1 << 14) if ENVTGTROUND else 0
    zi = _sar(np.add.reduceat(AMP_Q15 * c, FIRST) + rnd, 15)
    zq = _sar(np.add.reduceat(AMP_Q15 * s, FIRST) + rnd, 15)
    return zi, zq, _wrap_unsigned(bb_phase + BB_STEP, PHBITS_BB)


def to_polar_cordic(zi, zq):
    """(I,Q) -> (A in Q15, phase in PHBITS_CAR units), THE WAY THE FIRMWARE DOES IT.

    A 16-bit vectoring CORDIC, CORDIC_N iterations, written so that every integer
    here is an integer avas_type_ty_ck.c also computes -- host_check compares the two
    sample for sample, so a difference in rounding is a build failure, not a slow
    drift.  Vectorised over the clusters only because there are eleven of them; the
    C loops.

    Three details that are decisions rather than arithmetic:

      * The input is halved first.  The CORDIC's own gain is K = 1.6468, so K*|Z|
        with |Z| up to 32767 does not fit int16 -- and 16-bit x/y is the entire
        reason this is affordable on this core (one asr and one add per rotation
        instead of a 32-bit shift pair).  The cost is the bottom bit of A, i.e. one
        Q15 count on a quantity whose interpolation error is measured in tens.
      * The pre-rotation is by half a turn, not a quarter: negating both components
        puts the vector in the right half plane, where the CORDIC converges, and
        adding HALF to the angle is exact at any phase width because a half turn is
        representable.  A quarter-turn scheme needs a swap and gets the sign wrong
        for one quadrant.
      * The angle needs no wrap and no atan2 quadrant logic: it is accumulated in
        phase units, where the accumulator's overflow IS the modulo.
    """
    x = _sar(np.asarray(zi, dtype=np.int64), 1)
    y = _sar(np.asarray(zq, dtype=np.int64), 1)
    neg = x < 0
    ang = np.where(neg, CORDIC_HALF, 0)
    x = np.where(neg, -x, x)
    y = np.where(neg, -y, y)
    for i in range(CORDIC_N):
        up = y >= 0                      # rotate towards y == 0
        dx = _sar(y, i)
        dy = _sar(x, i)
        x, y = (np.where(up, x + dx, x - dx),
                np.where(up, y - dy, y + dy))
        ang = ang + np.where(up, CORDIC_ATAN[i], -CORDIC_ATAN[i])
    # >> 14 not >> 15: the input was halved, so this undoes both scalings at once.
    a = _sar(x * CORDIC_KINV_Q15, 14)
    return np.minimum(a, Q15), _wrap_unsigned(ang, PHBITS_CAR)


def to_polar(zi, zq):
    """(I,Q) -> (A in Q15, phase in PHBITS_CAR units), at the target's widths.

    A is clamped to Q15: the amplitude scale guarantees |Z_k| <= 32767, but hypot of
    the two ALREADY-ROUNDED halves can land a count above it, and the C's CORDIC
    would saturate rather than wrap.  The angle needs no wrap logic -- a phase
    accumulator's modulo is its overflow, which is the same reason the carrier phase
    is unsigned here.

    POLAR_CONV picks which conversion the run uses; this one is the bound, and
    to_polar_cordic() is the firmware.
    """
    if POLAR_CONV == "cordic":
        return to_polar_cordic(zi, zq)
    a = np.round(np.hypot(np.asarray(zi, dtype=np.float64),
                          np.asarray(zq, dtype=np.float64))).astype(np.int64)
    p = np.round(np.mod(np.arctan2(np.asarray(zq, dtype=np.float64),
                                   np.asarray(zi, dtype=np.float64))
                        / (2.0 * np.pi), 1.0) * 2.0 ** PHBITS_CAR).astype(np.int64)
    if POLAR_ABITS < 15:
        sh = 15 - POLAR_ABITS
        a = (a + (1 << (sh - 1))) >> sh << sh
    if POLAR_PBITS < PHBITS_CAR:
        sh = PHBITS_CAR - POLAR_PBITS
        p = (p + (1 << (sh - 1))) >> sh << sh
    return np.minimum(a, Q15), _wrap_unsigned(p, PHBITS_CAR)


ACC_PEAK = 0
CLIP_COUNT = 0     # output samples the OUT_SHIFT clamp caught in the last run()
LAST_Q15 = None    # the raw Q15 samples of the most recent run(), for host_check
# Per-cluster envelope trace of the most recent run(), as the ENGINE saw it: the
# (I,Q) actually multiplied into the carrier at every sample, whichever coordinates
# it interpolated in.  Only populated when TRACE_ENV is set, because it is
# 2*K*n int64 and the point of the model is normally just the audio.
TRACE_ENV = False
LAST_ENV_I = None
LAST_ENV_Q = None
LAST_ENV_D = None   # per-block envelope SLOPES, for the V3 truncation question
LAST_ENV_T = None   # per-block envelope TARGETS, i.e. what the slope had to reach


def run(n=N):
    assert ENVI == "rect" or CARRIER == "table",         "ENVINTERP=polar is a property of the TABLE carrier; the rotator has no phase to fold into"
    bb_phase = BB_PHA0.copy()
    car_phase = np.zeros(K, dtype=np.int64)
    cr = np.full(K, QROT, dtype=np.int64)    # rotator state, (cos, sin) at theta=0
    ci = np.zeros(K, dtype=np.int64)

    # Reset: env at t=0, which leaves the baseband phases one dec step ahead --
    # exactly what the rebuild expects to find (the AK lookahead, unchanged).
    zi, zq, bb_phase = eval_cluster(bb_phase)
    env_i = _env(zi << ENVFRAC)
    env_q = _env(zq << ENVFRAC)
    env_di = np.zeros(K, dtype=np.int64)
    env_dq = np.zeros(K, dtype=np.int64)
    dec_count = 1

    # Polar state (inert when ENVI == "rect", and vice versa, so the per-sample
    # updates below need no branch).  amp has the same layout as env_i -- Q15 in the
    # top 16 bits, the slope's fraction in the bottom -- and step_eff is the carrier
    # step with the envelope's phase slope FOLDED IN, which is what makes phi cost no
    # add of its own.  car_nom is not state the firmware would have: it is the
    # unmodified carrier phase, kept only so TRACE_ENV can report the envelope
    # separately from the carrier it is riding on.
    amp = np.zeros(K, dtype=np.int64)
    amp_da = np.zeros(K, dtype=np.int64)
    phi_app = np.zeros(K, dtype=np.int64)     # the phi the accumulator has REACHED
    step_eff = CAR_STEP.copy()
    car_nom = np.zeros(K, dtype=np.int64)
    if ENVI == "polar":
        a0, p0 = to_polar(zi, zq)
        amp = _env(a0 << ENVFRAC)
        phi_app = p0.copy()
        # Start the accumulator AT phi, so sample 0 is A*cos(phi) == zi: the same
        # first sample rect produces, which is what makes the two comparable.
        car_phase = p0.copy()

    tr_i = np.zeros((n, K)) if TRACE_ENV else None
    tr_q = np.zeros((n, K)) if TRACE_ENV else None
    # The SLOPES, as the rebuild computed them, one row per block.  Traced because
    # V3's question ("does env_d* quantise to zero?") is about this integer and not
    # about the audio it eventually produces -- a slope of 0 with a non-zero step to
    # bridge is the defect itself, visible here before any metric is applied.
    tr_d = [] if TRACE_ENV else None
    tr_t = [] if TRACE_ENV else None

    out = np.empty(n, dtype=np.float64)
    nclip = 0
    for i in range(n):
        dec_count -= 1
        if dec_count == 0:
            dec_count = DEC
            zi, zq, bb_phase = eval_cluster(bb_phase)
            if ENVI == "rect":
                # The DIFFERENCE is taken one word wider than the state and only
                # then shifted, which is what the C does today and what a V3 C
                # would have to keep doing (32767 - -32768 does not fit an int16,
                # but the same value >> DECSHIFT does).  So V3 costs a
                # sign-extension in the REBUILD to save a word in the LOOP.
                env_di = _env(_slope((zi << ENVFRAC) - env_i))
                env_dq = _env(_slope((zq << ENVFRAC) - env_q))
            else:
                a_t, p_t = to_polar(zi, zq)
                amp_da = _env(_slope((a_t << ENVFRAC) - amp))
                # The unwrap is FREE and exact: read the difference of two phase
                # accumulators as a SIGNED value of the same width and it is already
                # the shortest way round, in (-pi, pi].  No branch, no comparison --
                # the same property that makes the phase wrap disappear.  What it
                # cannot do is know that the true rotation went the LONG way round,
                # which is the null hazard this parameter exists to measure.
                dphi = _sar(_wrap_signed(p_t - phi_app, PHBITS_CAR), DECSHIFT)
                step_eff = _wrap_unsigned(CAR_STEP + dphi, PHBITS_CAR)
                # Track what the DEC increments of dphi will actually add, not the
                # target: >> DECSHIFT truncates, and taking the next difference from
                # the reached value is what stops that truncation accumulating.
                phi_app = _wrap_unsigned(phi_app + (dphi << DECSHIFT), PHBITS_CAR)

            if TRACE_ENV:
                z = np.zeros(K, dtype=np.int64)
                tr_d.append((env_di.copy(), env_dq.copy()) if ENVI == "rect"
                            else (amp_da.copy(), z))
                # The TARGET the slope was supposed to reach, traced next to the
                # slope so "quantised to zero" can be counted exactly rather than
                # inferred from a float reference.
                tr_t.append((np.asarray(zi).copy(), np.asarray(zq).copy())
                            if ENVI == "rect" else (np.asarray(a_t).copy(), z))

        if ENVI == "rect":
            ei = _envout(env_i)
            eq = _envout(env_q)
        else:
            # ei = A, eq = 0 is not a trick: it makes the accumulation below compute
            # exactly A*cos(theta+phi) -- ONE product, the second term identically
            # zero -- so both schemes share one accumulator model and the ACC width
            # question stays answered the same way for both.
            ei = _envout(amp)
            eq = np.zeros(K, dtype=np.int64)

        ph_used = car_phase
        if CARRIER == "table":
            c = cos_q15(ph_used, PHBITS_CAR)
            # The second lookup does not exist in polar: zeroed rather than computed,
            # so the model cannot accidentally benefit from a value the C never has.
            s = (np.zeros(K, dtype=np.int64) if ENVI == "polar"
                 else sin_q15(ph_used, PHBITS_CAR))
            car_phase = _wrap_unsigned(car_phase + step_eff, PHBITS_CAR)
        else:
            # Q14 complex rotator: 4 mul + 2 add per carrier, and no table.
            c, s = _sar(cr, ROTBITS - 15), _sar(ci, ROTBITS - 15)   # -> Q15
            ncr = _i32(_sar(cr * ROT_C - ci * ROT_S, ROTBITS))
            nci = _i32(_sar(cr * ROT_S + ci * ROT_C, ROTBITS))
            cr, ci = ncr, nci

        global ACC_PEAK
        if ACC == "acc40":
            # mac/msc the untruncated products.  In fractional mode (CORCON.IF=0,
            # which is the reset state and the C runtime leaves it alone) the
            # accumulator holds TWICE the integer sum, and `sac A,#0` stores
            # A[31:16] -- so the >>15 below is the same instruction, not an extra
            # one.  ACC_PEAK is tracked in accumulator units for that reason.
            acc = int(np.sum(ei * c - eq * s))
            ACC_PEAK = max(ACC_PEAK, 2 * abs(acc))
            y = _sar(acc, 15)
            # SATDW=1: the sac store saturates.  Mode int32's output has no such
            # bound -- 11*32767 times OUT_GAIN_Q15 overflows int32 in the caller --
            # so this clamp is the safer behaviour, not a limitation.
            y = 32767 if y > 32767 else (-32768 if y < -32768 else y)
        else:
            terms = _sar(ei * c - eq * s, TERMSHIFT)
            acc = int(np.sum(terms))
            # The C accumulates in int32.  The model uses int64 and would silently
            # hide an overflow the C cannot, so assert the width TERMSHIFT was
            # chosen to guarantee.
            ACC_PEAK = max(ACC_PEAK, abs(acc), int(np.abs(terms).max()))
            y = _sar(acc, 15 - TERMSHIFT)
        # The output scaling, and the clamp that only a gain above unity needs.  At
        # OUT_SHIFT = 0 this is the single shift the engine has always done.
        o = _sar(int(y) * OUT_GAIN_Q15, 15 - OUT_SHIFT)
        if OUT_SHIFT and (o > 32767 or o < -32768):
            # Counted, not just clamped: how often the normalisation's 11 % of
            # headroom is actually spent is a measurement, and a table that clips
            # audibly is a table whose peak was measured over too short a window.
            nclip += 1
            o = 32767 if o > 32767 else -32768
        out[i] = o

        if TRACE_ENV:
            # The complex envelope the engine ACTUALLY multiplied into the carrier
            # this sample, expressed in one coordinate system for both schemes: rect
            # has it directly, polar has it as A at the angle its accumulator has
            # been pushed away from the nominal carrier.  This is the quantity whose
            # error near a null is the whole question, and it is measurable here
            # without the audio in the way.
            if ENVI == "rect":
                tr_i[i] = ei
                tr_q[i] = eq
            else:
                d = (_wrap_signed(ph_used - car_nom, PHBITS_CAR)
                     * (2.0 * np.pi / 2.0 ** PHBITS_CAR))
                tr_i[i] = ei * np.cos(d)
                tr_q[i] = ei * np.sin(d)
            car_nom = _wrap_unsigned(car_nom + CAR_STEP, PHBITS_CAR)

        env_i = _env(env_i + env_di)
        env_q = _env(env_q + env_dq)
        amp = _env(amp + amp_da)

        if CARRIER != "table" and dec_count == DEC:
            # Renormalise the rotators at each rebuild: |c|^2 should be Q14^2.
            m2 = _sar(cr * cr + ci * ci, ROTBITS)
            err = _sar(QROT - m2, 1)
            cr = _i32(cr + _sar(cr * err, ROTBITS))
            ci = _i32(ci + _sar(ci * err, ROTBITS))

    global LAST_Q15, LAST_ENV_I, LAST_ENV_Q, LAST_ENV_D, LAST_ENV_T, CLIP_COUNT
    CLIP_COUNT = nclip
    LAST_Q15 = out.astype(np.int64).copy()
    LAST_ENV_I, LAST_ENV_Q = tr_i, tr_q
    # (blocks, 2, K): [:, 0] is env_di / amp_da, [:, 1] is env_dq (0 in polar).
    LAST_ENV_D = np.array(tr_d, dtype=np.int64) if TRACE_ENV else None
    LAST_ENV_T = np.array(tr_t, dtype=np.int64) if TRACE_ENV else None
    return out / 32768.0     # Q15 sample -> -1..+1


def ideal(n=N):
    """The offline reference: float64, exact cosine, normalised as AK does."""
    t = np.arange(n) / FS
    y = (AMP[:, None] * np.cos(2 * np.pi * FRQ[:, None] * t + PHA[:, None])).sum(0)
    return y * (0.9 / PEAK_ABS)


def true_envelope(n=N):
    """The exact complex envelope, per cluster, per sample, in the engine's Q15 units.

    Z_k(t) = sum_{j in k} a_j * e^{i(2*pi*(f_j - f_k)*t + phi_j)} -- the quantity
    eval_cluster() samples once per DEC and that any interpolation of it is
    approximating.  eval_cluster() sums AMP_Q15 times a Q15 sine and shifts by 15, so
    the Q15 envelope is the amplitude sum itself: the table's Q15 scaling cancels the
    shift.  Getting that factor wrong makes the reference tiny and every error read as
    100 %, which is how it first went in the polar study.

    The time origin is the engine's own: reset evaluates Z at t = 0 and the slope
    arrives at the next target after exactly DEC samples, so env(i) approximates
    Z(i/fs) with no offset to fit.  It lives in the model rather than in a study tool
    because two studies need it now (polar, section 16; the envelope width, section
    19) and a reference with two definitions is not a reference.
    """
    t = np.arange(n) / FS
    zi = np.zeros((n, K))
    zq = np.zeros((n, K))
    for k in range(K):
        f = int(FIRST[k])
        c = int(COUNT[k])
        w = 2.0 * np.pi * (FRQ[f:f + c] - FC[k])
        ph = w[None, :] * t[:, None] + PHA[f:f + c][None, :]
        a = AMP_Q15[f:f + c].astype(np.float64)[None, :]
        zi[:, k] = (a * np.cos(ph)).sum(1)
        zq[:, k] = (a * np.sin(ph)).sum(1)
    return zi, zq


# ---------------------------------------------------------------------------
# Metrics -- same definitions as verify_c_l1_model.py so the numbers compare
# directly with the AK figures.  Absolute band levels, not ratios: bands with no
# line are numerically empty in the reference and any ratio against them blows up.
# ---------------------------------------------------------------------------
def band_levels(a, b, fs=FS, nb=18, lo_hz=100.0, hi_hz=12000.0):
    w = np.hanning(len(a))
    cal = 2.0 / np.sum(w)
    A = np.abs(np.fft.rfft(a * w)) * cal
    B = np.abs(np.fft.rfft(b * w)) * cal
    f = np.fft.rfftfreq(len(a), 1.0 / fs)
    edges = np.geomspace(lo_hz, hi_hz, nb + 1)
    out = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (f >= lo) & (f < hi)
        pa = np.sqrt(np.sum(A[m] ** 2)) + 1e-30
        pb = np.sqrt(np.sum(B[m] ** 2)) + 1e-30
        out.append((lo, hi, 20 * np.log10(pa), 20 * np.log10(pb)))
    return out


def report(label, y, idl):
    d = y - idl
    bl = band_levels(idl, y)
    occ = [e for e in bl if e[2] > -80.0]
    emp = [e for e in bl if e[2] <= -80.0]
    err = [abs(e[3] - e[2]) for e in occ]
    dead = sum(1 for e in occ if e[3] <= -80.0)
    print("  %-30s %5.1f dB below | corr %.6f | peak %.4f | "
          "occ-band(%d) err mean %.3f max %.2f dB | dead %d | line-free %.1f dBFS"
          % (label, 20 * np.log10(idl.std() / max(d.std(), 1e-30)),
             np.corrcoef(y, idl)[0, 1], np.abs(y).max(),
             len(occ), np.mean(err), max(err), dead,
             max(e[3] for e in emp) if emp else float("nan")))


if __name__ == "__main__":
    print("table %s : %d lines, %d clusters, peak_abs %.6f%s"
          % (HDR, NLINE, K, PEAK_ABS,
             "  (clustered here, MAX_SPAN_HZ=%.0f)" % MAX_SPAN_HZ
             if SOURCE_IS_LINE_LIST else "  (clusters from the AK header)"))
    print("%.3f s @ %.0f Hz (%d samples)" % (SECONDS, FS, N))
    print("fixed-point: DEC=%d TABBITS=%d(%d entries) FRACBITS=%d ENVFRAC=%d"
          "(%d-bit env state, slope=%s read-out=%s) CARRIER=%s ENVINTERP=%s%s ACC=%s%s"
          % (DEC, TABBITS, TABN, FRACBITS, ENVFRAC, 16 + ENVFRAC,
             "round" if ENVROUND else "floor",
             "round" if ENVOUTROUND else "floor", CARRIER, ENVI,
             (("(%s conv, %d iters)" % (POLAR_CONV, CORDIC_N) if POLAR_CONV == "cordic"
               else "(exact conv, A %d bit, phi %d bit)" % (POLAR_ABITS, POLAR_PBITS))
              if ENVI == "polar" else ""),
             ACC, " TERMSHIFT=%d" % TERMSHIFT if ACC == "int32" else ""))
    print("phase: car=%d bit (res %.4f Hz, worst err %.4f Hz)  bb=%d bit"
          " (res %.4f Hz, worst err %.4f Hz)  frac bits car=%d bb=%d"
          % (PHBITS_CAR, FS / 2.0 ** PHBITS_CAR, CAR_F_ERR.max(),
             PHBITS_BB, FS / DEC / 2.0 ** PHBITS_BB, BB_F_ERR.max(),
             _fracbits(PHBITS_CAR), _fracbits(PHBITS_BB)))
    print("A_SCALE=%.1f  max cluster ampsum=%.5f -> Z_max=%d  OUT_GAIN_Q15=%d"
          "  OUT_SHIFT=%d (linear gain %.4f)"
          % (A_SCALE, CLUS_AMPSUM.max(), int(round(CLUS_AMPSUM.max() * A_SCALE)),
             OUT_GAIN_Q15, OUT_SHIFT, _OUT_GAIN_LIN))
    print("amp_q15 min=%d max=%d   |bb_step| min=%d max=%d"
          % (AMP_Q15.min(), AMP_Q15.max(),
             np.abs(BB_STEP[BB_STEP != 0]).min(), np.abs(BB_STEP).max()))
    idl = ideal()
    print("ideal peak %.6f rms %.6f" % (np.abs(idl).max(), idl.std()))
    report("fixed %s/%s D=%d" % (CARRIER, ENVI, DEC), run(), idl)
    if OUT_SHIFT:
        print("  output gain %d >> %d (shift %d): %d of %d samples clamped (%.4f %%)"
              % (OUT_GAIN_Q15, 15 - OUT_SHIFT, OUT_SHIFT, CLIP_COUNT, N,
                 100.0 * CLIP_COUNT / N))
    if ACC == "acc40":
        # 40-bit accumulator, and fractional mode doubles what lands in it.  SATA=0
        # means it wraps rather than saturates, so this headroom is the real bound.
        print("  y-accumulator peak %d in ACC units (40-bit limit %d, %.2f%% used,"
              " %.1f bits spare) -- no TERMSHIFT"
              % (ACC_PEAK, 2 ** 39 - 1, 100.0 * ACC_PEAK / (2 ** 39 - 1),
                 39 - math.log2(max(ACC_PEAK, 1))))
    else:
        print("  y-accumulator peak %d (int32 limit %d, %.1f%% used) -- TERMSHIFT=%d"
              % (ACC_PEAK, 2 ** 31 - 1, 100.0 * ACC_PEAK / (2 ** 31 - 1), TERMSHIFT))
