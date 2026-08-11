# `~/repo/psx` — the PSX-port WORKSPACE

**This file is a MAP AND A POINTER, nothing else**, and it lives in the psxport repo
(`docs/workspace/WORKSPACE.md`) so it survives a machine switch. `~/repo/psx/CLAUDE.md` is a SYMLINK
to it — the workspace directory itself is NOT a git repo and holds nothing durable.

| read this | for |
|---|---|
| **`psxport/CLAUDE.md`** | **how a game consumes the framework** — build/run, the CVar ladder, the seam, `generated/` rules, RE-first, diagnostics, registries, Dusklight. THE authority |
| **`psxport/docs/workspace/PROTOCOL.md`** | the multi-agent protocol (area claims) + the standing USER rules (no windowed agent runs, PC owns execution, housekeeping, worktrees) |
| `psxport/docs/workspace/LAYOUT.md` | the target directory organization for every tree |
| `psxport/docs/plans/*.md` | designs not yet implemented, each stating what is measured vs assumed |
| `<game>/CLAUDE.md` | that game's own specifics — the authority for that repo |

Everything above is TRACKED, in the psxport repo — so it survives a machine switch, reaches every game
tree through that repo's `external/psxport` submodule, and reaches every subagent by `grep`.

## What is here

`~/repo/psx` is a plain **directory** holding four independent repos side by side. There is no
workspace repo and no superproject: a game must build from a bare clone of itself, a gitlink at this
level would churn on every game commit, and a recursive clone would pull four copies of psxport +
beetle-psx.

| path | what it is |
|---|---|
| `psxport/` | **the framework DEV CLONE — the one writable framework checkout** · `github.com/SomeoneIsWorking/psxport`. Also the home of every doc listed above |
| `spyro/` | Spyro the Dragon (`SCUS_942.28`) port · `github.com/SomeoneIsWorking/spyro` |
| `spider1/` | Spider-Man (`SLUS_008.75`, USA) port · `github.com/SomeoneIsWorking/spider1` |
| `Tomba2Engine/` | Tomba! 2 port — psxport's reference consumer · `github.com/SomeoneIsWorking/Tomba2Engine` |
| `coord/` | **UNTRACKED, machine-local, EPHEMERAL ONLY**: `claims/` (the area locks — a lock coordinates the agents running on THIS machine, so it must not be a tracked file), plus historical `patches*/` and agent scratch. Nothing durable belongs here: a finding goes in the psxport repo or a game repo, never here |

`$PSX` in any doc means this workspace root. To reproduce the workspace on a fresh machine:

```sh
git clone https://github.com/SomeoneIsWorking/psxport.git ~/repo/psx/psxport
~/repo/psx/psxport/scripts/bootstrap-workspace.sh   # clones the games, inits vendors, relinks CLAUDE.md
```

## The structure rule: ONE writable framework checkout

Each game vendors the framework at `external/psxport` (a submodule, which itself nests
`vendor/beetle-psx` and `vendor/lucent`). **Four checkouts of one framework exist; exactly one is
writable.**

1. **Framework edits happen ONLY in `psxport/`.** A game's `external/psxport` is a read-only pinned
   consumer — `git checkout <pin>` territory. Never edit, commit, push or merge in there.
2. **Build a game against in-progress framework work** without touching its submodule:
   `PSXPORT_DIR=$PSX/psxport ./run.sh` (or `-DPSXPORT_DIR=...` to cmake). It defaults to the
   submodule, so each game repo still builds standalone from a bare clone — that is what keeps "each
   game is its own project using psxport as the framework" true rather than aspirational. `run.sh`
   announces which checkout it built from, and whether that checkout was dirty.
3. **Parallel framework work: `git worktree` off `psxport/`, one per claim area**, and point that
   agent's `PSXPORT_DIR` at its worktree. Claims (`PROTOCOL.md`) decide who owns an area; the worktree
   keeps two agents' files apart. Clean them up — no dangling worktrees. One sharp edge: a worktree
   shares `.git`, so inside one, do not `git stash` and do not move a vendor pin (`PROTOCOL.md` has
   the incident).
4. **Landing:** commit + push in `psxport/`, then bump each game's gitlink — and record the gitlink
   BEFORE building or gating that tree (`PROTOCOL.md` says why: `sync-submodules.sh` syncs
   toward the RECORDED pin, so an un-recorded checkout gets silently reverted under you).

## The two things to know even if you read nothing else

- **Never commit** disc images (`*.chd`), extracted executables, `generated/`, or machine-specific
  absolute paths (`/home/<user>/…`). Every game repo ships `tools/go_public.py` to audit history.
- **Never write run artifacts to `/tmp`** — small RAM-backed tmpfs here. Use the repo's git-ignored
  `scratch/`, split by kind. Diagnose "disk quota exceeded" with `quota -s`, not `df`.

## Known workspace defects

- **`sync-submodules.sh` certifies pins it never checked, and NO FIXED COPY EXISTS.**
  `git submodule status --recursive` aborts on beetle-psx's URL-less nested `deps/lightning/gnulib`,
  so it never reaches `vendor/lucent`; the script's `|| true` swallows the non-zero exit and it prints
  "all at recorded gitlinks" over a partial enumeration. Write-up:
  `KNOWN-DEFECT-sync-submodules.md`, next to this file. Verify with
  `md5sum $PSX/coord/st.sh */external/psxport/scripts/sync-submodules.sh` before trusting any claim that
  this is fixed. **Re-confirmed 2026-08-11 by the same abort in a different tool:** creating the dev
  clone with `git clone --recurse-submodules` died on that exact path *after* cloning beetle and
  *before* checking out `vendor/lucent`, which left lucent's worktree empty with every file staged
  deleted. Recursive submodule operations on this tree do not fail loudly — they stop early.

## DECIDED: how the workspace grows to the Spyro/Crash titles (2026-08-11)

The USER delegated this call ("I'm not gonna decide, maybe Fable should decide" → then "it's yours"), so
it is decided here rather than left open. Evidence is the engine-lineage measurement in
`tools/exe_similarity.py`'s docstring (validated both directions: 64.2% positive, 2.3% negative, 12.5%
cross-studio SDK ceiling) plus `docs/plans/game-seam-redesign.md`.

**One repo per ENGINE LINEAGE, multiple titles inside it. No third vendored layer. Nothing speculative.**

| tree | covers | when it is created |
|---|---|---|
| `psxport/` | the framework, and the ONLY framework | exists |
| `spyro/` | the Insomniac lineage — Spyro 1, 2, 3 as `titles/<t>/` with shared `game/` | exists; converts to multi-title WHEN Spyro 2 work actually starts |
| `spider1/` | the Neversoft lineage — Spider-Man (`SLUS_008.75`) + **Spider-Man 2: Enter Electro** (USER, 2026-08-12: "can be part of spider-man") | exists; converts to multi-title WHEN Enter Electro work actually starts |
| `Tomba2Engine/` | the Whoopee Camp lineage — Tomba! 2, and **Tomba! 1** is the candidate second title | exists; Tomba! 1's placement is UNMEASURED — see below |
| `crash/` | Crash 1, 2, 3 (Naughty Dog, GOOL VM) | when Crash work starts, not before |
| `ctr/`, `crashbash/` | one title each — measured as their own engines | when that work starts, not before |

**Why not the three things that were on the table.**

- **Not one repo for Spyro AND Crash**, which is what was first asked for. Different developer, different
  data formats (`WAD.WAD`/`SOURCE.TRD` vs `NSF`/`NSD`), and a Lisp-VM architecture needing its own
  tooling. They already share what they should share — psxport. The cross-shipped rival demos on every
  disc are why they *look* like one family; they are cross-promotion, not shared code.
- **Not an engine-family LIBRARY vendored between psxport and a game.** A middle layer whose whole nature
  is "holds facts for N games" is a factory for the residence defect `pc_scheduler.cpp` already is —
  invisible to `psxport_smoke`, outside `test_no_game_address_literals.cpp`'s scope, and a second pin to
  sync through a `sync-submodules.sh` that already certifies pins it never checked. Shared lineage code
  lives INSIDE the lineage repo, where inheritance is the seam plan's own answer and where the repo-local
  rule "shared `game/` may not hold title literals" can be linted the same way.
- **Not 8 sibling per-title repos.** Spyro 2↔3 measure 64.2% — one codebase — so per-title repos would
  duplicate exactly the code most worth sharing.

**MEASURED 2026-08-12 — and the two new titles came out on OPPOSITE sides of the line.** Run before
shaping either repo's shared `game/`, because assuming was exactly what the Spyro measurement punished.
`tools/exe_similarity.py`, boot executables extracted with `discdump` into gitignored `scratch/exes/`:

|  | TOMBA1 | TOMBA2 | SPIDER1 | SPIDER2 |
|---|---|---|---|---|
| **TOMBA1** | · | 18.8% | 7.3% | 6.9% |
| **TOMBA2** | 18.8% | · | 11.8% | 10.5% |
| **SPIDER1** | 7.3% | 11.8% | · | **74.2%** |
| **SPIDER2** | 6.9% | 10.5% | **74.2%** | · |

- **Spider-Man 1 ↔ 2: 74.2% — ONE CODEBASE, and the strongest pair measured in this workspace** (above
  even Spyro 2↔3's 64.2%). The USER's call to fold Enter Electro into `spider1/` is confirmed by
  measurement, not merely permitted: there is more to share here than anywhere else, so the multi-title
  split matters most in this repo.
- **Tomba! 1 ↔ Tomba! 2: 18.8% — NOT one codebase.** Above the 12.5% cross-studio SDK ceiling, so the
  two do share something real, but nowhere near a shared engine. This is the Spyro-1 situation with a
  little more overlap: expect FORMAT and tooling knowledge to transfer and expect native engine classes
  NOT to. A shared `game/` between them would mostly hold code one title cannot use.
- The cross-lineage cells (Tomba×Spider, 6.9–11.8%) sit at or below the SDK ceiling, which is the
  NEGATIVE CONTROL working — it is what makes the 74.2% and 18.8% readable rather than two bare numbers.

**One caveat, stated because it is the kind that inverts a conclusion.** Tomba! 2's engine is `MAIN.EXE`;
its `SCUS_944.54` is a small loader. Comparing Tomba! 1's single exe against that LOADER instead gives
47.6% — far higher — but the metric's denominator is the smaller shingle set, and the loader is precisely
where SDK init and boot boilerplate live, so a high score there measures shared Sony code, not a shared
engine. `TOMBA2_MAIN ↔ TOMBA2_SCUS` is 76.8% *within the same game*, which shows how much of that overlap
is generic. The engine-to-engine number, 18.8%, is the one that decides repo shape.

**Spyro 1 is in the Insomniac repo by PREFERENCE, not by measurement.****Spyro 1 is in the Insomniac repo by PREFERENCE, not by measurement.** It shares the asset pipeline and
tooling with 2/3 and **~no code**: 10-11% against a 12.5% SDK ceiling means Insomniac rewrote the engine
between 1 and 2. Do not expect its native classes to serve Spyro 2; expect its FORMAT knowledge to.

**Order, if it matters:** Spyro 2 first (strongest family prior, adjacent to a working port, and the
cheapest real test of the multi-title bet), then Spyro 3, then the Crash trio, then CTR/Bash.

**Prerequisite that is not negotiable:** do not bring up a second Insomniac title while the framework
still contains Tomba!2's frame loop and scheduler. `spyro/game/render/frame_loop.cpp` already documents
"THIS PORT CANNOT USE THE FRAMEWORK'S FRAME LOOP"; a third consumer would fork it again. Land
`docs/plans/game-seam-redesign.md`'s early steps first.

## DECIDED: the tooling hoist happens ADDITIVELY, starting now (2026-08-11)

The generic tool ENGINES (`info.py`, `catalog.py`, `re_frontier.py`, `codemap.py`, `whatis.py`,
`go_public.py`, …) move into `psxport/tools/port/`; the DATA (`docs/info/`, `docs/issues/`, codemaps,
roadmaps) stays per-game. Every game already vendors psxport, so the engines reach them all.

**The evidence is not theoretical.** `re_frontier.py`'s green-over-nothing bug has now been fixed FOUR
times across THREE diverged copies (890 / 443 / skill lines), and two of those copies were still reporting
"OK" over a zero-entry parse on 2026-08-11 — one of them the copy a CLAUDE.md tells you to run. Divergence
also produced the `RE_FRONTIER_ROADMAP` trap and the `codemap.md` vs `code-map.md` split.

**Additively, because Tomba2Engine is mid-Job-#1 and a flag day is unacceptable:** psxport gains the
engine as a new file; each game's existing tool keeps working untouched; a game switches to a 3-line shim
one tool at a time, when someone is already in that repo. No window exists in which a repo has no working
tool, and each step is independently revertible. Unify `codemap.md`/`code-map.md` naming in the same pass
as whichever repo switches that tool.
