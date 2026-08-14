# -*- coding: utf-8 -*-
"""Answer section 12's V3 question -- the 16-bit ENVELOPE -- with the model.

The lever:

    the two 32-bit envelope accumulates are 12 of the 48 instructions in the
    carrier loop, and only 4 of those 12 are the arithmetic

so halving the accumulator width is the one remaining state-width change that
attacks the loop where the cost actually is.  Section 8 rejected it in advance for
a STATED reason, and this study exists to test that exact sentence:

    "a Q15-only slope would truncate to zero for the weak clusters"

Why the model is authoritative here, unlike for the 16-bit phase: truncating an
envelope introduces no frequency error, so nothing drifts and the dB-below metric
keeps measuring arithmetic (section 12 says so explicitly, and it is the reason V3
was ordered as "a measurement, not a listen").

What this prints, in the order the argument needs it:
  1. the arithmetic of the threshold, per cluster, before any run: the slope is
     (target - now) >> DECSHIFT, so a block that moves less than DEC = 32 Q15 LSBs
     has NO representable slope once the fraction bits are gone;
  2. the slope itself, counted -- how many rebuilds quantise to zero, per cluster --
     and, because a zero slope is only a defect if it leaves something unreached,
     how far from its target the envelope is left sitting when that happens;
  3. what that does to each cluster's LEVEL, which is what "goes silent" would mean,
     and to the envelope's DC OFFSET, which is what actually costs the metric;
  4. the envelope error against the exact Z_k(t), carrier and gain removed;
  5. the headline metrics, so the answer lands in the document's own currency;
  6. all of it for ENVINTERP=polar too, because polar is a shipping candidate
     (section 18) and it changes the question in both directions: polar has ONE
     accumulator, not two, so it halves V3's prize, and its accumulator holds a
     MAGNITUDE, where a floored slope cannot cancel between two signed components.

ENVROUND is measured alongside because the floor is the mechanism, not the width:
one `add #16` per cluster per rebuild against four instructions per carrier per
sample.  If the offset is the cost, that add buys most of it back.

Run from the repo root:
    python tools/avas_type_ty_env16_study.py [seconds]
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
AK_TABLE = os.path.join(ROOT, "tools", "host_check", "ref",
                        "avas_synth_type_ty_tables.h")
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 2.0

# The shipped ARITHMETIC (acc40), but the 32-bit PHASE, and that combination is
# deliberate: section 18 established that at the shipped 16/16 phase width the
# dB-below metric measures drift rather than arithmetic and is meaningless for
# comparing schemes.  The envelope width is orthogonal to the phase width -- it
# changes no frequency -- so measuring it at 32/32 measures V3 and nothing else, and
# it keeps the one metric that would otherwise have nothing to say.
os.environ.setdefault("ACC", "acc40")
sys.argv = [sys.argv[0], AK_TABLE, str(SECONDS)]
sys.path.insert(0, os.path.join(ROOT, "tools"))
import avas_type_ty_fixed_model as M      # noqa: E402

N, FS, K, DEC = M.N, M.FS, M.K, M.DEC

# (ENVFRAC, ENVROUND).  16/floor is what shipped when this study was written, and
# 0/round -- the last row of the pairs below -- is what ships since section 25.
# 0/floor is V3 as section 8 imagined
# it; 0/round is V3 with the one instruction that makes the truncation zero-mean.
# The widths between 16 and 0 are DIAGNOSTIC only -- on a 16-bit core anything from 1
# to 15 fraction bits still occupies two words, so it costs what 16 costs and buys
# nothing.  They are here to show WHERE the failure starts, which is what separates
# "V3 is marginal" from "V3 is dead".
VARIANTS = ((16, 0), (4, 0), (2, 0), (1, 0), (0, 0), (2, 1), (1, 1), (0, 1))


def vlabel(w, r):
    return "%d%s" % (w, " round" if r else "")


def run_variant(envfrac, envround, envi, trace=False):
    """One model run at a given envelope width.  Globals, so every variant sees the
    same tables, scale and gain -- a re-import would not guarantee that."""
    M.ENVFRAC = envfrac
    M.ENVROUND = envround
    M.ENVI = envi
    M.TRACE_ENV = trace
    M.ACC_PEAK = 0
    return M.run(N)


def db(x):
    return 20.0 * np.log10(max(float(x), 1e-30))


IDX = np.arange(0, N, DEC)          # the rebuild instants
idl = M.ideal(N)
zi, zq = M.true_envelope(N)
Zt = zi + 1j * zq
mag = np.abs(Zt)
rms_k = np.sqrt((mag ** 2).mean(0))
# Energy share, which is how section 8 quantified "weak": the carriers are mutually
# incoherent, so each cluster contributes about half its |Z|^2 to the output power.
share = rms_k ** 2 / (rms_k ** 2).sum()
WEAK = np.argsort(rms_k)[:6]        # "the six weak clusters" of sections 8 and 12

print("16-bit ENVELOPE (V3) study -- %.2f s @ %.0f Hz, DEC=%d, ACC=%s, phase %d/%d"
      % (SECONDS, FS, DEC, M.ACC, M.PHBITS_CAR, M.PHBITS_BB))
print("%d lines in %d clusters;  slope = (target - now) >> %d, %d rebuilds"
      % (M.NLINE, K, M.DECSHIFT, len(IDX)))

# ---------------------------------------------------------------------------
# 1. The threshold, which is arithmetic and needs no run.
# ---------------------------------------------------------------------------
dZ = np.abs(np.diff(Zt[IDX], axis=0))       # how far the envelope moves per block
print("\n1. THE THRESHOLD, PER CLUSTER  (Q15 units; a block must move >= %d LSB to"
      " have a non-zero slope at ENVFRAC=0)" % DEC)
print("   %-3s %8s %5s %8s %7s | %8s %8s %8s | %s"
      % ("k", "fc Hz", "lines", "rms|Z|", "energy", "d|Z| med", "d|Z| p90",
         "d|Z| max", "blocks under threshold"))
for k in range(K):
    under = 100.0 * float((dZ[:, k] < DEC).mean())
    print("   %-3d %8.1f %5d %8.1f %6.2f%% | %8.1f %8.1f %8.1f | %6.1f %%%s"
          % (k, M.FC[k], int(M.COUNT[k]), rms_k[k], 100.0 * share[k],
             np.median(dZ[:, k]), np.percentile(dZ[:, k], 90), dZ[:, k].max(),
             under, "   <- weak" if k in WEAK else ""))
print("   the six weakest clusters (k=%s) hold %.2f %% of the energy between them"
      % (",".join(str(int(k)) for k in sorted(WEAK)), 100.0 * share[WEAK].sum()))
print("   NOTE the movement is per BLOCK, not per sample: a cluster whose lines beat")
print("   slowly barely moves in 32 samples even when it is LOUD, so the truncation")
print("   is not only a weak-cluster question -- which is the first correction this")
print("   study makes to section 8's sentence.")


def measure(envfrac, envround, envi):
    """Everything that needs the traces, for one variant."""
    y = run_variant(envfrac, envround, envi, trace=True)
    ti, tq = M.LAST_ENV_I, M.LAST_ENV_Q
    D, T = M.LAST_ENV_D, M.LAST_ENV_T
    if envi == "rect":
        si, sq = ti[IDX], tq[IDX]
        need = np.maximum(np.abs(T[:, 0] - si), np.abs(T[:, 1] - sq))
        zero = (D[:, 0] == 0) & (D[:, 1] == 0)
    else:
        sa = np.hypot(ti[IDX], tq[IDX])
        need = np.abs(T[:, 0] - sa)
        zero = D[:, 0] == 0
    # A zero slope with nothing to reach is correct behaviour, not a defect, so the
    # count is conditioned on there being a target left -- and the RESIDUAL is
    # reported next to it, because "frozen 1 LSB short" and "frozen 300 LSB short"
    # are the same count and different defects.
    frozen = zero & (need >= 1.0)
    res = np.where(frozen, need, np.nan)
    Ze = ti + 1j * tq
    return dict(y=y, frozen=frozen, res=res, err=np.abs(Ze - Zt),
                off=np.abs((Ze - Zt).mean(0)),          # the COHERENT part, per cluster
                lvl=np.sqrt((np.abs(Ze) ** 2).mean(0)),
                dead=(np.abs(Ze) < 0.5).mean(0))


def cols(fmt, vals):
    return " ".join(fmt % v for v in vals)


def study(envi):
    print("\n" + "=" * 78)
    print("ENVINTERP=%s  --  %s" % (envi, "two accumulators, I and Q" if envi == "rect"
                                    else "ONE accumulator, the magnitude A"))
    print("=" * 78)
    rows = [((w, r), measure(w, r, envi)) for w, r in VARIANTS]
    head = "   %-9s %s" % ("ENVFRAC", cols("%6s", ["k%d" % k for k in range(K)]))

    print("\n2. SLOPE QUANTISED TO ZERO WITH A TARGET STILL TO REACH  (%% of rebuilds)")
    print(head)
    for (w, r), m in rows:
        print("   %-9s %s" % (vlabel(w, r),
                              cols("%5.1f%%", [100.0 * m["frozen"][:, k].mean()
                                               for k in range(K)])))
    print("\n   ...and the RESIDUAL left unreached on those rebuilds"
          " (median / max, Q15 LSB):")
    print(head)
    for (w, r), m in rows:
        cell = []
        for k in range(K):
            c = m["res"][:, k]
            cell.append("--" if np.all(np.isnan(c))
                        else "%.0f/%.0f" % (np.nanmedian(c), np.nanmax(c)))
        print("   %-9s %s" % (vlabel(w, r), cols("%6s", cell)))

    print("\n3. LEVEL, and the COHERENT OFFSET  (dB and Q15 LSB, vs the exact Z)")
    print(head)
    for (w, r), m in rows:
        print("   %-9s %s" % (vlabel(w, r),
                              cols("%+6.2f", [db(m["lvl"][k]) - db(rms_k[k])
                                              for k in range(K)])))
    print("   |mean(env - Z)| per cluster -- the part that lands as a spurious LINE:")
    print(head)
    for (w, r), m in rows:
        print("   %-9s %s" % (vlabel(w, r), cols("%6.2f", [m["off"][k]
                                                          for k in range(K)])))
    if max(r[1]["dead"].max() for r in rows) <= 0.0005:
        print("   no cluster's envelope is ever exactly zero at any width:"
              " nothing goes silent in the literal sense section 8 feared")
    else:
        print("   %% of SAMPLES with an envelope of exactly zero:")
        print(head)
        for (w, r), m in rows:
            print("   %-9s %s" % (vlabel(w, r), cols("%5.1f%%",
                                                     [100.0 * m["dead"][k]
                                                      for k in range(K)])))

    print("\n4. ENVELOPE ERROR vs the exact Z_k(t), carrier and output gain removed")
    print("   %-9s %9s %9s %9s | %9s"
          % ("ENVFRAC", "err rms", "err max", "off max", "dB below"))
    for (w, r), m in rows:
        d = m["y"] - idl
        print("   %-9s %9.2f %9.1f %9.2f | %9.1f"
              % (vlabel(w, r), m["err"].std(), m["err"].max(), m["off"].max(),
                 db(idl.std()) - db(d.std())))

    print("\n5. THE HEADLINE, in the document's own currency")
    for (w, r), m in rows:
        M.report("ENVFRAC=%-8s %s" % (vlabel(w, r), envi), m["y"], idl)
    return dict(rows)


rows_rect = study("rect")
rows_polar = study("polar")

# ---------------------------------------------------------------------------
# The verdict, computed rather than asserted.  V3 was ordered as a DEFECT question,
# so the criterion is whether a cluster loses level -- not whether the total metric
# moves, and not whether a slope is ever zero.
# ---------------------------------------------------------------------------
print("\n" + "=" * 78)
print("VERDICT (computed)")
for name, rows in (("rect", rows_rect), ("polar", rows_polar)):
    ref = rows[(16, 0)]
    for key in ((0, 0), (0, 1)):
        m = rows[key]
        drops = [db(m["lvl"][k]) - db(rms_k[k]) for k in range(K)]
        worst = int(np.argmin(drops))
        dref = db(idl.std()) - db((ref["y"] - idl).std())
        dnow = db(idl.std()) - db((m["y"] - idl).std())
        print("  %-5s %-7s worst cluster k%-2d %+.2f dB (ENVFRAC=16: %+.2f dB) |"
              " offset %.2f -> %.2f LSB | metric %.1f -> %.1f dB (%+.1f)"
              % (name, vlabel(*key), worst, drops[worst],
                 db(ref["lvl"][worst]) - db(rms_k[worst]),
                 ref["off"].max(), m["off"].max(), dref, dnow, dnow - dref))
