#!/usr/bin/env bash
# bootstrap-workspace.sh — reproduce the PSX-port WORKSPACE from a clone of THIS repo.
#
#   git clone https://github.com/SomeoneIsWorking/psxport.git ~/repo/psx/psxport
#   ~/repo/psx/psxport/scripts/bootstrap-workspace.sh
#
# This repo IS the durable half of the workspace (docs/workspace/, docs/plans/, and the framework
# itself). This script adds the rest: the three game repos as siblings, their submodules, and the
# ~/repo/psx/CLAUDE.md symlink. See docs/workspace/WORKSPACE.md for the layout and the rules.
#
# It does NOT fetch a disc image (never in a repo) and does NOT build anything — run each game's
# ./run.sh for that.
set -euo pipefail
PSXPORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PSX="$(dirname "$PSXPORT")"
say() { printf '\033[1;36m[bootstrap]\033[0m %s\n' "$*"; }

GH=https://github.com/SomeoneIsWorking

# Vendors, ONE AT A TIME and NON-recursively. `--recurse-submodules` / `--recursive` ABORT on
# beetle-psx's URL-less nested deps/lightning/gnulib and stop BEFORE the submodules that follow,
# leaving them empty with every file staged-deleted (measured 2026-08-11 creating the dev clone; see
# docs/workspace/KNOWN-DEFECT-sync-submodules.md). The reset is what repairs that half-checkout.
init_vendors() {                       # init_vendors <psxport checkout>
  local root="$1"
  for sm in vendor/beetle-psx vendor/lucent; do
    git -C "$root" submodule update --init "$sm"
    git -C "$root/$sm" reset --hard -q HEAD
  done
  # libchdr lives under beetle-psx and IS needed (CHD disc access); gnulib is not.
  git -C "$root/vendor/beetle-psx" submodule update --init deps/libchdr
}

say "framework dev clone: $PSXPORT"
init_vendors "$PSXPORT"

for g in spyro spider1 Tomba2Engine; do
  if [ -d "$PSX/$g/.git" ]; then
    say "$g: present ($(git -C "$PSX/$g" rev-parse --short HEAD))"
  else
    say "cloning $g…"
    git clone "$GH/$g.git" "$PSX/$g"
  fi
  say "$g: init external/psxport (a READ-ONLY pinned consumer — never edit it)"
  git -C "$PSX/$g" submodule update --init external/psxport
  init_vendors "$PSX/$g/external/psxport"
done

# The workspace map, as a symlink into this repo, so it survives a machine switch and cannot drift.
if [ ! -e "$PSX/CLAUDE.md" ]; then
  ln -s "psxport/docs/workspace/WORKSPACE.md" "$PSX/CLAUDE.md"
  say "linked $PSX/CLAUDE.md -> psxport/docs/workspace/WORKSPACE.md"
fi
# Machine-local, ephemeral only: the area locks live here (a lock coordinates the agents on THIS
# machine, so it is deliberately not a tracked file). Nothing durable goes in coord/.
mkdir -p "$PSX/coord/claims"

say "done. Framework edits go in $PSXPORT and NOWHERE else."
say "Build a game against it with: PSXPORT_DIR=$PSXPORT ./run.sh"
say "A disc image is NOT provisioned here — see each game's CLAUDE.md for how it resolves one."
