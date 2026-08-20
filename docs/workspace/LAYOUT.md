# Target layout — organize like Dusklight/Aurora

> *"make sure to lay the projects out like Dusklight/Aurora, everything well organized"* — USER, 2026-08-06

Reference: `~/repo/dusklight` (CC0). Take the SHAPE, not a copy-paste. See the workspace `CLAUDE.md`
Dusklight section for the UI/config/interpolation specifics.

## Where we actually stand (measured 2026-08-06, not guessed)

| tree | state | verdict |
|---|---|---|
| `Tomba2Engine/game/` | 373 files across `world/ render/ ai/ audio/ camera/ core/ input/ items/ math/ object/ player/ scene/ ui/ cd/` | **Already Dusklight-shaped.** Leave it. It is the exemplar the other two grow into. |
| `spyro/game/`, `spider1/game/` | 21 and 12 files across `core/ render/` | Proportionate. Do NOT pre-create empty subsystem dirs — split a directory when it earns it, on Tomba2's names so the three ports stay legible together. |
| `psxport/runtime/recomp/` | **149 files, FLAT** — GPU, SPU, CD, MDEC, HLE, memcard, SBS harness, debug server, config, RmlUi overlay all siblings | **This is the whole problem.** Everything below is about this directory. |

## Target for `psxport/runtime/`

One directory per subsystem, mirroring how Dusklight splits `src/dusk/` into `audio/ imgui/ ui/ mods/`
rather than one bag of files:

    runtime/cpu/       dispatch, coro, core, the MIPS interpreter — execution substrate
    runtime/gpu/       gpu_vk, gpu_native, render_queue, present/video plans, shaders_gpu/
    runtime/audio/     SPU
    runtime/media/     CD, disc, XA, FMV, MDEC
    runtime/hle/       BIOS/SDK HLE, memcard
    runtime/harness/   SBS, dualcore, dualview, verify, native_diff — the differential machinery
    runtime/config/    cfg/config/config_var (the CVar work lands here)
    runtime/ui/        RmlUi overlay + glue  — componentised per Dusklight `src/dusk/ui/`
    runtime/dbg/       dbg_server, fntrace, hostprof — developer tooling, the ImGui-side analogue

`game_iface.h` / `recomp_iface.h` stay at `runtime/` root: they are THE SEAM, and burying a seam in a
subsystem directory hides what a game is allowed to touch. `psxport_smoke` keeps proving it.

## Rules for the move — ONE ATOMIC MOVE, proven inert MECHANICALLY

> *"'one big move' is fine imo, doing some things iteratively is more difficult than one at once"*
> — USER, 2026-08-06

**Do it in ONE commit, not subsystem by subsystem.** Include-path rewrites are global: moving `gpu/`
first and `media/` second edits many of the same files twice, needs N interdependent gates, and leaves
a half-organized tree in between where "is this file moved yet?" has no clean answer — which agents
will get wrong. Splitting it buys bisect granularity that is worth little when every piece lands in
one session anyway.

The safeguard is NOT small commits. It is proving the move changed nothing but paths:

- **DRIVE IT FROM A SCRIPT, not by hand.** The move must be reproducible and reviewable as a
  transformation rather than as 149 diffs. Keep the script.
- **PROVE CONTENT-IDENTITY MECHANICALLY.** Every moved file must be byte-identical to its original
  except for `#include` lines. Hash each file with include lines stripped, before and after, and
  assert the multiset of hashes is unchanged. That is a real check that no edit rode along — far
  stronger than "I only moved things", and it is the thing small commits were supposed to approximate.
- **NO BEHAVIOUR CHANGE IN THE SAME COMMIT.** This matters MORE in one big move, not less: it is the
  only reason the hash check above can be trusted. Anything that is not a path is a separate commit,
  before or after.
- **`git mv`**, so history follows the file. A delete+add loses every `git log --follow` and blame
  trail the RE work depends on — and on a 149-file move that is most of the framework's history.
- **`cmake/psxport.cmake` lists sources explicitly** — update it in the same commit. Leave
  `tests/CMakeLists.txt`'s glob alone (deliberately a glob so concurrent agents adding tests never
  edit a shared file).
- **Update the codemaps in the same change.** A stale path sends the next session to a file that no
  longer exists, which is worse than no map.
- **THE GATE, all three trees, after the single commit:** framework `ctest` green; each port builds;
  each port's headless boot reaches at least its recorded mark — spyro 212334 presents/no abort ·
  spider1 8969 presents/no abort · Tomba2 243 presents/exit 0 under
  `PSXPORT_GATE=1 PSXPORT_RENDER_PSX=1`. Headless PACES now, so pass `PSXPORT_NOPACE=1`.

## Sequencing — BUILD THE SCRIPT IN A WORKTREE NOW, APPLY IT TO A CLEAN TREE LATER

Be precise about what the collision actually is. A worktree isolates a WORKING DIRECTORY; it does not
make two changes to the same 149 files merge. So worktrees do NOT let the move land beside feature
work.

But the move is **script-driven**, and that is what splits the problem in two:

1. **PREPARE, in a worktree, in parallel with anything else.** Write the transformation, run it, fix
   the fallout, get all three trees building and gating from it. This is the expensive part and it
   needs nobody else to stop.
2. **APPLY, to a clean tree, as one commit.** Re-run the SCRIPT against whatever `main` has become —
   not a stale diff. A patch rots the moment someone touches a moved file; a transformation does not,
   which is the whole reason step 1 can run early.

So the only thing that must wait for clean trees is the application, which is minutes. Take the
`layout-move` claim at that point, not during preparation.

**Cleanup is part of the job** — see PROTOCOL.md's worktree rules. The preparation worktree goes under
`psxport/scratch/wt/layout-move` and is removed (`git worktree remove` + `git worktree prune`) in the
same task, whether the move lands or is abandoned.

## THE APPLY PROCEDURE — what the operator actually runs

The preparation is DONE. It produced a tool, not a patch: **`psxport/tools/layout_move.py`**, which
performs the whole move as a transformation and then proves it inert. Re-run it against whatever
`main` has become; never replay a diff.

**Before you start:** the move touches FOUR repos in lockstep (psxport + all three games), because
the games `#include` framework headers by name. There is no ordering that keeps every tree building
in between, so do all four in one sitting.

```sh
# 0. take the claim (this is the moment for it, not during preparation)
mkdir $PSX/coord/claims/layout-move        # "File exists" -> someone beat you to it

# 1. every tree clean, no agent mid-edit. The script REFUSES a dirty runtime/.
for r in spyro spider1 Tomba2Engine; do git -C ~/repo/psx/$r status --porcelain; done
git -C ~/repo/psx/Tomba2Engine/external/psxport status --porcelain

# 2. prove the checker still works before you trust it (negative control, ~5 s)
cd ~/repo/psx/Tomba2Engine/external/psxport
python3 tools/layout_move.py --selftest      # must print: clean=False, body-edit=True, tamper=True

# 3. look before you leap. --plan writes nothing and ABORTS if main grew an unmapped file.
python3 tools/layout_move.py --plan

# 4. do it. Auto-discovers game trees as <workspace>/*/{external/psxport,game} and PRINTS the list;
#    it says so LOUDLY if that number is zero. It must find THREE — spyro, spider1, Tomba2Engine.
#    Any other number means a stray tree is back in the workspace; check before proceeding.
python3 tools/layout_move.py --apply         # ends with the inertness proof; RESULT must be OK
```

`--apply` finishes by running `--verify` against `HEAD`, i.e. against the PRE-move commit — so run
it **before** you commit anything. To re-check later, `--verify --before-ref <the pre-move sha>`.

```sh
# 5. EVERY GAME'S generated/ MUST BE RE-EMITTED. The recompiler now emits `#include "cpu/core.h"`,
#    and the substrate already on disk still says `core.h`. Skipping this gets you a baffling
#    `generated/rec_decls.h:3: fatal error: core.h: No such file or directory`. generated/ is
#    SACROSANCT — regenerate it, never hand-edit it. --apply names each stale tree and its count.
#    NEVER ./run.sh here — it is the USER's play launcher (USER 2026-08-11) and its submodule re-sync
#    reverts in-progress framework work under you. Build explicitly, then use the repo's own gate tool.
for r in spyro spider1 Tomba2Engine; do (cd ~/repo/psx/$r && \
  cmake --build build -j$(nproc) --target "$(basename $r)_port" && python3 tools/gate.py boot); done
#    ensure_recomp.py hashes emit.py, so the move already invalidates the stamp and run.sh re-emits
#    on its own; PSXPORT_FORCE_RECOMP=1 if you want to be sure.
```

### What "RESULT: OK" is asserting

| check | what it proves | its blind spot |
|---|---|---|
| mapping table | every tracked file under `runtime/recomp/` has a home; **aborts** on one it has never seen | — |
| collision check | no game tree owns a header whose basename is also a framework header (which would silently re-point its include) | headers added after the run |
| file set | N expected after the move, N present, 0 missing, 0 unexpected | — |
| **A. content hashes** | multiset of per-file hashes identical before/after, with `#include` lines and `runtime`-path lines removed. Prints the excluded-line count (measured 1.3–3%) | the excluded lines — check B covers them |
| **B. pure re-derivation** | every tracked text file byte-equals the transform of its pre-move content; out-of-scope files must be byte-**unchanged** | non-UTF-8 files, `vendor/`, files untracked in both trees |
| build-file assertions | 12 required strings present in cmake/`gen_gpu_shaders.py`/`emit.py`/`port_gen.py` afterwards — a rule that became a no-op upstream **fails** | — |
| residual scan | zero `runtime/recomp` mentions left in any code/build file, in any of the four repos | historical docs, which are never rewritten by design |

### The one thing in this commit that is not a path

`vendor/beetle-psx/mednafen/psx/gte.c` does `#include "gte_state.h"` — a framework header, reached
by bare name, from a **submodule this repo may not edit**. The script therefore adds one line to
`cmake/psxport.cmake`:

```cmake
set_source_files_properties(${MED}/psx/gte.c PROPERTIES
  INCLUDE_DIRECTORIES ${PSXPORT_ROOT}/runtime/cpu)
```

Deliberately per-SOURCE-FILE, not on the target: `runtime/cpu` never reaches the framework's own
translation units, so no psxport source can go back to resolving `#include "core.h"` unqualified.
Measured, not assumed: `gte_state.h` is the ONLY framework header any file under
`vendor/beetle-psx/mednafen/` includes.

### The include convention the move installs

Taken from Dusklight (`~/repo/dusklight/src/dusk/ui/*.cpp`, CC0): **siblings bare, cross-subsystem
qualified.**

```cpp
// in runtime/gpu/gpu_vk.cpp
#include "gpu_vk.h"                 // sibling — resolves relative to the including file, no -I needed
#include "cpu/core.h"               // another subsystem — says so
#include "config/cfg.h"
#include "game_iface.h"             // THE SEAM, at runtime/ root — looks identical from every tree
```

`runtime/` (not `runtime/recomp/`) is the single PUBLIC include root, so a game writes
`#include "cpu/core.h"` too. The rewrite only matches BARE basenames, which makes it idempotent —
re-running on an already-moved tree is a no-op.

### Committing

**One commit per repo, four commits, and NO behaviour change in any of them** — that is the only
reason check A can be trusted. `git mv` is used throughout, so `git log --follow` and blame survive.

```sh
# psxport first, then the three games, then the three gitlink bumps
git -C ~/repo/psx/Tomba2Engine/external/psxport add -A && git -C ... commit
# ... operator's normal submodule-pin discipline (PROTOCOL.md: record the gitlink BEFORE you build)
```

Then regenerate the maps whose paths the script could not compute — `Tomba2Engine/tools/codemap.py`
(regenerates `docs/code-map.md`; `--selftest` must stay green) — and re-run each game's boot gate:
spyro 212334 presents/no abort · spider1 8969 presents/no abort · Tomba2 243 presents/exit 0 under
`PSXPORT_GATE=1 PSXPORT_RENDER_PSX=1`, all with `PSXPORT_NOPACE=1`.

### Judgement calls in the mapping table, flagged for review

Each is ONE line in `SUBSYS` — disagree and change it, then re-run. They are the files whose home
was not obvious from LAYOUT.md's nine buckets:

| file | placed in | why, and the case against |
|---|---|---|
| `timing.{cpp,h}` | `hle/` | it is the libetc `VSync()` counter mirror = SDK HLE. But it is also the frame clock, and the pacing/field-rate headers went to `gpu/`. |
| `fs_util.{cpp,h}` | `cpu/` | host `std::filesystem` utility used by everything; none of the nine buckets is "host services". |
| `mods.{cpp,h}` | `config/` | the live toggle/param model, seeded from settings. Its only UI is the overlay, so `ui/` is arguable. |
| `gte_beetle.cpp`, `gte_state.h` | `cpu/` | COP2 is part of the machine. `gpu/` is arguable since the GTE feeds geometry. |
| `pad_input.{cpp,h}` | `hle/` | a peripheral, like memcard. There is no `input/` bucket. |
| `snapshot.{cpp,h}`, `watchdog.cpp` | `dbg/` | diagnostics, not the differential harness. |
| `game.h` | `cpu/` | the whole-machine aggregate; sits beside `core.h`. |
| `scea_asset.h` | `cpu/` | a baked texture, but used only by the boot stub it ships with. |
