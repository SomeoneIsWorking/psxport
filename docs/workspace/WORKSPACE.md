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
3. **Parallel framework work: `git worktree` off `psxport/`, one per claim area**, and point
   `PSXPORT_DIR` at the worktree. Claims (`PROTOCOL.md`) decide who owns an area; worktrees keep
   two agents' files apart. Clean them up — no dangling worktrees.
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
