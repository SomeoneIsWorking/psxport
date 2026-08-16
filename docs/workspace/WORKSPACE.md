# `~/repo/psx` — the PSX-port WORKSPACE

**Unlabeled content is machine convention, revisable by any session. USER lines are verbatim dated
quotes, and only those.**

**This file is a MAP AND A POINTER, nothing else.** It lives in the psxport repo so it survives a machine
switch and reaches every game tree through that repo's `external/psxport` submodule; `~/repo/psx/CLAUDE.md`
is a SYMLINK to it. The workspace directory itself is NOT a git repo and holds nothing durable.

All of these live in the psxport repo, so they reach every game tree and every subagent by `grep`:

| read this | for |
|---|---|
| **`psxport/CLAUDE.md`** | **how a game consumes the framework** — build/run, CVars, the seam, `generated/`, RE-first, diagnostics, registries, Dusklight. THE authority |
| **`docs/workspace/PROTOCOL.md`** | the multi-agent protocol (area claims) and the standing rules |
| `docs/workspace/LAYOUT.md` · `docs/plans/*.md` | target directory organization · designs not yet implemented |
| `docs/findings/*.md` | measured findings, and the incidents the rules came from |
| `<game>/CLAUDE.md` | that game's own specifics — the authority for that repo |

## What is here

Independent repos live side by side, all public under `github.com/SomeoneIsWorking`. No workspace repo
and no superproject: a game must build from a bare clone of itself, a gitlink at this level would churn on
every game commit, and a recursive clone would pull seven copies of psxport + beetle-psx.

### Target title scope

The target ports are Spyro 1/2/3; Crash 1/2/3; Crash Bash; Crash Team Racing; Vagrant Story; Mega Man
X4; Tomba! 1/2; Tekken 3; and Spider-Man 1/2. Mega Man X4 is already 60 fps, so its enhancement path is
widescreen first and, much later, drop-in co-op — not interpolation. All planned lineage repositories
now have public, reproducible trees. Most newly added titles are honest harness-first scaffolds, not
implementation coverage; no widescreen or interpolation support is implied by repository existence.

| path | what it is |
|---|---|
| `psxport/` | **the framework DEV CLONE — the one writable framework checkout.** Also the home of every doc listed above |
| `Tomba2Engine/` | Tomba! 2 — psxport's reference consumer; also owns the separate Tomba! 1 title project, with no shared `game/` |
| `spyro/` | Spyro 1/2/3, the Insomniac-lineage repository; Spyro 1 (`SCUS_942.28`) is the current implementation |
| `spider1/` | Spider-Man 1/2, the Neversoft-lineage repository; Spider-Man 1 (`SLUS_008.75`, USA) is the current implementation |
| `vagrant/` | Vagrant Story (`SLUS_010.40`, USA). Vendors the CC0 `rood-reverse` decomp. Defining fact: the boot exe is ~15% of the code, 933,925 B lives in `.PRG` overlays |
| `megamanx4/` | Mega Man X4 (`SLUS_005.61`, USA) — the only **enhancement-class** port here: already 60fps, so no native producers, no lerp, no native depth. Wants widescreen + load removal + drop-in co-op. Vendors the AGPL-3.0 `mmx4` decomp, which may NOT be lifted into `psxport` |
| `crash/` | Crash Bandicoot 1/2/3 in one architecture repository; harness-first scaffold |
| `ctr/` | Crash Team Racing; standalone harness-first scaffold |
| `crashbash/` | Crash Bash; standalone harness-first scaffold |
| `tekken3/` | Tekken 3; standalone harness-first scaffold |
| `toystory2/` | Existing Toy Story 2 (`SLUS_008.93`, USA) checkout — not in the active title scope above |
| `coord/` | **UNTRACKED, machine-local, EPHEMERAL ONLY**: `claims/` (the area locks — a lock coordinates the agents on THIS machine, so it must not be tracked), plus agent scratch. Nothing durable belongs here |

`$PSX` in any doc means this workspace root. To reproduce the workspace on a fresh machine:

```sh
git clone https://github.com/SomeoneIsWorking/psxport.git ~/repo/psx/psxport
~/repo/psx/psxport/scripts/bootstrap-workspace.sh   # clones the games, inits vendors, relinks CLAUDE.md
```

All active target repositories are in that script's `REMOTE_BACKED` list. `toystory2` is not because
it is outside the active target scope.

## The structure rule: ONE framework checkout, and every port runs off it

**There is exactly ONE psxport working tree on a machine, and every game uses it.** `psxport/` is that
tree. Each game has `external/psxport`, which is **not tracked and not a submodule** — it is a SYMLINK to
`psxport/` when the workspace is present, or a private clone at that game's `psxport.pin` on a fresh
machine / CI / a stranger's clone of one repo. `tools/psxport_sync.py --auto` (run by `run.sh`)
establishes whichever applies. The PATH is unchanged, so every `external/psxport/...` reference in docs,
tools and code keeps working.

**So a framework edit is live in every port immediately, with no bump, no sync and no ceremony** — which
is the whole point. There is no longer a "read-only consumer" copy to drift from the writable one,
because there is no second copy.

1. **Framework edits happen in the one tree.** Reaching it through `psxport/` or through a game's
   `external/psxport` symlink is the same directory; both are the dev clone. Commit and push framework
   work in `psxport/`.
2. **`psxport.pin` records the framework commit a game was built and VERIFIED against.** It is
   provenance and the fresh-clone fallback, not what you build against day to day. `psxport_sync.py
   --bump` records it; `--check` (wired into each game's precommit gate) FAILS when the framework you
   built against is not the one the repo records, comparing against `build/psxport_resolved.txt`, which
   CMake writes at configure time.
3. **Ports are deliberately NOT all on framework HEAD.** Measured 2026-08-16: six ports spanned 55
   commits of framework history. With one maintainer that is a feature — it is what lets one port be
   worked on daily while the others sit untouched, and it is why the beetle GTE commit that broke
   `PSXPORT_ORACLE=1` in every 3D scene broke one tree rather than six. Bump a port when you are ready
   to re-verify it.
4. **Parallel framework work** is still one `git worktree` off `psxport/` per claim area, with that
   agent's `PSXPORT_DIR` pointing at it (PROTOCOL's).

### Why the submodule was dropped (2026-08-16)

Two incidents in one day, both caused by the mechanism rather than by anyone's mistake:

- Tomba2Engine was **built against psxport `25dd7826` while recording `a1c53d7c`**, so a bare clone did
  not compile — the game's hook table named a `GameHooks` field the pinned framework did not have.
  Nothing noticed, because a submodule working tree and its recorded gitlink drift silently.
- "Fixing" that drift by syncing to the recorded pin is what pulled a **broken beetle GTE commit** into
  the working build; it had already broken `PSXPORT_ORACLE=1` in every 3D scene for two days (8 of 9
  replays segfaulting). That commit was made on a **detached HEAD inside the submodule** — the default
  state of a submodule checkout, and the reason it was never reviewed.

Add to that: `git submodule update --recursive` **fails outright** on this tree, because beetle-psx
carries a URL-less nested gitlink (`deps/lightning/gnulib`) git itself cannot resolve. `psxport`'s own
`vendor/beetle-psx` and `vendor/lucent` remain submodules — that is genuine third-party vendoring inside
one repo, and with a single psxport tree there is no duplication to drift.

## The two things to know even if you read nothing else

- **Never commit** disc images (`*.chd`), extracted executables, `generated/`, or machine-specific
  absolute paths (`/home/<user>/…`). Every game repo ships `tools/go_public.py` to audit history.
- **Never write run artifacts to `/tmp`** — small RAM-backed tmpfs here. Use the repo's git-ignored
  `scratch/`, split by kind. Diagnose "disk quota exceeded" with `quota -s`, not `df`.

## Repo shape: one repo per ENGINE LINEAGE, multiple titles inside it. No third vendored layer

Evidence for every verdict below — matrices, null distributions, the per-decision survival check:
`docs/findings/lineage-metric.md`. A bare similarity percentage means nothing without its multiple of the
measured cross-studio null.

**This call was DELEGATED and is now USER-CONFIRMED, so do not re-litigate it.** USER, 2026-08-11:
*"I'm not gonna decide, maybe Fable should decide"*, then *"it's yours"*. A session may still revise the
shape on new MEASUREMENT, but must not re-open it as a question for the USER.

**USER CONFIRMED the four-repo shape on 2026-08-12** — not a quote: they asked whether Crash would be
bundled with Spyro "like I asked", were shown three concrete layouts (one repo holding all 8
Spyro/Crash/CTR/Bash titles · two repos split by studio · the four repos below), and chose the four. Two
things were put in front of them and did not change the answer: that **their original ask WAS one repo
for Spyro AND Crash** (`docs/findings/lineage-metric.md` records it as "what was first asked for"), and
that the measured family signal is weak in BOTH directions — `SPYRO1↔SPYRO2` reads 2.25× the null on the
whole-function tool but **5.6% = 0.8×, BELOW the floor** on Jaccard, with `CRASH1↔CRASH2` inverted the
same way, so the evidence for grouping the Spyro trio is barely stronger than the evidence against
adding Crash to it. Reversal was also free at that moment: no `crash/`, `ctr/` or `crashbash/` existed
and `spyro/` had not converted to multi-title, so nothing was defended by inertia.

- Spider-Man 1 + 2 share a repo · Spyro 1 + 2 + 3 share a repo (`titles/<t>/` over a shared `game/`), each
  converting to multi-title WHEN that title's work starts, not before.
- Tomba! 1, Vagrant Story, Mega Man X4, CTR, Crash Bash, and Tekken 3: **no shared `game/`.**
- The Crash trio (1/2/3) is ONE architecture, on direct evidence rather than the aggregate metric; `crash/`
  is created when Crash work starts. `ctr/` and `crashbash/` likewise, one title each.
- Rejected: one repo for Spyro AND Crash · an engine-family library vendored between psxport and a game ·
  8 sibling per-title repos.

## Submodule sync: FIXED, and what it now guarantees

`scripts/sync-submodules.sh` used to certify pins it never checked. It now enumerates gitlinks DIRECTLY
(`ls-files -s`, filtering mode 160000) instead of trusting `git submodule status --recursive`, which
aborts on beetle-psx's URL-less nested `deps/lightning/gnulib` and so never reached `vendor/lucent`.
It prints a DENOMINATOR and names what it cannot cover:

    [submodules] checked 2 of 2 submodule(s), all at this repo's recorded gitlinks — NOT covered
    (gitlink(s) no .gitmodules declares, so git itself cannot sync them):
    vendor/beetle-psx/deps/lightning/gnulib

Verified 2026-08-13: all SEVEN trees carry the fixed script (md5 `535fd152dba6…`), and
`tests/test_sync_submodules.cpp` passes. The lesson it cost is in
`docs/findings/workspace-incidents.md` — the check had no denominator, so a short enumeration read as a
clean bill of health.
