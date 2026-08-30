#!/usr/bin/env bash
# Re-cost the complex ROTATOR against the shipped table carriers, by compiling both
# and counting.  See carrier_loop_probe.c for what each PROBE is and why.
#
# The CALIBRATION comes first and is not optional: the same counter is pointed at the
# real engine, compiled with the real production recipe, and must reproduce the two
# published scales -- 48/30 (rect) and 33/19 (polar) at the shipped ENVFRAC=0, and
# section 18's 56/36 and 37/21 at ENVFRAC=16, which is what every micro-second in
# sections 18-21 is quoted against.  If it does not, the probe numbers below have no
# scale.  Both are printed because the default moved once already (V3, section 25)
# and a single expectation silently became the wrong one.
#
#   bash tools/cost_probe/run_probe.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/ck_rotator_probe"
mkdir -p "$OUT"

GCC="/c/Program Files/Microchip/xc-dsc/v3.31.01/bin/xc-dsc-gcc.exe"
DFP="$(cygpath -m ~ 2>/dev/null || echo "$HOME")/.mchp_packs/Microchip/dsPIC33CK-MC_DFP/1.10.386/xc16"
[ -x "$GCC" ] || { echo "no compiler at $GCC"; exit 1; }
[ -d "$DFP" ] || { echo "no DFP at $DFP"; exit 1; }

# Section 15's recipe, verbatim apart from the source file and the -o.
ENGINE_FLAGS=(-mcpu=33CK64MC105 -mno-eds-warn -g -omf=elf
  -DXPRJ_CK64MC105_EV88G73A=CK64MC105_EV88G73A -ffunction-sections -O2
  -I../src -I../src/app -I../src/app/dsp -I../src/uart_app -I../src/uart_platform
  -I../src/chip_drivers -I../src/hal_adc -I../src/hal_clock -I../src/hal_reset
  -I../src/hal_dma -I../src/hal_gpio -I../src/hal_timer -I../src/hal_uart
  -I../src/hal_i2c -I../src/hal_spi_i2s_tdm -I../src/boards/ev88g73a
  -DDSPIC33CK_BOARD_EV88G73A=1 -DAPP_TRAPS_POLICY=1 -msmart-io=1 -msfr-warn=off
  -mdfp="$DFP")
# The probe is a free-standing TU: no engine headers, so no -I and no board defines.
PROBE_FLAGS=(-mcpu=33CK64MC105 -mno-eds-warn -g -omf=elf -ffunction-sections -O2
  -msmart-io=1 -Wall -msfr-warn=off -mdfp="$DFP")

count() { python "$HERE/count_loop.py" "$@"; }

# One row: loop body size, plus the two numbers that explain it.
row() {
    local name=$1 asm=$2 fn=$3
    local rng a b body
    rng=$(count "$asm" "$fn" | sed -n 's/.*loop [^ ]* *\([0-9]*\) instr *(lines \([0-9]*\)-\([0-9]*\)).*/\1 \2 \3/p' | head -1)
    if [ -z "$rng" ]; then printf '  %-28s %s\n' "$name" "no loop (peeled)"; return; fi
    set -- $rng
    body=$(sed -n "$2,$3p" "$asm" | grep -vE '^[[:space:]]*\.|^[[:space:]]*;|^[[:space:]]*$')
    # `w15` is the stack pointer: an instruction naming it is a spill, a reload, or the
    # loop's own compare -- which is exactly one, so a count of 1 means "no spilling".
    printf '  %-28s %4s instr   %3s stack-ops  %3s mul/mac/msc\n' "$name" "$1" \
        "$(printf '%s\n' "$body" | grep -c 'w15')" \
        "$(printf '%s\n' "$body" | grep -cE '^[[:space:]]+(mul|mac|msc)')"
}

echo "=============================================================================="
echo "CALIBRATION -- the real engine, the production recipe, this counter"
# The expectation moved with the shipped default and this line said so a release
# late: 56/36 and 37/21 are the ENVFRAC=16 numbers of doc section 18, and the
# 16-bit envelope became the default at 133cd4f (section 25), so a recipe with no
# -D now produces the V3 figures -- which is also why the block below prints the
# same numbers as this one. Both are quoted so a mismatch names its own cause.
echo "  expected, shipped default (ENVFRAC=0, V3): rect 48 whole / 30 loop, polar 33 whole / 19 loop"
echo "  expected at ENVFRAC=16 (doc section 18):   rect 56 whole / 36 loop, polar 37 whole / 21 loop"
echo "=============================================================================="
cd "$ROOT/firmware.X" || exit 1
for e in 0 1; do
    "$GCC" ../src/app/dsp/avas_synth_line_ck.c -S -o "$OUT/engine$e.s" \
        -DAVAS_TYPE_TY_CK_ENVINTERP=$e "${ENGINE_FLAGS[@]}" || exit 1
    lbl=$([ "$e" = 0 ] && echo "engine rect (ships)" || echo "engine polar (candidate)")
    count "$OUT/engine$e.s" _avas_line_ck_process_carriers | sed "s/_avas_line_ck_process_carriers/$lbl/"
done

echo
echo "=============================================================================="
echo "THE WIDE ENVELOPE ON THE REAL ENGINE -- ENVFRAC=16, floored slope"
echo "  the scale doc section 18 published, kept because every micro-second in"
echo "  sections 18-21 is quoted against it; V3 (the default above) is -6 rect / -2"
echo "  polar from here, which is what the free-standing probes predicted"
echo "=============================================================================="
for e in 0 1; do
    "$GCC" ../src/app/dsp/avas_synth_line_ck.c -S -o "$OUT/engine${e}wide.s" \
        -DAVAS_TYPE_TY_CK_ENVINTERP=$e -DAVAS_TYPE_TY_CK_ENVFRAC=16 \
        -DAVAS_TYPE_TY_CK_ENVROUND=0 "${ENGINE_FLAGS[@]}" || exit 1
    lbl=$([ "$e" = 0 ] && echo "engine rect  wide" || echo "engine polar wide")
    count "$OUT/engine${e}wide.s" _avas_line_ck_process_carriers | sed "s/_avas_line_ck_process_carriers/$lbl/"
done

echo
echo "=============================================================================="
echo "THE PROBES -- same shape, same flags, same counter"
echo "=============================================================================="
cd "$HERE" || exit 1
names=([1]="1 table  + rect  (baseline)" [2]="2 table  + polar (baseline)"
       [3]="3 rotator Q15 + rect" [4]="4 rotator Q20 + rect, int64 C"
       [5]="5 rotator Q20 + rect, split 32x16" [6]="6 rotator Q15 + polar"
       [7]="7 table  + rect,  V3 env16" [8]="8 table  + polar, V3 env16")
for p in 1 2 3 4 5 6 7 8; do
    "$GCC" carrier_loop_probe.c -S -o "$OUT/probe$p.s" -DPROBE=$p "${PROBE_FLAGS[@]}" \
        2>/dev/null || { echo "  PROBE=$p FAILED TO COMPILE"; continue; }
    row "${names[$p]}" "$OUT/probe$p.s" _probe_carriers
done

echo
echo "  renormalisation -- once per carrier per REBUILD, so /$((32)) per sample:"
for p in 3 4 5 6; do
    row "${names[$p]} renorm" "$OUT/probe$p.s" _probe_renorm
done

echo
echo "Listings in $OUT for the attribution."
