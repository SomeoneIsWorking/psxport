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

### MEASURED 2026-09-27 — three titles' 60 fps SCOPE IS UNKNOWN, and I asserted it anyway

**Correction first, because I got this wrong in conversation and nearly acted on it.** A survey of the
per-title docs produced `ctr 60 fps`, `spider1 60fps`, `vagrant 60fps`, and I was about to use that to
declare all three outside lerp scope. **It is false.** Every one of those strings is a GOAL, not a
measurement — `ctr/docs/project-goals.md:59` "## G003 — Interpolated presentation at 60 fps and above",
`spider1/docs/project-goals.md:54` "The 60fps outcome is true interpolation", `vagrant/docs/project-goals.md:35`
"the only 60fps difference is insertion of the additional lerped presentation". Reading a goal as a
measurement is the same error as arming the store observer on a data address, and it is the reason this
paragraph exists.

**What is actually recorded, by method — the VSync argument, which IS a field count:**

| title | `VSync(n)` sites found in its docs and sources | verdict |
|---|---|---|
| `vagrant` | `VSync(2)` x3, `VSync(3)` x2, plus 0 and -1 | **leads to 30 fps** (2 fields/frame) |
| `ctr` | `VSync(2)` x1, plus 0 and -1 | **leads to 30 fps**, one site only |
| `spider1` | `VSync(0)` x3, `VSync(-1)` x7 — **no waiting call at all** | **unknown**, and see below |

The method is established, not assumed: `crashbash` is now MEASURED at 2 fields per game frame
(`crashbash/docs/issues/0031`), and it was established by reading the argument reaching the display owner
and by the wait routine branching to NO-WAIT for `a0 == 1` and `a0 <= 0` and only waiting for `a0 >= 2`.
**A `VSync(0)` or `VSync(-1)` site therefore says nothing about rate** — 0 and -1 are the return-current and
query modes. `spyro` is a confirmed 30 fps title and has ONLY `VSync(0)` and `VSync(-1)`, which is exactly
why `spider1` having no waiting call proves nothing either way.

**So: `vagrant` and `ctr` each have ONE lead toward 30 fps, and `spider1` has none. None of the three has a
measured rate, and all three carry an interpolation goal whose scope depends on it.** `spider1` has a
provisioned image, so its rate is measurable and simply has not been measured. `ctr` and `vagrant` have no
disc image on this machine, so for those two the rate cannot be established from the retail binary at all
until media is provisioned — **which also means the two remaining widescreen gaps and this scope question
share one blocker.**

**Do not add an interpolation path to, or rule one out of, any of these three until the rate is measured.**

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
| Crash 1 | `crash` | 2 | retail `H=1000, OFX=0, OFY=0`. **CORRECTED 2026-09-27: the old note here — "no literal horizontal cull in 72,192 instructions", read as "widening cannot clip new geometry" — was right about the scan and wrong about the inference.** The bound is a main-RAM global `0x800578D0` (1 writer, 20 readers), so a literal-immediate scan cannot see it, and it is **not a screen-space cull: `FUN_8003A144` uses it as the GTE NEAR PLANE** (`H < Z < 12000`). Widening is safe only because the port widens the GPU projection and leaves that global at retail's value; a change that raised it would cull near geometry. See `crash/docs/issues/0016`. |
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

   **MEASURED 2026-09-27 — and the coverage is worse than "uneven", because the pin was never
   CALLED.** Two claims in an earlier revision of this file were wrong, both the same way:

   - *"`crash` registers none — `ctest -N` finds zero tests matching `pin`."* It does register one, as
     **`crash_dependency_provenance`** (`crash/CMakeLists.txt:348`). The name simply lacks the substring
     `pin`. That is a grep for a string reported as a fact about a port — the same class of error as
     arming a store observer on a data address and reading the guaranteed `MATCHED NONE` as absence.
   - *"No pin was bumped; the bumps are outstanding work."* They were done that day, in the required
     `reconfigure → build → test → --bump` order, once the guard below was in place.

   **The real finding is one level down: NO PORT EVER RAN THE CHECK.** Every occurrence of `--check` in
   all ten `CMakeLists.txt` files was inside a CMake **comment**. The registered "pin tests" were
   **selftests of the check *function***, run against a temporary fixture with a fabricated receipt: they
   assert the function behaves and cannot fail when the repository's real pin is stale. That is why all
   ten ports reported green while four pins were demonstrably stale. A gate that cannot fail on the
   thing it gates is not a gate. The fix is a *live* test per port — registered in `Tomba2Engine` as
   `tomba_psxport_pin`:

   ```cmake
   add_test(NAME tomba_psxport_pin
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/psxport_sync.py"
                    --check --build "${CMAKE_BINARY_DIR}")
   ```

   `--build ${CMAKE_BINARY_DIR}` is the directory CTest is running in, configured moments earlier, so its
   receipt is current by construction and the staleness guard cannot fire spuriously. **Verified it can
   fail:** perturbing `psxport.pin` to a zero commit turns it red, restoring it turns it green.
   **The other nine ports still have no live check.**

   **AND THE BUMP ITSELF COULD RE-CREATE THE ORIGINAL INCIDENT.** `do_bump` recorded
   `head_of(external/psxport)` — the framework's *current* HEAD — and never read a build receipt. So the
   documented order was a convention nothing enforced, and `--bump` alone, with no build at all, recorded
   a commit the tree had never compiled against. **That is exactly how this workspace came to record
   `a1c53d7c` while building against `25dd7826`.** `do_bump` now reads the same receipt, applies the same
   staleness guard and selects the same build as `--check`, and refuses a receipt naming a framework
   other than the one `external/psxport` links. The regression test is
   `test_bump_refuses_when_the_framework_advanced_since_configure`.

   **The staleness guard is now in 10 of 10 copies**, verified per repo against each repo's own copy. Its
   absence was measurable: with `psxport_resolved.txt` naming a repo's own pin while the shared
   framework sat eight commits later, a guarded copy refuses and an unguarded one answers `check OK` —
   same input, opposite answers, and the wrong one is a pass. A second half of the same hole: an ABSENT
   receipt printed "Asserting nothing" and returned 0 in five copies; those now exit 2.

   **The cause is the duplication itself, and it is deliberate** — a port must build from a bare clone,
   so the tool travels with it. That makes "keep the copies in step" an obligation rather than an
   accident. **MEASURED 2026-09-27: the ten copies had TEN DISTINCT HASHES, 298–322 lines each, and 61–226
   changed lines against each other.** The check for that obligation was itself duplicated, which is why the
   guard could go missing from seven of them unnoticed.

   **DONE, and the consolidation found two more defects the duplicated check had been hiding.** There is now a
   canonical `psxport/tools/psxport_sync.py` and a canonical `psxport/tests/test_psxport_sync.py`, and
   `psxport/tools/check_port_pin_tools.py` gates every port against both (`pin_tools_in_step`,
   `pin_sync_behaviour`, `pin_tools_selftest` in psxport's CTest). All ten ports are in step and pinned. Four
   things came out of making the copies identical:

   - **The canonical was missing `import argparse`, and the byte gate reported all ten ports "in step" while
     every copy was unable to run.** The import is reached only from `main()`, so each port's test — which
     imports the module and never calls `main()` — raised nothing. The gate therefore asks **two** questions:
     is this copy the canonical text, *and* does it run (`--help`, exit 0). On the same ten files the
     byte-only gate answered `10 of 10 in step` and the two-question gate answers `0 of 10 in step AND
     runnable`. Byte-equality is not workingness, and the selftest pins the case: a script that imports
     cleanly and still fails `--help` is exactly the real defect, and a module-import test cannot see it.
   - **7 of the 10 ports shipped the pin tool with NO test gating it at all.** The six that had one were the
     six whose copies had drifted least — the ports nobody revisited are the ports nobody guarded, so the
     drift and the missing tests are one fact seen twice.
   - **Three call sites passed `--build-dir` and one `--resolved` to a tool that has neither flag**
     (`crashbash`, `spider1`, `crash/tools/verify.py`, `ctr`). They were calling a flag that did not exist,
     which is precisely what duplication produces.
   - **`ctr` had a second, older copy of the behaviour test under `tools/`** that the canonical is not
     installed over, and its registration pointed at it — a test reading as present while exercising a file the
     canonical no longer governed.

   **`tekken3`'s registrations are unguarded on purpose**: it calls `enable_testing()` directly and never
   defines `BUILD_TESTING`, so an `if(BUILD_TESTING) add_test(...)` there is silently inert. `ctest -N` went
   17 → 18 when that was corrected.

   **An operational cost of the guard, stated rather than left to be discovered later:** any commit to
   psxport — including a documentation-only one — moves HEAD, so every port's receipt goes stale and all ten
   need a `reconfigure → build → test → --bump` round. That is the guard working, not misbehaving: the pin
   records "the framework commit this tree was built and verified against", and after a framework commit that
   is genuinely no longer true. Whether a non-runtime commit should invalidate a *build* pin would need a
   build-relevance classifier — a larger design question, deliberately not attempted here.

   **MEASURED 2026-09-27, every port rebuilt and gated against the framework as it stood during that
   session, after the store-observer and control-surface changes. NO REGRESSIONS:**

   | port | gate | the failures, and what each one is |
   |---|---|---|
   | `psxport` | 168/168 | — |
   | `crash` | 22/22 | — (registers `crash_dependency_provenance`, a selftest; see above) |
   | `ctr` | 13/14 | `ctr_framework_pin` — **correct**: framework moved, pin not bumped |
   | `crashbash` | 28/29 | `crashbash_psxport_pin` — **correct**, same reason |
   | `spider1` | 20/21 | `psxport_pin` — **correct**, same reason |
   | `megamanx4` | 27/27 | — |
   | `tekken3` | 18/18 | — |
   | `Tomba2Engine` | 33/33 | — (`tools/verify_ci.py`'s pin check fails separately, same reason) |
   | `vagrant` | 8/8 | — |
   | `toystory2` | 16/18 | both are **refusals for a missing provisioned corpus**: `scratch/flat` is empty, so `overlay_map_selftest` and `verify_fmv_boundary_selftest` each print `REFUSED: … provision the verified images` and fail. `toystory2` is outside the active title scope and was never provisioned here. |

   **SUPERSEDED LATER THE SAME DAY, and the supersession is the finding.** The four red pin rows above were
   correct while they stood, and every one of them was then made green by
   `reconfigure → build → test → --bump`: `spyro` 86/86, `ctr` 15/15, `crashbash` 29/29, `spider1` 21/21,
   `megamanx4` 27/27, `tekken3` 18/18, `Tomba2Engine` 34/34. `vagrant` and `toystory2` were not reconfigured
   that round. `toystory2`'s two failures remain the missing-corpus refusals, which are correct.

   The row that did NOT change is the one about the gate itself: those "pin" rows were selftests, so
   **making them green proved nothing about this port's real pin** — which is why adding the live check
   in `Tomba2Engine` immediately found a genuinely stale receipt in `build/ci` (configured against
   `2b07a8f6` while the tree sat at `9c962c08`), five build directories into the tree, none of which any
   test had been reading.
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
