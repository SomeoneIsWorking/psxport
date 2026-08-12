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

Seven independent repos side by side, all public under `github.com/SomeoneIsWorking`. No workspace repo
and no superproject: a game must build from a bare clone of itself, a gitlink at this level would churn on
every game commit, and a recursive clone would pull seven copies of psxport + beetle-psx.

| path | what it is |
|---|---|
| `psxport/` | **the framework DEV CLONE — the one writable framework checkout.** Also the home of every doc listed above |
| `Tomba2Engine/` | Tomba! 2 — psxport's reference consumer. Tomba! 1 may live here by preference; no shared `game/` |
| `spyro/` | Spyro the Dragon (`SCUS_942.28`); the Insomniac lineage repo (Spyro 2/3 join it) |
| `spider1/` | Spider-Man (`SLUS_008.75`, USA); the Neversoft lineage repo (Enter Electro joins it) |
| `vagrant/` | Vagrant Story (`SLUS_010.40`, USA). Vendors the CC0 `rood-reverse` decomp. Defining fact: the boot exe is ~15% of the code, 933,925 B lives in `.PRG` overlays |
| `megamanx4/` | Mega Man X4 (`SLUS_005.61`, USA) — the only **enhancement-class** port here: already 60fps, so no native producers, no lerp, no native depth. Wants widescreen + load removal + drop-in co-op. Vendors the AGPL-3.0 `mmx4` decomp, which may NOT be lifted into `psxport` |
| `toystory2/` | Toy Story 2 (`SLUS_008.93`, USA), Traveller's Tales — its own engine, no shared `game/` |
| `coord/` | **UNTRACKED, machine-local, EPHEMERAL ONLY**: `claims/` (the area locks — a lock coordinates the agents on THIS machine, so it must not be tracked), plus agent scratch. Nothing durable belongs here |

`$PSX` in any doc means this workspace root. To reproduce the workspace on a fresh machine:

```sh
git clone https://github.com/SomeoneIsWorking/psxport.git ~/repo/psx/psxport
~/repo/psx/psxport/scripts/bootstrap-workspace.sh   # clones the games, inits vendors, relinks CLAUDE.md
```

A new tree is only reproducible once it is added to that script's `REMOTE_BACKED` list; `toystory2` is
not in it yet.

## The structure rule: ONE writable framework checkout

Each game vendors the framework at `external/psxport` (a submodule, itself nesting `vendor/beetle-psx` and
`vendor/lucent`). **Seven checkouts of one framework exist; exactly one is writable.**

1. **Framework edits happen ONLY in `psxport/`.** A game's `external/psxport` is a read-only pinned
   consumer — `git checkout <pin>` territory. Never edit, commit, push or merge in there.
2. **Build a game against in-progress framework work** with `-DPSXPORT_DIR=$PSX/psxport`. It defaults to
   the submodule, so each game still builds standalone from a bare clone — which is what makes "each game
   is its own project using psxport as the framework" true rather than aspirational.
3. **Parallel framework work: one `git worktree` off `psxport/` per claim area**, that agent's
   `PSXPORT_DIR` pointing at it. Claims, the stash/vendor-pin edge, and landing order are PROTOCOL's.

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
- Spyro 1, Tomba! 1, Toy Story 2, Vagrant Story, Mega Man X4, CTR, Crash Bash: **no shared `game/`.**
- The Crash trio (1/2/3) is ONE architecture, on direct evidence rather than the aggregate metric; `crash/`
  is created when Crash work starts. `ctr/` and `crashbash/` likewise, one title each.
- Rejected: one repo for Spyro AND Crash · an engine-family library vendored between psxport and a game ·
  8 sibling per-title repos.

## Known workspace defect

`sync-submodules.sh` certifies pins it never checked — recursive submodule operations on this tree stop
early instead of failing loudly, and no fixed copy exists. `KNOWN-DEFECT-sync-submodules.md`, beside this
file.
