# -*- coding: utf-8 -*-
"""Answer section 12's polar-carrier question with the MODEL, not with hardware.

The lever:

    I_k*cos(theta) - Q_k*sin(theta)  ==  A_k * cos(theta + phi_k)

halves the lookups and the multiplies in the loop that holds ~62 % of this
engine's cost, and phi_k is free because it folds into the carrier's own phase
accumulator.  The identity is exact AT the rebuild instants.  What the identity
does NOT cover is the 31 samples BETWEEN them, and that is the whole question:

  * rect  interpolates a straight line in (I,Q).  Through a beat null the chord
          passes close to the origin, so the null is reproduced.
  * polar interpolates a straight line in (A,phi).  Through a beat null the
          magnitude walks from one endpoint to the other while the angle sweeps,
          so the path goes AROUND the origin: the null is FILLED IN, and the
          angle sweep becomes a frequency excursion of up to +-fs/(2*DEC).

Section 12 says to measure that here first, because -- unlike the 16-bit phase
experiment -- polar introduces no frequency error, so the model is authoritative.

What this prints, in the order the argument needs it:
  1. the headline metrics, rect vs polar, in the same currency as every other
     figure in the doc (dB below signal, line-free band floor);
  2. the ENVELOPE error against the exact complex envelope, which removes the
     carrier and the output gain from the picture;
  3. the same error restricted to the neighbourhood of a null, which is what
     turns "polar is worse" into "polar is worse BECAUSE of nulls";
  4. per-cluster attribution, so the answer says WHICH clusters pay and whether
     a hybrid (polar for the null-free ones) is even worth costing;
  5. a sweep of the CORDIC's angle precision, which is the second gate and is
     free to measure once the first one is set up.

Run from the repo root:
    python tools/avas_type_ty_polar_study.py [seconds]
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
AK_TABLE = os.path.join(ROOT, "tools", "host_check", "ref",
                        "avas_synth_type_ty_tables.h")
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 2.0

# The shipped design point: mode 2 arithmetic (acc40), DEC/TABBITS/FRACBITS as the
# generated header pins them, which are already the model's defaults.
os.environ.setdefault("ACC", "acc40")
sys.argv = [sys.argv[0], AK_TABLE, str(SECONDS)]
sys.path.insert(0, os.path.join(ROOT, "tools"))
import avas_type_ty_fixed_model as M      # noqa: E402

N = M.N
FS = M.FS
K = M.K
DEC = M.DEC


def run_variant(envi, abits=15, pbits=32, trace=False):
    """One model run with the envelope interpolated in the given coordinates.

    The model reads these as globals inside run(), so a variant is an assignment
    rather than a re-import -- which also guarantees both variants see the exact
    same tables, scale and gain.
    """
    M.ENVI = envi
    M.POLAR_ABITS = abits
    M.POLAR_PBITS = pbits
    M.TRACE_ENV = trace
    M.ACC_PEAK = 0
    return M.run(N)


# ---------------------------------------------------------------------------
# The exact complex envelope, per cluster, per sample.
#
# Z_k(t) = sum_{j in cluster k} a_j * e^{i(2*pi*(f_j - f_k)*t + phi_j)}, in the
# same Q15 units the engine carries: the integer amplitudes summed and shifted by
# 15, which is what eval_cluster() does.  This is the reference the interpolation
# is approximating, and it is what makes the null visible at all -- the engine
# only ever samples it once per DEC.
#
# The time origin is fixed by the engine's own convention: reset evaluates Z at
# t = 0 and the slope arrives at the next target after exactly DEC samples, so
# env(i) approximates Z(i/fs) with no offset to fit.
# ---------------------------------------------------------------------------
def true_envelope():
    # Moved into the model (M.true_envelope) when section 19's envelope-width study
    # needed the same reference: one definition, two studies.  Same maths, same time
    # origin, same Q15 scaling -- this is a delegation, not a re-derivation.
    return M.true_envelope(N)


def db(x):
    return 20.0 * np.log10(max(float(x), 1e-30))


print("polar-carrier interpolation study -- %.2f s @ %.0f Hz, DEC=%d, ACC=%s"
      % (SECONDS, FS, DEC, M.ACC))
print("%d lines in %d clusters" % (M.NLINE, K))

# ---------------------------------------------------------------------------
# 1. The headline metrics, in the doc's currency.
# ---------------------------------------------------------------------------
idl = M.ideal(N)
print("\n1. HEADLINE (same metric as every other figure in the doc)")
y_rect = run_variant("rect", trace=True)
env_ri, env_rq = M.LAST_ENV_I.copy(), M.LAST_ENV_Q.copy()
M.report("rect  (ships)", y_rect, idl)
y_pol = run_variant("polar", trace=True)
env_pi, env_pq = M.LAST_ENV_I.copy(), M.LAST_ENV_Q.copy()
M.report("polar (exact A/phi)", y_pol, idl)
d_rect = y_rect - idl
d_pol = y_pol - idl
print("     rect -> polar: %+.2f dB on the error, and the error is %.2fx larger"
      % (db(d_rect.std()) - db(d_pol.std()), d_pol.std() / d_rect.std()))

# ---------------------------------------------------------------------------
# 2. and 3. The envelope error, whole-run and near a null.
# ---------------------------------------------------------------------------
zi, zq = true_envelope()
Zt = zi + 1j * zq
Zr = env_ri + 1j * env_rq
Zp = env_pi + 1j * env_pq
er = np.abs(Zr - Zt)
ep = np.abs(Zp - Zt)
mag = np.abs(Zt)

print("\n2. ENVELOPE error vs the exact Z_k(t), carrier and gain removed")
print("   (Q15 units; a cluster's |Z| runs to 32767 at full scale)")
print("   %-22s %10s %10s %10s" % ("", "rms", "max", "vs rect"))
print("   %-22s %10.1f %10.1f" % ("rect", er.std(), er.max()))
print("   %-22s %10.1f %10.1f %9.2fx" % ("polar", ep.std(), ep.max(),
                                         ep.std() / er.std()))

# A null is where the exact envelope comes close to the origin.  Threshold as a
# fraction of that cluster's own rms |Z|, per cluster, because the clusters differ
# by 20 dB in level and one global threshold would only ever find the loudest.
rms_k = np.sqrt((mag ** 2).mean(0))
near = mag < (0.05 * rms_k)[None, :]
print("\n3. THE SAME ERROR, SPLIT BY WHETHER |Z| IS NEAR A NULL"
      "  (|Z| < 5 % of that cluster's rms)")
for name, m in (("near a null", near), ("elsewhere", ~near)):
    if not m.any():
        print("   %-14s (none)" % name)
        continue
    print("   %-14s %7.3f %% of samples | rms err rect %8.1f  polar %8.1f  (%5.2fx)"
          % (name, 100.0 * m.mean(), er[m].std(), ep[m].std(),
             ep[m].std() / max(er[m].std(), 1e-30)))
# How much of the TOTAL polar error energy comes from those samples.
share = (ep[near] ** 2).sum() / (ep ** 2).sum() if near.any() else 0.0
share_r = (er[near] ** 2).sum() / (er ** 2).sum() if near.any() else 0.0
print("   share of total error ENERGY from the near-null samples:"
      "  rect %.1f %%  polar %.1f %%" % (100.0 * share_r, 100.0 * share))

# Is the null a SPECIAL CASE or the far end of a continuum?  This is the part that
# decides whether a null-detecting fallback could rescue the scheme: a penalty that
# only appears in the deepest bin can be special-cased, one that grows smoothly with
# |Z| cannot be, because there is no threshold to put the fallback behind.
print("\n3b. THE SAME ERROR, BINNED BY HOW CLOSE TO THE ORIGIN |Z| IS")
print("    %-16s %9s %9s %9s %8s" % ("|Z| / rms|Z|", "samples", "err rect",
                                     "err pol", "ratio"))
rel = mag / rms_k[None, :]
edges = [0.0, 0.05, 0.1, 0.2, 0.4, 0.7, 1.0, 10.0]
for lo, hi in zip(edges[:-1], edges[1:]):
    m = (rel >= lo) & (rel < hi)
    if not m.any():
        continue
    print("    %-16s %8.3f%% %9.1f %9.1f %7.2fx"
          % ("%.2f - %.2f" % (lo, hi), 100.0 * m.mean(), er[m].std(), ep[m].std(),
             ep[m].std() / max(er[m].std(), 1e-30)))

# ---------------------------------------------------------------------------
# 4. Per-cluster attribution, plus the two hazard counters.
# ---------------------------------------------------------------------------
# The per-block angle step the interpolator has to bridge.  Sampled exactly where
# the engine samples it -- at the rebuild instants -- and wrapped to (-pi, pi],
# which is what the signed phase difference does for free in the firmware.  A block
# whose TRUE rotation exceeds pi is one the free unwrap sends the WRONG WAY: that is
# a distinct failure from filling in the null, and it needs its own count.
idx = np.arange(0, N, DEC)
arg = np.angle(Zt[idx])
dang = np.diff(arg, axis=0)          # axis=0: per BLOCK, not across clusters
dang = (dang + np.pi) % (2.0 * np.pi) - np.pi
# The true rotation over the block, unwrapped by oversampling INSIDE the block, so
# "went the long way" is detectable rather than aliased away.
fine = np.angle(Zt)
turn = np.zeros((len(idx) - 1, K))
for b in range(len(idx) - 1):
    seg = fine[idx[b]:idx[b + 1] + 1]
    d = np.diff(seg, axis=0)
    d = (d + np.pi) % (2.0 * np.pi) - np.pi
    turn[b] = d.sum(0)

print("\n4. PER CLUSTER -- who pays, and which hazard fires")
print("   %-3s %8s %5s %8s %8s | %8s %8s %6s | %6s %6s"
      % ("k", "fc Hz", "lines", "rms|Z|", "min|Z|", "err rect", "err pol",
         "ratio", "|da|>pi/2", "wrong"))
for k in range(K):
    wrong = int(np.sum(np.abs(turn[:, k]) > np.pi))
    big = int(np.sum(np.abs(dang[:, k]) > np.pi / 2))
    print("   %-3d %8.1f %5d %8.1f %8.2f | %8.1f %8.1f %6.2fx | %6d %6d"
          % (k, M.FC[k], int(M.COUNT[k]), rms_k[k], mag[:, k].min(),
             er[:, k].std(), ep[:, k].std(),
             ep[:, k].std() / max(er[:, k].std(), 1e-30), big, wrong))
print("   'wrong' counts blocks whose TRUE rotation exceeded pi, i.e. where the"
      " free unwrap picks the short way and the envelope went the long way.")

# Attribution to the output: y = sum_k Re{e^{i theta_k} Z_k}, the theta_k are
# mutually incoherent, so each cluster contributes about half its |dZ|^2 to the
# error power.  This says whether one cluster is the whole story.
pw = (ep ** 2).mean(0)
print("   share of polar envelope error energy: " +
      "  ".join("k%d %.0f%%" % (k, 100.0 * pw[k] / pw.sum())
                for k in np.argsort(pw)[::-1][:5]))

# ---------------------------------------------------------------------------
# 5. The CORDIC's precision, which is the second gate.
# ---------------------------------------------------------------------------
print("\n5. HOW ACCURATE THE (I,Q)->(A,phi) CONVERSION HAS TO BE")
print("   (phi bits are significant bits of a turn; ~1 CORDIC iteration each)")
for pbits in (32, 16, 14, 12, 10, 8):
    y = run_variant("polar", pbits=pbits)
    d = y - idl
    print("   phi %2d bit : %5.1f dB below signal   (rect ships at %.1f dB)"
          % (pbits, db(idl.std()) - db(d.std()), db(idl.std()) - db(d_rect.std())))

# ---------------------------------------------------------------------------
# 6. Does a shorter rebuild interval buy it back?  DEC is the only knob that
# shortens the interpolation span, and it is not free: the rebuild is ~38 % of the
# load, so DEC=16 doubles that half.  Measured rather than argued.
# ---------------------------------------------------------------------------
print("\n6. WOULD A SHORTER SPAN RESCUE IT (DEC=16, i.e. 2x the rebuild cost)")
M.DEC = 16
M.DECSHIFT = 4
M.BB_STEP = M._wrap_signed(np.round((M.FRQ - M.FC_PER_LINE) * M.DEC / FS
                                    * 2.0 ** M.PHBITS_BB), M.PHBITS_BB)
for envi in ("rect", "polar"):
    y = run_variant(envi)
    d = y - idl
    print("   DEC=16 %-6s: %5.1f dB below signal" % (envi, db(idl.std()) - db(d.std())))
