#!/usr/bin/env bash
# Headless VK render-diff: default OT-order depth vs PSXPORT_NATIVE_DEPTH (Phase-2 real per-vertex depth).
# Runs the PORT offscreen (PSXPORT_VK_HEADLESS — no window, deterministic), captures the same field
# frames both ways, and diffs them. Also runs a determinism control (default vs default) so a real
# default-vs-depth difference can't be confused with capture noise.
#
# Usage: tools/vk_depth_diff.sh [first:last:step]   (default 360:420:20)
# Output: scratch/screenshots/vkdiff/{off,on,ctl}/vk_*.ppm + per-frame diff stats + *_diff.ppm masks.
# Disc: CLI arg unused; set PSXPORT_TOMBA2_DISC (.env) or rely on *.chd autodetect (see run.sh).
set -u
cd "$(dirname "$0")/.."
PORT=scratch/bin/tomba2_port
MAIN=scratch/bin/tomba2/MAIN.EXE
SEQ="${1:-360:420:20}"
first="${SEQ%%:*}"; rest="${SEQ#*:}"; last="${rest%%:*}"; step="${rest##*:}"
OUT=scratch/screenshots/vkdiff
mkdir -p "$OUT/off" "$OUT/on" "$OUT/ctl"
cap=$((last + 12))

run() {  # $1=dir  $2=NATIVE_DEPTH(0/1)
  PSXPORT_VK_HEADLESS=1 PSXPORT_NATIVE_DEPTH="$2" PSXPORT_NO_FMV=1 PSXPORT_AUTO_GAMEPLAY=1 \
    PSXPORT_VK_SHOTSEQ="$first:$last:$step:$OUT/$1" PSXPORT_NATIVE_FRAMES="$cap" \
    timeout 300 "$PORT" "$MAIN" >/dev/null 2>&1
}
echo "[vkdiff] capturing default (off)…";  run off 0
echo "[vkdiff] capturing native-depth (on)…"; run on 1
echo "[vkdiff] capturing determinism control (off #2)…"; run ctl 0
python3 tools/vk_depth_diff.py "$OUT" "$first" "$last" "$step"
