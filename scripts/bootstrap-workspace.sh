#!/usr/bin/env bash
# bootstrap-workspace.sh — reproduce the PSX-port WORKSPACE from a clone of THIS repo.
#
#   git clone https://github.com/SomeoneIsWorking/psxport.git ~/repo/psx/psxport
#   ~/repo/psx/psxport/scripts/bootstrap-workspace.sh
#
# This repo IS the durable half of the workspace (docs/workspace/, docs/plans/, and the framework
# itself). This script adds the rest: the game repos as siblings, their submodules, and the
# ~/repo/psx/CLAUDE.md symlink. See docs/workspace/WORKSPACE.md for the layout and the rules.
#
# IT CANNOT REPRODUCE EVERY TREE, AND IT SAYS SO RATHER THAN PRINTING "done" OVER A PARTIAL
# WORKSPACE. Game trees that have no remote yet exist on ONE machine only; this script names them,
# with their last known local head where it can read it, instead of letting a fresh machine infer
# from a clean exit that the workspace is complete. Give a tree a remote and move it from
# LOCAL_ONLY to REMOTE_BACKED below in the same change.
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
# leaving them empty with every file staged-deleted (measured 2026-08-11 creating the dev clone; the
# same abort is the incident recorded in docs/findings/workspace-incidents.md, where the fix for
# scripts/sync-submodules.sh is described). The reset is what repairs that half-checkout.
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

REMOTE_BACKED=(spyro spider1 Tomba2Engine vagrant megamanx4)
LOCAL_ONLY=()                          # no non-reproducible tree is currently registered here

cloned=0; present=0
for g in "${REMOTE_BACKED[@]}"; do
  if [ -d "$PSX/$g/.git" ]; then
    say "$g: present ($(git -C "$PSX/$g" rev-parse --short HEAD))"; present=$((present+1))
  else
    say "cloning $g…"
    git clone "$GH/$g.git" "$PSX/$g"; cloned=$((cloned+1))
  fi
  say "$g: init external/psxport (a READ-ONLY pinned consumer — never edit it)"
  git -C "$PSX/$g" submodule update --init external/psxport
  init_vendors "$PSX/$g/external/psxport"
done

# Local-only trees: init what is here, and be explicit when it is NOT here. A fresh machine must not
# read this script's exit 0 as "the workspace is complete".
missing=()
for g in "${LOCAL_ONLY[@]}"; do
  if [ -d "$PSX/$g/.git" ]; then
    say "$g: present ($(git -C "$PSX/$g" rev-parse --short HEAD)) — LOCAL ONLY, has no remote"
    git -C "$PSX/$g" submodule update --init external/psxport 2>/dev/null || \
      say "$g: external/psxport not initialised (no gitlink recorded yet)"
    [ -d "$PSX/$g/external/psxport/.git" ] && init_vendors "$PSX/$g/external/psxport"
  else
    missing+=("$g")
  fi
done

# The workspace map, as a symlink into this repo, so it survives a machine switch and cannot drift.
if [ ! -e "$PSX/CLAUDE.md" ]; then
  ln -s "psxport/docs/workspace/WORKSPACE.md" "$PSX/CLAUDE.md"
  say "linked $PSX/CLAUDE.md -> psxport/docs/workspace/WORKSPACE.md"
fi
# Machine-local, ephemeral only: the area locks live here (a lock coordinates the agents on THIS
# machine, so it is deliberately not a tracked file). Nothing durable goes in coord/.
mkdir -p "$PSX/coord/claims"

# The accounting, with its denominator. "done" alone would be the lie.
total=$(( ${#REMOTE_BACKED[@]} + ${#LOCAL_ONLY[@]} ))
have=$(( present + cloned + ${#LOCAL_ONLY[@]} - ${#missing[@]} ))
say "game trees: $have of $total present ($cloned cloned, $present already here)"
if [ ${#missing[@]} -gt 0 ]; then
  printf '\033[1;33m[bootstrap] NOT REPRODUCED — %d of %d game trees are missing and this script\n'  \
         "${#missing[@]}" "$total"
  printf '            CANNOT fetch them: %s\n' "${missing[*]}"
  printf '            They exist on one machine only (no remote). This workspace is INCOMPLETE.\n'
  printf '            To fix permanently: create a remote for each, then move it from LOCAL_ONLY to\n'
  printf '            REMOTE_BACKED in %s.\033[0m\n' "${BASH_SOURCE[0]#"$PSX/"}"
fi
say "Framework edits go in $PSXPORT and NOWHERE else."
say "Build a game against it with: PSXPORT_DIR=$PSXPORT ./run.sh"
say "A disc image is NOT provisioned here — see each game's CLAUDE.md for how it resolves one."
