#!/usr/bin/env bash
# Test harness for the psxport runtime + wide60 reprojecting renderer.
#
# Builds runtime/wide60rt and runs a battery of headless checks against Tomba! 2,
# each reporting PASS/FAIL with the measured number:
#   1. boot           - reaches the in-engine scene without the watchdog firing
#   2. determinism    - identical RAM hashes across two runs (full instant config)
#   3. objects        - the per-object cull chokepoint enumerates live objects
#   4. rtps-reproject - our faithful RTPS reproduces the GTE's screen coords 100%
#   5. wide60-verify  - in-runtime reprojection reproduces object SXY 100%
#   6. wide60-join    - reports GTE-object join coverage (informational)
#
# Usage: scripts/test-wide60.sh [disc.chd] [frames]
#   disc:   $1 > PSXPORT_TOMBA2_DISC (env or .env) > Tomba*2*.chd drop-in
#   frames: how far to run the 3D-scene checks (default 7400; objects live ~7035+)
set -u

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

# --- disc resolution (parse .env raw; values may contain spaces) -------------
env_disc=""
[ -f .env ] && env_disc="$(sed -n 's/^PSXPORT_TOMBA2_DISC=//p' .env | head -1)"
disc="${1:-${PSXPORT_TOMBA2_DISC:-$env_disc}}"
if [ -z "$disc" ]; then
  for f in Tomba*2*.chd; do [ -f "$f" ] && disc="$f" && break; done
fi
if [ -z "$disc" ] || [ ! -f "$disc" ]; then
  echo "Tomba! 2 CHD not found. Pass as arg, set PSXPORT_TOMBA2_DISC in .env," >&2
  echo "or drop 'Tomba! 2 ... .chd' into the repo root." >&2
  exit 2
fi
frames="${2:-7400}"
bios="$repo/bios"
rt="$repo/runtime/wide60rt"
log="$repo/scratch/logs"
raw="$repo/scratch/raw"
mkdir -p "$log" "$raw"

green=$'\033[32m'; red=$'\033[31m'; dim=$'\033[2m'; rst=$'\033[0m'
fails=0
pass() { printf "  ${green}PASS${rst} %-16s %s\n" "$1" "${2:-}"; }
fail() { printf "  ${red}FAIL${rst} %-16s %s\n" "$1" "${2:-}"; fails=$((fails+1)); }
info() { printf "  ${dim}info${rst} %-16s %s\n" "$1" "${2:-}"; }

echo "disc:   $disc"
echo "frames: $frames"

# --- build -------------------------------------------------------------------
echo "== build =="
if make -C "$repo/runtime" >"$log/build.log" 2>&1; then
  pass build "$(ls -la "$rt" | awk '{print $5" bytes"}')"
else
  fail build "see $log/build.log"; echo "aborting (build failed)"; exit 1
fi

# --- 1. boot smoke -----------------------------------------------------------
echo "== run checks =="
if timeout 120 "$rt" "$disc" -bios "$bios" -frames 700 >/dev/null 2>"$log/boot.log"; then
  if grep -q "EXEC:PC0" "$log/boot.log"; then pass boot "game EXEC reached"; else fail boot "no EXEC in TTY"; fi
else
  fail boot "nonzero exit / watchdog (see $log/boot.log)"
fi

# --- 2. determinism ----------------------------------------------------------
PSXPORT_RAMHASH="100:$log/rh_a.csv" timeout 120 "$rt" "$disc" -bios "$bios" -frames 600 >/dev/null 2>&1
PSXPORT_RAMHASH="100:$log/rh_b.csv" timeout 120 "$rt" "$disc" -bios "$bios" -frames 600 >/dev/null 2>&1
if [ -s "$log/rh_a.csv" ] && diff -q "$log/rh_a.csv" "$log/rh_b.csv" >/dev/null 2>&1; then
  pass determinism "RAM hashes identical across runs"
else
  fail determinism "RAM hashes differ or missing"
fi

# --- 3. object enumeration ---------------------------------------------------
PSXPORT_T2_OBJLOG=1 timeout 180 "$rt" "$disc" -bios "$bios" -frames "$frames" 2>"$log/obj.txt" >/dev/null
maxobj="$(grep -oE 'objs n=[0-9]+' "$log/obj.txt" | grep -oE '[0-9]+' | sort -n | tail -1)"
if [ "${maxobj:-0}" -gt 20 ]; then pass objects "up to $maxobj live objects/frame"; else fail objects "few/no objects (${maxobj:-0})"; fi

# --- 4. RTPS reprojection (offline, faithful) --------------------------------
PSXPORT_T2_RTPDUMP="$raw/rtp.csv" timeout 180 "$rt" "$disc" -bios "$bios" -frames "$frames" >/dev/null 2>&1
rp="$(python3 "$repo/tools/reproject.py" "$raw/rtp.csv" 2>/dev/null | head -1)"
if echo "$rp" | grep -q "(100.000%)"; then pass rtps-reproject "$rp"; else fail rtps-reproject "${rp:-no rows}"; fi

# --- 5/6. wide60 in-runtime reprojection + join coverage ---------------------
PSXPORT_WIDE60=1 PSXPORT_WIDE60_LOG=1 timeout 180 "$rt" "$disc" -bios "$bios" -frames "$frames" 2>"$log/w60.txt" >/dev/null
verify="$(grep -oE 'REPROJECT-VERIFY: [0-9]+/[0-9]+ object verts reproduced exactly \([0-9.]+%\)' "$log/w60.txt" | head -1)"
if echo "$verify" | grep -q "(100.0%)"; then pass wide60-verify "${verify#REPROJECT-VERIFY: }"; else fail wide60-verify "${verify:-not reached}"; fi
avgjoin="$(grep '^\[flip' "$log/w60.txt" | grep -oE '\([0-9]+%\)' | tr -d '()%' | awk '{s+=$1;n++} END{if(n)printf "%.0f%% avg over %d flips",s/n,n}')"
info wide60-join "${avgjoin:-n/a} (GTE geometry; terrain/UI are 30fps by design)"

# --- summary -----------------------------------------------------------------
echo "== summary =="
if [ "$fails" -eq 0 ]; then
  printf "${green}all checks passed${rst}\n"; exit 0
else
  printf "${red}%d check(s) failed${rst}\n" "$fails"; exit 1
fi
