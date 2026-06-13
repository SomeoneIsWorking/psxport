#!/usr/bin/env bash
# Play Tomba! 2 in the psxport runtime with the PURE NATIVE HLE BIOS
# (PSXPORT_HLE_BIOS=1) — no real/patched PSX BIOS ROM required.
#
# Status (2026-06-13): the HLE BIOS boots and renders the Whoopee Camp intro
# logo, then HOLDS at it (the intro frame counter 0x800267B4 is frozen — the
# game's VBLANK callback isn't bumped yet; see docs/tomba2-hle-irq.md). So this
# is "renders the intro, not playable yet". For actual gameplay use the ROM
# path: scripts/play-tomba2.sh (cp bios/openbios-fast.bin bios/scph5501.bin).
#
# Controls: arrows = d-pad; Z/X/A/S = Cross/Circle/Square/Triangle;
#   Enter/RShift = Start/Select; Q/E = L1/R1; W/R = L2/R2;
#   Tab = hold to fast-forward; F12 = screenshot; Esc = quit.
#
# Usage: scripts/play-tomba2-hle.sh [disc.chd]
#   disc: $1 > PSXPORT_TOMBA2_DISC (env or .env) > Tomba*2*.chd drop-in
#
# Env knobs: PSXPORT_FPSLOG=1 prints the measured frame rate;
#            PSXPORT_IRQ_LOG=1 traces the HLE exception/IRQ path (for debugging).
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

# --- submodule sanity: the play loop calls psxport_emulate_frame(), defined in
# the beetle fork. If the submodule is stale the link fails — catch it early. ---
if ! grep -q 'psxport_emulate_frame' vendor/beetle-psx/libretro.c 2>/dev/null; then
  echo "vendor/beetle-psx is stale (missing psxport_emulate_frame). Updating submodule..." >&2
  git submodule sync >/dev/null 2>&1 || true
  git submodule update --init --recursive --force
fi

# --- build (keeps the binary current) ----------------------------------------
make -C "$repo/runtime"

# The HLE BIOS needs no ROM; -bios points at the committed dir only to satisfy
# the arg (its contents are ignored under PSXPORT_HLE_BIOS).
bios="$repo/bios"

echo "Playing (HLE BIOS): $disc"
exec env PSXPORT_HLE_BIOS=1 "$repo/runtime/wide60rt" "$disc" -bios "$bios" -play
