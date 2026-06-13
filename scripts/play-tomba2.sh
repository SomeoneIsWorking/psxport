#!/usr/bin/env bash
# Play Tomba! 2 in the psxport runtime (wide60rt) — interactive SDL window.
#
# Active enhancements:
#   - cull-cone widening (native override, runtime/games/tomba2.cpp)
#   - instant CD + fastboot OpenBIOS (default)
#   - wide60 reprojecting renderer: capture is ON; the 60fps in-between PRESENT
#     is not wired yet, so it currently plays at the game's native 30fps. Set
#     WIDE60=0 to disable the (currently visual-no-op) capture hooks.
#
# Controls: arrows = d-pad; Z/X/A/S = Cross/Circle/Square/Triangle;
#   Enter/RShift = Start/Select; Q/E = L1/R1; W/R = L2/R2;
#   Tab = hold to fast-forward; F5/F9 = quicksave/quickload; Esc = quit.
#
# Usage: scripts/play-tomba2.sh [disc.chd]
#   disc: $1 > PSXPORT_TOMBA2_DISC (env or .env) > Tomba*2*.chd drop-in
set -eu

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

# --- build (keeps the binary current) ----------------------------------------
make -C "$repo/runtime"

bios="$repo/bios"
[ -f "$bios/scph5501.bin" ] || { echo "BIOS not found in $bios (need scph5501.bin)" >&2; exit 1; }

# wide60 capture hooks (no visible effect until the present pipeline lands).
export PSXPORT_WIDE60="${WIDE60:-1}"
[ "$PSXPORT_WIDE60" = "0" ] && unset PSXPORT_WIDE60

echo "Playing: $disc"
exec "$repo/runtime/wide60rt" "$disc" -bios "$bios" -play
