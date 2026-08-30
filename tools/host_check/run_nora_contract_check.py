# -*- coding: utf-8 -*-
"""Diff the CK transport HAL's PUBLIC DECLARATIONS against the canonical NORA contract.

Run from the repo root:
    python tools/host_check/run_nora_contract_check.py
    python tools/host_check/run_nora_contract_check.py --verbose
    python tools/host_check/run_nora_contract_check.py --canonical-header <path-to-nora_spi_i2s_tdm.h>
    python tools/host_check/run_nora_contract_check.py --canonical-header <path> --update

WHAT THIS CHECKS, AND WHAT IT CANNOT
------------------------------------
It checks SHAPE ONLY.  It normalises the module prefix away (``nora_spi_i2s_tdm_``
-> ``<p>_spi_i2s_tdm_``, ``NORA_TDM_`` -> ``<P>_TDM_``, ...) and then compares the
two headers' declaration sets: type
names, enumerator names and order, struct field names and types, and function
signatures.  A green run means "the declarations match, modulo an explicitly
declared divergence list".

A green run does NOT mean CK implements the canonical contract.  The alignment
plan splits the work deliberately:

    phase 1  declarations / signatures      <- this check starts passing HERE
    phase 2  failure semantics for set_port / open / close   <- DONE
    phase 3  configure-ownership mode semantics              <- DONE
    phase 4  SYSTEM / sync-domain API                        <- DONE

Phase 4 removed the last phase-numbered waiver from this file, so a green run no
longer has any "not yet implemented" excuse in it.  That makes over-reading it
EASIER, not harder: the shape is now complete while several behaviours remain
unobserved on silicon.  Plan section 15 is the register of those -- read it
before treating this output as evidence of anything beyond declarations.  In
particular the arm/go split's whole purpose (two legs latching the same FS edge)
is section 15 D-1, UNPROVEN, and unobservable on the only board that has run this
code.

After phase 2 the open-state gates ARE live (``set_port`` / ``inst_configure``
reject while open, ``close`` rejects while running, ``inst_start`` rejects while
closed, and ``open()`` derives its role from the committed leg), so
``ERR_NOT_OPEN`` and ``ERR_ALREADY_OPEN`` are reachable.  Phase 3 makes
``ERR_CONFIG_MODE`` reachable too: the ``inst_*`` family is now SINGLE-mode and
PRIMARY-only.  This tool cannot see THAT at all -- the mode is a file-static in
the .c, deliberately not queryable, so nothing in any header changes when the
gate is added or removed.  The only state that proves the gate is load-bearing is
mode == NONE, which exists solely before the first ``inst_configure()`` (close()
does not reset the mode), so it is observable only from a probe run during init --
``*tl(virgin)`` on hardware.  This tool cannot see any of that either way -- a bool that is always true has the same shape as one that is
load-bearing, which is why the gates were verified on HARDWARE (*tl, plan
section 12) and not here.  It therefore prints "SHAPE MATCHES" and never the
word "aligned", because reporting "aligned" would be the exact overstatement
plan section 9.1 warns against.

Two further scope limits, stated so a green run is not over-read:
  * PREPROCESSOR TEXT IS NOT COMPARED.  The device-identity adapter
    (``__dsPIC33CK256MP508__`` / ``#error``) and the conf.h macro surface are
    out of scope; section 6.2 already lists the device adapter as a permanent
    header-identity blocker.
  * COMMENTS ARE NOT COMPARED.  Contract prose is reviewed by humans; this
    tool would otherwise fail on every wording difference.

THE BASELINE
------------
The canonical side is a VENDORED SNAPSHOT (``nora_canonical_contract.txt``),
extracted from the reference commit recorded in the plan.  Vendoring is
deliberate: the check must run in a clone that has no sonora checkout, and the
snapshot pins WHICH canonical contract CK claims to match -- so a later Sonora
change shows up as a reviewed baseline update rather than as silent drift on
either side.  Refresh it with ``--canonical-header <path> --update``; the diff
that produces is the Sonora-side change, and it wants reading, not rubber-
stamping.

DECLARED DIVERGENCES
--------------------
Everything CK legitimately does differently lives in ``WAIVERS`` below, each
with a reason and the phase that retires it (or ``permanent``).  A waiver that
stops matching reality is itself an error: the check reports UNUSED waivers, so
finishing phase 2 forces the phase-2 waivers to be deleted rather than left to
rot.  Adding a waiver is how a divergence gets declared -- that is the whole
mechanism section 6.2 asks for ("fail on undeclared drift").
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CK_HEADER = os.path.join(ROOT, "src", "hal_spi_i2s_tdm", "nora_spi_i2s_tdm.h")
BASELINE = os.path.join(HERE, "nora_canonical_contract.txt")

# The reference commit the vendored baseline was taken from (plan section 0).
#
# RE-PINNED to sonora main. The provenance warning that stood here -- "3955b78 lives
# only on design/nora-dma-tdm-contract, so every 'matches canonical' statement is about
# a design branch, not published AK" -- is retired: 3955b78 is now contained in
# origin/main, and main's header differs from it in COMMENTS ONLY (dsPIC33A -> dsPIC33AK
# wording, one filename). Re-extracting the baseline from 9f9d380 produced a zero-byte
# diff, which is the evidence that this re-pin changes provenance and not contract.
# Sonora main is the reference for the whole fleet now; there is no starter-side or
# branch-side canonical left to choose between.
CANONICAL_REF = ("sonora 9f9d380f1bdb (main -- \"NORA HAL becomes a superset\"); "
                 "baseline byte-identical to the earlier 3955b78 pin")

# ---------------------------------------------------------------------------
# Prefix normalisation.  Longest first so no rule eats another's prefix.
# ---------------------------------------------------------------------------
# Since the CK rename both sides carry ``nora_``, so the NORA rules below are an
# identity mapping in practice.  They stay because the neutral ``<p>`` form is
# what the vendored baseline is stored in, and because the pre-rename CK prefixes
# must keep normalising -- a baseline or a branch from before the rename still
# compares, instead of reporting every symbol as MISSING+EXTRA.
PREFIX_RULES = [
    ("dspic33ck_spi_i2s_tdm_", "<p>_spi_i2s_tdm_"),
    ("DSPIC33CK_SPI_I2S_TDM_", "<P>_SPI_I2S_TDM_"),
    ("nora_spi_i2s_tdm_", "<p>_spi_i2s_tdm_"),
    ("NORA_SPI_I2S_TDM_", "<P>_SPI_I2S_TDM_"),
    ("dspic33ck_tdm_", "<p>_tdm_"),
    ("DSPIC33CK_TDM_", "<P>_TDM_"),
    ("nora_tdm_", "<p>_tdm_"),
    ("NORA_TDM_", "<P>_TDM_"),
]


def set_prefix_rules(rules):
    """Swap the normalisation table.

    ``normalise`` reads PREFIX_RULES at call time, so this re-aims the whole
    extractor without threading a parameter through it. nora_parity_matrix.py uses
    it to reuse extract() across every module with family-generic rules; nothing in
    this file calls it, so the TDM contract check is unaffected.
    """
    global PREFIX_RULES
    PREFIX_RULES = rules


def normalise(text):
    for src, dst in PREFIX_RULES:
        text = text.replace(src, dst)
    return text


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def squash(text):
    return re.sub(r"\s+", " ", text).strip()


def flatten_to_statements(text):
    """Reduce a header to top-level `;`-separated statements.

    Order matters and each step earns its place:

    1. Preprocessor lines go first (with backslash continuations). Otherwise a
       `#if`-guarded prototype ends up in the same `;`-segment as the directive.
    2. ``extern "C" {`` is removed BEFORE brace blocks are, because it wraps the
       whole API: treated as a brace block it would delete every declaration in the
       file. This was measured -- it is why the parity matrix first reported zero
       functions for reset/timer/i2c/uart/clock, all of which use the wrapper.
    3. Brace blocks, innermost first, leaving a `;` behind. The `;` matters: a
       removed inline-function body would otherwise let the signature above it run
       into the next declaration as one statement.
    4. The now-unmatched closing brace of the wrapper removed in step 2.
    """
    # `\\[\s\S]` and not `\\.`: without re.S a dot does not match the newline, so a
    # backslash-continued directive stopped AT the backslash and its remaining lines
    # stayed in the text. Measured defect: AK's multi-line NORA_I2C_IRQ_ALL left an
    # unbalanced `)` in front of nora_i2c_irq_enable, which then failed the prototype
    # regex -- and the parity matrix reported the function as CK-only when both
    # families declare it identically. A false gap is worse than a missed one: it gets
    # "fixed" by adding a symbol that is already there.
    text = re.sub(r"^[ \t]*#(?:[^\n\\]|\\[\s\S])*", " ", text, flags=re.M)
    text = re.sub(r'extern\s*"C"\s*\{', " ; ", text)
    prev = None
    while prev != text:
        prev, text = text, re.sub(r"\{[^{}]*\}", " ; ", text)
    return text.replace("}", " ")


def drop_param_names(params):
    """Keep parameter TYPES, drop parameter names -- a name is not the contract.

    'const nora_x_t* port' -> 'const nora_x_t*';  'void' -> 'void';
    'bool (*hook)( nora_y_t role )' -> 'bool(*)(nora_y_t)'.
    """
    params = squash(params)
    if params in ("", "void"):
        return "void"
    out = []
    for raw in split_top_level(params, ","):
        p = squash(raw)
        fn = re.match(r"^(.*?)\(\s*\*\s*\w*\s*\)\s*\((.*)\)$", p)
        if fn:                                   # function-pointer parameter
            out.append("%s(*)(%s)" % (squash(fn.group(1)), drop_param_names(fn.group(2))))
            continue
        # Trailing identifier is the parameter name unless the whole thing is one token
        m = re.match(r"^(.*?[\w\*\s])(\w+)\s*(\[\s*\w*\s*\])?$", p)
        if m and re.search(r"[\w\*]\s*$", m.group(1)) and len(p.split()) > 1:
            t = squash(m.group(1)) + (m.group(3) or "")
        else:
            t = p
        out.append(re.sub(r"\s+\*", "*", t))
    return ",".join(out)


def split_top_level(text, sep):
    """Split on `sep` that is not nested inside parentheses."""
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    return [p for p in parts if p.strip()]


def extract(header_text):
    """Return {normalised-key: shape-string} for one header's public declarations."""
    text = strip_comments(header_text)
    decls = {}

    # typedef enum { A, B = 1, } name;   -- enumerator names and their order are contract
    for body, name in re.findall(r"typedef\s+enum\s*\{(.*?)\}\s*(\w+)\s*;", text, flags=re.S):
        names = []
        for entry in split_top_level(body, ","):
            m = re.match(r"\s*(\w+)", entry)
            if m:
                names.append(m.group(1))
        decls["enum " + normalise(name)] = normalise(", ".join(names))

    # typedef struct { ... } name;   -- field names and types are contract
    #
    # Kind is "type", not "struct", so that a name whose LAYOUT is per-family still compares
    # as ONE declaration against the other family. nora_tdm_slot_t is exactly that case: AK
    # spells it `typedef int32_t`, CK a 4-byte wire struct, and candidate A's whole claim is
    # that the NAME is shared. Keying it by C construct instead would report MISSING+EXTRA and
    # so would stay green if either side dropped or renamed the type -- the drift the shared
    # vocabulary exists to catch.
    for body, name in re.findall(r"typedef\s+struct\s*\{(.*?)\}\s*(\w+)\s*;", text, flags=re.S):
        fields = []
        for member in body.split(";"):
            m = squash(member)
            if not m:
                continue
            fn = re.match(r"^(.*?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$", m)
            if fn:                               # function-pointer member
                fields.append("%s %s(%s)" % (squash(fn.group(1)), fn.group(2),
                                             drop_param_names(fn.group(3))))
                continue
            parts = m.rsplit(" ", 1)
            if len(parts) == 2:
                fields.append("%s %s" % (re.sub(r"\s+\*", "*", squash(parts[0])), parts[1]))
            else:
                fields.append(m)
        decls["type " + normalise(name)] = normalise("; ".join(fields))

    # typedef <scalar> name;   -- same "type" kind as the struct typedefs above, so a
    # per-family layout of a shared name lands on one key. The struct/enum/union guard keeps
    # `typedef struct tag name;` in the opaque rule below where it belongs.
    for base, name in re.findall(r"typedef\s+((?:const\s+)?\w+(?:\s+\w+)?(?:\s*\*)*)\s+(\w+)\s*;",
                                 text):
        if re.match(r"^(?:const\s+)?(?:struct|enum|union)\b", base):
            continue
        decls["type " + normalise(name)] = normalise(re.sub(r"\s+\*", "*", squash(base)))

    # typedef struct tag name;   -- the opaque instance handle
    for tag, name in re.findall(r"typedef\s+struct\s+(\w+)\s+(\w+)\s*;", text):
        decls["opaque " + normalise(name)] = "opaque struct"

    # typedef ret (*name)( params );   -- the block callback
    for ret, name, params in re.findall(r"typedef\s+([\w\s\*]+?)\(\s*\*\s*(\w+)\s*\)\s*\((.*?)\)\s*;",
                                        text, flags=re.S):
        decls["callback " + normalise(name)] = normalise(
            "%s (*)(%s)" % (re.sub(r"\s+\*", "*", squash(ret)), drop_param_names(params)))

    # extern ret name( params );
    for ret, name, params in re.findall(r"\bextern\s+([\w\s\*]+?)\b(\w+)\s*\((.*?)\)\s*;",
                                        text, flags=re.S):
        decls["fn " + normalise(name)] = normalise(
            "%s(%s)" % (re.sub(r"\s+\*", "*", squash(ret)), drop_param_names(params)))

    # ret name( params );   -- a prototype WITHOUT `extern`, and the C++ wrapper case.
    #
    # The transport header externs every function, so this rule finds nothing there and the
    # contract check is unchanged by it. Every other module in the fleet declares plain
    # prototypes, and without this rule nora_parity_matrix.py reported "0 functions" for eight
    # of nine modules -- a parity table that was measuring only typedefs while looking like it
    # measured the API. flatten_to_statements() is what keeps struct MEMBERS and inline BODIES
    # out of this rule; the remaining `static`/`typedef` segments are skipped because the rules
    # above already own them.
    for stmt in flatten_to_statements(text).split(";"):
        s = squash(stmt)
        if not s or s.startswith(("typedef", "static", "_Static_assert")):
            continue
        m = re.match(r"^(?:extern\s+)?([\w\s\*]+?)\b(\w+)\s*\((.*)\)$", s, flags=re.S)
        if not m:
            continue
        ret, name, params = m.group(1), m.group(2), m.group(3)
        if not squash(ret):                      # a bare `name(...)` is a macro use, not a decl
            continue
        decls["fn " + normalise(name)] = normalise(
            "%s(%s)" % (re.sub(r"\s+\*", "*", squash(ret)), drop_param_names(params)))

    # extern <type> name;   -- a published DATA object, not a function.
    #
    # This rule was missing, and its absence was silent rather than noisy: the docstring
    # claimed the extractor read "typedefs, externs and inlines", but the extern rule above
    # requires a `(` and so only ever matched functions. hal_gpio's four shared pin
    # descriptions (`extern const nora_gpio_config_t nora_gpio_cfg_output_low;` and
    # friends) were therefore absent from every count, which understated one family's
    # surface by four declarations while the table read as complete. A missing declaration
    # is the failure mode that makes a parity table dangerous in the safe-looking
    # direction, so it is worth a rule of its own.
    #
    # Arrays keep their extent (`[8]`) in the recorded shape, because a caller compiling
    # against `[8]` and linking `[4]` is exactly the kind of mismatch this tool exists to
    # catch. Anything containing `(` is left to the function rules above.
    for stmt in flatten_to_statements(text).split(";"):
        s = squash(stmt)
        if not s.startswith("extern ") or "(" in s:
            continue
        m = re.match(r"^extern\s+(.+?)\b(\w+)\s*((?:\[[^\]]*\])*)$", s, flags=re.S)
        if not m:
            continue
        type_, name, extent = m.group(1), m.group(2), m.group(3)
        if not squash(type_):
            continue
        decls["data " + normalise(name)] = normalise(
            re.sub(r"\s+\*", "*", squash(type_)) + squash(extent))

    # static inline ret name( params ) { ... }   -- header-published inline helpers
    for ret, name, params in re.findall(
            r"static\s+inline\s+(?:__attribute__\(\(.*?\)\)\s*)?([\w\s\*]+?)\b(\w+)\s*\((.*?)\)\s*\{",
            text, flags=re.S):
        decls["inline " + normalise(name)] = normalise(
            "%s(%s)" % (re.sub(r"\s+\*", "*", squash(ret)), drop_param_names(params)))

    return decls


# ---------------------------------------------------------------------------
# DECLARED DIVERGENCES.
#   key   = (kind, normalised-key)   kind in {MISSING, EXTRA, CHANGED}
#   value = (phase, reason)
# 'permanent' means it is never coming back into alignment; a phase number means
# the waiver must be DELETED when that phase lands.
# ---------------------------------------------------------------------------
WAIVERS = {
    # -- The one payload divergence left, and it is now a LAYOUT difference under a SHARED
    # name rather than a disagreement about the interface (plan section 5.5 -> candidate A).
    #
    # NOTE: the three CHANGED waivers that stood here (block_cb_t, inst_tx_fill_ptr,
    # inst_tx_fill_mirror) and the three EXTRA waivers for the slot inline helpers
    # (_encode_s32, _decode_s32, _scale_q15) were DELETED when the baseline was re-pinned to
    # an AK commit that carries candidate A: AK adopted nora_tdm_slot_t and all three inlines,
    # so those six declarations now compare byte-equal after prefix normalisation and the
    # waivers had become stale. That is the table working as designed -- a waiver that stops
    # matching reality is a check failure, not a comment to leave rotting.
    ("CHANGED", "type <p>_tdm_slot_t"): (
        "permanent",
        "The buffer element type -- candidate A's declared per-family LAYOUT, not a shape "
        "divergence. Same name on both families, `typedef int32_t` on AK and a 4-byte "
        "{ uint16_t wire[2]; } here, because on this part the DMA element IS a 16-bit wire "
        "word and the struct is what makes `dst[i] = sample` a compile error -- how defect 7 "
        "stayed hidden. Both sides _Static_assert 4 bytes, so every declaration that carries "
        "the type compares equal; only this one line differs, which is exactly the property "
        "candidate A was chosen for."),

    # NOTE: open() had a phase-2 waiver here ("CK still takes the role argument"). Phase 2
    # landed: open(void) now derives the clock role from the committed block-timing reference,
    # so the declaration compares equal and the waiver was DELETED rather than left to rot --
    # which is the mechanism this table was built for (a waiver that no longer matches reality
    # fails the check).

    # -- Phase 4: the SYSTEM / sync-domain API is COMPLETE. All five waivers that stood here
    # (configure_system in commit 3, start_domain/stop_domain in commit 4,
    # start_all_domains/stop_all_domains in commit 5) were DELETED in the same commit as their
    # declaration, per the rule that a waiver which no longer matches reality is a check failure.
    # No phase-numbered waiver remains: what is left is "permanent" and "optional-D" only.
    # NOTE: inst_get_setup + leg_setup_t had phase-3 waivers here. Phase 3 landed them (B6/B7),
    # so the waivers were DELETED in the same commit as the declarations -- a waiver that no
    # longer matches reality fails this check, which is the point of the table.

    # -- Class D: optional on Sonora's own say-so; CK adds each when its hardware exists.
    #
    # The FOUR co-clocked entries that stood here (inst_tx_fill_mirror, mirror_result_t,
    # inst_tx_active_half, inst_tx_active_pos) were DELETED with item 7: clause T4 removed the
    # class-D exit because CK's silicon CAN co-clock two legs, so "no board built with it" was
    # never a capability limit. All four are now declared and implemented, which is why the
    # only entry left for the mirror is the payload-type CHANGED below -- and it is the same
    # candidate-A difference the callback already carries, not a shape disagreement.
    # NOTE: the FOUR TDMsum entries (tdmsum_t + _tdmsum_configure/_reset/_get) that stood here
    # as "owed-item-10" are GONE, and not by re-labelling: the owner ruled the profiler INTO the
    # mirror list (AK 71369d1, contract section 3 item 10) because NORA_TDM_SUMPROF defaults to 1
    # and the profiler is declared in the public header, so it IS public API -- R0 is not decided
    # by whether the owner's own app calls it. CK implemented all four, so the waivers went stale
    # and were deleted in the same commit as the declarations. Both constraints that came with the
    # ruling are honoured in the implementation, not here: the time base is expressed through
    # nora_high_res_timer_is_initialized() (SCCP1 on this family, no Timer2/3) rather than by
    # deleting a declaration, and ROM is answered by R0.1 -- the SUMPROF switch plus the linker.

    # NOTE: config_t deliberately has NO waiver. After A2 (clock_role) and A7 (dropping
    # ignore_overflow/ignore_underrun) it compares field-for-field identical to canonical, and
    # the first draft of this table waived it on a guess -- the check caught that as a stale
    # waiver on its first run. CK's remaining config divergence (TDM 4/8/16, no 32) lives in
    # field COMMENTS and in tdm_config_is_supported()'s runtime rejection, not in the shape.
}


def main():
    ap = argparse.ArgumentParser(description="Prefix-normalised NORA transport contract diff.")
    ap.add_argument("--ck-header", default=CK_HEADER)
    ap.add_argument("--canonical-header", default=None,
                    help="Re-extract the canonical side from a live sonora header instead of "
                         "the vendored baseline.")
    ap.add_argument("--update", action="store_true",
                    help="With --canonical-header: rewrite the vendored baseline snapshot.")
    ap.add_argument("--verbose", action="store_true", help="List every matching declaration too.")
    args = ap.parse_args()

    with open(args.ck_header, "r", encoding="utf-8", errors="replace") as f:
        ck = extract(f.read())

    if args.canonical_header:
        with open(args.canonical_header, "r", encoding="utf-8", errors="replace") as f:
            canon = extract(f.read())
        source = args.canonical_header
        if args.update:
            # Default newline translation on purpose: the fleet checks out CRLF
            # (.gitattributes `* text=auto eol=crlf`), so regeneration on Windows is
            # idempotent instead of showing a whole-file EOL diff every time.
            with open(BASELINE, "w", encoding="utf-8") as f:
                f.write("# Prefix-normalised canonical NORA transport declaration set.\n")
                f.write("# Extracted by tools/host_check/run_nora_contract_check.py --update\n")
                f.write("# Reference: %s\n" % CANONICAL_REF)
                f.write("# Do not hand-edit: regenerate, and review the diff as a Sonora change.\n")
                for k in sorted(canon):
                    f.write("%s\t%s\n" % (k, canon[k]))
            print("baseline updated: %s" % os.path.relpath(BASELINE, ROOT))
    else:
        if not os.path.exists(BASELINE):
            print("FAIL: no vendored baseline at %s" % BASELINE)
            print("      generate it with --canonical-header <sonora nora_spi_i2s_tdm.h> --update")
            return 2
        canon, source = {}, "%s (vendored snapshot)" % os.path.relpath(BASELINE, ROOT)
        with open(BASELINE, "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                k, _, v = line.rstrip("\n").partition("\t")
                canon[k] = v

    missing = [k for k in canon if k not in ck]
    extra = [k for k in ck if k not in canon]
    changed = [k for k in ck if k in canon and ck[k] != canon[k]]

    undeclared, declared, used = [], [], set()
    for kind, keys in (("MISSING", missing), ("EXTRA", extra), ("CHANGED", changed)):
        for k in sorted(keys):
            w = WAIVERS.get((kind, k))
            if w:
                used.add((kind, k))
                declared.append((kind, k, w))
            else:
                undeclared.append((kind, k))

    unused = sorted(set(WAIVERS) - used)

    print("CK header      : %s" % os.path.relpath(args.ck_header, ROOT))
    print("canonical side : %s" % source)
    print("reference      : %s" % CANONICAL_REF)
    print("declarations   : %d canonical / %d CK  (%d compared equal)"
          % (len(canon), len(ck), len(canon) - len(missing) - len(changed)))
    print("")

    if args.verbose:
        for k in sorted(k for k in ck if k in canon and ck[k] == canon[k]):
            print("  ok        %s" % k)
        print("")

    if declared:
        print("DECLARED DIVERGENCES (%d) -- each one is a decision on record:" % len(declared))
        for kind, k, (phase, reason) in declared:
            print("  %-8s %-52s [retires: %s]" % (kind, k, phase))
            for line in wrap(reason, 92):
                print("             %s" % line)
        print("")

    if unused:
        print("STALE WAIVERS (%d) -- these no longer describe reality; delete them:" % len(unused))
        for kind, k in unused:
            print("  %-8s %s   [was: %s]" % (kind, k, WAIVERS[(kind, k)][0]))
        print("")

    if undeclared:
        print("UNDECLARED DRIFT (%d):" % len(undeclared))
        for kind, k in undeclared:
            print("  %-8s %s" % (kind, k))
            if kind == "CHANGED":
                print("             canonical: %s" % canon[k])
                print("             CK       : %s" % ck[k])
            elif kind == "EXTRA":
                print("             CK       : %s" % ck[k])
            else:
                print("             canonical: %s" % canon[k])
        print("")
        print("FAIL: %d undeclared difference(s). Either align the declaration, or add a "
              "waiver to WAIVERS with the reason and the phase that retires it." % len(undeclared))
        return 1

    if unused:
        print("FAIL: %d stale waiver(s). A waiver that no longer matches reality hides the "
              "next real drift." % len(unused))
        return 1

    print("SHAPE MATCHES: every declaration is either identical modulo the module prefix, or "
          "on the declared-divergence list above.")
    print("")
    print("This certifies SHAPE ONLY. It does NOT certify that CK implements the canonical")
    print("contract: phase 2 made the open-state gates live and phase 3 made the inst_* family")
    print("SINGLE-mode + PRIMARY-only (ERR_CONFIG_MODE is now reachable) -- both exercised on")
    print("hardware by *tl, never here. The mode is a file-static, invisible to this tool: it")
    print("would print exactly this on a build with every mode gate deleted. Phase 4 added the")
    print("SYSTEM / sync-domain API, so no phase-numbered waiver is left -- but its gates and its")
    print("arm-all-then-go phase-locked startup are likewise invisible here and are exercised on")
    print("hardware by *tm.")
    print("The property that split serves -- two legs latching the same FS edge -- is UNPROVEN on")
    print("CK silicon and unobservable on the board that ran this code.")
    return 0


def wrap(text, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > width:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + " " + w).strip()
    if cur:
        lines.append(cur)
    return lines


if __name__ == "__main__":
    sys.exit(main())
