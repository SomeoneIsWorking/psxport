#!/usr/bin/env bash
# Decompile a PSX RAM dump (2 MB, KSEG0) to C with headless Ghidra.
#
# Usage:
#   tools/decomp.sh import <dump.bin> [projname]      # import + auto-analyze (slow, once)
#   tools/decomp.sh decomp <projname> <out.c> [lo hi] # decompile a [lo,hi) entry-addr range
#   tools/decomp.sh decomp <projname> <out.c> list <addr...>   # decompile an explicit fn list
#   tools/decomp.sh all <dump.bin> <out.c> [lo hi | list <addr...>]   # import+analyze+decompile
#
# The dump is loaded as a raw binary based at 0x80000000 so addresses match the
# virtual addresses in docs/journal.md. Projects live in scratch/ghidra/ (gitignored);
# output C goes wherever you point <out.c> (default convention: scratch/decomp/).
set -eu
repo="$(cd "$(dirname "$0")/.." && pwd)"; cd "$repo"
# Ghidra headless: use `pyghidraRun -H` from $PATH (symlink at <HOME>/.local/bin, per user directive
# 2026-07-03 — no filesystem paths so a future upgrade only touches the symlink target).
# Ghidra 12 dropped bundled Jython; `analyzeHeadless` alone can't run our `ghidra_decomp.py`
# postScript ("Ghidra was not started with PyGhidra. Python is not available"). `pyghidraRun -H`
# invokes AnalyzeHeadless via the PyGhidra script provider so .py scripts work — same CLI shape.
HEADLESS_BIN="pyghidraRun"
HEADLESS_ARGS=(-H)
PROJDIR="scratch/ghidra"
PROC="MIPS:LE:32:default"
mkdir -p "$PROJDIR" scratch/decomp scratch/logs

cmd="${1:?import|decomp|all}"; shift
case "$cmd" in
  import)
    dump="${1:?dump.bin}"; proj="${2:-$(basename "$dump" .bin)}"
    "$HEADLESS_BIN" "${HEADLESS_ARGS[@]}" "$PROJDIR" "$proj" -overwrite \
      -import "$dump" -loader BinaryLoader -loader-baseAddr 0x80000000 \
      -processor "$PROC" -scriptlog scratch/logs/ghidra.log
    ;;
  decomp)
    proj="${1:?projname}"; out="${2:?out.c}"; shift 2
    "$HEADLESS_BIN" "${HEADLESS_ARGS[@]}" "$PROJDIR" "$proj" -process -noanalysis \
      -scriptPath "$repo/tools" -postScript ghidra_decomp.py "$out" "$@" \
      -scriptlog scratch/logs/ghidra.log
    ;;
  all)
    dump="${1:?dump.bin}"; out="${2:?out.c}"; shift 2
    proj="$(basename "$dump" .bin)"
    "$HEADLESS_BIN" "${HEADLESS_ARGS[@]}" "$PROJDIR" "$proj" -overwrite \
      -import "$dump" -loader BinaryLoader -loader-baseAddr 0x80000000 \
      -processor "$PROC" \
      -scriptPath "$repo/tools" -postScript ghidra_decomp.py "$out" "$@" \
      -scriptlog scratch/logs/ghidra.log
    ;;
  *) echo "unknown: $cmd" >&2; exit 2;;
esac
