#!/usr/bin/env python3
"""Per-module CK-vs-AK parity of the NORA public surface, at SIGNATURE level.

This replaces an earlier identifier-set count, which compared NAMES only and
said so. A name match is the weakest possible evidence of portability: two
families can agree on ``nora_dma_configure`` and disagree on every parameter.
What an application actually needs is that a declaration it compiles against has
the same shape on both sides, which is what section 8 asked for and what
``run_nora_contract_check.py`` already does for one module.

So this tool is that checker, generalised: it imports the SAME extractor and
re-aims its prefix normalisation at family-generic rules, rather than
reimplementing a second, subtly different parser whose disagreements with the
first would be indistinguishable from real drift.

Three numbers per module, and the third is the one section 5 could not produce:

* ``both`` -- the name exists on both sides
* ``equal`` -- ...and the normalised signature is byte-identical
* ``shape`` -- ...and it is NOT: same name, different contract. A caller that
  compiles against one family and links the other silently gets the wrong ABI,
  so this column is worse than a missing symbol, not better.

WHAT IS NOT MEASURED, stated so no row is read as stronger than it is:

1. Semantics are invisible. Identical shape says nothing about whether the two
   backends do the same thing -- that is what the hardware ledger (section 6) is
   for, and no count in this table substitutes for it.
2. Only the header files listed in MODULES are read. A module's surface is
   whatever an application may include; adding a private header here would
   inflate both sides with symbols no caller can use.

Two entries that used to sit on that list are now MEASURED, by opt-in flags,
because both were being re-derived by hand every time the compatibility question
was asked -- and a hand-run probe is exactly what a procedure cannot reproduce:

* ``--macros`` -- the ``#define`` surface. The table's declaration counts still
  exclude it (the extractor reads typedefs, externs and inlines), so a family that
  publishes a constant as a macro and the other as an accessor shows up in the main
  table only as a one-sided function. This flag is what makes that visible, and it
  is where AK's ``NORA_UART_EVENT_*`` masks and CK's virtual-pin ``NORA_PPS_RPV*``
  live -- neither is reachable from any row above.
* ``--enum-values`` -- enumerator VALUES. The main table compares member names and
  their ORDER, so a row can read "equal" while the numbers underneath disagree. Where
  the value IS the contract this decides it: ``nora_clock_source_t`` carries explicit
  codes (``PLL_1 = 0x10``). Members with no ``=`` are compared by ordinal, and
  ``(implicit 2)`` compares EQUAL to a written ``2`` -- the two spellings are one
  contract. What survives that is a real numeric disagreement, of which there are two
  kinds: ``*_INST_COUNT`` differing because the part has three instances and not four
  (the instance count restated, not a break), and a positional enum whose member sets
  differ, which shifts every later ordinal and is reported as one summary line rather
  than once per shifted member.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_nora_contract_check as probe          # noqa: E402  (needs sys.path above)

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
# The AK repo beside this one.  Tried in order so a clone under either the repo
# name or a shorter local name is found; --ak-root overrides.
def _find_ak_root():
    for name in ("dspic33ak-audio-dsp-sonora-dev",
                 "dspic33ak-audio-dsp-sonora",
                 "dsp-sonora-dev"):
        cand = os.path.abspath(os.path.join(ROOT, "..", name))
        if os.path.isdir(cand):
            return cand
    return os.path.abspath(os.path.join(ROOT, "..", "dspic33ak-audio-dsp-sonora-dev"))


DEFAULT_AK_ROOT = _find_ak_root()
DEFAULT_AK_REF = "origin/main"

# Family-generic normalisation. Longest first so no rule eats another's prefix.
# Both families spell the module prefix ``nora_`` since the rename, so these rules
# are mostly an identity mapping -- they stay because a pre-rename CK branch or an
# AK tag from before its own rename must still compare, instead of reporting every
# symbol as CK-only + AK-only.
GENERIC_PREFIX_RULES = [
    ("DSPIC33CK_", "<P>_"),
    ("DSPIC33AK_", "<P>_"),
    ("dspic33ck_", "<p>_"),
    ("dspic33ak_", "<p>_"),
    ("NORA_", "<P>_"),
    ("nora_", "<p>_"),
]

# module -> (CK headers, AK headers), relative to each repo root.
# Asymmetric lists are deliberate and are the finding, not a mistake: a header that
# exists on one side only means that side publishes a surface the other does not.
MODULES = [
    ("timer", ["src/hal_timer/nora_high_res_timer.h",
               "src/hal_timer/nora_tick_timer.h"],
              ["src/hal_timer/nora_high_res_timer.h",
               "src/hal_timer/nora_tick_timer.h"]),
    ("reset", ["src/hal_reset/nora_reset.h"],
              ["src/hal_reset/nora_reset.h"]),
    ("gpio",  ["src/hal_gpio/nora_gpio.h",
               "src/hal_gpio/nora_gpio_event.h",
               "src/hal_gpio/nora_gpio_table.h",
               "src/hal_gpio/nora_pps.h"],
              ["src/hal_gpio/nora_gpio.h",
               "src/hal_gpio/nora_gpio_event.h",
               "src/hal_gpio/nora_gpio_table.h",
               "src/hal_gpio/nora_pps.h"]),
    ("adc",   ["src/hal_adc/nora_adc.h"],
              ["src/hal_adc/nora_adc.h"]),
    ("dma",   ["src/hal_dma/nora_dma.h"],
              ["src/hal_dma/nora_dma.h"]),
    ("i2c",   ["src/hal_i2c/nora_i2c.h",
               "src/hal_i2c/nora_i2c_master.h",
               "src/hal_i2c/nora_i2c_slave.h"],
              ["src/hal_i2c/nora_i2c.h",
               "src/hal_i2c/nora_i2c_master.h",
               "src/hal_i2c/nora_i2c_slave.h"]),
    ("spi_i2s_tdm", ["src/hal_spi_i2s_tdm/nora_spi_i2s_tdm.h"],
                    ["src/hal_spi_i2s_tdm/nora_spi_i2s_tdm.h"]),
    ("uart",  ["src/hal_uart/nora_uart.h"],
              ["src/hal_uart/nora_uart.h"]),
    # clock: the portable header only. `nora_clock_device.h` stood here and no longer
    # exists -- the naming convergence renamed it to nora_clock_device_dspic33ak.h, and
    # the counterpart here is nora_clock_device_dspic33ck.h. Neither is listed, and the
    # chip tag in those names is the reason: they publish the backend's own encode/decode
    # of that family's COSC/NOSC/CLKGEN fields (four `..._device_*` functions on AK, five
    # differently-named ones here), which no portable caller may include. Listing them
    # would report a 100 % divergent module for a surface that is out of scope by
    # construction; leaving the stale path in reported a phantom missing header instead.
    ("clock", ["src/hal_clock/nora_clock.h"],
              ["src/hal_clock/nora_clock.h"]),
]


def read_local(path):
    full = os.path.join(ROOT, path)
    if not os.path.exists(full):
        return None
    with open(full, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def read_git(ak_root, ref, path):
    try:
        out = subprocess.run(["git", "-C", ak_root, "show", "%s:%s" % (ref, path)],
                             capture_output=True, check=True)
    except subprocess.CalledProcessError:
        return None
    return out.stdout.decode("utf-8", errors="replace")


def public_macros(text):
    """``{normalised name: normalised body}`` for the published ``#define`` surface.

    Comments are stripped BEFORE matching and continuations are joined, in that
    order, and both steps are load-bearing:

    * A trailing ``/* pin is High */`` is part of no contract, but it is part of the
      line -- comparing raw bodies reported NORA_GPIO_LEVEL_HIGH as a value
      divergence between two families that both say ``((nora_gpio_level_t)1)``.
      Three of the four "value diffs" in the first hand-run of this probe were that.
    * A backslash-continued body (AK's multi-line ``NORA_I2C_IRQ_ALL``) is one macro;
      matched line-by-line it compares unequal against the same macro written on one
      line, which is how this repo's copy happens to be spelled.

    Include guards are dropped: they are per-file plumbing, and their names differ by
    construction on any family whose header is named after the chip.
    """
    text = re.sub(r"\\\r?\n", " ", probe.strip_comments(text))
    out = {}
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(\([^)]*\))?[ \t]*(.*)$",
                         text, flags=re.M):
        name, params, body = m.group(1), m.group(2), m.group(3)
        if not name.startswith(("NORA_", "nora_", "DSPIC33")):
            continue
        if name.endswith("_H"):
            continue
        key = probe.normalise(name) + ("()" if params else "")
        out[key] = probe.normalise(probe.squash(body))
    return out


def enum_values(text):
    """``{enum: (any_explicit, {member: value})}`` for EVERY published enum.

    Members with no ``=`` carry their ordinal as ``(implicit N)``, so an enum is
    comparable whether or not either family spells its numbers out. An earlier cut of
    this probe kept only enums with at least one ``=``, reasoning that a purely
    positional enum is already covered by the table's name-and-order check. That was
    true but it printed a lie: ``nora_dma_addr_mode_t`` exists on both families, and
    because only CK assigns values it was reported as "CK-only enum" -- indistinguishable
    from a type AK does not have. Comparing ordinals costs nothing and says the true
    thing, so nothing is filtered now.
    """
    stripped = probe.strip_comments(text)
    out = {}
    for body, name in re.findall(r"typedef\s+enum\s*\{(.*?)\}\s*(\w+)\s*;", stripped, flags=re.S):
        members, ordinal, explicit = {}, 0, False
        for entry in probe.split_top_level(body, ","):
            m = re.match(r"\s*(\w+)\s*(?:=\s*(.+))?$", entry, flags=re.S)
            if not m:
                continue
            value = (m.group(2) or "").strip()
            if value:
                explicit = True
            members[probe.normalise(m.group(1))] = (probe.normalise(probe.squash(value))
                                                    if value else "(implicit %d)" % ordinal)
            ordinal += 1
        out[probe.normalise(name)] = (explicit, members)
    return out


def report_macros(name, ck_texts, ak_texts):
    ck, ak = {}, {}
    for t in ck_texts:
        ck.update(public_macros(t))
    for t in ak_texts:
        ak.update(public_macros(t))
    diff = [k for k in sorted(set(ck) & set(ak)) if ck[k] != ak[k]]
    ck_only, ak_only = sorted(set(ck) - set(ak)), sorted(set(ak) - set(ck))
    print("  %-12s CK %2d / AK %2d   CK-only %d  AK-only %d  value-diff %d"
          % (name, len(ck), len(ak), len(ck_only), len(ak_only), len(diff)))
    for k in ck_only:
        print("      CK-only  %s = %s" % (k, ck[k]))
    for k in ak_only:
        print("      AK-only  %s = %s" % (k, ak[k]))
    for k in diff:
        print("      VALUE    %s\n                 CK: %s\n                 AK: %s" % (k, ck[k], ak[k]))


def as_number(value):
    """The integer a member evaluates to, or None if it is not a plain literal.

    ``(implicit 2)`` and ``2`` and ``0x02`` are the SAME contract, and a probe that
    calls them a disagreement is unusable: it reported all three of CK's DMA enums as
    100 % divergent from AK's, where the only difference is that CK spells the numbers
    out and AK lets the compiler count. Expressions that are not literals (``PLL_1 |
    0x40``, a reference to another member) return None and fall back to string compare,
    which is stricter than C but never claims a false match.
    """
    m = re.match(r"^\(implicit (\d+)\)$", value)
    if m:
        return int(m.group(1))
    try:
        return int(value, 0)
    except ValueError:
        return None


def values_agree(a, b):
    na, nb = as_number(a), as_number(b)
    if na is not None and nb is not None:
        return na == nb
    return a == b


def report_enum_values(name, ck_texts, ak_texts):
    ck, ak = {}, {}
    for t in ck_texts:
        ck.update(enum_values(t))
    for t in ak_texts:
        ak.update(enum_values(t))
    for e in sorted(set(ck) & set(ak)):
        (ck_expl, ck_m), (ak_expl, ak_m) = ck[e], ak[e]
        how = ("explicit on both" if ck_expl and ak_expl else
               "positional on both" if not ck_expl and not ak_expl else
               "explicit on CK, positional on AK" if ck_expl else
               "explicit on AK, positional on CK")
        shared = set(ck_m) & set(ak_m)
        bad = sorted(k for k in shared if not values_agree(ck_m[k], ak_m[k]))

        # A positional enum whose MEMBER SETS differ has every later ordinal shifted, and
        # listing 26 shifted PPS selects restates one fact 26 times -- the fact being the
        # set difference the table above already reports. Say it once instead. This only
        # applies when NEITHER side pins a number: where the value is written down, a
        # mismatch is a contract break regardless of what else the enum contains.
        if bad and not ck_expl and not ak_expl and set(ck_m) != set(ak_m):
            print("  %-12s %s -- member SETS differ, so all ordinals after the first "
                  "difference shift" % (name, e))
            print("      %d of %d shared member(s) land on a different ordinal; not a value "
                  "contract on either side" % (len(bad), len(shared)))
            print("      (the set difference itself is the table's CK-only/AK-only count)")
            continue

        if bad:
            print("  %-12s %s -- %d of %d shared member(s) DISAGREE  [%s]"
                  % (name, e, len(bad), len(shared), how))
            for k in bad:
                print("      %-42s CK=%-22s AK=%s" % (k, ck_m[k], ak_m[k]))
        else:
            print("  %-12s %s -- all %d shared member(s) agree  [%s]"
                  % (name, e, len(shared), how))
    for e in sorted(set(ck) - set(ak)):
        print("  %-12s %s -- CK-only type, nothing to compare against" % (name, e))
    for e in sorted(set(ak) - set(ck)):
        print("  %-12s %s -- AK-only type, nothing to compare against" % (name, e))


def extract_set(texts):
    """Union the declarations of several headers of one module, on one family."""
    decls = {}
    for text in texts:
        decls.update(probe.extract(text))
    return decls


def kind_of(key):
    return key.split(" ", 1)[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ak-root", default=DEFAULT_AK_ROOT,
                    help="path to the AK repo; read-only, never checked out")
    ap.add_argument("--ak-ref", default=DEFAULT_AK_REF,
                    help="AK commit-ish to read the canonical side from")
    ap.add_argument("--module", action="append",
                    help="restrict to this module (repeatable)")
    ap.add_argument("--detail", action="append",
                    help="list every divergent declaration for this module (repeatable)")
    ap.add_argument("--macros", action="store_true",
                    help="also diff the #define surface (invisible to the table above)")
    ap.add_argument("--enum-values", action="store_true",
                    help="also compare enumerator VALUES, not just names and order")
    args = ap.parse_args()

    probe.set_prefix_rules(GENERIC_PREFIX_RULES)

    if not os.path.isdir(os.path.join(args.ak_root, ".git")):
        print("FAIL: no AK repo at %s" % args.ak_root)
        return 2
    ak_sha = subprocess.run(["git", "-C", args.ak_root, "rev-parse", args.ak_ref],
                            capture_output=True).stdout.decode().strip()
    if not ak_sha:
        print("FAIL: cannot resolve %s in %s" % (args.ak_ref, args.ak_root))
        return 2
    ak_where = subprocess.run(
        ["git", "-C", args.ak_root, "branch", "-a", "--contains", ak_sha],
        capture_output=True).stdout.decode()
    on_main = any(b.strip().lstrip("* ").endswith("main") for b in ak_where.splitlines())

    print("CK side       : %s (worktree)" % ROOT)
    print("AK side       : %s @ %s  %s"
          % (args.ak_ref, ak_sha[:12],
             "(on main)" if on_main else "(NOT on main -- a branch-only claim)"))
    print("normalisation : family-generic prefixes; NAMES and SIGNATURES, not semantics")
    print("not measured  : behaviour. The table also excludes #define surface and")
    print("                enumerator values -- add --macros / --enum-values for those")
    print()

    rows, missing_files, total_shape = [], [], 0
    macro_probes, enum_probes = [], []
    wanted = set(args.module or [m[0] for m in MODULES])
    detail = set(args.detail or [])

    for name, ck_paths, ak_paths in MODULES:
        if name not in wanted:
            continue
        ck_texts, ak_texts = [], []
        for p in ck_paths:
            t = read_local(p)
            if t is None:
                missing_files.append("CK %s" % p)
            else:
                ck_texts.append(t)
        for p in ak_paths:
            t = read_git(args.ak_root, args.ak_ref, p)
            if t is None:
                missing_files.append("AK %s@%s" % (p, args.ak_ref))
            else:
                ak_texts.append(t)

        ck, ak = extract_set(ck_texts), extract_set(ak_texts)
        both = sorted(set(ck) & set(ak))
        equal = [k for k in both if ck[k] == ak[k]]
        shape = [k for k in both if ck[k] != ak[k]]
        ck_only = sorted(set(ck) - set(ak))
        ak_only = sorted(set(ak) - set(ck))
        total_shape += len(shape)

        fns_ck = sum(1 for k in ck if kind_of(k) in ("fn", "inline"))
        fns_ak = sum(1 for k in ak if kind_of(k) in ("fn", "inline"))
        fns_both = sum(1 for k in both if kind_of(k) in ("fn", "inline"))

        rows.append((name, len(ck), len(ak), len(both), len(equal), len(shape),
                     len(ck_only), len(ak_only), fns_ck, fns_ak, fns_both))

        if args.macros:
            macro_probes.append((name, ck_texts, ak_texts))
        if args.enum_values:
            enum_probes.append((name, ck_texts, ak_texts))

        if name in detail:
            print("---- %s: divergent declarations ----" % name)
            for k in shape:
                print("  SHAPE   %s\n            CK: %s\n            AK: %s" % (k, ck[k], ak[k]))
            for k in ck_only:
                print("  CK-only %s" % k)
            for k in ak_only:
                print("  AK-only %s" % k)
            print()

    w = max(len(r[0]) for r in rows) if rows else 6
    print("| %-*s | decls CK/AK | both | equal | SHAPE | CK-only | AK-only | fns CK/AK/both |"
          % (w, "module"))
    print("|%s|---:|---:|---:|---:|---:|---:|---:|" % ("-" * (w + 2)))
    for (name, nck, nak, nboth, neq, nsh, nco, nao, fck, fak, fboth) in rows:
        print("| %-*s | %d / %d | %d | %d | **%d** | %d | %d | %d / %d / %d |"
              % (w, name, nck, nak, nboth, neq, nsh, nco, nao, fck, fak, fboth))
    print()

    if missing_files:
        print("HEADERS NOT FOUND (%d) -- a one-sided surface, or a stale path in MODULES:"
              % len(missing_files))
        for m in missing_files:
            print("  %s" % m)
        print()

    if macro_probes:
        print("---- #define surface (NOT counted in the table above) ----")
        for name, ck_texts, ak_texts in macro_probes:
            report_macros(name, ck_texts, ak_texts)
        print()

    if enum_probes:
        print("---- enumerator VALUES (explicit where written, ordinal where not) ----")
        for name, ck_texts, ak_texts in enum_probes:
            report_enum_values(name, ck_texts, ak_texts)
        print()

    print("SHAPE total: %d declaration(s) share a name and differ in signature." % total_shape)
    print("This is a MEASUREMENT, not a gate: a nonzero SHAPE count is a finding to")
    print("declare or fix, and only run_nora_contract_check.py carries per-declaration")
    print("waivers. Re-run with --detail <module> to see which ones.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
