# Tomba2Engine — a PC-native reimplementation of Tomba! 2's engine

**Goal (2026-06-16; scope clarified 2026-06-19; sharpened by the user 2026-06-20):** turn Tomba! 2 into a
real **PC-native GAME ENGINE** — **REBUILD Tomba! 2's engine as a complete, self-contained PC game
engine** in native C, running the real game content. Not an emulator, not a recompiled-MIPS blob with
I/O bolted on. **Behave like a PC game, do NOT simulate the PSX.**

**STANDING DIRECTIVE — FULL OWNERSHIP IS ALWAYS THE ANSWER (user, 2026-06-20). Do NOT ask which side of
the boundary something falls on, do NOT offer a "lighter"/incremental-keep-PSX option, do NOT propose
retaining a PSX mechanism for convenience.** Every fork defaults to the engine owning it PC-native.
Effort/time is NOT a constraint ("I don't care if it takes years"). **NOTHING is fenced off from native
ownership any more (user, 2026-06-21): the old "keep the game CONTENT as recompiled PSX" rule is REMOVED.**
Characters/AI, physics/collision response, quests/event/game-rule logic — the things THE BOUNDARY used to
reserve for PSX — are now ALSO valid native-ownership targets, exactly like the engine. Un-ported code still
runs as the recomp body so the game stays playable at every step, but "it's content" is no longer a reason
NOT to port something (port it when it helps — e.g. the per-object state machines that dominate the perf
hot-list). When in doubt, own it; just build it, don't ask.

**ONE BEHAVIOR = the PC game. Do NOT env-gate behavior (user, 2026-06-20).** New ownership work is THE
path, not an opt-in flag. Do NOT add a `PSXPORT_*` toggle to A/B the PC-native path against the old PSX
one "until verified" — make it the behavior; the user verifies the single behavior via `./run.sh`. The
legacy A/B / `*_RECOMP` / `FAITHFUL` flags are scaffolding to RETIRE, not a pattern to extend. (Genuine
DIAGNOSTIC tools — `PSXPORT_DEBUG=chan` probes, `PSXPORT_SBS` compare — are not "behavior" and may stay.)

**The engine must NOT deal with PSX intricacies AT ALL — it OWNS the game itself.** A complete PC game
engine owns its world and draws it the way a PC game does: it owns the game objects, the camera and
**projection**, and — critically — **its own render ordering / visibility (real depth buffer + its own
sort)**. It does NOT read, honor, or reproduce *any* PSX rendering decision: not the PSX ordering table
(OT), not PSX draw order, not GTE output, not GP0 packets, not the PSX disp-env. Those are PSX
intricacies; the rebuilt engine has no business knowing they exist. If the engine is consulting "what
the PSX would draw / in what order" to decide what appears on screen, it is wrong — it must decide
that itself from the scene (e.g. the sea-background-drawn-on-top bug is exactly this: order inherited
from PSX instead of owned by the engine).

## THE BOUNDARY — REBUILD everything PC-native; the recomp is the reference + the live fallback (read this first)
**UPDATE 2026-06-21: the engine-vs-content boundary is GONE.** It used to fence content (characters/AI,
physics, quests) off as "stays PSX"; that rule is removed — everything is now a native-ownership target.
What remains true and still most-misread: a game engine is FAR more than rendering, and the verb is
**REBUILD**, not "transcribe" — we build a PC game that produces the same result, not a line-by-line C copy
of the PSX body. The ENGINE list below is still the TOP-TO-BOTTOM priority spine (own it in execution
order); content is now ALSO fair game, especially where it's hot (the per-object state machines).

**THE ENGINE — we REIMPLEMENT it natively in C, as a PC game would do it (the WHOLE list, not just render):**
- **main menu / title / front-end UI menus**, and the main game loop
- **level / stage LOADING** — read the level's data and build the world
- **character / model / texture / animation / ASSET loading**
- **terrain** — load it, place it, build the mesh, AND render it (terrain is an engine SYSTEM, not "render")
- **object / entity PLACEMENT & spawning** — where things are put in the world
- **scene / world management, camera, PC-native projection, per-object cull / LOD**
- **render ordering / visibility** — the engine decides what occludes what with its OWN real depth buffer
  and its OWN sort; it NEVER inherits draw order from the PSX OT / GP0 stream (see RENDER below)
- **render submission + the renderer** (PC-native: float transforms, real depth — see RENDER below)
- save/load flow, 60fps interpolation, widescreen.

**The game CONTENT / LOGIC — now ALSO a native target (rule removed 2026-06-21).** Previously this was
fenced off ("characters/AI, physics/collision response, level data, quests/events stay recompiled PSX, we
do NOT reimplement"). That fence is gone: per-enemy AI / NPC behavior, how Tomba moves/jumps/lands,
quest/event/progression/game-rule logic are all valid to reimplement native, same methodology as the
engine. The **game objects** (entity/object model, placement, spawning, transforms, scene graph, ordering)
are PC-owned as before. Two things still hold regardless: (1) un-ported content keeps running as the recomp
body, so the game stays playable while you advance; (2) **CONTENT-INTERFACE CORRECTNESS** — wherever native
code and still-recomp content share guest RAM / scratchpad, the handoff must be exactly right; a wrong guest
write silently corrupts the other side (that is how the native terrain made Tomba fall through the ground,
later-158). When native and recomp coexist for the same data, the native writes must match the reference.

**The recomp has TWO distinct roles — never conflate them:**
1. For the **ENGINE**, the recompiled body (`gen_func_*`) is the **behavioral REFERENCE / oracle only**:
   read what it does, understand the data, then **REIMPLEMENT it PC-native**. A native function that
   merely reproduces the PSX body's instructions/packets byte-for-byte is **still PSX-simulation, not a
   PC engine** — that is NOT the deliverable. Match the engine's *observable result* (the world it
   builds, the picture it draws, the interface state the content reads), not its PSX mechanism.
2. For **still-unported CONTENT/LOGIC**, the recompiled body is the **live runtime** that keeps running in
   guest memory so the game stays playable. The engine calls it. As of 2026-06-21 we MAY rewrite it native
   too (the fence is gone) — until we do, it runs as recomp; once owned, it's a native function like any other.

**PORT PROGRESS TRACKER — `docs/port-progress.md` — READ THIS FIRST, every session.** It is the single
source of truth: the boot→gameplay execution SPINE with per-function status (owned/partial/todo), the
"how to own a function" LOOP, the REPL-driven VERIFY recipe, and the CURRENT FRONTIER. We port the
engine TOP-TO-BOTTOM in execution order — advance the frontier, don't cherry-pick. UPDATE it (status + the
journal) in the SAME commit whenever you own a function, so the next session doesn't re-derive what's done.

Active plan: `<local-notes>/plans/fancy-tinkering-kite.md`. Engine RE: `docs/engine_re.md` (read first).
Findings/dead-ends: `docs/journal.md`. Project map / build cheat-sheet: `docs/project-map.md`.
**Render/present pipeline (VK renderer, native depth, headless offscreen VK): `docs/render-arch.md`.
Config & the `cfg` module (do NOT add raw `getenv`; diagnostics are REPL channels via `debug <chan>`,
NOT env vars): `docs/config.md`. How to DRIVE the game (the REPL, reaching a scene): `docs/driving-the-
game.md`.** Read these before touching graphics or driving the game — don't re-derive them.
**Graphics/render debugging: `docs/gfx-debug.md` (skill `gfx-debug`) — READ FIRST before any rendering
bug; the engine OWNS its render (no oracle to diff against), so verify on the LIVE game + USER eyeball.**

## Repo structure — framework vs game (N64Recomp model, boundary-first)
End state (like N64Recomp/Zelda64Recomp): a **generic, reusable PSX→PC framework** as a submodule, and
**Tomba2Engine** as the game repo on top. We are establishing that boundary **in-tree first**, keeping
one build, then extracting the common PSX part into its own repo (gh authed as `SomeoneIsWorking`).
- `engine/` — **Tomba2Engine, the game-specific native engine + RE.** game_tomba2.c (engine registration /
  the PC-driven call tree), wide60.c (60fps interpolation), tomba2_types.h, and the native engine modules
  being lifted (engine_tomba2.c …). This is the heart of the project. (No override flips — see Methodology.)
- `runtime/recomp/` — **common PSX→PC platform** (future `psxport` submodule): R3000 interp + recomp
  glue (mem, interp, hle, threads, timing, boot, native_boot/stub, watchdog, stubs, sync_overrides),
  PSX hardware natives (gpu_native, spu_*, gte_beetle, mdec_beetle, cdc_native, disc, pad_input,
  memcard), and the CD/XA/FMV subsystems (cd_override, xa_stream, native_fmv — generic mechanisms with
  game-tunable hooks; their game-specific bits get hooked out as the boundary firms up).
- `tools/` — recompiler (`recomp/emit.py`), `discdump`, `disas.py`, `dbgclient.py`, bgm/frame tooling.
- `generated/` — recompiled Tomba! 2 MAIN.EXE (`shard_*.c`); gitignored, rebuilt by run.sh.
- `vendor/beetle-psx` — our committed Beetle PSX fork (GPL-2), submodule. **It is the port's GTE/MDEC/
  SPU/CHD HARDWARE BACKEND** (the port links mednafen `gte.c`/`mdec.c`/`spu.c` + libchdr) — NOT a
  reference emulator. Cutting it off Beetle = porting those subsystems native (a standing long-term
  goal, not a deletion). `scratch/` = gitignored artifacts by type (**never /tmp**, RAM tmpfs ~6 GB).

## ONE binary — the native port (no oracle anymore)
- **The native PORT — `scratch/bin/tomba2_port` — IS THE GAME.** Recompiled MAIN.EXE (`generated/
  shard_*.c` from `tools/recomp/emit.py`) linked with the native engine (`engine/*.cpp`) + the PSX
  platform (`runtime/recomp/*`). **NO Makefile builds it; `make` builds nothing now.**
    - `./run.sh [disc.chd]` — full: extract MAIN.EXE, recompile, compile every TU, link, **and run**.
    - `tools/build_port.sh [files…|all]` — incremental compile+relink, no run (~0.5s/file). Object
      cache `scratch/obj/`. **Keep its SRC list in sync with run.sh** when adding an `engine/*` or
      `runtime/recomp/*` file.
- **Drive/observe via the REPL** (`PSXPORT_REPL=1`, commands piped on stdin) or the live TCP debug
  server (`PSXPORT_DEBUG_SERVER=1`, `tools/dbgclient.py`). Headless render for screenshots:
  `PSXPORT_VK_HEADLESS=1`. Key REPL commands: `run N`, `newgame` (pulse to the GAME prologue), `skip N`
  (advance the intro into the field), `press`/`release`/`tap <btn>`, `r`/`rw`/`w` (memory), `dumpram
  <path>` (+ `.spad` scratchpad sidecar), `shot <path>` (VK-readback PPM, works headless), `debug
  <chans|all>` (enable diagnostic channels at runtime), `stage`/`regs`/`seq`/`quit`. See
  `docs/driving-the-game.md`.
- **THE ORACLE IS GONE (2026-06-20).** There is no reference emulator in the repo — `runtime/wide60rt`
  and all diff/compare tooling (gpu_differ, dualcore, drive.py, vk_depth_diff, …) were removed by user
  directive ("we are past the harness point"). Do NOT look for them, re-create them, or describe the
  port as validated against an oracle.

## Methodology — TOP-DOWN PC-DRIVEN; PC calls PC, `rec_dispatch` the PSX leaves, PSX NEVER calls PC
**THE OVERRIDE SYSTEM IS REMOVED (user, 2026-06-22).** The old "flip" model — recompiled PSX code is the
driver and the interpreter flips into native C wherever `rec_set_override(addr, fn)` is registered — is
GONE. Do NOT use `rec_set_override` / `rec_super_call`, and do NOT add new ones. The interpreter no longer
consults an override table, so a PSX body run via `rec_dispatch` runs PURE PSX all the way down — it can
never re-enter native code. **PSX never calls PC.**

The architecture is **top-down, PC-driven**:
- **PC is the driver.** Ownership starts at the boot/main entry and grows DOWNWARD in execution order. A
  native function is reached because its PARENT is already native and **calls it directly (a plain C
  call)** — never because an address got intercepted.
- **PC calls PC** for everything it owns (direct C calls, a real call tree), and **`rec_dispatch(c, leaf)`
  for the still-PSX leaves** it hasn't reimplemented yet. The leaf runs as pure recomp and returns.
- **Contiguity is required.** You can only `rec_dispatch` a PSX leaf when EVERYTHING above it is already
  PC. You can only own a function once its caller is PC. This is why we port strictly top-down in
  execution order (see `docs/port-progress.md`) — no island overrides of deep leaves.
- The recomp body is still the **behavioral REFERENCE** (read it to learn what to build) and the **live
  fallback** for un-owned leaves (run it via `rec_dispatch`). It is no longer entered by a flip.

**ONE PC-native behavior — no env A/B gating.** Do NOT add a `PSXPORT_*` toggle to A/B a new native path
against the old one "until verified"; make it the behavior and verify by running the game (see below).

**Two different gates — pick by WHICH side of the boundary you are on:**
- **Engine reimplementation → gate on the OBSERVABLE RESULT, NOT a byte-match.** Reimplement the system
  the way a PC game would, then verify the *result*: the world/menu/scene it builds, the picture it
  draws (the USER eyeballs a build — the agent builds + sends pics / asks), and — critically — **the
  interface state the retained PSX content reads back is correct** (the guest fields/structs the AI/
  physics consume). Do NOT gate the engine on reproducing the PSX body's instructions/registers/packets
  byte-for-byte; that gate forces transcription, the wrong deliverable. "Looks/works like the running
  game" + "the content still behaves" is the bar.
- **Content interface (the guest RAM the PSX AI/physics read) → MUST be correct.** Where the engine
  writes state the PSX content consumes, those writes must be right, and a divergence there is a real
  bug. Inspect that guest RAM on the LIVE port via the REPL (`r`/`rw`/`dumpram`) and reason about
  correctness from the recomp REFERENCE body — there is no oracle to auto-diff against. NB main RAM is
  BLIND to **scratchpad (0x1F800000)** (use `dumpram`'s `.spad` sidecar) and GTE regs — a wrong
  scratchpad write can corrupt the PSX content invisibly (later-158: terrain clobbered scratchpad →
  broke collision).

### RENDER — the clearest case of "reimplement, don't transcribe"
The renderer is part of the engine, so it follows the engine rule above. What the tree once called
"native" render (`engine_submit.cpp` `submit_terrain`, `ov_submit_poly_gt4_bp`, `native_dl`) was actually
a **transcription of the PSX render path** — it reproduced the GTE matrix compose (MVMVA columns + CR0-7
packing), drove `gte_op` (emulated PSX GTE), and filled GP0 packet words byte-identical to the guest
packet. That is PSX-simulation, not a PC renderer. Instead:
- Render the world's CONTENT with a PC renderer: read the SCENE DATA the engine has (camera/view,
  per-object transform = node euler + translation + scale, model GEOMETRY = geomblk prim-lists of
  positions/UVs/colors), transform with **float matrices**, project, draw textured tris/quads with a
  **real depth buffer** (`gpu_draw_world_quad` / `gpu_vk_draw_*_f`). NO GTE compose, NO `gte_op` for
  render, NO byte-packed PSX packet. (PC-native terrain render landed this way — later-158.)
- **The engine OWNS render ordering — never the PSX.** Visibility/occlusion is decided PC-native: a real
  depth buffer for 3D, and the engine's own explicit layer/sort for 2D (backgrounds, HUD, sprites). Do
  NOT read the PSX ordering table (OT), the GP0 submission order, the z/otz the game computed, or any PSX
  draw-order signal to decide what ends up on top. If something draws in the wrong order (e.g. a
  background painted over the scene), the fix is to give the engine the correct ordering rule, NOT to
  re-sync with what the PSX would have done.
- The PSX render is a **VISUAL REFERENCE only** (what the frame should roughly look like) — never a
  byte-match target and never the source of draw order. There is no running oracle to diff against.
- The engine may WRITE the guest interface state the PSX content reads (e.g. terrain sway/world data),
  but those writes must match the reference (see the content-interface gate above); it must NOT
  gratuitously clobber other guest/scratchpad state.

## Hard rules
- **No bandaids / no magic constant offsets** — name the root cause; every lifted function / patched
  value is documented with the RE that justifies it (see global "No bandaids").
- **Single `main` branch**; verified milestones committed AND pushed to `origin`. (Remote is currently
  `github.com/SomeoneIsWorking/psxport.git`; will become Tomba2Engine + a psxport-framework submodule.)
- **Verification = observed behavior on the RUNNING port, cited** (drive it via the REPL; inspect guest
  RAM/scene state with `r`/`rw`/`dumpram`; the USER eyeballs a build for render). There is no oracle —
  reason about correctness from the recomp REFERENCE body + the live game, not an automated A/B diff.
- **Rendering bugs: inspect the LIVE game, never conclude from a cherry-picked still.** The order is
  (1) state/stream inspection on the running port — the `debug <chan>` channels (scene/provat/gte/…)
  via the REPL or the live debug server (`scene`, `provat x y`, `vkvram`); (2) build it and have the
  USER eyeball (the agent cannot self-verify a render — it builds, captures a headless `shot`, and
  asks). There is NO gpu_differ / oracle to replay against anymore — the engine OWNS its render, so
  there is nothing PSX to diff. Stills hide bugs (water "looked fine" while broken) — verify on the
  running game, not one frame. See `docs/gfx-debug.md`.
- **Improve the tools when they fall short — don't hand-grind or reinvent.** Grep `docs/gfx-debug.md` +
  `tools/` before building anything; extend the existing tool; update the doc + skill in the same change.
- Never commit CHDs or machine-specific paths. Disc: CLI arg > `PSXPORT_TOMBA2_DISC` (.env) > `*.chd`.
- Beetle changes live in the committed fork (`vendor/beetle-psx`), NOT out-of-tree .patch files.
