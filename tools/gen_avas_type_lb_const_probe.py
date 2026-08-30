#!/usr/bin/env python3
"""Emit the phase-0 const probe header: the Type_LB line tables' SHAPE only.

WHY THIS EXISTS
---------------
The design's ROM budget rests on one
inferred number: a byte of `.const` data costs about 1.5 program bytes, read out
of a single map file (`.const` 0x3668 units = 6 964 words = 20 892 program bytes
for roughly 13.9 KB of content).  Every "fits / does not fit" line in that
section is that inference multiplied by a table size, and the inference alone
moves the answer by ~800 B.  So it gets measured before anything is designed
around it, which is what phase 0 is.

WHAT IT IS NOT
--------------
It is NOT the shipped Type_LB coefficient table.  The integers here are a
provisional quantisation written to be non-trivial (distinct, non-zero, sign-
varied) so nothing folds or collapses; the real table comes out of the model in
phase 2, via the generator, so that the measured design and the shipped integers
are the same numbers by construction.  What is real here is the SHAPE: the same
element types the engine already uses at its shipped widths, in the counts L3
actually needs.

HOW IT IS USED
--------------
Two sizes are built, not one.  The probe's own code and its cluster-sized arrays
are a fixed cost that a single build cannot separate from the per-line cost, so
the slope between two line counts is the answer and the intercept is the noise:

    python tools/gen_avas_type_lb_const_probe.py --lines 16
    ... build, read program bytes ...
    python tools/gen_avas_type_lb_const_probe.py --lines 32
    ... build, read program bytes ...

    program bytes per data byte = (P32 - P16) / (16 * BYTES_PER_LINE)

The sizes are small because the image is: with a fixed short -BuildId the
EV88G73A build has ~421 B of program memory left, so a 132-line probe does not
link at all (that failure is itself part of the phase-0 answer).  16 and 32 lines
both fit, and the slope between them is what the ratio question needs.

The consumer lives behind -Define AVAS_TYPE_LB_CK_CONST_PROBE in avas_type_ty_ck.c,
following the AVAS_TYPE_TY_CK_SATPROBE precedent: absent from every normal build,
and present in the tree so the number can be re-measured rather than trusted.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
PARAMS = HERE / "L3_params_type_lb.txt"
OUT = HERE.parent / "src" / "app" / "dsp" / "avas_type_lb_ck_const_probe.h"

FS = 48000.0


def read_lines(path: pathlib.Path) -> list[tuple[float, float, float]]:
    out = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        frq, amp, pha = line.split()
        out.append((float(frq), float(amp), float(pha)))
    return out


def q(value: float, lo: int, hi: int) -> int:
    return max(lo, min(hi, int(round(value))))


def rows(values: list[int], per_row: int, width: int) -> str:
    chunks = []
    for i in range(0, len(values), per_row):
        chunk = ", ".join(f"{v:>{width}}" for v in values[i:i + per_row])
        chunks.append("    " + chunk + ",")
    return "\n".join(chunks)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lines", type=int, default=264,
                    help="how many of the 264 detected lines to emit (default 264)")
    ap.add_argument("--clusters", type=int, default=4,
                    help="cluster count, MAX_SPAN_HZ=200 gives 4 (default 4)")
    ap.add_argument("--out", type=pathlib.Path, default=OUT)
    args = ap.parse_args(argv)

    lines = read_lines(PARAMS)
    if args.lines > len(lines):
        print(f"only {len(lines)} lines in {PARAMS.name}", file=sys.stderr)
        return 2
    if args.clusters < 4:
        # The consumer indexes cluster arrays with `& 3`, so that the generated
        # CODE is identical for every size and the slope carries data only.
        print("clusters must be >= 4 so the consumer's mask stays in bounds",
              file=sys.stderr)
        return 2
    if args.lines < 16:
        print("lines must be >= 16 for the same reason (`& 15`)", file=sys.stderr)
        return 2

    lines = lines[:args.lines]
    peak = max(a for _, a, _ in lines)

    amp = [q(a / peak * 29491.0, -32768, 32767) for _, a, _ in lines]
    # Baseband step: the line's offset from its cluster's carrier, in phase units.
    # Provisional -- the real split comes from the model's clustering.
    base = lines[0][0]
    step = [q((f - base) % 200.0 * 65536.0 / FS * 128.0, -32768, 32767)
            for f, _, _ in lines]
    pha = [q(p / (2.0 * math.pi) % 1.0 * 65536.0, 0, 65535) for _, _, p in lines]

    car = [q(f * 65536.0 / FS, 0, 65535)
           for f in (120.0, 300.0, 620.0, 1050.0)[:args.clusters]]
    per = args.lines // args.clusters
    first = [min(args.lines, i * per) for i in range(args.clusters)] + [args.lines]

    text = f'''/*
 * avas_type_lb_ck_const_probe.h -- GENERATED by tools/gen_avas_type_lb_const_probe.py
 * DO NOT EDIT.  Regenerate with --lines <n>.
 *
 * THIS IS NOT THE SHIPPED TYPE_LB TABLE.  It is the phase-0 measurement of
 * what `const` data costs in program bytes on this device: the element types and
 * the counts are the real ones, the integers are a provisional quantisation
 * chosen only to be non-trivial.  The shipped table comes from the model in
 * phase 2.
 *
 * Included only by a -Define AVAS_TYPE_LB_CK_CONST_PROBE build.
 *
 * lines    = {args.lines}
 * clusters = {args.clusters}
 * data bytes = {args.lines} * 6 + {args.clusters} * 2 + {args.clusters + 1} * 2
 *            = {args.lines * 6 + args.clusters * 2 + (args.clusters + 1) * 2}
 */

#ifndef AVAS_TYPE_LB_CK_CONST_PROBE_H
#define AVAS_TYPE_LB_CK_CONST_PROBE_H

#define AVAS_TYPE_LB_CK_PROBE_LINES       ({args.lines}u)
#define AVAS_TYPE_LB_CK_PROBE_CLUSTERS    ({args.clusters}u)

static const int16_t s_probe_amp_q15[AVAS_TYPE_LB_CK_PROBE_LINES] = {{
{rows(amp, 12, 6)}
}};

static const avas_type_ty_ck_bbstep_t s_probe_bb_step[AVAS_TYPE_LB_CK_PROBE_LINES] = {{
{rows(step, 12, 6)}
}};

static const avas_type_ty_ck_bbph_t s_probe_bb_pha0[AVAS_TYPE_LB_CK_PROBE_LINES] = {{
{rows(pha, 12, 6)}
}};

static const avas_type_ty_ck_carph_t s_probe_car_step[AVAS_TYPE_LB_CK_PROBE_CLUSTERS] = {{
{rows(car, 12, 6)}
}};

static const uint16_t s_probe_cluster_first[AVAS_TYPE_LB_CK_PROBE_CLUSTERS + 1u] = {{
{rows(first, 12, 6)}
}};

#endif /* AVAS_TYPE_LB_CK_CONST_PROBE_H */
'''

    args.out.write_text(text, encoding="utf-8", newline="\r\n")
    data_bytes = args.lines * 6 + args.clusters * 2 + (args.clusters + 1) * 2
    print(f"{args.out}: lines={args.lines} clusters={args.clusters} "
          f"data_bytes={data_bytes}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
