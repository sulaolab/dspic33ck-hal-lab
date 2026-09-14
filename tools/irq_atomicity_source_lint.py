#!/usr/bin/env python3
"""Source-side lint for the IRQ-register-atomicity rule (dsPIC33CK).

IFSx / IECx are shared by every peripheral on the device, so they must only ever
be touched one bit at a time through a DFP bit alias whose register, bit and
written value are all compile-time constant:

    if (enable) { _SPI1RXIE = 1; } else { _SPI1RXIE = 0; }

On EV88G73A a single word, IEC0, carries the 1 ms tick, both audio DMA legs, the
load-monitor time base, the TDM transport's SPI1 RX/TX and the console UART.
Any form that reaches such a
register through a pointer or a runtime mask compiles to a read-modify-write of
the whole word, and the write-back restores whatever the other peripherals' bits
were at the load.

Why a *source* lint and not just the disassembly check: the grep in section 3 of
that document matches a direct address (`mov.w w1, 0x820`). The pointer form
carries no address in the operand -- `mov.w w1, [w2]` where `w2 == &IEC0` is
invisible to it -- and a translation unit that `--gc-sections` drops, or a
configuration that does not link it, contributes nothing to the image at all.
This lint closes both gaps from the other end, by banning the constructs from
which a non-atomic write can arise in the first place.

Banned (outside comments and string literals):

  &IFS0, & IEC1          taking the address of a shared interrupt register
  *ifs, *iec             dereferencing such a pointer
  ->ifs, ->iec, .ifs     a descriptor field holding one
  if_mask, ie_mask,      the runtime mask that went with it
  ifs_mask, iec_mask
  _DMA0IE = enable       a *runtime* value written to a bit alias. On CK this
  _DMA0IE = e ? 1 : 0    happens to fold to a single BFINS today, but nothing in
  _DMA0IE |= 1           the C says so -- the same line is a live defect on AK,
                         where there is no BFINS (section 8). Only the literal
                         forms `= 0` / `= 1` (optional u suffix) are guaranteed.
  IEC0 |= m              a read-modify-write of the whole shared register
  IFS0 = 0               a whole-register store: not an RMW, but it clears every
                         other peripheral's bit in the same register
  IEC0bits, IFS0bits     the bitfield struct, which also compiles to an RMW
  _IFS0_CNAIF_MASK       per-device bank knowledge -- allowed *only* inside
  _IFS4_CNEIF_MASK       defined(...) as a capability probe

Reads are never flagged: `if (IEC0 & mask)` and `x = IFS0` cannot race. `_XxxIP`
is out of scope on purpose: priority bits live in IPCx, which is a 4-bit field
(a read-modify-write either way), is never written by hardware, and is programmed
at init only, so there is no concurrent writer to race with.

Usage:
    python tools/irq_atomicity_source_lint.py [--root src] [--verbose]
    python tools/irq_atomicity_source_lint.py --self-test

Exit status 0 = clean, 1 = violations found. Both modes are CI-ready as they are.
"""

import argparse
import os
import re
import sys

EXTS = (".c", ".h")

# Rules are (name, compiled pattern, human explanation).
RULES = [
    ("address-of-IFS/IEC",
     re.compile(r"&\s*(?:IFS|IEC)\d"),
     "take no address of a shared interrupt register; write the _XxxIE/_XxxIF alias"),
    ("IFS/IEC pointer deref",
     re.compile(r"\*\s*(?:ifs|iec)\b"),
     "dereferencing an IFS/IEC pointer is a read-modify-write"),
    # A descriptor field holding a pointer to a shared interrupt register.
    # `x.iec[0] = IEC0` is *not* this: an indexed value array cannot hold a
    # register pointer, because taking &IECn is banned by the rule above.
    # The optional leading identifier is captured so the finding text is the
    # whole qualified expression (`dev->iec`), which is what ALLOW keys on.
    ("IFS/IEC descriptor field",
     re.compile(r"(?:[A-Za-z_]\w*\s*)?(?:->|\.)\s*(?:ifs|iec)\b(?!\s*\[)"),
     "no descriptor may carry an IFS/IEC pointer"),
    ("IFS/IEC runtime mask",
     re.compile(r"\b(?:if_mask|ie_mask|ifs_mask|iec_mask)\b"),
     "a runtime mask forces the read-modify-write path"),
    # Direct writes to the whole shared register. `IEC0 |= mask` is the same
    # read-modify-write as the pointer form and would be invisible to the
    # disassembly check in a translation unit that --gc-sections drops.
    # Reads (`if (IEC0 & mask)`) are not a hazard and are not matched.
    # The right-hand side up to the statement end is part of the finding text, so
    # an ALLOW entry pins the exact assignment rather than the register name.
    ("IFS/IEC whole-register write",
     re.compile(r"\b(?:IFS|IEC)\d+\s*(?:\|=|&=|\^=|\+=|-=|<<=|>>=|=(?!=))[^;]*"),
     "write the _XxxIE/_XxxIF bit alias; a whole-register write is a read-modify-write"),
    ("IFS/IECbits write",
     re.compile(r"\b(?:IFS|IEC)\d+bits\b"),
     "IFSxbits/IECxbits compiles to a word RMW; use the _XxxIE/_XxxIF alias"),
]

# Documented exceptions, keyed by repo-relative posix path. The value is a set
# of (rule name, exact finding text) pairs: an entry excuses *that expression*
# under *that rule* only, never the whole file or the whole rule. A path key
# alone would silently excuse a different expression added later on the same
# register in the same file.
#
# This repository has none. Keep it that way if you can; each pair added here is
# a promise that the site cannot race, and it has to be argued, not just listed.
ALLOW = {}

# A DFP interrupt bit alias -- `_DMA0IE`, `_U1RXIF`, `_SPI1TXIE`. Leading
# underscore, then uppercase/digits with no further underscore (so the bank
# macros `_IFS0_CNAIF_MASK` are not caught here), ending in IE or IF. `IP` is
# deliberately out of scope: priority bits live in IPCx, not IFSx/IECx.
BIT_ALIAS_ASSIGN = re.compile(
    r"\b(_[A-Z][A-Z0-9]*I[EF])\s*(\|=|&=|\^=|<<=|>>=|=(?!=))([^;]*)")
# The only right-hand sides the compiler can fold into one bset.b / bclr.b.
BIT_ALIAS_LITERAL = re.compile(r"^[01][uU]?$")

# _IFSn_..._MASK / _IECn_..._MASK: bank knowledge. Legal only as defined(...).
BANK_MACRO = re.compile(r"\b_(?:IFS|IEC)\d+_[A-Z0-9_]+_MASK\b")
BANK_MACRO_PROBED = re.compile(r"\bdefined\s*\(\s*_(?:IFS|IEC)\d+_[A-Z0-9_]+_MASK\s*\)")


def strip_comments_and_strings(text):
    """Blank out /*...*/, //... and "..." / '...' while keeping line structure."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            quote, j = c, i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def normalize(text):
    """Collapse internal whitespace so ALLOW keys stay readable."""
    return re.sub(r"\s+", "", text.strip())


def check_file(path):
    allowed = ALLOW.get(path.replace("\\", "/"), frozenset())
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        raw = fh.read()
    return scan_text(raw, allowed)


def scan_text(text, allowed=frozenset()):
    """Rule engine over a string. Shared by check_file() and the self-test."""
    code = strip_comments_and_strings(text.replace("\r\n", "\n"))
    findings = []
    for lineno, line in enumerate(code.split("\n"), start=1):
        for name, pattern, why in RULES:
            for m in pattern.finditer(line):
                findings.append((lineno, name, normalize(m.group(0)), why))
        # The original defect: a bit alias assigned a runtime value. Plain
        # `= 0` / `= 1` is the required form and is not a finding; a compound
        # assignment never is, because it reads the register back.
        for m in BIT_ALIAS_ASSIGN.finditer(line):
            if m.group(2) == "=" and BIT_ALIAS_LITERAL.match(normalize(m.group(3))):
                continue
            findings.append((lineno, "IFS/IEC bit alias runtime write",
                             normalize(m.group(0)),
                             "write the literal 0 or 1 (`if (e) { A = 1; } else { A = 0; }`); "
                             "a runtime value is not guaranteed to fold to one instruction"))
        for m in BANK_MACRO.finditer(line):
            span = m.span()
            probed = any(p.span()[0] <= span[0] and span[1] <= p.span()[1]
                         for p in BANK_MACRO_PROBED.finditer(line))
            if not probed:
                findings.append((lineno, "IFS/IEC bank macro", m.group(0),
                                 "bank numbers are per-device; use the bit alias, "
                                 "or keep the macro inside defined(...)"))
    # Exclude only exact (rule, expression) pairs listed in ALLOW.
    return [f for f in findings if (f[1], f[2]) not in allowed]


# (source line, allow-set, expected finding texts). Keeps the rules honest, and
# in particular pins that an ALLOW entry excuses one expression, not a file.
_EXAMPLE_ALLOW = frozenset({("IFS/IEC whole-register write", "IEC0=0u")})

SELF_TEST = [
    ("/* &IFS0 and IEC0 |= m in a comment */", frozenset(), []),
    ('write("IEC0 |= m");', frozenset(), []),
    # The shape this repository's SPI/TDM and GPIO-event HALs used to carry.
    ("static volatile uint16_t *iec = &IEC0;", frozenset(), ["&IEC0", "*iec"]),
    ("*iec |= mask;", frozenset(), ["*iec"]),
    # `*d->iec` is caught by the descriptor-field rule, not the deref rule --
    # the `*` is not adjacent to the field name.
    ("*d->iec |= d->ie_mask;", frozenset(), ["d->iec", "ie_mask"]),
    # `regs->ifs_mask` is reported once, by the runtime-mask rule: the
    # descriptor-field rule needs a word boundary after `ifs`, and `ifs_mask`
    # does not have one. One finding per expression, not two.
    ("*regs->ifs &= (uint16_t)~regs->ifs_mask;", frozenset(),
     ["regs->ifs", "ifs_mask"]),
    ("IEC0 |= mask;", frozenset(), ["IEC0|=mask"]),
    ("IFS0 &= ~mask;", frozenset(), ["IFS0&=~mask"]),
    ("IEC0bits.DMA0IE = 1;", frozenset(), ["IEC0bits"]),
    ("if (IEC0 & mask) { x = IFS0; }", frozenset(), []),
    ("#if defined(_IFS4_CNEIF_MASK)", frozenset(), []),
    ("k = _IFS0_CNAIF_MASK;", frozenset(), ["_IFS0_CNAIF_MASK"]),
    # The required shape passes; every runtime form fails. On CK the runtime
    # forms currently fold to BFINS, which is why they are pinned here rather
    # than trusted: nothing in the C guarantees it and AK proves it can differ.
    ("if (enable) { _DMA0IE = 1; } else { _DMA0IE = 0; }", frozenset(), []),
    ("_DMA0IF = 0;", frozenset(), []),
    ("_U1TXIF = 1u;", frozenset(), []),
    ("_DMA0IE = v;", frozenset(), ["_DMA0IE=v"]),
    ("_U1RXIE = enable ? 1 : 0;", frozenset(), ["_U1RXIE=enable?1:0"]),
    ("_CNAIE = v;", frozenset(), ["_CNAIE=v"]),
    ("_DMA0IF = flag;", frozenset(), ["_DMA0IF=flag"]),
    ("_DMA0IE |= 1;", frozenset(), ["_DMA0IE|=1"]),
    ("_DMA0IE = 2;", frozenset(), ["_DMA0IE=2"]),
    # `_T1IE = saved_ie` -- the tick timer's live defect, fixed on main by
    # making the surviving arm a literal. Both forms are pinned.
    ("_T1IE = saved_ie;", frozenset(), ["_T1IE=saved_ie"]),
    ("if (saved_ie) { _T1IE = 1; }", frozenset(), []),
    # Neither a read of an alias nor a capability probe is a write.
    ("if (_DMA0IF) { n++; }", frozenset(), []),
    ("#if defined(_CNAIE) && defined(_CNAIF)", frozenset(), []),
    # A bank macro on the right of an assignment stays with the bank-macro rule
    # and must not also trip the alias rule.
    ("k = _IFS0_CNAIF_MASK | m;", frozenset(), ["_IFS0_CNAIF_MASK"]),
    # ALLOW is per (rule, expression): the pinned expression passes, anything
    # else written to the same register in the same file still fails. ALLOW is
    # empty in this repository; this pins the mechanism, not a real exception.
    ("IEC0 = 0u;", _EXAMPLE_ALLOW, []),
    ("IEC0 = saved_iec0;", _EXAMPLE_ALLOW, ["IEC0=saved_iec0"]),
    ("IEC0 |= 0u;", _EXAMPLE_ALLOW, ["IEC0|=0u"]),
    ("dev->iec = irq_reg;", _EXAMPLE_ALLOW, ["dev->iec"]),
]


def self_test():
    failures = 0
    for line, allowed, expected in SELF_TEST:
        got = [f[2] for f in scan_text(line, allowed)]
        if got != expected:
            failures += 1
            print("SELF-TEST FAIL: {!r}\n  expected {}\n  got      {}".format(
                line, expected, got))
    print("irq_atomicity_source_lint self-test: {} cases, {} failure(s)".format(
        len(SELF_TEST), failures))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="src", help="directory to scan (default: src)")
    ap.add_argument("--verbose", action="store_true", help="list every scanned file")
    ap.add_argument("--self-test", action="store_true",
                    help="check the rules against known good/bad lines and exit")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    files = []
    for dirpath, _dirs, names in os.walk(args.root):
        for name in names:
            if name.endswith(EXTS):
                files.append(os.path.join(dirpath, name))
    files.sort()

    total = 0
    for path in files:
        findings = check_file(path)
        if args.verbose and not findings:
            print("ok   {}".format(path))
        for lineno, name, text, why in findings:
            total += 1
            print("{}:{}: {}: '{}' -- {}".format(
                path.replace("\\", "/"), lineno, name, text, why))

    print("irq_atomicity_source_lint: scanned {} files, {} violation(s)".format(
        len(files), total))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
