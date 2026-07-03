# Tomba2Engine — a PC-native reimplementation of Tomba! 2's engine

**Goal:** REBUILD Tomba! 2 as a complete, self-contained **PC-native game engine** in C, running the real
game content. Not an emulator, not a recompiled-MIPS blob with I/O bolted on. **Behave like a PC game; do
NOT simulate the PSX.** Effort/time is not a constraint — correctness over shortcuts, always.

## WORKFLOW FIRST (read this first; it outranks the task)
**If the workflow itself has a problem, fixing it takes PRIORITY over the task in hand.** Re-deriving a
solved bug, a doc/index that's unsearchable, untracked env-var sprawl, a tool that falls short, a
bloated CLAUDE.md — these are workflow defects, and when you hit one you STOP and fix the workflow
*first*, then resume the task. The workflow is meant to evolve itself as needs grow; an un-fixed workflow
defect costs every future session. (This page exists because that wasn't being done.)

The failure mode this project keeps hitting: re-solving things a past session already solved, because
findings were buried. Fight it with the loop below. **Knowledge lives in structured docs, NOT in this
file** — keep CLAUDE.md lean (durable DIRECTIVES only); findings go to the registry; status goes to the
trackers.

- **BEFORE investigating any bug, search the findings registry:** `tools/findings.py <symptom words>`.
  It answers "have we already solved / ruled this out?" If it matches, use it; don't re-derive. It
  searches BOTH the curated registry (`docs/findings/<subsystem>.md`, tagged by symptom, authoritative)
  AND the raw `docs/journal.md` history (so the ~140 un-promoted entries are still findable). Read
  `docs/findings/INDEX.md` at session start. When a raw journal hit was useful, PROMOTE it to a curated
  finding (that's the opportunistic backfill — the journal is raw, the registry is the distilled layer).
- **AFTER you fix/learn something durable, record it** as a block in `docs/findings/<subsystem>.md`
  (format at the top of `tools/findings.py`: symptom / status / cause / fix / refs — record DEAD ENDS
  too), then run `tools/findings.py` to regenerate the index. A finding's home is the registry, not the
  flat journal and not this file.
- **LIVE bug list — check at session start + before diving in:** `tools/bugs.py` (skill:
  `bug-tracker`). This is the "known but not yet fixed" tier — port-visible symptoms the USER has
  reported (wrong render, wrong SFX, missing behavior…) or that diagnostic tools have surfaced. Pairs
  with the findings registry above: findings = distilled root cause + fix; bug list = live symptoms
  awaiting one. When the USER reports a new symptom, ADD it (`## BUG-<n>: <title>` block in
  `docs/known-bugs.md`) before you investigate — the entry IS the "we're on it" signal. When you
  land a plausible fix, flip status to `PORTED-BUT-UNVERIFIED`; when the user confirms, PROMOTE it
  to `docs/findings/<subsystem>.md` and mark the bug entry `FIXED`.
- **CHECK before reimplementing any `FUN_xxxx`:** `tools/codemap.py --addr <hex>` — ~350 native
  reimplementations are indexed by guest address (`docs/code-map.md`). Don't re-derive what exists.
  Regenerate (`tools/codemap.py`) when you add/move a native.
- **Status & spine:** `docs/port-progress.md` is the boot→gameplay execution spine with per-function
  status (owned/partial/todo), the "how to own a function" loop, and the CURRENT FRONTIER. Port
  top-to-bottom in execution order; advance the frontier, don't cherry-pick. Update it in the SAME commit
  as the work.
- **Reference docs (read the relevant one before touching that area):** `docs/engine_re.md` (engine RE),
  `docs/render-arch.md` (VK renderer / depth / headless), `docs/gfx-debug.md` + skill `gfx-debug` (any
  render bug), `docs/config.md` (the `cfg` module — no raw getenv; diagnostics are `debug <chan>`
  channels), `docs/driving-the-game.md` (REPL / reaching a scene), `docs/project-map.md` (build
  cheat-sheet). `docs/journal.md` is the chronological log (raw history); the findings registry is the
  searchable distillation.
- **Self-governance:** when a doc/tool/workflow falls short, IMPROVE it in the same session (the project
  is meant to evolve its own workflow). Keep CLAUDE.md short — if you're tempted to append a long
  explanation here, it's a finding (→ registry) or a doc (→ docs/), not a CLAUDE.md rule.

## Core directives
- **Full native ownership is always the answer.** Every fork defaults to the engine owning it PC-native;
  don't ask which side of a "boundary" something is on, don't offer a keep-PSX option. There is NO
  engine-vs-content fence: the engine systems (below) AND the game content/logic (per-enemy AI/NPC
  behavior, how Tomba moves/jumps/lands, physics/collision, quest/event/progression/game-rule logic, the
  game objects) are ALL valid native-ownership targets. Un-ported code runs as the statically-recompiled
  shard body (the substrate) where it was recompiled (resident MAIN.EXE); "it's content" is not a reason
  not to port it. When in doubt, own it — just build it, don't ask.
- **ONE behavior = the PC game; the enemy is UNTRACKED env sprawl, not gating itself.** Two separate
  things: (a) **behavior** must be single — do NOT add an env toggle to A/B a new native path against the
  old one "until verified"; make it THE behavior, verified via `./run.sh`. Legacy A/B / `*_RECOMP` /
  `FAITHFUL` behavior-flags are scaffolding to RETIRE. (b) **diagnostics** (probes, compares, dumps) ARE
  useful and allowed — but only through the TRACKED `cfg` registry / `debug <chan>` channels, which are
  the single source of truth listed in `docs/config.md`. NEVER raw ad-hoc `getenv` (that's how you get a
  thousand untracked vars nobody can find). When you add a channel, register it + document it in
  config.md; when one is dead, PRUNE it. The rule isn't "no env vars" — it's "every diagnostic env/channel
  is tracked in one place and removed when stale" (this is a WORKFLOW-FIRST concern).
- **The engine OWNS the game; it must NOT deal with PSX intricacies AT ALL.** It owns the world, objects,
  camera, projection, and — critically — its own **render ordering / visibility** (a real depth buffer +
  its own sort). It does NOT read, honor, or reproduce any PSX rendering decision: not the ordering table
  (OT), not draw order, not GTE output, not GP0 packets, not the disp-env. If the engine is consulting
  "what the PSX would draw / in what order," it is wrong — decide it from the scene.
- **REBUILD, don't transcribe.** A native function that reproduces the PSX body's instructions/packets
  byte-for-byte is still PSX-simulation, not a PC engine. Match the engine's *observable result* (the
  world it builds, the picture it draws, the interface state content reads back), not its PSX mechanism.
- **Top-down, PC-driven; PSX never calls PC.** The override/flip system is REMOVED — do NOT use or add
  `rec_set_override` / `rec_super_call`. Ownership starts at boot/main and grows DOWNWARD: a native
  function is reached because its PARENT is native and calls it directly (a plain C call). PC calls PC for
  what it owns; `rec_dispatch(c, leaf)` runs a still-PSX leaf via the static-recomp SUBSTRATE — the
  generated `func_<addr>` body (it can never re-enter native). Contiguity is required — own a function only
  once its caller is PC; no island overrides of deep leaves. The recomp body is the behavioral REFERENCE
  (read it to learn what to build) and the substrate that runs un-owned RESIDENT leaves. **The interpreter
  is GONE (later-254):** a `rec_dispatch` to any address NOT in the recompiled set (overlay code, an
  indirect/computed target discovery missed) FAILS FAST (abort + backtrace) — there is no interpreter
  fallback. Resolve a miss by seeding it into the recompiler (resident, indirectly-reached) or porting it.
- **FAIL-FAST.** All I/O and timing must be PC-native and synchronous. Any PSX async/wait/hardware-poll
  primitive (VSync waits, CD command-waits CD_cw/CdSync, the async CD reader, GPU/MDEC DMA timeouts,
  IRQ-driven loops) and any invalid state (a derailed opcode) must NOT be papered over (instant-return
  stub, fake-complete, spew-and-continue). Either make it PC-native + synchronous (the work is done inline
  so no wait is reached) or, if reached, ABORT with a diagnostic (caller `ra` + guest-stack backtrace).
  Implemented: `sync_overrides.cpp` platform-HLE table (traps VSync; native-syncs CD/GPU/MDEC); a recomp
  MISS (no recompiled fn for an address) aborts in `rec_dispatch_miss` (hle.cpp). Do NOT re-introduce
  instant-VSync / fake-CD bandaids or env escape hatches (`PSXPORT_VSYNC_OK`-style), or an interpreter.
- **No bandaids / no magic constant offsets.** Name the root cause; document every lifted function /
  patched value with the RE that justifies it. (See the global "No bandaids" rule.)
- **REAL C++ CLASSES, no `extern "C"` shims.** Every PC-native subsystem is a `class Foo`, never a
  grab-bag of free functions. Cross-file entries are class methods (never `extern "C" void foo(Core*)`),
  and legacy free-function/`extern "C"` scaffolding in touched files is CLEANUP TERRITORY — class-ify
  it in the same commit; don't leave two shapes half-alive.
  - **DEFAULT = INSTANCE method on a Core-owned subsystem.** `class Foo` is embedded on `Engine` (or
    directly on `Core` for a platform subsystem) with a `core` back-pointer wired in `Core::Core()`;
    callers do `c->engine.foo.method(args)`. This is the shape for every SUBSYSTEM — anything the
    engine/platform OWNS (asset loading, font, placement, pool, sound, camera, render, fade, HUD, …),
    even when the class has no fields today. The class IS the ownership boundary; the fact that a
    subsystem grew state later is normal, and the back-pointer costs nothing. Examples: `Engine`,
    `CutsceneCamera`, `ScreenFade`, `Render`, `Font`, `Placement`, `Pool`, `GraphicsBind`,
    `Animation`, `Asset`.
  - **NARROW EXCEPTION: `static` methods for pure-MATH / UTILITY libraries only.** Use `class Math`
    with all-`static` methods (`Math::rotmat(c, a, b)`) when the class is a math/utility LIBRARY
    with no owned engine domain — pure operations over `Core*` that any caller can invoke stateless,
    the way `std::sqrt` is stateless. The bar is high: it's NOT "no fields today" (subsystems often
    have no fields today either — see the default above); it's "this class will never own anything,
    because it's a computation library, not a subsystem". Examples: `Math` (GTE transform cluster),
    `Mtx` (matrix helpers), `Trig` (rsin/rcos). Static classes are NOT embedded on Engine — callers
    reach them by `ClassName::method(c, …)`, not `c->engine.classname`.
  - **When in doubt, INSTANCE.** A subsystem misclassified as static (`Asset::load(c, …)`) is worse
    than an instance-with-empty-body: it reads as "this isn't really part of the engine", it breaks
    the `c->engine.X.method()` pattern the rest of the codebase uses, and it fights back when the
    subsystem grows real state.
  (User directive, 2026-07-01, refined 2026-07-02 — "does it need to be object oriented?" for pure
  math → static ok; but subsystems stay instance even when stateless.)

## RENDER — reimplement, don't transcribe (the clearest case)
The old "native" render (`engine_submit.cpp` submit_terrain / ov_submit_poly_gt4_bp / native_dl) was a
transcription of the PSX path (GTE compose via `gte_op`, byte-identical GP0 packets) — PSX-simulation, not
a PC renderer. Instead:
- Render CONTENT with a PC renderer: read the engine's SCENE DATA (camera/view, per-object transform =
  node euler+translation+scale, geometry = geomblk prim-lists of pos/UV/color), transform with **float
  matrices**, project, draw textured tris/quads with a **real depth buffer** (`gpu_draw_world_quad` /
  `gpu_vk_draw_*_f`). NO GTE compose, NO `gte_op` for render, NO byte-packed PSX packet.
- The engine OWNS ordering: real depth buffer for 3D, its own explicit layer/sort for 2D (backgrounds,
  HUD, sprites). Never read the OT / GP0 order / computed otz to decide what's on top. Wrong order? Give
  the engine the correct rule, don't re-sync with the PSX.
- The PSX render is a VISUAL REFERENCE only (roughly what the frame should look like) — never a byte-match
  target, never the source of draw order. There is no running oracle.

## Verification — observed behavior on the RUNNING port, cited (no oracle)
- **Engine reimplementation → gate on the OBSERVABLE RESULT, not a byte-match.** Verify the world/menu/
  scene it builds and the picture it draws (the USER eyeballs a build — the agent builds + sends pics /
  asks; it cannot self-verify a render). "Looks/works like the running game" + "the content still behaves"
  is the bar. Don't gate the engine on reproducing PSX instructions/registers/packets — that forces
  transcription.
- **Content-interface correctness MUST hold.** Wherever native code and still-recomp content share guest
  RAM / scratchpad, the handoff must be exactly right — a wrong guest write silently corrupts the other
  side (native terrain clobbered scratchpad 0x1F8001C0 → broke collision, later-158). Inspect that guest
  RAM on the live port (`r`/`rw`/`dumpram`; main RAM is BLIND to scratchpad 0x1F800000 — use the `.spad`
  sidecar — and to GTE regs); reason about correctness from the recomp REFERENCE body.
- **Rendering bugs: inspect the LIVE game; never conclude from a cherry-picked still** (water "looked
  fine" while broken). Order: (1) state/stream inspection on the running port (`debug <chan>`:
  scene/provat/gte/…, or the debug server `scene`/`provat x y`/`vkvram`); (2) build + USER eyeball.
- **Improve tools when they fall short — don't hand-grind or reinvent.** Grep `docs/gfx-debug.md` +
  `tools/` first; extend the existing tool; update the doc + skill in the same change.

## Build / drive / repo
- **`scratch/bin/tomba2_port` IS THE GAME** — recompiled MAIN.EXE (`generated/shard_*.c` from
  `tools/recomp/emit.py`) + native game (`game/*`) + PSX platform (`runtime/recomp/*`). **The build is
  CMake** (`cmake/tomba2_port.cmake` owns the source list, RmlUi static-lib subbuild, SDL_GPU shader gen,
  and link — keep that list in sync when adding a `game/*` / `runtime/recomp/*` file; every `game/`
  subsystem folder is on the include path, so `#include "foo.h"` resolves regardless of subfolder).
  - `./run.sh [disc.chd]` — full: extract MAIN.EXE + `cmake --build build --target tomba2_port` + run.
  - Incremental rebuild without running: `cmake --build build --target tomba2_port` (configure once with
    `cmake -S . -B build`). Renderer regression check: `tools/test_gpu_render.sh`. (The old per-file
    `tools/build_port.sh` g++ build is RETIRED — CMake is the single source of truth.)
- **Drive/observe:** REPL (`PSXPORT_REPL=1`, stdin) or live TCP debug server (`PSXPORT_DEBUG_SERVER=1`,
  `tools/dbgclient.py`). Headless render: `PSXPORT_VK_HEADLESS=1`. Key commands: `run N`, `newgame`,
  `skip N`, `press`/`release`/`tap <btn>`, `r`/`rw`/`w`, `dumpram <path>` (+`.spad`), `shot <path>`
  (VK-readback PPM, works headless), `debug <chans|all>`, `stage`/`regs`/`seq`/`quit`. See
  `docs/driving-the-game.md`. **The oracle is GONE** — no reference emulator, no diff/compare tooling;
  don't look for or recreate it.
- **Repo (N64Recomp model — a reusable PSX→PC framework + the game on top, established in-tree first):**
  `game/` = the PC-native game, organized like a PC game engine by SUBSYSTEM FOLDER — `ai/` (per-object
  behaviors), `object/` (object-list walk+dispatch, animation), `world/` (entity/pool/spawn/placement),
  `render/` (submit/walk/project/cull/fps60/gpu_lib/lighting/terrain + scene/ + mesh/), `camera/`, `scene/`
  (stage/demo/level/init/cutscene), `audio/`, `input/`, `player/` (collision/hitbox), `ui/`, `items/`,
  `math/`, `core/` (clib/asset); top-level `game_tomba2.cpp` + `tomba2_types.h`. `runtime/recomp/` = the
  common PSX→PC platform (dispatch/hle/boot/native_boot, PSX-hw natives gpu_native/spu/gte/mdec/cd, CD/XA/FMV
  subsystems); `tools/` = recompiler + tooling; `generated/` = recompiled MAIN.EXE shards = the SUBSTRATE (gitignored);
  `vendor/beetle-psx` = committed GPL-2 fork used as the GTE/MDEC/SPU/CHD HARDWARE BACKEND (NOT a reference
  emulator; going off it = porting those native, a long-term goal). Put a new native in the SUBSYSTEM FOLDER
  it belongs to — never a general-purpose grab-bag file, never cram unrelated subsystems into one file (the
  old flat `engine/` and `native_path*.cpp` grab-bags are GONE; never recreate them). `scratch/` = gitignored
  artifacts by type (**never /tmp** — RAM tmpfs ~6 GB).

## Hard rules
- **Single `main` branch.** Verified milestones are committed AND pushed to `origin`
  (`github.com/SomeoneIsWorking/psxport.git`).
- Never commit CHDs or machine-specific paths. Disc resolution: CLI arg > `PSXPORT_TOMBA2_DISC` (.env) >
  `*.chd` drop-in.
- Beetle changes live in the committed fork (`vendor/beetle-psx`), NOT out-of-tree `.patch` files.
