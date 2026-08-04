#!/usr/bin/env bash
# Sync this repo's submodules to their recorded gitlinks, safely and VISIBLY.
#
# Every consuming game's run.sh calls this; it exists because each of them had grown its own copy
# with the same two defects.
#
#   1. THE DIRT GUARD CHECKED ONE HARDCODED SUBMODULE, THEN UPDATED ALL OF THEM.
#      spider1 and spyro checked `external/psxport`; Tomba2Engine checked `vendor/beetle-psx`. Each
#      then ran `git submodule update --recursive`, which touches every submodule including nested
#      ones. So the promise "never clobber local work" held for exactly one path per repo, and
#      Tomba2Engine would discard uncommitted framework work while carefully protecting beetle.
#      Here the guard covers EVERY submodule, recursively, and names the ones that blocked the sync.
#
#   2. THE SYNC WAS SILENT ABOUT WHAT IT MOVED.
#      `git submodule status` marks '+' when a submodule's CHECKOUT differs from the recorded
#      gitlink. That happens after a pull (stale checkout — sync is right), and it also happens when
#      someone deliberately checks out a different commit to test it (sync DISCARDS their work).
#      The old message, "updating git submodules to recorded commits…", could not tell those apart,
#      so a revert looked exactly like a no-op. That cost a real measurement: a port was checked out
#      onto a fixed framework, run.sh silently put it back on the broken one, and the run that
#      followed measured the wrong binary while looking entirely plausible. Every move is now
#      reported as `path: <old> -> <new>`, so a revert is impossible to miss.
#
# Run from a consuming repo's root. Exits non-zero only on a genuine failure; a skipped sync is a
# warning, because refusing to clobber local work is correct behaviour, not an error.
set -eu

say()  { printf '\033[1;36m[submodules]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[submodules]\033[0m %s\n' "$*" >&2; }

command -v git >/dev/null || { say "git not found — skipping submodule sync"; exit 0; }
[ -f .gitmodules ]        || { say "no .gitmodules here — nothing to sync"; exit 0; }

# `path sha` for every submodule, recursively, with git's status prefix stripped from the sha.
sub_state() { git submodule status --recursive 2>/dev/null | sed 's/^[-+U]//' | awk '{print $2, $1}' | sort; }

status_raw="$(git submodule status --recursive 2>/dev/null || true)"

# '-' = never initialised. Nothing local can be lost, so just bring it up.
if printf '%s\n' "$status_raw" | grep -q '^-'; then
  say "initializing submodules…"
  # A nested submodule with no URL in .gitmodules (beetle-psx registers deps/lightning/gnulib that
  # way) makes a fully recursive init report failure for that path alone. It is unused, so warn
  # rather than abort — the psxport and beetle checkouts are what the build needs.
  git submodule update --init --recursive \
    || warn "some nested submodules did not init (expected for beetle-psx/deps/lightning/gnulib)"
fi

# '+' = a checkout differs from its recorded gitlink.
# Report the in-sync case too, WITH ITS DENOMINATOR. Exiting silently here would make "checked every
# submodule and all of them match" look identical to "this script never ran" — and the build that
# follows is only trustworthy if you know which one happened.
status_now="$(git submodule status --recursive 2>/dev/null || true)"
n_subs="$(printf '%s\n' "$status_now" | grep -c . || true)"
if ! printf '%s\n' "$status_now" | grep -q '^+'; then
  say "$n_subs submodule(s) checked, all at this repo's recorded gitlinks"
  exit 0
fi

# THE GUARD: every submodule, not one hardcoded name. `foreach` already recurses, and $displaypath
# is the path relative to where we started, which is what a human needs in order to go look.
dirty="$(git submodule foreach --recursive --quiet \
           'if [ -n "$(git status --porcelain 2>/dev/null)" ]; then echo "$displaypath"; fi' 2>/dev/null || true)"

if [ -n "$dirty" ]; then
  warn "NOT syncing — these submodules have uncommitted changes and a sync would discard them:"
  printf '%s\n' "$dirty" | sed 's/^/    /' >&2
  warn "commit that work (the operator lands framework changes), then re-run."
  warn "the build will use the CHECKED-OUT commits, which differ from this repo's recorded gitlinks."
  exit 0
fi

before="$(sub_state)"
git submodule update --recursive \
  || warn "some nested submodules did not update (expected for beetle-psx/deps/lightning/gnulib)"
after="$(sub_state)"

# Report every path whose sha actually moved. Printing the transition is the whole point: a sync
# that reverts a deliberately checked-out commit must never be mistakable for a no-op.
moved="$(join -j 1 <(printf '%s\n' "$before") <(printf '%s\n' "$after") 2>/dev/null \
         | awk '$2 != $3 { printf "    %s: %s -> %s\n", $1, substr($2,1,10), substr($3,1,10) }')"

if [ -n "$moved" ]; then
  say "synced submodules to this repo's recorded gitlinks:"
  printf '%s\n' "$moved"
else
  say "submodules already at their recorded gitlinks"
fi
