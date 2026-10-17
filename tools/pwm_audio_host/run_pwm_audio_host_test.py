"""Build and run the hal_pwm_audio host test against the firmware sources.

    python tools/pwm_audio_host/run_pwm_audio_host_test.py [--cc gcc]

Compiles src/hal_pwm_audio/*.c unchanged with a generated conf.h for each
case (mono / stereo, prime 1 / 2) and runs the checks in
pwm_audio_host_test.c. Needs a host gcc with __int128 (64-bit); on this
Windows fleet that is C:\\msys64\\usr\\bin\\gcc.exe, found automatically.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HAL = ROOT / "src" / "hal_pwm_audio"
TEST = Path(__file__).resolve().parent / "pwm_audio_host_test.c"

CASES = [
    ("mono_prime1", 1, 4, 1),
    ("mono_prime2", 1, 4, 2),
    ("stereo_prime1", 2, 4, 1),
    ("stereo_prime2", 2, 6, 2),
]

CONF = """#ifndef NORA_PWM_AUDIO_CONF_H
#define NORA_PWM_AUDIO_CONF_H
#define NORA_PWM_AUDIO_ENABLE 1
#define NORA_PWM_AUDIO_SAMPLE_RATE_HZ 48000u
#define NORA_PWM_AUDIO_FRAMES_PER_BLOCK 32u
#define NORA_PWM_AUDIO_MAX_CHANNELS {ch}u
#define NORA_PWM_AUDIO_FIFO_BLOCKS {fifo}u
#define NORA_PWM_AUDIO_PRIME_BLOCKS {prime}u
#define NORA_PWM_AUDIO_DUTY_T uint16_t
#endif
"""


def find_cc(name):
    if name:
        return name
    found = shutil.which("gcc")
    if found:
        return found
    msys = Path(r"C:\msys64\usr\bin\gcc.exe")
    if msys.exists():
        os.environ["PATH"] = str(msys.parent) + os.pathsep + os.environ.get("PATH", "")
        return str(msys)
    sys.exit("no host gcc found; pass --cc")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc")
    args = ap.parse_args()
    cc = find_cc(args.cc)
    failed = 0
    # A Windows-visible temp dir: cygwin gcc cannot resolve its own /tmp from here.
    with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as tmp:
        for name, ch, fifo, prime in CASES:
            case = Path(tmp) / name
            case.mkdir()
            (case / "nora_pwm_audio_conf.h").write_text(CONF.format(ch=ch, fifo=fifo, prime=prime))
            exe = case / "t.exe"
            cmd = [cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-pedantic",
                   "-I", str(case), "-I", str(HAL),
                   str(TEST), str(HAL / "nora_pwm_audio.c"), str(HAL / "nora_pwm_audio_modulator.c"),
                   "-o", str(exe)]
            r = subprocess.run(cmd, cwd=case, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"BUILD FAIL {name}\n{r.stdout}{r.stderr}")
                failed += 1
                continue
            r = subprocess.run([str(exe)], cwd=case, capture_output=True, text=True)
            print(f"[{name}] {r.stdout.strip()}")
            if r.returncode != 0:
                failed += 1
    if failed:
        print(f"FAILED: {failed} case(s)")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
