#!/bin/sh
# Launches Tomba! 2 in the wide60 DuckStation fork with all psxport enhancements:
#   - wide60 60fps interpolation + GTE object tagging (forces CPU interpreter)
#   - cull-cone widening pokes (patches/tomba2/cull-widen.md)
# Disc resolution: $1 > PSXPORT_TOMBA2_DISC (env or .env) > Tomba*2*.chd drop-in.
set -eu
repo="$(cd "$(dirname "$0")/.." && pwd)"

# .env values are raw (unquoted, may contain spaces) — parse, don't source
env_disc=""
[ -f "$repo/.env" ] && env_disc="$(sed -n 's/^PSXPORT_TOMBA2_DISC=//p' "$repo/.env" | head -1)"
disc="${1:-${PSXPORT_TOMBA2_DISC:-$env_disc}}"
if [ -z "$disc" ]; then
  for f in "$repo"/Tomba*2*.chd; do [ -f "$f" ] && disc="$f" && break; done
fi
[ -n "$disc" ] && [ -f "$disc" ] || {
  echo "Tomba! 2 CHD not found. Pass it as arg, set PSXPORT_TOMBA2_DISC in .env," >&2
  echo "or drop 'Tomba! 2 ... .chd' into the repo root." >&2
  exit 1
}

qt="$repo/build-duckstation/bin/duckstation-qt"
[ -x "$qt" ] || { echo "duckstation-qt not built (cmake --build build-duckstation)" >&2; exit 1; }

export PSXPORT_WIDE60=1
# Tomba! 2 cull-cone widening (six slti thresholds, conditional old:new form);
# documented in patches/tomba2/cull-widen.md
export PSXPORT_POKE="800772D4=28620370:28620278;80077368=28620358:28620268;80077414=28620358:28620268;800774A8=28620370:28620278;8007753C=28620350:28620260;800775D0=28620368:28620270"

exec "$qt" -- "$disc"
