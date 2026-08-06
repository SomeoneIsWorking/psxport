#!/usr/bin/env bash
# Sync this repo's submodules to their recorded gitlinks, safely and VISIBLY.
#
# Every consuming game's run.sh calls this; it exists because each of them had grown its own copy
# with the same defects.
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
#   3. IT CERTIFIED AN ENUMERATION IT NEVER MADE — the defect this file now exists to prevent.
#      `git submodule status --recursive` and `git submodule foreach --recursive` both ABORT partway
#      through these repos: beetle-psx registers a nested `deps/lightning/gnulib` with no URL, git
#      gives up at that path, and NEVER REACHES `vendor/lucent`.
#
#          fatal: no submodule mapping found in .gitmodules for path 'deps/lightning/gnulib'
#          fatal: failed to recurse into submodule 'vendor/beetle-psx'
#
#      The old code ran both under `|| true`, so the non-zero exit vanished and a PARTIAL result was
#      reported as a clean bill of health: "2 submodule(s) checked, all at this repo's recorded
#      gitlinks" — measured, with vendor/lucent genuinely off its gitlink at the time. It saw 2 of 3
#      submodules in spider1/Tomba2Engine and 3 of 4 in spyro. Three builds died of it in one day
#      (`ot_attr.h` needs lucent >= 07c5836; the stale checkout at 02ea34b could not compile), and
#      the dirt guard was blind to the same path, so "never clobber local work" did not cover
#      vendor/lucent either.
#
#      The cause is not the `|| true` — it is that the check HAD NO DENOMINATOR, so a short
#      enumeration and a complete one were indistinguishable. Both are fixed here:
#
#        * `enumerate` walks the DECLARED submodules itself, recursively, straight out of each
#          level's `.gitmodules`, and compares each recorded gitlink (the superproject's index)
#          against the checkout's own HEAD. That walk cannot be truncated by an unmapped nested
#          path, because an undeclared gitlink is simply not one of the things it is looking for.
#        * Every declared path it could NOT resolve is collected and NAMED, and the script then
#          REFUSES to certify — `checked N of M … CANNOT SEE: <paths>`, exit non-zero. There is no
#          arm of this script that reports "all at recorded gitlinks" over an incomplete walk.
#        * git's own `submodule status --recursive` is kept purely as a cross-check in the other
#          direction: anything IT lists that the walk did not produce means the walk has a hole, and
#          that is also a refusal.
#
# Run from a consuming repo's root. Exits non-zero on a genuine failure — which now includes "this
# script cannot see the whole submodule set", because certifying that case is what caused the bug.
set -eu

say()  { printf '\033[1;36m[submodules]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[submodules]\033[0m %s\n' "$*" >&2; }

command -v git >/dev/null || { say "git not found — skipping submodule sync"; exit 0; }
[ -f .gitmodules ]        || { say "no .gitmodules here — nothing to sync"; exit 0; }

TOP="$(pwd)"

# ---- the enumerator ----------------------------------------------------------------------------
# Fills three globals, and it is the ONLY place submodule state is read:
#
#   ENUM       one line per DECLARED submodule:  "<display_path> <recorded_sha> <checkout_sha|->"
#              '-' in the third field = declared and recorded, but not checked out here.
#   BLIND      one line per declared path this run could not resolve: "<display_path>\t<why>"
#   UNMANAGED  one line per gitlink recorded in some index that NO .gitmodules declares.
#
# `walk` recurses through DECLARED paths only. That is the whole point: the thing that breaks git's
# own recursion is an UNDECLARED gitlink (beetle-psx's deps/lightning/gnulib — measured: beetle-psx
# has no .gitmodules at all, and one 160000 entry for that path), and a walk driven by .gitmodules
# never looks at it. A submodule that is not checked out hides its own .gitmodules, so its children
# are unknowable — that is recorded as blindness rather than skipped silently.
ENUM=""
BLIND=""
UNMANAGED=""

blind() { BLIND="${BLIND}$1	$2
"; }

walk() { # $1 = repo directory, $2 = display-path prefix ("" at the top)
  local repo="$1" pfx="$2" decl="" path disp rec chk gp
  [ ! -f "$repo/.gitmodules" ] \
    || decl="$(git config -f "$repo/.gitmodules" --get-regexp '^submodule\..*\.path$' 2>/dev/null \
               | sed 's/^[^ ]* //' || true)"

  # Gitlinks this repo's index records that its .gitmodules does not declare. git has no URL for
  # them, so it can neither clone nor update them — they are outside the set this script is able to
  # certify, and they are precisely what aborts git's own recursion. Reporting them is not a
  # courtesy: a count of "4 of 4 checked" that silently omitted an entire class of gitlink would be
  # the same lie in a smaller font, so the blind spot travels with every verdict below.
  while read -r gp; do
    [ -n "${gp:-}" ] || continue
    printf '%s\n' "$decl" | grep -qxF -- "$gp" || UNMANAGED="${UNMANAGED}$pfx$gp
"
  done < <(git -C "$repo" ls-files -s 2>/dev/null | awk '$1=="160000"{sub(/^[^\t]*\t/,""); print}' || true)

  [ -n "$decl" ] || return 0
  while read -r path; do
    [ -n "${path:-}" ] || continue
    disp="$pfx$path"

    rec="$(git -C "$repo" ls-files -s -- "$path" 2>/dev/null | awk '$1=="160000"{print $2; exit}')"
    if [ -z "$rec" ]; then
      # Declared in .gitmodules but the superproject's index has no gitlink for it. Nothing to
      # compare against, so this path's sync state is unknown — not "fine".
      blind "$disp" "declared in .gitmodules but no gitlink in this repo's index"
      continue
    fi

    if [ -e "$repo/$path/.git" ]; then
      chk="$(git -C "$repo/$path" rev-parse HEAD 2>/dev/null || true)"
      if [ -z "$chk" ]; then
        blind "$disp" "checkout exists but is not a readable git repo (HEAD unreadable)"
        continue
      fi
    else
      chk="-"
    fi

    ENUM="${ENUM}$disp $rec $chk
"
    if [ "$chk" = "-" ]; then
      blind "$disp" "not checked out — and nothing IT declares can be seen from here"
    else
      walk "$repo/$path" "$disp/"
    fi
  done <<< "$decl"
}

enumerate() { ENUM=""; BLIND=""; UNMANAGED=""; walk "$TOP" ""; }

enum_paths()   { printf '%s' "$ENUM" | awk 'NF{print $1}' | sort; }
enum_offpin()  { printf '%s' "$ENUM" | awk '$3 != "-" && $2 != $3 {print $1}'; }
enum_uninit()  { printf '%s' "$ENUM" | awk '$3 == "-" {print $1}'; }
# RESOLVED = recorded gitlink AND checkout HEAD both read, i.e. genuinely compared. This is the
# numerator; it is deliberately NOT "lines of output from some git command".
enum_seen()    { printf '%s' "$ENUM" | awk '$3 != "-" {print $1}' | sort; }
blind_paths()  { printf '%s' "$BLIND" | awk -F'\t' 'NF{print $1}' | sort -u; }
# The DENOMINATOR: every path this repo declares, whether or not it could be resolved. A path can be
# in both lists (declared, gitlink read, but not checked out), hence -u over the union.
all_paths()    { { enum_paths; blind_paths; } | sort -u; }
count()        { printf '%s' "$1" | grep -c . || true; }

# ---- 1. enumerate, initialising anything that has never been checked out ------------------------
enumerate
if [ -n "$(enum_uninit)" ]; then
  say "initializing submodules…"
  # A nested submodule with no URL in .gitmodules (beetle-psx registers deps/lightning/gnulib that
  # way) makes a fully recursive init report failure for that path alone. It is unused, so warn
  # rather than abort — and the enumeration below, not this command's exit status, decides whether
  # the result is acceptable.
  git submodule update --init --recursive \
    || warn "some nested submodules did not init (expected for beetle-psx/deps/lightning/gnulib)"
  enumerate
fi

# ---- 2. REFUSE rather than certify a partial view -----------------------------------------------
# Two independent ways the view can be short, both fatal to a "they all match" claim:
#   (a) a declared path the walk could not resolve;
#   (b) a path git's own recursion listed that the walk did not produce — i.e. the walk has a hole.
observed_extra="$(comm -23 \
  <(git submodule status --recursive 2>/dev/null | sed 's/^[-+U]//' | awk 'NF{print $2}' | sort -u) \
  <(enum_paths) || true)"
if [ -n "$observed_extra" ]; then
  while read -r p; do [ -n "$p" ] && blind "$p" "listed by git's own recursion but MISSED by this walk"; done \
    <<< "$observed_extra"
fi

n_declared="$(count "$(all_paths)")"
n_seen="$(count "$(enum_seen)")"

# Every verdict this script prints carries its blind spot. On these repos that is exactly one path,
# vendor/beetle-psx/deps/lightning/gnulib, and stating it on the same line as the count is what makes
# "4 of 4" mean something: the reader can see what the 4 does NOT include.
unmanaged_note=""
if [ -n "$UNMANAGED" ]; then
  unmanaged_note=" — NOT covered (gitlink(s) no .gitmodules declares, so git itself cannot sync them): $(
    printf '%s' "$UNMANAGED" | tr '\n' ' ' | sed 's/ *$//')"
fi

if [ -n "$BLIND" ]; then
  warn "checked $n_seen of $n_declared submodule(s)$unmanaged_note — CANNOT SEE:"
  printf '%s' "$BLIND" | awk -F'\t' 'NF{printf "    %s  (%s)\n", $1, $2}' >&2
  warn "refusing to certify: this script cannot tell whether those are at their recorded gitlinks,"
  warn "and reporting 'all in sync' over a partial enumeration is the defect this check exists for."
  warn "fix the listed paths (usually: git submodule update --init --recursive), then re-run."
  exit 1
fi

# ---- 3. the in-sync case, WITH ITS DENOMINATOR --------------------------------------------------
# Reporting this is not noise: exiting silently would make "checked every submodule and all of them
# match" look identical to "this script never ran", and the build that follows is only trustworthy
# if you know which one happened. The count is now the number of submodules the repo DECLARES, and
# step 2 has already proved the walk saw all of them.
offpin="$(enum_offpin)"
if [ -z "$offpin" ]; then
  say "checked $n_declared of $n_declared submodule(s), all at this repo's recorded gitlinks$unmanaged_note"
  exit 0
fi

# ---- 4. THE GUARD: every submodule, not one hardcoded name --------------------------------------
# Driven by the walk for the same reason as everything else: `git submodule foreach --recursive`
# aborts at beetle-psx's unmapped nested path (measured: it enumerates 3 of 4 paths in spyro), so a
# guard built on it silently does not protect vendor/lucent.
#
# `--ignore-submodules=all` is load-bearing, not a loosening. A submodule whose OWN nested submodule
# is off its pin shows up in `git status --porcelain` as ` M vendor/lucent` — pointer drift, not
# local work. Counting that as dirt makes the guard refuse to sync in exactly the situation the sync
# exists for (measured on the fixture: lucent one commit behind its gitlink made external/framework
# read as "uncommitted changes", and the script declined to fix it). Nothing is lost, because each
# submodule is checked in its own right by this same loop — dirt in vendor/lucent is reported
# against vendor/lucent. Validated against both classes: pointer drift -> clean; an untracked file
# in the same checkout -> `?? DIRT`.
dirty=""
while read -r p _rec _chk; do
  [ -n "${p:-}" ] || continue
  if [ -n "$(git -C "$TOP/$p" status --porcelain --ignore-submodules=all 2>/dev/null)" ]; then dirty="$dirty$p
"; fi
done <<< "$ENUM"

if [ -n "$dirty" ]; then
  warn "NOT syncing — these submodules have uncommitted changes and a sync would discard them:"
  printf '%s' "$dirty" | sed 's/^/    /' >&2
  warn "commit that work (the operator lands framework changes), then re-run."
  warn "the build will use the CHECKED-OUT commits, which differ from this repo's recorded gitlinks."
  exit 0
fi

# ---- 5. sync, then re-enumerate and report what actually moved ----------------------------------
before="$ENUM"
git submodule update --recursive \
  || warn "some nested submodules did not update (expected for beetle-psx/deps/lightning/gnulib)"
enumerate

if [ -n "$BLIND" ]; then
  warn "the sync left submodules this script can no longer see:"
  printf '%s' "$BLIND" | awk -F'\t' 'NF{printf "    %s  (%s)\n", $1, $2}' >&2
  exit 1
fi

moved="$(awk 'NR==FNR { was[$1]=$3; next } NF && was[$1] != $3 {
                 printf "    %s: %s -> %s\n", $1, substr(was[$1],1,10), substr($3,1,10) }' \
           <(printf '%s' "$before") <(printf '%s' "$ENUM"))"

if [ -n "$moved" ]; then
  say "synced submodules to this repo's recorded gitlinks:"
  printf '%s\n' "$moved"
fi

# The post-condition, checked rather than assumed: `git submodule update` is the same recursive
# machinery that cannot reach every path, so "it exited 0" is not evidence that it moved everything.
still="$(enum_offpin)"
if [ -n "$still" ]; then
  warn "sync did NOT bring these to their recorded gitlinks:"
  printf '%s\n' "$still" | sed 's/^/    /' >&2
  warn "the build would use the wrong sources; fix these before building."
  exit 1
fi

[ -n "$moved" ] || say "checked $n_declared of $n_declared submodule(s), all at this repo's recorded gitlinks$unmanaged_note"
