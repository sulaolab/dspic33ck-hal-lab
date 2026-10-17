#!/usr/bin/env python3
"""Compile GPIO family against four real DFPs; run DFP-derived host SFR checks.

Requires XC-DSC 3.31.01 and a host GCC. No serial, flash or hardware access.
The host mock models storage/bitfields, not electrical or interrupt timing.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
DEVICES = (
    ("33CK1024MP710", "dsPIC33CK-MP_DFP", "1.16.521"),
    ("33CK256MP508", "dsPIC33CK-MP_DFP", "1.15.423"),
    ("33CK64MC105", "dsPIC33CK-MC_DFP", "1.10.386"),
    ("33CK256MC005", "dsPIC33CK-MC_DFP", "1.11.412"),
)


def run(args, cwd, env=None):
    result = subprocess.run([str(a) for a in args], cwd=cwd, env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.stdout:
        lines = result.stdout.splitlines()
        print("\n".join(lines[:40]), flush=True)
        if len(lines) > 40:
            print(f"... {len(lines) - 40} additional lines; see build/gpio_check/last_error.log")
    if result.returncode:
        (ROOT / "build/gpio_check/last_error.log").write_text(result.stdout)
        raise SystemExit(result.returncode)


def mock_header(header, sources):
    """Use DFP register types, aliases, masks and codes as the test oracle."""
    defines = dict(re.findall(r"^#define\s+(\w+)\s+([^\n]+)", header, re.M))
    types = dict(re.findall(r"typedef struct tag(\w+) \{(.*?)\} \1;", header, re.S))
    wanted = set(re.findall(r"\bdefined\((\w+)\)|^#ifdef\s+(\w+)", sources, re.M))
    wanted = {a or b for a, b in wanted}
    wanted.add("_RPOR0_RP32R_MASK")
    # These register names are created by token pasting in the production rows.
    for letter in "ABCDEFGH":
        wanted.update(prefix + letter for prefix in
                      ("ANSEL", "TRIS", "PORT", "LAT", "ODC", "CNPU", "CNPD",
                       "CNCON", "CNEN0", "CNEN1", "CNF"))
    out = ["#ifndef GPIO_CHECK_XC_H", "#define GPIO_CHECK_XC_H",
           "#include <stdint.h>", "#include <string.h>"]
    registers = {}
    aliases = []
    for name in sorted(wanted):
        if name not in defines:
            continue
        value = defines[name].strip()
        if value == name:
            registers.setdefault(name, None)
        elif re.fullmatch(r"\w+bits\.\w+", value):
            bank, field = value.split(".")
            reg = bank[:-4]
            registers[reg] = reg + "BITS"
            aliases.append(f"#define {name} {value}")
        else:
            aliases.append(f"#define {name} {value}")
    # Readback depends on a contiguous RPOR bank, exactly as in the target SFRs.
    rp_banks = [int(reg[4:]) for reg in registers if re.fullmatch(r"RPOR\d+", reg)]
    out.append(f"static volatile uint16_t mock_rpor[{max(rp_banks) + 1}];")
    for reg, typ in sorted(registers.items()):
        if reg.startswith("RPOR"):
            out.extend((f"typedef struct {{{types[typ]}}} {typ};",
                        f"#define {reg} mock_rpor[{int(reg[4:])}]",
                        f"#define {reg}bits (*(volatile {typ} *)&{reg})"))
        elif typ:
            out.extend((f"typedef struct {{{types[typ]}}} {typ};",
                        f"static volatile union {{ uint16_t word; {typ} bits; }} mock_{reg};",
                        f"#define {reg} mock_{reg}.word",
                        f"#define {reg}bits mock_{reg}.bits"))
        else:
            out.extend((f"static volatile uint16_t mock_{reg};", f"#define {reg} mock_{reg}"))
    out.extend(aliases)
    out.extend(("static uint16_t mock_lock = 0x0800u;",
                "#define __builtin_write_RPCON(v) (mock_lock = (v))",
                "struct rp_expectation { uint8_t rp, bank, shift; uint16_t mask; };",
                "static const struct rp_expectation rp_expected[] = {"))
    for rp in range(32, 100):
        alias = defines.get(f"_RP{rp}R")
        if alias:
            reg, field = alias.split(".")
            prefix = f"_{reg[:-4]}_{field}"
            out.append(f"{{{rp}, {int(reg[4:-4])}, {defines[prefix + '_POSITION']}, "
                       f"{defines[prefix + '_MASK']}}},")
    out.extend(("};", "#endif"))
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xc", type=Path, default=Path(
        "C:/Program Files/Microchip/xc-dsc/v3.31.01/bin/xc-dsc-gcc.exe"))
    parser.add_argument("--host-cc", type=Path, default=Path("C:/msys64/usr/bin/gcc.exe"))
    parser.add_argument("--packs-root", type=Path, default=Path.home() / ".mchp_packs/Microchip")
    parser.add_argument("--source-root", type=Path, default=ROOT,
                        help="Alternative source tree, useful for a regression negative control")
    args = parser.parse_args()
    src = args.source_root.resolve() / "src/hal_gpio"
    files = sorted(src.glob("*_dspic33ck.c"))
    if len(files) != 4:
        raise SystemExit("Expected GPIO, event, table and PPS backend sources")
    sources = "\n".join(p.read_text() for p in files)
    build = ROOT / "build/gpio_check"
    build.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["PATH"] = str(args.host_cc.parent) + os.pathsep + env.get("PATH", "")
    for device, pack, version in DEVICES:
        out = build / device
        out.mkdir(exist_ok=True)
        dfp = args.packs_root / pack / version / "xc16"
        header = dfp / f"support/dsPIC33C/h/p{device}.h"
        print(f"Checking {device}, {pack} {version}", flush=True)
        for file in files:
            run([args.xc, f"-mcpu={device}", f"-mdfp={dfp}", "-Wall", "-Wextra",
                 "-Werror", "-mno-eds-warn", "-O1", "-c", file,
                 "-o", out / (file.stem + ".o")], ROOT)
        (out / "xc.h").write_text(mock_header(header.read_text(), sources))
        # One translation unit shares the mock registers across the real backends.
        includes = "\n".join(f'#include "{p.as_posix()}"' for p in files)
        harness = out / "check.c"
        harness.write_text('#include "xc.h"\n' + includes + "\n" +
                           (Path(__file__).parent / "gpio_behavior.c").read_text())
        exe = out / "check.exe"
        run([args.host_cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-variable", "-fno-strict-aliasing", "-I", out, "-I", src,
             harness, "-o", exe], ROOT, env)
        run([exe], ROOT, env)
    print("PASS: four-device target compilation and DFP-derived GPIO/PPS/CN behavior")
    print("EDS advisory suppressed for compile checks; MP710 stack placement still requires link validation.")


if __name__ == "__main__":
    main()
