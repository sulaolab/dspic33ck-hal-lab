# -*- coding: utf-8 -*-
"""The L3 noise bank in fixed point: the structure, the fitted gains, the measurement.

L3 is `tone + noise(mod_db=1.5)`.  The tone
part needed no new DSP -- it is the line engine with a second coefficient set -- and
this is the half that is genuinely new.  The design lives here for the same reason the
line model's does: a fixed-point filter bank fails in ways a float one cannot, and on a
speaker those failures sound like a wrong noise level rather than like a broken filter.

    python tools/avas_type_lb_noise_fixed.py [seconds] [bands]

WHAT THE OFFLINE MODEL DOES, exactly (a16_lines.py's `noise()`), because two details
decide the firmware:

  * ONE white realisation is shared by all 18 bands, brick-walled by an FFT mask per
    band, each normalised to unit rms, multiplied by its own slow gust, rescaled to
    NG[b], and SUMMED.  Shared source + disjoint brick-walls means the composite is a
    single shaped-noise signal whose spectrum is piecewise constant.
  * the rescale to NG[b] happens AFTER the gust, so NG[b] is the band's long-term rms
    and the gust only moves energy around in time.

So the firmware's job is one shaped-noise signal with the same piecewise-constant
spectrum and the same per-band movement.  A bank of bandpasses fed from one white
source is that -- except that biquad skirts are not brick walls, so the composite is
NOT the sum of the targets.  The gains are therefore FITTED (see fit_gains), and they
are fitted on the COHERENT sum |sum_b g_b H_b(f)|, not on a power sum, because one
shared source means overlapping skirts add with their phases.

WHY THE STATE-VARIABLE FORM AND NOT A DIRECT-FORM BIQUAD.  MEASURED: at fs = 48 kHz a
direct-form 2nd-order bandpass at the lowest band's 24 Hz centre needs
a1 = -1.998817, and Q14 (the widest fixed-point format that can hold |a1| < 2)
quantises it by 2.3e-5 -- which moves the pole ANGLE by 100 %, i.e. the centre
frequency is not approximately wrong, it is arbitrary.  The Chamberlin state-variable
form tunes on F = 2*sin(pi*f0/fs) instead, which at 24 Hz is 0.00314 -> 103 counts in
Q15, a 0.3 % relative error.  The trade is at the OTHER end: the SVF's F approaches its
stability limit (2 - 1/Q) near fs/4, so the top bands are where it stops being usable
and the direct form starts being fine.  Both facts are printed below rather than
asserted.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
TABLE = os.path.join(HERE, "L3_noise_type_lb.txt")

FS = 48000.0
DEC = 32                      # the engine's control rate, which the gusts run at
Q15 = 32767

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 4.0
# THE CHOSEN BAND COUNT, measured in design section 14: 13 bands (20 Hz - 2.5 kHz).
# Not 18, and the reason is not only load -- the bands above 2.5 kHz are the ones the
# bank fits WORST (the target wiggles +3.8 dB and then falls off a 12.8 dB cliff up
# there), so dropping them improves the in-band fit from 1.99 dB rms to 0.52 while
# taking the load from 187 to 119 us per block.  What it gives up is the 3.6-11 kHz
# hiss, which is 10 to 40 dB down and is a LISTENING question -- pass 17 to render the
# other candidate.  0 = every band the SVF can hold.
NBANDS = int(sys.argv[2]) if len(sys.argv) > 2 else 13

# ---------------------------------------------------------------------------
# The targets, from the file the analysis generated.
# ---------------------------------------------------------------------------
if not os.path.isfile(TABLE):
    sys.exit("%s is missing -- run tools/gen_avas_type_lb_lines.py, which measures it "
             "from the same residual a16_lines.py stage 4 does." % TABLE)
t = np.loadtxt(TABLE)
LO, HI, NG = t[:, 0], t[:, 1], t[:, 2]
NB_ALL = len(NG)
F0 = np.sqrt(LO * HI)                       # geometric centre
RATIO = float((HI[0] / LO[0]))
QQ = float(np.sqrt(RATIO) / (RATIO - 1.0))  # -3 dB edges at the band edges
NOISE_RMS = float(np.sqrt(np.sum(NG ** 2)))

# The SVF's stability bound is F < 2 - 1/Q; past about 0.8 of that the response is so
# warped that the fit would be paying for a filter that is not a bandpass any more.
F_RAW = 2.0 * np.sin(np.pi * F0 / FS)
F_LIMIT = 2.0 - 1.0 / QQ
USABLE = F_RAW < 0.8 * F_LIMIT
NB = int(NBANDS) if NBANDS else int(USABLE.sum())


def q15(x):
    """Round to Q15, the way the generated table will hold these."""
    return np.round(np.asarray(x, dtype=np.float64) * 32768.0) / 32768.0


FQ = q15(F_RAW)
QQ1 = q15(1.0 / QQ)


def svf_response(f, Fc, Q1):
    """|H_bp(z)| of the EXACT difference equations the C will run, at quantised
    coefficients -- so the fit is against the filter that ships, not its ideal.

        low  += F*band                       (band = the previous sample's)
        high  = in - low - Q1*band
        band += F*high
        H(z) = F(z-1) / [ (z-1)^2 + (F^2 + F*Q1)(z-1) + F^2 ]
    """
    z = np.exp(2j * np.pi * np.asarray(f) / FS)
    zm = z - 1.0
    return (Fc * zm) / (zm ** 2 + (Fc * Fc + Fc * Q1) * zm + Fc * Fc)


# The target spectrum as a piecewise-constant AMPLITUDE DENSITY.  NG[b] is a band rms,
# so the density inside band b is NG[b]/sqrt(bandwidth): that is what a filter's
# |H(f)| has to reproduce for the band rms to come out right, and getting this factor
# wrong is the classic way a fitted bank ends up tilted.
GRID = np.geomspace(LO[0] * 0.7, min(HI[-1], 0.49 * FS), 900)
DENS = np.zeros_like(GRID)
for b in range(NB_ALL):
    m = (GRID >= LO[b]) & (GRID < HI[b])
    DENS[m] = NG[b] / np.sqrt(HI[b] - LO[b])
# Outside the modelled range the target is zero, and the fit is not asked to chase it:
# a bandpass bank cannot make a brick wall, and weighting the stop-band would spend
# gain accuracy in the pass-band to buy skirt shape nobody hears.
#
# INSIDE the range the weight is 1/target, i.e. the fit minimises RELATIVE error --
# which is what "the band level is within N dB" means.  With absolute weighting the
# 34 dB spread between the loudest and quietest band made the loud ones the whole
# objective: three quiet bands came out of the solver at exactly zero gain, 5 to 9 dB
# low, and the reported rms was 2.78 dB while every band anyone would notice was fine.
# A metric that ignores the quiet bands is the wrong metric for a noise floor, which
# is heard as its shape and not as its energy.
W = np.where(DENS > 0, 1.0 / np.maximum(DENS, 1e-30), 0.0)

# PER-BAND ORDER.  A 2nd-order bandpass skirt falls at 6 dB/octave; cascading two
# identical SVFs doubles that for one more copy of the same three multiplies.  It is
# spent only where the TARGET demands a slope the single order cannot hold: above about
# 2 kHz this spectrum wiggles +3.8 dB and then falls off a 12.8 dB cliff between
# adjacent bands, and MEASURED with 2nd order everywhere the solver drove those bands
# to zero gain and still missed by -6.0 and +7.4 dB -- it wanted NEGATIVE gain, i.e. to
# cancel its neighbours' skirts, which is a shape problem wearing a gain problem's
# clothes.  Set it ABOVE the top centre frequency for 2nd order everywhere, or to 0 for
# 4th order everywhere -- both were measured, and 4th-order-everywhere is what costs too
# much (17 bands = 289 us/block raw, which lands the whole voice at 97-104 % with the
# tone).  The chosen point is one 4th-order band, not five and not seventeen.
ORDER4_ABOVE_HZ = float(os.environ.get("ORDER4_ABOVE_HZ", 2000.0))
ORD = np.where(F0[:NB] > ORDER4_ABOVE_HZ, 2, 1)      # exponent: 1 = 2nd, 2 = 4th order
H0 = np.array([svf_response(GRID, FQ[b], QQ1) ** ORD[b] for b in range(NB)])


def pre_response(f, a, poles):
    """The SOURCE shaping filter, cascaded one-poles: `y += (a*(x-y)) >> 15`.

    WHY THIS EXISTS, measured before it was added: a 2nd-order bandpass skirt falls at
    6 dB/octave, and this target falls FASTER than that -- from 0 dB at 987 Hz to
    -39.7 dB at 9.2 kHz is 12.4 dB/octave.  So the 820-1189 Hz band, which is the
    loudest, puts a floor over the whole top of the spectrum that no choice of gains
    can lift off: fitting white-driven bands left five gains at exactly zero and the
    composite still +27 dB too loud at 9 kHz.  A gain vector cannot fix a slope.

    Two ways out.  Cascading a second SVF per band would double the skirt AND the
    cost -- 17 more bands' worth of instructions.  Tilting the SOURCE costs 3-4
    instructions ONCE for the whole bank and gives every band's upper skirt the extra
    slope, which is what the target actually needs.  The gains then only correct the
    residual, which is what a gain vector is good at.
    """
    if poles == 0:
        return np.ones_like(np.asarray(f), dtype=np.complex128)
    z = np.exp(2j * np.pi * np.asarray(f) / FS)
    h = (a * z) / (z - (1.0 - a))
    return h ** poles


def fit_gains(H, iters=250, lam=1e-3):
    """Least squares on the COHERENT composite |sum_b g_b H_b(f)| vs the target.

    Gauss-Newton by hand: no scipy on this box, and the problem is 18 parameters with
    an analytic Jacobian, so a solver would be a dependency for nothing.
    Non-negativity is a clamp rather than a constraint -- a negative gain would mean
    the fit had decided to cancel a neighbour, which is a fit worth rejecting, not
    accommodating, and it does not happen at this Q.
    """
    # Start where each band would sit if it were alone: the level its own response
    # already gives at its own centre.
    g = np.array([DENS[np.argmin(np.abs(GRID - F0[b]))]
                  / max(abs(svf_response(F0[b], FQ[b], QQ1)), 1e-12)
                  for b in range(NB)])
    # The objective is in LOG magnitude, which is the same thing as "the band level in
    # dB" and is what makes it scale-free: a band 34 dB down then counts as much as the
    # loudest one, without a weight to tune.  Damping is per PARAMETER (Levenberg-
    # Marquardt's diagonal, not lambda*I): with a scalar damping scaled by the average
    # Jacobian, a band whose response the source tilt has already attenuated gets a
    # step of essentially zero and freezes at whatever it started from -- MEASURED, it
    # left three bands parked at zero gain and 3 to 7 dB low.
    msk = DENS > 0
    tgt = np.log(np.maximum(DENS[msk], 1e-30))
    for _ in range(iters):
        C = (g[:, None] * H).sum(0)
        mag = np.maximum(np.abs(C), 1e-30)
        r = np.log(mag[msk]) - tgt
        # d ln|C| / dg_b = Re{ conj(C) * H_b } / |C|^2
        J = np.real(np.conj(C)[None, msk] * H[:, msk]) / (mag[msk] ** 2)[None, :]
        JJ = J @ J.T
        A = JJ + lam * np.diag(np.maximum(np.diag(JJ), 1e-30))
        step = np.linalg.solve(A, -(J @ r))
        g = np.maximum(g + step, 0.0)
        # A band clamped to zero contributes nothing and the next step's gradient is
        # then computed at a point the filter can never be at.  Nudging it back to a
        # small positive value keeps every band in the objective; without this the
        # solver parks the quiet ones at zero and reports the rest.
        g[g <= 0.0] = 1e-6 * max(g.max(), 1e-12)
    return g


def band_errors(C):
    """dB error of the composite's band rms against NG, per fitted band."""
    out = []
    for b in range(NB):
        m = (GRID >= LO[b]) & (GRID < HI[b])
        got = np.sqrt(np.trapezoid(np.abs(C[m]) ** 2, GRID[m])) if m.any() else 0.0
        out.append(20 * np.log10(max(got, 1e-30) / max(NG[b], 1e-30)))
    return np.array(out)


# The source tilt is CHOSEN by search rather than assumed, over the two parameters it
# has: how many one-poles and where.  The cutoff is quantised to Q15 first, so the
# search picks among filters the firmware can actually hold.
BEST = None
for poles in (0, 1, 2):
    for fc in ([0.0] if poles == 0 else np.geomspace(150.0, 4000.0, 40)):
        a = min(q15(2.0 * np.pi * fc / FS) if poles else 0.0, 0.99)
        P = pre_response(GRID, a, poles)
        Hp = H0 * P[None, :]
        g = fit_gains(Hp)
        e = band_errors((g[:, None] * Hp).sum(0))
        rms = float(np.sqrt((e ** 2).mean()))
        if BEST is None or rms < BEST[0]:
            BEST = (rms, poles, fc, a, g, Hp)

RMS_BEST, PRE_POLES, PRE_FC, PRE_A, G, H = BEST
C = (G[:, None] * H).sum(0)
print("source tilt, chosen by search over what Q15 can hold: %d one-pole%s"
      % (PRE_POLES, "" if PRE_POLES == 1 else "s")
      + ("" if PRE_POLES == 0 else " at %.0f Hz (a = %d/32768)"
         % (PRE_FC, int(round(PRE_A * 32768)))))

print("L3 noise bank, %d of %d bands (%.1f-%.0f Hz), Q = %.3f, fs = %.0f"
      % (NB, NB_ALL, LO[0], HI[NB - 1], QQ, FS))
print("  target noise rms %.6e (all %d bands), %.2f dB of it inside the %d fitted"
      % (NOISE_RMS, NB_ALL,
         20 * np.log10(np.sqrt(np.sum(NG[:NB] ** 2)) / NOISE_RMS), NB))
print("  SVF usability: F = 2 sin(pi f0/fs) against the 2-1/Q = %.3f stability bound"
      % F_LIMIT)
for b in range(NB_ALL):
    print("    band %2d %6.0f-%6.0f Hz  f0 %7.1f  F %.5f -> Q15 %5d (%+.2f %% err)"
          "  %s%s"
          % (b, LO[b], HI[b], F0[b], F_RAW[b], int(round(F_RAW[b] * 32768)),
             100 * (FQ[b] - F_RAW[b]) / F_RAW[b],
             "usable" if USABLE[b] else "TOO HIGH for the SVF",
             "" if b < NB else "  (not fitted)"))

# ---------------------------------------------------------------------------
# The realised composite, from the FITTED gains -- checked in the frequency domain
# before any simulation, because this is the quantity section 4 said must be right
# before the ear is asked anything.
# ---------------------------------------------------------------------------
print("\nfitted gains and the composite they produce (frequency domain):")
print("  band      target dB   composite dB    err dB     gain Q15")
err_db = []
for b in range(NB):
    m = (GRID >= LO[b]) & (GRID < HI[b])
    if not m.any():
        continue
    # Band rms of the composite = sqrt(integral |C|^2 df) over the band.
    got = np.sqrt(np.trapezoid(np.abs(C[m]) ** 2, GRID[m]))
    want = NG[b]
    e = 20 * np.log10(max(got, 1e-30) / max(want, 1e-30))
    err_db.append(e)
    print("  %2d %6.0f-%6.0f  %+8.2f    %+8.2f   %+7.2f    %6d"
          % (b, LO[b], HI[b], 20 * np.log10(want / NG.max()),
             20 * np.log10(max(got, 1e-30) / NG.max()), e,
             int(round(G[b] / max(G.max(), 1e-30) * 32767))))
err_db = np.array(err_db)
if NB < NB_ALL:
    print("  ABOVE THE LAST FITTED BAND, where the target is not synthesised and the"
          " composite is whatever the skirts and the tilt leave:")
    for b in range(NB, NB_ALL):
        m = (GRID >= LO[b]) & (GRID < HI[b])
        got = (np.sqrt(np.trapezoid(np.abs(C[m]) ** 2, GRID[m])) if m.any() else 0.0)
        print("  %2d %6.0f-%6.0f  target %+7.2f dB   composite %+7.2f dB   %+7.2f dB"
              % (b, LO[b], HI[b], 20 * np.log10(NG[b] / NG.max()),
                 20 * np.log10(max(got, 1e-30) / NG.max()),
                 20 * np.log10(max(got, 1e-30) / max(NG[b], 1e-30))))
print("  band-level error: mean %+.2f dB, worst %+.2f dB, rms %.2f dB"
      % (err_db.mean(), err_db[np.argmax(np.abs(err_db))], np.sqrt((err_db ** 2).mean())))

# ---------------------------------------------------------------------------
# The integer bank, run for real: xorshift white source, Q15 coefficients, the gusts
# at the engine's control rate.  Two state widths, because the low bands are where a
# 16-bit state's own quantisation noise gets amplified by 1/F -- 318x at 24 Hz -- and
# on a NOISE generator that does not sound like distortion, it just moves the floor.
# ---------------------------------------------------------------------------
def run_bank(n, state_bits=32, seed=0x1234567, gusts=True):
    x = np.uint32(seed)
    out = np.zeros(n)
    frac = state_bits - 16                      # fraction bits below the Q15 value
    lo_s = np.zeros(NB, dtype=np.int64)
    bd_s = np.zeros(NB, dtype=np.int64)
    fq = np.round(FQ * 32768).astype(np.int64)
    q1 = int(round(QQ1 * 32768))
    gq = np.round(G / max(G.max(), 1e-30) * 32767).astype(np.int64)
    lim = 1 << (state_bits - 1)
    pa = int(round(PRE_A * 32768))
    pre = [0] * PRE_POLES
    # Gusts: a slow random walk in Q15 dB per band, updated once per DEC samples and
    # held between updates -- 18 multiplies per block, which is free next to the bank.
    rng = np.random.default_rng(12345)
    gain_mod = np.ones(NB)
    tau_blocks = FS / DEC / (2 * np.pi * 1.2)   # 1.2 Hz -> one-pole at the control rate
    a = 1.0 / max(tau_blocks, 1.0)
    walk = np.zeros(NB)
    for i in range(n):
        if i % DEC == 0 and gusts:
            # sd 1.5 dB after the one-pole: scale the drive so the steady-state sd is
            # right, which is the same sd-preserving trick slow_noise() uses.
            walk = (1 - a) * walk + np.sqrt(a * (2 - a)) * rng.standard_normal(NB)
            gain_mod = 10 ** (1.5 * walk / 20.0)
        # xorshift32, the generator sonora's Type_LB uses, so the two boards'
        # noise is comparable rather than merely similar.
        x ^= np.uint32(x << np.uint32(13))
        x ^= np.uint32(x >> np.uint32(17))
        x ^= np.uint32(x << np.uint32(5))
        w = int(np.int16(np.int32(x) >> 16))
        # The source tilt, in the same integer form the C will run: one-pole
        # `y += (a*(x-y)) >> 15`, cascaded.  Kept in a 16+15 bit state so the tilt
        # itself does not become the bank's noise floor.
        v = w << 15
        for pi in range(PRE_POLES):
            pre[pi] += (pa * (v - pre[pi])) >> 15
            v = pre[pi]
        w = v >> 15 if PRE_POLES else w
        acc = 0
        for b in range(NB):
            lo_s[b] += (fq[b] * bd_s[b]) >> 15
            hi = (w << frac) - lo_s[b] - ((q1 * bd_s[b]) >> 15)
            bd_s[b] += (fq[b] * hi) >> 15
            # wrap like the C's integer state, so an unstable band shows up as one
            lo_s[b] = ((lo_s[b] + lim) & (2 * lim - 1)) - lim
            bd_s[b] = ((bd_s[b] + lim) & (2 * lim - 1)) - lim
            acc += int(gq[b] * gain_mod[b]) * (bd_s[b] >> frac)
        out[i] = acc >> 15
    return out


N = int(round(SECONDS * FS))
print("\ninteger bank, %.1f s, %d bands, gusts on:" % (SECONDS, NB))
for bits in (32, 16):
    y = run_bank(N, state_bits=bits)
    # Measure the realised band levels the same way the target was measured: a
    # PSD-calibrated band rms.
    w = np.hanning(len(y))
    cal = np.sqrt(2.0) / (len(y) * np.sqrt(np.mean(w ** 2)))
    S = np.abs(np.fft.rfft(y * w)) * cal
    f = np.fft.rfftfreq(len(y), 1.0 / FS)
    lv = np.array([np.sqrt(np.sum(S[(f >= LO[b]) & (f < HI[b])] ** 2))
                   for b in range(NB)])
    # One free scalar: the bank's output is in Q15 counts and the target is in the
    # reference sound's units, so the SHAPE is what is being checked here.  The absolute
    # level is set once, by the generator, from the same NG the tone's AMPs share.
    k = float(np.sum(lv * NG[:NB]) / max(np.sum(lv * lv), 1e-30))
    e = 20 * np.log10(np.maximum(lv * k, 1e-30) / np.maximum(NG[:NB], 1e-30))
    print("  %2d-bit state: shape error mean %+.2f dB worst %+.2f dB rms %.2f dB"
          "   peak |y| %.0f counts, rms %.1f"
          % (bits, e.mean(), e[np.argmax(np.abs(e))], np.sqrt((e ** 2).mean()),
             np.abs(y).max(), y.std()))

# ---------------------------------------------------------------------------
# Load, in the units the rest of this design is priced in.
# ---------------------------------------------------------------------------
INSTR = 15          # per band per sample: 3 mac/shift pairs + the output accumulate
INSTR_TOTAL = INSTR * int(ORD.sum())   # a 4th-order band is two copies of the same work
US_PER_INSTR = 0.01768
us = INSTR_TOTAL * DEC * US_PER_INSTR
print("\nload, ESTIMATED on the calibration in design section 4 (%.5f us per "
      "instruction-execution):" % US_PER_INSTR)
print("  %d instructions/band/sample, %d bands (%d of them 4th order = two copies)"
      " x %d samples = %.0f us/block = %.1f %% of 667 us"
      % (INSTR, NB, int((ORD == 2).sum()), DEC, us, 100 * us / 667.0))
print("  with this repo's measured 1.28-1.44x optimism band: %.0f-%.0f us"
      " = %.1f-%.1f %%" % (1.28 * us, 1.44 * us,
                           100 * 1.28 * us / 667.0, 100 * 1.44 * us / 667.0))
print("  on top of the tone's MEASURED-calibration 275 us (41.2 %%) at span 100:"
      " %.1f-%.1f %% total" % (100 * (275 + 1.28 * us) / 667.0,
                               100 * (275 + 1.44 * us) / 667.0))
