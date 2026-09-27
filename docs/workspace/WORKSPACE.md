# `~/repo/psx` — the PSX-port WORKSPACE

**This file is a MAP AND A POINTER, nothing else.** It lives in the psxport repo so it survives a machine
switch and reaches every game tree through that repo's `external/psxport` submodule; the workspace
`AGENTS.md` and `CLAUDE.md` entries are symlinks to it. The workspace directory itself is not a git
repo and holds nothing durable.

All of these live in the psxport repo, so they reach every game tree and every subagent by `grep`:

| read this | for |
|---|---|
| **`psxport/AGENTS.md`** | **how a game consumes the framework** — the per-Core Lightrec/native seam, state/exits, image-scoped calls, invalidation, RE-first, diagnostics, and registries. THE authority |
| **`docs/workspace/PROTOCOL.md`** | the multi-agent protocol (area claims) and the standing rules |
| `docs/codemap.md` | current responsibility ownership and placement |
| `docs/findings/*.md` | measured findings, and the incidents the rules came from |
| `<game>/AGENTS.md` | that game's own specifics — the authority for that repo (its `CLAUDE.md` is a symlink) |

## What is here

Independent repos live side by side, all public under `github.com/SomeoneIsWorking`. No workspace repo
and no superproject: a game must build from a bare clone of itself, a gitlink at this level would churn on
every game commit, and a recursive clone would pull seven copies of psxport + beetle-psx.

### Target title scope

The target ports are Spyro 1/2/3; Crash 1/2/3; Crash Bash; Crash Team Racing; Vagrant Story; Mega Man
X4; Tomba! 1/2; Tekken 3; and Spider-Man 1/2.

Tekken 3 (`SLUS_004.02`), Tomba! 1 (`SCUS_942.36`), and Mega Man X4 (`SLUS_005.61`) are already 60 fps, so their rendering-enhancement scope is
widescreen only: no fps60 mode, interpolation/lerp, or temporal pipeline added solely to support
interpolation. This does not apply to Tomba! 2 (`SCUS_944.54`, then `MAIN.EXE`). X4 separately retains its later load-removal and
drop-in co-op goals. All planned lineage repositories now have public, reproducible trees. Most newly
added titles are honest harness-first scaffolds, not implementation coverage; no widescreen or
interpolation support is implied by repository existence.

### MEASURED 2026-09-27 — which titles actually HAVE a widescreen owner

The sentence above is a POLICY, stated where a reader would take it for a measurement. This is what the
trees contain. Method, so it can be falsified: for each title, the number of first-party source files under
`game/` and `titles/` matching `wide_engine|wideEngine|wide_project|presentationAspect|Wide16x9`, which is
what a title-owned widening owner actually contains.

| title | repo | widescreen owner files | note |
|---|---|---|---|
| Spyro 1 | `spyro` | 24 | verified live: `render_width=684` against `native_width=512` |
| Crash 1 | `crash` | 2 | retail `H=1000, OFX=0, OFY=0`; no literal horizontal cull in 72,192 instructions |
| Crash Bash | `crashbash` | 2 | widens in BOTH the model producer (OFX moved to the new left margin, draw area clamped so the authored briefing keeps its centred viewport) and the sprite-quad producer (authored canvas shift) |
| Mega Man X4 | `megamanx4` | 2 | widescreen-only profile, as its 60 fps status requires |
| Tekken 3 | `tekken3` | 2 | widescreen-only; the stage wedge is a direction, so widening is `atan(k·tan θ)` |
| Spider-Man 1 | `spider1` | 4 | the viewport window is a projection INPUT; `H` re-derived from the span |
| Tomba! 1 | `Tomba2Engine` | 14 (shared) | widescreen-only, `RenderCapabilities::widescreenOnly()` |
| Tomba! 2 | `Tomba2Engine` | 14 (shared) | in the lerp scope |
| **Crash Team Racing** | `ctr` | **0** | **no owner.** `game/video/projection_owner.h` captures the retail projection publication and says "Widescreen begins here later" — an honest seam, not a capability |
| **Vagrant Story** | `vagrant` | **0** | **no owner**, and no dynarec adapter, so nothing runs yet |

**Both remaining gaps are blocked on a missing disc image on this machine, not on effort.** `ctr/scratch`
and `vagrant/scratch` hold no authenticated images, and recovering a projection or cull owner requires
reading the retail binary — guessing an address is forbidden, so neither can be started honestly.
`crashbash` shows the contrast: its images ARE present at `scratch/bin/crashbash/SCUS_945.70` with seven
overlays, which is why it has a widening owner and the other two do not. Note the main image has **no file
extension**, so `find -iname 'SCUS*.BIN'` misses it; the correct probe is `-size +100k`.

| path | what it is |
|---|---|
| `psxport/` | **the framework DEV CLONE — the one writable framework checkout.** Also the home of every doc listed above |
| `Tomba2Engine/` | Tomba! 2 (`SCUS_944.54` → `MAIN.EXE`) — psxport's reference consumer; also owns the separate, widescreen-only Tomba! 1 (`SCUS_942.36`) title project, with no shared `game/` |
| `spyro/` | Spyro 1/2/3, the Insomniac-lineage repository; Spyro 1 (`SCUS_942.28`) is the current implementation |
| `spider1/` | Spider-Man 1/2, the Neversoft-lineage repository; Spider-Man 1 (`SLUS_008.75`, USA) is the current implementation |
| `vagrant/` | Vagrant Story (`SLUS_010.40`, USA). Vendors the CC0 `rood-reverse` decomp. Defining fact: the boot exe is ~15% of the code, 933,925 B lives in `.PRG` overlays |
| `megamanx4/` | Mega Man X4 (`SLUS_005.61`, USA) — already 60 fps, so no fps60, native-producer, lerp, or native-depth pipeline. Wants widescreen + load removal + drop-in co-op. Vendors the AGPL-3.0 `mmx4` decomp, which may NOT be lifted into `psxport` |
| `crash/` | Crash Bandicoot 1/2/3 in one architecture repository; harness-first scaffold |
| `ctr/` | Crash Team Racing; standalone harness-first scaffold |
| `crashbash/` | Crash Bash; standalone harness-first scaffold |
| `tekken3/` | Tekken 3 (`SLUS_004.02`); standalone harness-first scaffold, already 60 fps and targeting widescreen only |
| `toystory2/` | Existing Toy Story 2 (`SLUS_008.93`, USA) checkout — not in the active title scope above |
| `coord/` | **UNTRACKED, machine-local, EPHEMERAL ONLY**: `claims/` (the area locks — a lock coordinates the agents on THIS machine, so it must not be tracked), plus agent scratch. Nothing durable belongs here |

`$PSX` in any doc means this workspace root. To reproduce the workspace on a fresh machine:

```text
git clone https://github.com/SomeoneIsWorking/psxport.git ~/repo/psx/psxport
cd ~/repo/psx/psxport
uv run --frozen python scripts/bootstrap_workspace.py
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

   **KNOWN LIMIT, measured 2026-09-27: that file is written at CONFIGURE time, so a plain
   `cmake --build` does not refresh it.** A port can therefore relink against newer framework code while
   its `psxport_resolved.txt` still names the older commit, and `--check` passes — which is precisely the
   drift the pin exists to catch. Measured on `crash`: the resolved file was written at 11:30 naming
   framework `492adace`, the binary was relinked at 16:05 against framework `2b07a8f6`, and the check
   would have passed. **`--check` is only meaningful after a reconfigure**, so a pin bump must be
   `reconfigure → build → test → --bump`, never `build → --bump`.

   Coverage is also uneven: `ctr`, `spider1`, `Tomba2Engine` and `megamanx4` register a pin test and
   `crash` does not, so `crash`'s green gate says nothing about framework drift at all. Registering it
   everywhere is the cheap half of this fix and has not been done.

   **MEASURED 2026-09-27, every port rebuilt and gated against the framework as it stood during that
   session, after the store-observer and control-surface changes. NO REGRESSIONS:**

   | port | gate | the failures, and what each one is |
   |---|---|---|
   | `psxport` | 168/168 | — |
   | `crash` | 22/22 | — (registers no pin test at all) |
   | `ctr` | 13/14 | `ctr_framework_pin` — **correct**: framework moved, pin not bumped |
   | `crashbash` | 28/29 | `crashbash_psxport_pin` — **correct**, same reason |
   | `spider1` | 20/21 | `psxport_pin` — **correct**, same reason |
   | `megamanx4` | 27/27 | — |
   | `tekken3` | 18/18 | — |
   | `Tomba2Engine` | 33/33 | — (`tools/verify_ci.py`'s pin check fails separately, same reason) |
   | `vagrant` | 8/8 | — |
   | `toystory2` | 16/18 | both are **refusals for a missing provisioned corpus**: `scratch/flat` is empty, so `overlay_map_selftest` and `verify_fmv_boundary_selftest` each print `REFUSED: … provision the verified images` and fail. `toystory2` is outside the active title scope and was never provisioned here. |

   No pin was bumped, deliberately: a bump records a verification, and no port has been re-verified by a
   product run against this session's framework. The pin bumps are the outstanding work, and the order is
   `reconfigure → build → test → --bump`.
3. **Ports are deliberately NOT all on framework HEAD.** Measured 2026-08-16: six ports spanned 55
   commits of framework history. With one maintainer that is a feature — it is what lets one port be
   worked on daily while the others sit untouched, and it is why a Beetle GTE regression in every
   3D scene broke one tree rather than six. Bump a port when you are ready
   to re-verify it.
4. **Parallel framework work** is still one `git worktree` off `psxport/` per claim area, with that
   agent's `PSXPORT_DIR` pointing at it (PROTOCOL's).

### Why the submodule was dropped (2026-08-16)

Two incidents in one day, both caused by the mechanism rather than by anyone's mistake:

- Tomba2Engine was **built against psxport `25dd7826` while recording `a1c53d7c`**, so a bare clone did
  not compile — the game's hook table named a `GameHooks` field the pinned framework did not have.
  Nothing noticed, because a submodule working tree and its recorded gitlink drift silently.
- "Fixing" that drift by syncing to the recorded pin is what pulled a **broken Beetle GTE commit** into
  the working build; it had already broken concurrent 3D execution for two days (8 of 9
  replays segfaulting). That commit was made on a **detached HEAD inside the submodule** — the default
  state of a submodule checkout, and the reason it was never reviewed.

Recursive updates also entered Beetle's nested `deps/lightning/gnulib` dependency. Its mapping was
absent in an earlier checkout, causing Git to abort; a mapped checkout instead cloned a large unrelated
repository during launcher setup. `psxport` still manages its own declared top-level vendor submodules
without recursing into their dependencies. With a single psxport tree there is no duplicate framework
checkout to drift.

## The two things to know even if you read nothing else

- **Never commit** disc images (`*.chd`), extracted executables, or machine-specific
  absolute paths (`/home/<user>/…`). Every game repo ships `tools/go_public.py` to audit history.
- **Never write run artifacts to `/tmp`** — small RAM-backed tmpfs here. Use the repo's git-ignored
  `scratch/`, split by kind. Diagnose "disk quota exceeded" with `quota -s`, not `df`.
- **Some `scratch/` subdirectories hold PROVISIONED INPUTS, not artifacts, and THE DIRECTORY IS NOT
  THE SAME IN EVERY REPO.** The runtime images extracted from the user's disc live there, so a
  `scratch_gc.py --days 0` over a game repo deletes them and the next run refuses. **Check the repo
  before sweeping it**, because this line used to name only `bin/` and that is Tomba! 2's layout:

  | repo | provisioned inputs | re-provision with |
  |---|---|---|
  | `Tomba2Engine` | `scratch/bin/` | `tools/tomba2_provision.py --discdump external/psxport/build/tools/discdump "$DISC"` |
  | `spyro` | `scratch/assets/<title>/` | `tools/provision_title.py --title spyro1 --discdump external/psxport/build/tools/discdump "$DISC"` |

  Measured 2026-09-20: a sweep carrying `--keep 'bin/*'`, taken straight from this line, removed
  `scratch/assets/spyro1/SCUS_942.28` and the next drive died with `[boot:error] cannot read
  .../scratch/assets/spyro1/SCUS_942.28: No such file or directory`. Nothing is lost but the minute
  re-provisioning takes — it re-authenticates every image — but the refusal reads like a broken
  port rather than a missing input, which is the part that costs time. Pass every provisioned path
  this table names for the repo you are sweeping.

## Repo shape: one repo per ENGINE LINEAGE, multiple titles inside it. No third vendored layer

Evidence for every verdict below — matrices, null distributions, the per-decision survival check:
`docs/findings/lineage-metric.md`. A bare similarity percentage means nothing without its multiple of the
measured cross-studio null.

The accepted repository grouping is listed below. `docs/findings/lineage-metric.md` holds its measured
evidence; revise the grouping only when new evidence changes the ownership boundary.

- Spider-Man 1 + 2 share a repo · Spyro 1 + 2 + 3 share a repo (`titles/<t>/` over a shared `game/`), each
  converting to multi-title WHEN that title's work starts, not before.
- Tomba! 1, Vagrant Story, Mega Man X4, CTR, Crash Bash, and Tekken 3: **no shared `game/`.**
- The Crash trio (1/2/3) is ONE architecture, on direct evidence rather than the aggregate metric; `crash/`
  is created when Crash work starts. `ctr/` and `crashbash/` likewise, one title each.
- Rejected: one repo for Spyro AND Crash · an engine-family library vendored between psxport and a game ·
  8 sibling per-title repos.

## Submodule sync: FIXED, and what it now guarantees

`scripts/sync_submodules.py` manages only this repository's declared top-level gitlinks. It enumerates
them directly from `.gitmodules` and `ls-files -s`, updates only named top-level paths, and never uses
recursive Git updates. Thus first-run setup does not clone Beetle's nested
`deps/lightning/gnulib`, even when that nested dependency has a valid URL. The verdict gives a
denominator and names nested gitlinks it saw but deliberately excluded:

    [submodules] checked 2 of 2 submodule(s), all at this repo's recorded gitlinks — nested
    gitlink(s) outside this sync: vendor/beetle-psx/deps/lightning/gnulib

The focused test covers cold initialization, warm pin correction, missing declared paths, dirty and
deliberately advanced checkouts, and both mapped and unmapped nested gitlinks. A clean verdict applies
to the declared top-level set only.
