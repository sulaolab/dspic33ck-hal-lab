# -*- coding: utf-8 -*-
"""Count the instructions in a compiled loop body, from an XC-DSC `-S` listing.

Section 15 counted the shipped loop by hand, from `.L2` to `bra nz,.L2`.  This does
the same thing mechanically, because the rotator re-cost needs four counts taken the
same way rather than one count taken carefully.

Definition, stated because "instructions in the loop" has three plausible readings:
the body is every instruction from the label the backward branch targets, up to and
INCLUDING that branch.  That is section 15's convention -- its 39-row table ends with
`bra nz,.L2` -- so the numbers here are comparable to the doc's.

Usage:  python count_loop.py <file.s> [function]
"""
import re
import sys

# `bra .L2`, `bra nz,.L2`, `goto .L2`.  The condition, when present, sits between the
# mnemonic and the target with no space after the comma -- which is why this is one
# pattern with an optional group rather than the three alternatives that missed it.
BRANCH = re.compile(r"^\s+(?:bra|goto|b\w+)\s+(?:\w+\s*,\s*)?(\.L\d+)\s*$")
LABEL = re.compile(r"^(\.L\d+):")
# Everything the assembler does not turn into an instruction word.
DIRECTIVE = re.compile(r"^\s*(\.|;|#|$)")
FUNC = re.compile(r"^(\w+):")


def instrs(lines):
    """(index, text) for real instructions only."""
    out = []
    for i, ln in enumerate(lines):
        if DIRECTIVE.match(ln) or LABEL.match(ln) or FUNC.match(ln):
            continue
        out.append((i, ln.rstrip()))
    return out


def analyse(path, want=None):
    lines = open(path).read().splitlines()
    # Which function each line belongs to, so a listing with several is unambiguous.
    cur, owner = None, {}
    for i, ln in enumerate(lines):
        m = FUNC.match(ln)
        if m and not ln.startswith(".L"):
            cur = m.group(1)
        owner[i] = cur

    labels = {m.group(1): i for i, ln in enumerate(lines) for m in [LABEL.match(ln)] if m}
    body = []
    for i, ln in enumerate(lines):
        m = BRANCH.match(ln)
        if not m:
            continue
        tgt = m.group(1)
        if tgt not in labels or labels[tgt] > i:      # forward branch: not a loop
            continue
        fn = owner.get(i)
        if want and fn != want:
            continue
        n = len(instrs(lines[labels[tgt]:i + 1]))
        body.append((fn, tgt, n, labels[tgt] + 1, i + 1))
    return body, lines, owner


def main():
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else None
    body, lines, owner = analyse(path, want)
    total = {}
    for i, ln in instrs(lines):
        fn = owner.get(i)
        total[fn] = total.get(fn, 0) + 1
    for fn, n in sorted(total.items(), key=lambda x: -x[1]):
        if fn and (not want or fn == want):
            print("  %-20s whole function %3d instr" % (fn, n))
    if not body:
        print("  NO BACKWARD BRANCH -- the loop was peeled or unrolled;"
              " the whole-function count above IS the executed count")
    for fn, tgt, n, a, b in body:
        print("  %-20s loop %-5s %3d instr   (lines %d-%d)" % (fn, tgt, n, a, b))


main()
