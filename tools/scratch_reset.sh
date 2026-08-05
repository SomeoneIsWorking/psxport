#!/usr/bin/env bash
# scratch_reset.sh — empty a scratch OUTPUT directory, safely and audibly.
#
# WHY IT EXISTS: run artifacts (frame dumps, logs, WAV captures) pile up fast and fill the disk, so
# they do need clearing between runs. But an `rm -rf` typed into an agent's shell raises a permission
# prompt every single time, and a prompt in the middle of a long measurement costs more than the
# cleanup is worth. This is that cleanup as a reviewable, guarded script instead.
#
# THE GUARD IS THE POINT. It refuses any path that is not inside a directory literally named
# `scratch`, refuses to touch the repo root, and resolves the path before checking — so a relative
# path, a symlink, or a `..` cannot walk it out of the sandbox. A cleanup tool that can be pointed at
# the wrong directory is a footgun with a convenient handle.
#
#   tools/scratch_reset.sh scratch/frames/flick      # empty it (created if absent)
#   tools/scratch_reset.sh scratch/logs scratch/wav  # several at once
#
# It reports what it removed, with the count, for every directory — including zero. "removed 0 files"
# and "that path was never cleared" must not look the same.
set -eu

if [ "$#" -eq 0 ]; then
  echo "usage: scratch_reset.sh <dir-under-scratch> [more...]" >&2
  exit 2
fi

for target in "$@"; do
  # Resolve BEFORE validating, so symlinks / .. / relative paths are judged by where they land.
  abs=$(cd "$(dirname "$target")" 2>/dev/null && printf '%s/%s' "$(pwd -P)" "$(basename "$target")") || {
    echo "scratch_reset: parent of '$target' does not exist — refusing" >&2; exit 1; }

  case "$abs" in
    */scratch|*/scratch/*) ;;
    *) echo "scratch_reset: '$target' -> '$abs' is not inside a 'scratch' directory — refusing" >&2
       exit 1 ;;
  esac
  if [ "$(basename "$abs")" = "scratch" ]; then
    echo "scratch_reset: refusing to empty the scratch ROOT ('$abs') — name a subdirectory" >&2
    exit 1
  fi

  if [ -e "$abs" ] && [ ! -d "$abs" ]; then
    echo "scratch_reset: '$abs' exists and is not a directory — refusing" >&2
    exit 1
  fi

  if [ -d "$abs" ]; then
    n=$(find "$abs" -mindepth 1 | wc -l | tr -d ' ')
    find "$abs" -mindepth 1 -delete
    echo "scratch_reset: emptied $target ($n entr$([ "$n" = 1 ] && echo y || echo ies) removed)"
  else
    mkdir -p "$abs"
    echo "scratch_reset: created $target (did not exist; 0 entries removed)"
  fi
done
