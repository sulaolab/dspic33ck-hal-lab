"""Check that a peer tree's hal_pwm_audio copy matches this CK copy.

    python tools/pwm_audio_host/check_pwm_audio_parity.py <peer-repo-root>

Read-only. Compares the portable files byte for byte after CRLF -> LF. The
peer layout is found automatically: src/app/hal_pwm_audio (Sonora family) or
src/hal_pwm_audio (dspic33ck-hal-lab).
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CANON = ROOT / "src" / "hal_pwm_audio"
FILES = [
    "nora_pwm_audio.h",
    "nora_pwm_audio.c",
    "nora_pwm_audio_backend.h",
    "nora_pwm_audio_modulator.h",
    "nora_pwm_audio_modulator.c",
    "nora_pwm_audio_conf.h_example",
]

# Files a copy may change for its hardware (16- vs 32-bit core, headroom).
# The change must keep the canonical result bit for bit; the host test pins
# that, including both sides of the amp <= 0xffff switch in the modulator.
# A difference here is reported, not failed.
HW_VARIANT = {
    "nora_pwm_audio_modulator.c": "16-bit core path without 64-bit arithmetic",
}


def text(p):
    return p.read_bytes().replace(b"\r\n", b"\n")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    peer = Path(sys.argv[1])
    for cand in (peer / "src" / "app" / "hal_pwm_audio", peer / "src" / "hal_pwm_audio"):
        if cand.is_dir():
            break
    else:
        sys.exit(f"no hal_pwm_audio under {peer}")
    bad = 0
    for f in FILES:
        if not (cand / f).exists():
            print(f"MISSING {f}")
            bad += 1
        elif text(CANON / f) != text(cand / f):
            if f in HW_VARIANT:
                print(f"VARIANT {f}  ({HW_VARIANT[f]})")
            else:
                print(f"DIFF    {f}")
                bad += 1
        else:
            print(f"PASS    {f}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
