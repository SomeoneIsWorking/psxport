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
repo="$(cd "$(dirname "$0")/.." && pwd)"
# psxport is a game-AGNOSTIC framework, usually VENDORED (external/psxport) by a game repo. The RAM
# dumps, the Ghidra project and the decompiled output all belong to the GAME, so scratch/ must resolve
# against the caller's repo — not the framework's, which would silently point at an empty project dir
# and fail with "Could not find project". Use the cwd when it looks like a project root.
workdir="${PSXPORT_WORKDIR:-$PWD}"
if [ ! -d "$workdir/scratch" ] && [ ! -d "$workdir/.git" ]; then workdir="$repo"; fi
cd "$workdir"
# Ghidra headless: use `pyghidraRun -H` resolved from $PATH (put a symlink to it on your PATH, per
# user directive 2026-07-03 — no filesystem paths so a future upgrade only touches the symlink target).
# Ghidra 12 dropped bundled Jython; `analyzeHeadless` alone can't run our `ghidra_decomp.py`
# postScript ("Ghidra was not started with PyGhidra. Python is not available"). `pyghidraRun -H`
# invokes AnalyzeHeadless via the PyGhidra script provider so .py scripts work — same CLI shape.
HEADLESS_BIN="pyghidraRun"
HEADLESS_ARGS=(-H)
PROJDIR="scratch/ghidra"
PROC="MIPS:LE:32:default"
mkdir -p "$PROJDIR" scratch/decomp scratch/logs

# WORKFLOW GUARD (2026-07-15): before any from-scratch decompile, cross-reference each requested address
# against the CODEMAP + RE MAP and shout if it is already owned/documented. A from-scratch Ghidra RE of an
# already-ported FUN_xxxx (or one on a RAM dump whose overlay/state differs) silently MISLABELS it — this
# happened (0x80075824 read as a "sprite fade" when the codemap has it as MusicCoord::voiceMixTick, audio).
# CLAUDE.md already mandates `codemap.py --addr` first; this makes decomp.sh enforce it, not just advise.
codemap_crossref() {
  local any_owned=0 a low
  for a in "$@"; do
    case "$a" in 0x[0-9A-Fa-f]*|[0-9A-Fa-f][0-9A-Fa-f]*) : ;; *) continue ;; esac
    low="$(python3 tools/codemap.py --addr "$a" 2>/dev/null | head -1)"
    [ -z "$low" ] && continue
    if printf '%s' "$low" | grep -qiE '\[LIVE\]|native owner'; then
      if printf '%s' "$low" | grep -qi '\[LIVE\]'; then
        echo "  ⚠ OWNED   $low" >&2; any_owned=1
      else
        echo "    unowned $low" >&2
      fi
    fi
    grep -niE "$(printf '%s' "$a" | sed 's/^0x//I')" docs/port-progress.md docs/engine_re.md 2>/dev/null \
      | grep -iE 'frontier|own|LIVE|=|render|native' | head -1 | sed 's/^/    map: /' >&2 || true
  done
  if [ "$any_owned" = 1 ]; then
    echo "  ⚠⚠ One or more addresses are ALREADY OWNED natively. READ THE PORT (codemap path above)" >&2
    echo "     instead of trusting a fresh decompile — the dump may hold different/overlay code, and" >&2
    echo "     re-deriving an owned fn risks mislabeling it. Ctrl-C now if you skipped the codemap." >&2
  fi
}

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
    echo "[decomp] codemap/RE-map cross-reference (WORKFLOW GUARD):" >&2
    codemap_crossref "$@"
    "$HEADLESS_BIN" "${HEADLESS_ARGS[@]}" "$PROJDIR" "$proj" -process -noanalysis \
      -scriptPath "$repo/tools" -postScript ghidra_decomp.py "$out" "$@" \
      -scriptlog scratch/logs/ghidra.log
    ;;
  all)
    dump="${1:?dump.bin}"; out="${2:?out.c}"; shift 2
    proj="$(basename "$dump" .bin)"
    echo "[decomp] codemap/RE-map cross-reference (WORKFLOW GUARD):" >&2
    codemap_crossref "$@"
    "$HEADLESS_BIN" "${HEADLESS_ARGS[@]}" "$PROJDIR" "$proj" -overwrite \
      -import "$dump" -loader BinaryLoader -loader-baseAddr 0x80000000 \
      -processor "$PROC" \
      -scriptPath "$repo/tools" -postScript ghidra_decomp.py "$out" "$@" \
      -scriptlog scratch/logs/ghidra.log
    ;;
  *) echo "unknown: $cmd" >&2; exit 2;;
esac
