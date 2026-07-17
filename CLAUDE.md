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

### Enhancements (third behavior class, USER 2026-07-16)

- **pc_enh** = `PSXPORT_ENH=<name,name|all>` — deliberate, MEANINGFUL guest-state changes on top of
  the faithful engine (planned: expanded object load/unload, faster fades/transitions). One name per
  enhancement + `all` umbrella, gated via `cfg_enh("name")` and registered in `docs/config.md`.
  Force-suppressed under `PSXPORT_ORACLE`/SBS inside `cfg.c`, so byte-compares stay enhancement-free
  by construction. Contrast: pc_render never writes guest memory; pc_skip changes no meaningful
  end-state; pc_enh is the only class allowed to change what the game does.

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
- **Live bug list** — `tools/kanban.py` (skill `bug-tracker`) is a LOCAL in-repo kanban (`docs/kanban/`
  cards; columns backlog|todo|doing|done). Add a card when the USER reports a symptom; `move <id> doing`
  when chasing; promote to findings + `move <id> done` on confirm. Evidence images in `docs/reference/issues/`.
- **CHECK before reimplementing any `FUN_xxxx`:** `tools/codemap.py --addr <hex>` — ~350 natives indexed
  by guest address (`docs/code-map.md`; warns ⚠ DUAL-OWNERSHIP if already owned in another file).
  Regenerate on add/move. `tools/codemap.py --conflicts` lists every duplicate-owned address — a native
  RE'ing a `FUN_xxxx` some other subsystem already owns (how FUN_80040B48/80040CDC got duplicated).
- **The TRACKING STACK — four orthogonal maps, one question each (consult at task start, update in the
  SAME commit as the work):**
  - **codemap** (`tools/codemap.py` → `docs/code-map.md`) — WHERE is it: guest addr → native owner,
    LIVE/ORPHAN, `--addr`/`--conflicts`. Auto-scanned from source.
  - **port-map** (`tools/portmap.py` → `docs/port-map.md`) — IS it ported, and REAL not a HACK: the RE
    dependency chain with a `verified | ported-unverified | hack | todo | blocked` axis. `portmap.py next`
    = the next RE-ready step (work THAT, not a downstream one); `portmap.py hacks` = the debt list (keep
    it shrinking — a hack is a stopgap standing in for the real mechanism, and MUST name its real fix +
    death condition); `portmap.py check` flags jumped-ahead work. The focused frontier over port-progress.md.
  - **parity-map** (`tools/parity.py` → `docs/parity-map.md`) — IS it SBS byte-exact (Job #1): per unit,
    `verified` (cite frames+gate+evidence) | `diverges` (a live Job#1 bug — `check` FAILS) | `partial` |
    `untested` | `n/a` (pc_render overlays are n/a — they never write guest RAM). Record every SBS 0-diff
    here so the proof is durable: `parity.py set <unit> --status verified --frames N --gate '<cmd>' --evidence <commit>`.
  - **behavior-map** (`tools/behavior.py` → `docs/behavior-map.md`) — WHAT it DELIBERATELY changes vs
    recomp_path: the ledger of SANCTIONED divergences (pc_render, fps60, widescreen, ires, pc_skip,
    pc_enh), so a byte diff triages instantly as bug-or-intended. Primary axis = GUEST-MEMORY AFFECT:
    `none` (host-only overlay — a guest write is a BUG) | `non-canon` (writes guest RAM but byte-matches
    at the rendezvous — pc_skip) | `full` (deliberately changes canon state — pc_enh; MUST be
    force-suppressed under ORACLE/SBS or `behavior.py check` FAILS). Register every enhancement here.
- **Status & spine:** `docs/port-progress.md` — boot→gameplay execution spine, per-function status,
  current frontier (the DETAIL behind port-map). Port top-to-bottom. Update in the same commit as the work.
- **Fleet workflow (operator + subagents):** `docs/fleet-workflow.md` — how to drive ownership at scale
  (one operator orchestrates isolated sonnet subagents; dispatch → integrate → honest SBS gate → push).
  Read it before running a fleet.
- **Reference docs:** `docs/faithful-execution.md` (HOW pc_faithful achieves byte-exactness — guest-stack
  residency, native fibers, ported scheduler primitives; read before touching any faithful path),
  `docs/engine_re.md` (engine RE), `docs/render-arch.md` (VK renderer), `docs/gfx-
  debug.md` + skill `gfx-debug` (render bugs), `docs/config.md` (cfg module — no raw getenv), `docs/
  driving-the-game.md` (REPL + scenario replays), `replays/` (deterministic pad-capture library —
  reproduce bugs/scenarios headless without live input; see `replays/README.md`), `docs/project-map.md` (build).
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
- **One override registry, one dispatch point (USER 2026-07-16).** A native engine class is wired by
  guest address in THE registry — `overrides::install(addr, name, native, gen[, setter])`
  (runtime/recomp/override_registry.h) — then EVERY caller, substrate included, reaches the native
  method. This retires the old "PSX never calls PC / contiguity required / only hardware-sync HLE"
  rule: a leaf engine (e.g. the fade engine) can be owned globally on its own. ONE entry per address
  holds `{ native, gen }`; one dispatch decision runs `gen` on the oracle leg (core B / `psx_fallback`
  / `verify.inSubstrateLeg`) and `native` everywhere else, shared by the two interception points — the
  `g_<mod>_override[]` thunk (direct `func_X(c)` calls) and `rec_dispatch`. `gen` is the recompiled
  body (`gen_func_`/`ov_<tag>_gen_`); `gen == native` expresses an oracle-allowed primitive (scheduler
  leaves that must fire on both cores). `setter` = the module's `shard_set_override`/`ov_<tag>_set_override`
  to intercept direct callers too, or `nullptr` for rec_dispatch-only wiring (keeps a direct call the
  port deliberately leaves on the substrate). Handlers use guest ABI (args in r4..r7, ret in r2) and
  must byte-match the substrate body — SBS gates it. There is NO separate register_/traceHit table and
  NO hand-rolled `psx_fallback` guard: the gate lives in ONE place (`PSXPORT_DEBUG=dispatch`/`ovhit`).
  PlatformHle remains the separate BIOS/hardware-sync HLE table (raw `shard_set_override`, fires on
  both cores). **No interpreter fallback**: a `rec_dispatch` miss aborts with a backtrace. Fix a miss
  by seeding the recompiler, porting, or wiring an override.
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
  `docs/abi-extract.md`. For a NEW port, prefer `tools/port_gen.py` (byte-faithful draft) +
  `runtime/recomp/guest_abi.h` (opt-in register/frame proxies) + `tools/port_check.py` (equivalence
  gate) — see `docs/port-framework.md`.

## BREAK FIRST, THEN REBUILD (USER 2026-07-16; generalizes the 2026-07-15 render directive)

When replacing a transcription/fallback/transitional mechanism with a native producer, the ORDER is:
**delete the old mechanism FIRST** — let the gap be honestly visible (missing layer, crash-with-
identity) — **then rebuild it natively**. Never keep a stopgap alive because removal is inconvenient,
and never build the replacement alongside the hack "to be safe": the hack's presence hides whether
the rebuild actually works. Applies to render fallbacks, observer/registry shims, compare
relaxations, and any "transitional" machinery a plan doc promises to retire.

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
  only PlatformHle + the `gen_func_*` body. Engine/game natives MUST install via
  `overrides::install(addr, name, native, gen[, setter])` (runtime/recomp/override_registry.h), which
  carries the `gen` body the oracle leg runs and installs the shared oracle-gated thunk; NEVER call the
  raw `shard_set_override`/`ov_<tag>_set_override` directly for an engine/game native (that reintroduces
  the fake-0-diff-on-core-B bug). A sudden 0-div where SBS used to diverge = suspect the oracle.
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
