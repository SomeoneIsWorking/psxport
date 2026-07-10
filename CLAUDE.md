# Tomba2Engine — a PC-native reimplementation of Tomba! 2's engine

**Goal:** REBUILD Tomba! 2 as a self-contained **PC-native game engine** in C++, running the real game
content. Not an emulator, not a recompiled-MIPS blob with I/O bolted on. **Behave like a PC game; do NOT
simulate the PSX.** Effort/time is not a constraint.

## The 5 paths — canonical vocabulary (read this first)

Two execution paths and two rendering paths. Any run is one exec × one render.

### Execution

- **recomp_path** = `PSXPORT_GATE=1` — the full static-recomp substrate (`generated/shard_*.c`) driving
  gameplay under a native boot+frame loop. Not strictly PSX-faithful — has async→sync conversions and PC-
  native ops (CD/file I/O). **Runs perfectly.** THIS IS THE ORACLE for byte-comparison.
- **pc_faithful** = default (`./run.sh` with no flags) — supposed to be a byte-exact clone of recomp_path
  in clean OOP. Currently BROKEN. Making this byte-exact to recomp_path is **Job #1**.
- **pc_skip** = a **PER-FORK bool** (`Game::mPcSkip`), NOT a third path. Every collapsed multi-step init
  is a fork:
      if (game->mPcSkip) load_in_one_step();  // shortcut — end-state only
      else               load_in_multi_step_faithfully();  // MUST byte-match recomp_path
  Default (`./run.sh`) uses `mPcSkip=true` (shortcut where forks exist). SBS forces `mPcSkip=false` so the
  faithful branch can be byte-compared.

### Rendering (orthogonal to exec)

- **psx_render** = `PSXPORT_RENDER_PSX=1` — the substrate's GTE + OT + GP0 renderer.
- **pc_render** = default — native renderer. Rules (USER 2026-07-07):
  - A READ-ONLY OVERLAY: the PSX render path still EXECUTES underneath (its guest-memory operations
    — packet pool, OT, libgs state — are part of the faithful byte-exact state); pc_render produces
    the PICTURE from its own pass. It bypasses GTE/OT/PSX render subsystems for DRAWING only.
  - Reads guest RAM + PC engine classes (e.g. fade state from the fade engine); writes ONLY its own
    host memory. **Any guest-memory write from pc_render is a bug** that will surface as an SBS diff.
  - Clean OOP. Render bugs are EXPECTED until fixed — and fixes are DEFERRED until pc_faithful is
    recomp-identical.

### Combinations

- `PSXPORT_GATE=1 PSXPORT_RENDER_PSX=1` — recomp_path + psx_render. **Works perfectly** (skips OP.FMV,
  fix deferred). The reference build.
- `PSXPORT_GATE=1` — recomp_path + pc_render. Works, has known rendering issues (deferred).
- `./run.sh` — pc_faithful + pc_render. Currently broken. Target: byte-exact to recomp_path.

### SBS

- `PSXPORT_SBS_MODE=full` — two `Game`s in one process. Core A = pc_faithful, Core B = recomp_path. Byte-
  compares guest RAM step-for-step. Both cores get `mPcSkip=false`. Divergences are FATAL — no allowlist,
  no residual list, no "known diff". A diff means pc_faithful is wrong (usually) or recomp_path is wrong
  (rare — recomp itself has bugs; validate against the running game).

### Job #1 — right now

Run SBS full, find the first pc_faithful divergence, root-cause it, fix. Repeat until zero-diff.
Rendering bugs are deferred UNTIL (a) zero-diff is reached, OR (b) an SBS diff traces back to pc_render
writing to guest memory (rule violation).

---

## WORKFLOW FIRST (outranks the task)

If the workflow itself has a problem, fixing it takes PRIORITY over the task in hand. Re-deriving a
solved bug, a doc that's unsearchable, untracked env-var sprawl, a bloated CLAUDE.md — these are
workflow defects. Stop and fix them, then resume.

**Knowledge lives in structured docs, NOT in this file.** Keep CLAUDE.md lean (durable DIRECTIVES only).

- **BEFORE investigating any bug**, search: `tools/findings.py <symptom words>`. Searches curated
  registry (`docs/findings/<subsystem>.md`) + raw `docs/journal.md`. Read `docs/findings/INDEX.md` at
  session start. Promote useful journal hits.
- **AFTER a durable fix**, record it in `docs/findings/<subsystem>.md` (symptom / status / cause / fix /
  refs — record DEAD ENDS too), then run `tools/findings.py` to regen the index.
- **Live bug list** — `tools/bugs.py` (skill `bug-tracker`) is GitHub Issues. Add a bug when the USER
  reports a symptom; flip `ported-unverified` on a plausible fix; promote to findings on confirm.
- **CHECK before reimplementing any `FUN_xxxx`:** `tools/codemap.py --addr <hex>` — ~350 natives indexed
  by guest address (`docs/code-map.md`). Regenerate on add/move.
- **Status & spine:** `docs/port-progress.md` — boot→gameplay execution spine, per-function status,
  current frontier. Port top-to-bottom. Update in the same commit as the work.
- **Fleet workflow (operator + subagents):** `docs/fleet-workflow.md` — how to drive ownership at scale
  (one operator orchestrates isolated sonnet subagents; dispatch → integrate → honest SBS gate → push).
  Read it before running a fleet.
- **Reference docs:** `docs/faithful-execution.md` (HOW pc_faithful achieves byte-exactness — guest-stack
  residency, native fibers, ported scheduler primitives; read before touching any faithful path),
  `docs/engine_re.md` (engine RE), `docs/render-arch.md` (VK renderer), `docs/gfx-
  debug.md` + skill `gfx-debug` (render bugs), `docs/config.md` (cfg module — no raw getenv), `docs/
  driving-the-game.md` (REPL), `docs/project-map.md` (build).
- **Self-governance:** improve doc/tool/workflow when it falls short, same session.

## RE first — never black-box debug

At any diff-hunt trigger (magic offset, `sub_XXXX`, taxi free-fn, mystery `obj[+0xNN]`), PAUSE and RE
the surrounding function into named struct types + owned methods FIRST. Mechanical fold-in is only for
already-RE'd code. RE default = **Ghidra headless** (`tools/decomp.sh`) — never `tools/disas.py` to
understand behavior, never hand-walk backwards through addresses. `disas.py` is single-instruction
spot-check AFTER Ghidra only.

## Core directives

- **Full native ownership is always the answer.** No engine-vs-content fence. Enemy AI, physics,
  quests, game rules — all portable native. Un-ported code runs as the substrate (`func_<addr>`). When
  in doubt, own it.
- **ONE behavior, no env-gate.** Do NOT add a PSXPORT_* toggle to A/B a new native path against the old
  one. Make the new path THE path. Legacy `*_RECOMP` / `FAITHFUL` behavior-flags are scaffolding to
  retire. Diagnostics (`PSXPORT_DEBUG=chan,chan`) are fine — but every channel is registered in `cfg`
  and documented in `docs/config.md`; never raw `getenv`.
- **Engine owns the game; NO PSX intricacies leak in.** Own the world, objects, camera, projection, and
  render ordering. Never read/honor/reproduce OT / draw order / GTE output / GP0 packets / disp-env. If
  reasoning about GP0 to explain something, STOP — rebuild from GAME STATE.
- **REBUILD, don't transcribe.** A native fn that reproduces PSX instructions/packets byte-for-byte is
  PSX-simulation. Match the observable RESULT (world it builds, picture it draws, interface state
  content reads back), not the PSX mechanism.
- **One global dispatch point; engine overrides allowed (USER 2026-07-07).** `rec_dispatch(c, addr)`
  is THE choke point. A native engine class may be wired by guest address in `EngineOverrides`
  (runtime/recomp/engine_overrides.h, registered on Game) — then EVERY caller, substrate included,
  reaches the native method. This retires the old "PSX never calls PC / contiguity required / only
  hardware-sync HLE" rule: a leaf engine (e.g. the fade engine) can be owned globally on its own.
  Rules: handlers use guest ABI (args in r4..r7, ret in r2) and must byte-match the substrate body
  (SBS gates it — core B / `psx_fallback` never consults the table, staying the pure reference);
  native call sites prefer routing wired addresses through `rec_dispatch` so calls stay uniform and
  traceable (`PSXPORT_DEBUG=dispatch` logs every override hit with frame/core/ra/args). PlatformHle
  remains the separate BIOS/hardware-sync HLE table. **No interpreter fallback**: a `rec_dispatch`
  miss aborts with a backtrace. Fix a miss by seeding the recompiler, porting, or wiring an override.
- **FAIL-FAST.** All I/O and timing PC-native + synchronous. Any PSX async/wait (VSync waits, CD
  command-waits, async CD reader, GPU/MDEC DMA timeouts, IRQ loops) must be done inline or ABORT with a
  diagnostic. Do NOT re-introduce instant-VSync / fake-CD bandaids or env escape hatches.
- **No bandaids / no magic constant offsets.** Name the root cause; every lifted fn / patched value gets
  RE justification. Especially: **no residual RAM diverges** — no SBS diff may be waved off as
  known/expected/residual. Only exception: memory the still-recomp side never reads.
- **REAL C++ CLASSES, no `extern "C"` shims.** Subsystems are instance methods on Core-owned classes
  (`c->engine.foo.method(args)`), pure math/utility is static (`Math::rotmat(c, a, b)`). No `ov_*` free
  functions, no `foo_impl` helpers under `Class::foo` wrappers. When in doubt, INSTANCE. See
  `docs/oop.md` for the shape.
- **No file-scope globals.** No `g_*`, no non-const file-scope statics anywhere in `game/` or
  `runtime/recomp/`. Everything a real class with a header, state on `Game`/`Engine`/subsystem.
- **MIRROR THE GUEST STACK — never revert/exclude a leaf because it pushes a frame.** If the substrate
  body of a leaf you're owning descends `sp` (`addiu sp,-N` + register spills), the native port MUST
  reproduce that frame: `c->r[29] -= N` at entry, write the callee-save spills (`ra`/`s0..s3`) at their
  RE'd offsets with the LIVE values (ra = the RE'd guest return-address constant, not a magic number),
  `c->r[29] += N` before return. Then the guest-stack bytes byte-match and SBS is 0-diff. This IS the
  port — `game/world/object_table.cpp` is the reference (also beh_pickup_collect_trigger/typed_anim_spawn/
  a06_script_fades). "Diverges at 0x801FE9xx because native doesn't replicate the stack frame" is NOT a
  reason to revert, leave unwired, or add an `isDeadStackScratch`-style exclusion — mirror the frame. A
  dead-scratch exclusion is a last resort ONLY for a slot proven unread AND proven impossible to mirror;
  it is never a substitute for guest-stack residency. See `docs/faithful-execution.md`. Run
  `tools/abi_extract.py <addr> --contract`/`--scaffold` FIRST — it derives the frame size/spill offsets/
  ra constants/callee-saved liveness straight from `generated/`, so this stops being hand-derived. See
  `docs/abi-extract.md`.

## Render — reimplement, don't transcribe

`pc_render` reads scene data (camera/view, per-object transform, geomblk prims) and draws with float
matrices + real depth buffer (`gpu_draw_world_quad` / `gpu_vk_draw_*_f`). NO GTE compose, NO `gte_op`
for render, NO byte-packed PSX packet. Engine owns ordering — real depth for 3D, explicit layer/sort
for 2D. `psx_render` is the visual reference / substrate renderer; never the source of draw order,
never a byte-match target.

`pc_render` reads state from other classes (fade → `ScreenFade`, camera → `CutsceneCamera`, …). It
**MUST NOT WRITE to guest memory** — that's what would break the pc_faithful byte-compare invariant. If
you find yourself wanting to write guest RAM from render code, you're building the wrong abstraction.

## Verification

- **Bug-hunt loop + oracle integrity: `docs/bug-hunt-workflow.md` (read first).** Find bugs by oracle
  compare across the PC SKIP × RENDERER matrix; NEVER debug a divergence before the divergent call
  chain is FULLY OWNED end-to-end (grow ownership first). The SBS oracle (core B) must stay pure —
  only PlatformHle + the `gen_func_*` body; engine/game natives on the process-global `g_override[]`/
  `g_ov_*` tables MUST install via `engine_set_override_*` (runtime/recomp/engine_override_thunk.cpp),
  never the raw `shard_set_override`. A sudden 0-div where SBS used to diverge = suspect the oracle.
- **Job #1 — SBS byte-exact.** `PSXPORT_SBS_MODE=full ./run.sh`. Every diff is fatal. Root-cause + fix.
- **Rendering (once byte-exact) — observable result.** Agent builds, USER eyeballs. Non-visual RAM/
  state probes fine. Don't gate render on reproducing PSX packets — that forces transcription.
- **Content-interface correctness must hold.** Where native and substrate share guest RAM /
  scratchpad, the handoff must be exact. Native terrain once clobbered scratchpad 0x1F8001C0 → broke
  collision (later-158). Inspect via `r`/`rw`/`dumpram` (+ `.spad` sidecar; main RAM is BLIND to
  scratchpad and GTE regs).
- **Improve tools when they fall short.** Grep `docs/gfx-debug.md` + `tools/` first; extend, don't
  reinvent; update doc + skill same change.

## Build / drive / repo

- **`scratch/bin/tomba2_port` IS THE GAME** — recompiled MAIN.EXE (`generated/shard_*.c`) + native game
  (`game/*`) + PSX platform (`runtime/recomp/*`). Build = **CMake** (`cmake/tomba2_port.cmake` owns the
  source list — keep in sync when adding files; every `game/` subfolder is on the include path).
  - `./run.sh [disc.chd]` — extract MAIN.EXE + build + run.
  - Rebuild-only: `cmake --build build --target tomba2_port` (configure once with `cmake -S . -B build`).
- **Drive/observe:** REPL (`PSXPORT_REPL=1`, stdin) or debug server (`PSXPORT_DEBUG_SERVER=1`,
  `tools/dbgclient.py`). Headless render: `PSXPORT_VK_HEADLESS=1`. Key REPL: `run N`, `newgame`,
  `skip N`, `press/release/tap <btn>`, `r`/`rw`/`w`, `dumpram <path>` (+`.spad`), `shot <path>`,
  `debug <chans|all>`, `stage`/`regs`/`seq`/`quit`. See `docs/driving-the-game.md`.
- **Repo layout:**
  - `game/` — PC-native game, organized by SUBSYSTEM FOLDER: `ai/` `object/` `world/` `render/`
    `camera/` `scene/` `audio/` `input/` `player/` `ui/` `items/` `math/` `core/`. Top-level
    `game_tomba2.cpp` + `tomba2_types.h`.
  - `runtime/recomp/` — common PSX→PC platform (dispatch/hle/boot/native_boot; PSX-hw natives
    gpu_native/spu/gte/mdec/cd; CD/XA/FMV subsystems; `Game` + `Sbs`).
  - `generated/` — recompiled MAIN.EXE shards (gitignored) = the substrate.
  - `vendor/beetle-psx` — committed GPL-2 fork, GTE/MDEC/SPU/CHD hardware backend (not a reference
    emulator; going off it = porting those native, long-term).
  - `tools/` — recompiler + tooling. `scratch/` — gitignored artifacts by type (**never `/tmp`**).
  - Put a new native in its SUBSYSTEM FOLDER; never a grab-bag file.

## Hard rules

- **Single `main` branch.** Verified milestones committed AND pushed to `origin`
  (`github.com/SomeoneIsWorking/psxport.git`).
- Never commit CHDs or machine-specific paths. Disc: CLI arg > `PSXPORT_TOMBA2_DISC` (.env) > `*.chd`
  drop-in.
- Beetle changes in the committed fork, NOT out-of-tree `.patch` files.
