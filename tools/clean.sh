#!/usr/bin/env bash
# tools/clean.sh — reclaim disk from regenerable build/diagnostic artifacts.
#
# SAFE BY DESIGN: deletes only an explicit ALLOWLIST of known-regenerable dumps + dead
# build dirs. It never touches anything not named below, so RE assets and state survive.
#
# PRESERVED (never deleted): tracked source, .env, generated/, build/ (holds the libchdr
# static libs run.sh links against), and inside scratch/: decomp/ ghidra/ *_re/ (RE assets),
# state/ states/ saves/ (savestates the diff harness uses), bios/ bios-fast/, bin/ (the built
# port), obj/ (the incremental object cache), and the small *.py/*.sh/*.txt/*.md tooling.
#
# Run from anywhere:  tools/clean.sh
set -u
cd "$(dirname "$0")/.."

human() { du -sh "$1" 2>/dev/null | cut -f1; }
freed_kb=0
rm_target() {  # $1 = path or glob (already expanded by caller); print size, then remove
  for p in $1; do
    [ -e "$p" ] || continue
    sz=$(du -sk "$p" 2>/dev/null | cut -f1)
    freed_kb=$((freed_kb + sz))
    printf '  %-34s %8s\n' "$p" "$(human "$p")"
    rm -rf "$p"
  done
}

echo "[clean] removing dead build dirs + regenerable diagnostic dumps:"

# 1. Dead build trees.
rm_target build-duckstation          # removed DuckStation oracle build (~770M)

# 2. Empty dead scaffolding dirs (only if truly empty — rmdir refuses otherwise).
for d in recompiler overrides harness; do [ -d "$d" ] && rmdir "$d" 2>/dev/null && echo "  $d (empty dir)"; done

# 3. Large regenerable scratch dumps: frame/GPU/audio traces, logs, stale parallel-agent dirs.
rm_target "scratch/frames"
rm_target "scratch/gpustream"
rm_target "scratch/screenshots"
rm_target "scratch/logs"
rm_target "scratch/raw"
rm_target "scratch/wav"
rm_target "scratch/spuperf"
rm_target "scratch/fmvdev"
rm_target "scratch/objdbg"
rm_target "scratch/gpuobj"
rm_target "scratch/gputest"
rm_target "scratch/spudev"
rm_target "scratch/mdecdev"
rm_target "scratch/mdecfixdev"
rm_target "scratch/cont"
rm_target "scratch/agent-*"           # stale parallel-subagent OBJDIR/EXE build dirs

# 4. Loose stray artifacts in scratch root.
rm_target "scratch/*.o"
rm_target "scratch/*.log"
rm_target "scratch/shot*.png"

echo "[clean] freed ~$((freed_kb/1024)) MB"
