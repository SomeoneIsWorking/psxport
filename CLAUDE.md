# Tomba2Engine — a PC-native reimplementation of Tomba! 2's engine

**Goal (2026-06-16; scope clarified 2026-06-19; sharpened by the user 2026-06-20):** turn Tomba! 2 into a
real **PC-native GAME ENGINE** — **REBUILD Tomba! 2's engine as a complete, self-contained PC game
engine** in native C, running the real game content. Not an emulator, not a recompiled-MIPS blob with
I/O bolted on. **Behave like a PC game, do NOT simulate the PSX.**

**STANDING DIRECTIVE — FULL OWNERSHIP IS ALWAYS THE ANSWER (user, 2026-06-20). Do NOT ask which side of
the boundary something falls on, do NOT offer a "lighter"/incremental-keep-PSX option, do NOT propose
retaining a PSX mechanism for convenience.** Every fork defaults to the engine owning it PC-native.
Effort/time is NOT a constraint ("I don't care if it takes years"). The ONLY things that stay PSX are the
game CONTENT — characters (AI/behavior), level data, quests/game-rules (see THE BOUNDARY); everything
else is PC-owned. When in doubt, own it; just build it, don't ask.

**The engine must NOT deal with PSX intricacies AT ALL — it OWNS the game itself.** A complete PC game
engine owns its world and draws it the way a PC game does: it owns the game objects, the camera and
**projection**, and — critically — **its own render ordering / visibility (real depth buffer + its own
sort)**. It does NOT read, honor, or reproduce *any* PSX rendering decision: not the PSX ordering table
(OT), not PSX draw order, not GTE output, not GP0 packets, not the PSX disp-env. Those are PSX
intricacies; the rebuilt engine has no business knowing they exist. If the engine is consulting "what
the PSX would draw / in what order" to decide what appears on screen, it is wrong — it must decide
that itself from the scene (e.g. the sea-background-drawn-on-top bug is exactly this: order inherited
from PSX instead of owned by the engine).

## THE BOUNDARY — REBUILD the ENGINE PC-native; keep the game CONTENT/LOGIC as recompiled PSX (read this first)
This is the single most-misread thing in the project. Sessions keep collapsing "rebuild the engine" into
"port the renderer" or into "transcribe the PSX body to C." Both are wrong. The line is **engine vs
content**, and a game engine is FAR more than rendering. Note the verb is **REBUILD**, not "port" — we are
building a PC game engine that produces the same game, not translating PSX engine code into C.

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

**The game CONTENT / LOGIC — STAYS as recompiled PSX code/data; we do NOT reimplement it. The split (user,
2026-06-20): PSX owns ONLY the actual game CONTENT — `characters`, `level data`, `quests`. EVERYTHING ELSE
is PC-owned, INCLUDING the game objects themselves.** Concretely, PSX-owned content is:
- **characters** — per-enemy AI, per-character / NPC behavior
- **game physics & collision response** (how Tomba moves / jumps / lands)
- **level data** (the stage's content/asset payload) and **quest / event / progression / game-rule logic**
We are explicitly NOT porting every enemy type, every character, the quests, etc. But the **game objects**
— the world's entity/object model, its placement, spawning, transforms, scene graph, ordering — are
**PC-owned**, owned by the engine, not "whatever the PSX struct happens to be." The engine calls into the
PSX content code as the interface (e.g. it builds the level + terrain + places objects, and the PSX
physics/AI then move the player/enemies within that PC-owned world). Where the engine must hand state to
the retained PSX content, that handoff must be CORRECT (a wrong guest write corrupts the PSX content — that
is how the native terrain made Tomba fall through the ground, later-158) — but the goal is for the engine
to OWN the object/world model PC-native and only marshal what the PSX content genuinely needs, not to treat
the guest entity structs as the source of truth.

**The recomp has TWO distinct roles — never conflate them:**
1. For the **ENGINE**, the recompiled body (`gen_func_*`) is the **behavioral REFERENCE / oracle only**:
   read what it does, understand the data, then **REIMPLEMENT it PC-native**. A native function that
   merely reproduces the PSX body's instructions/packets byte-for-byte is **still PSX-simulation, not a
   PC engine** — that is NOT the deliverable. Match the engine's *observable result* (the world it
   builds, the picture it draws, the interface state the content reads), not its PSX mechanism.
2. For the **CONTENT/LOGIC**, the recompiled body is the **live runtime** that keeps running in guest
   memory. The engine calls it; we don't rewrite it.

Active plan: `<local-notes>/plans/fancy-tinkering-kite.md`. Engine RE: `docs/engine_re.md` (read first).
Findings/dead-ends: `docs/journal.md`. Project map / build cheat-sheet: `docs/project-map.md`.
**Render/present pipeline (SW vs VK, depth, headless offscreen VK, the VK render-diff tool):
`docs/render-arch.md`. Config & debug flags (the `cfg` module + the single `PSXPORT_DEBUG=chan` var —
do NOT add raw `getenv`): `docs/config.md`. How to DRIVE the game (input/automation/debug-server/
reaching a scene): `docs/driving-the-game.md`.** Read these before touching graphics, adding a flag, or
driving the game — don't re-derive them.
**Graphics/render debugging: `docs/gfx-debug.md` (skill `gfx-debug`) — READ FIRST before any rendering
bug or comparison tooling; it has the tool catalog + the mandatory render-diff-first workflow.**

## Repo structure — framework vs game (N64Recomp model, boundary-first)
End state (like N64Recomp/Zelda64Recomp): a **generic, reusable PSX→PC framework** as a submodule, and
**Tomba2Engine** as the game repo on top. We are establishing that boundary **in-tree first**, keeping
one build, then extracting the common PSX part into its own repo (gh authed as `SomeoneIsWorking`).
- `engine/` — **Tomba2Engine, the game-specific native engine + RE.** game_tomba2.c (engine hooks /
  native overrides), wide60.c (60fps interpolation), tomba2_types.h, and the native engine modules
  being lifted (engine_tomba2.c …). This is the heart of the project.
- `runtime/recomp/` — **common PSX→PC platform** (future `psxport` submodule): R3000 interp + recomp
  glue (mem, interp, hle, threads, timing, boot, native_boot/stub, watchdog, stubs, sync_overrides),
  PSX hardware natives (gpu_native, spu_*, gte_beetle, mdec_beetle, cdc_native, disc, pad_input,
  memcard), and the CD/XA/FMV subsystems (cd_override, xa_stream, native_fmv — generic mechanisms with
  game-tunable hooks; their game-specific bits get hooked out as the boundary firms up).
- `runtime/` (root: main.cpp, wide60.cpp/h, Makefile) — the **Beetle oracle** frontend (separate from
  the port). Also slated to move out of `runtime/` into its own home during the cleanup.
- `tools/` — recompiler (`recomp/emit.py`), `discdump`, `drive.py` (diff driver), bgm/frame tooling.
- `generated/` — recompiled Tomba! 2 MAIN.EXE (`shard_*.c`); gitignored, rebuilt by run.sh.
- `vendor/beetle-psx` — our committed Beetle PSX fork (GPL-2), submodule. `docs/`, `patches/`,
  `common/` (.env reader), `scratch/` (gitignored artifacts by type — **never /tmp**, RAM tmpfs ~6 GB).

## TWO binaries — do not confuse them (trips up every fresh session)
- **The native PORT — `scratch/bin/tomba2_port` — IS THE THING UNDER TEST.** Recompiled MAIN.EXE
  (`generated/shard_*.c` from `tools/recomp/emit.py`) linked with the native engine (`engine/*.c`) +
  the PSX platform (`runtime/recomp/*.c`). **NO Makefile builds it.**
    - `./run.sh [disc.chd]` — full: extract MAIN.EXE, recompile, compile every TU, link, **and run**.
    - `tools/build_port.sh [files…|all]` — incremental compile+relink, no run (~0.5s/file). Object
      cache `scratch/obj/`. **Keep its SRC list in sync with run.sh** when adding an `engine/*.c` or
      `runtime/recomp/*.c` file.
  Drive/observe with `tools/drive.py` (does NOT build). See `docs/diff-driver.md`.
- **The ORACLE — `runtime/wide60rt`** — Beetle reference emulator, full emulation of the real disc,
  used to validate the port. Built by **`make -C runtime`** (the only thing that Makefile builds — NOT
  the port). Run: `runtime/wide60rt <chd> -bios <dir> -play` (OpenBIOS as scph5501.bin works).

## Methodology — REIMPLEMENT the engine PC-native; the recomp is the reference + the content runtime
Use `rec_set_override(addr, ov_fn)` to swap an engine function for a native C reimplementation while
keeping the recomp body callable as the reference/super-call (A/B toggle). Determinism check first. The
game stays playable at every step (un-ported engine functions + ALL content/logic keep running as recomp).

**Two different gates — pick by WHICH side of the boundary you are on:**
- **Engine reimplementation → gate on the OBSERVABLE RESULT, NOT a byte-match.** Reimplement the system
  the way a PC game would, then verify the *result*: the world/menu/scene it builds, the picture it
  draws, and — critically — **the interface state the retained PSX content reads back is correct** (the
  guest fields/structs the AI/physics consume). Do NOT gate the engine on reproducing the PSX body's
  instructions/registers/packets byte-for-byte; that gate forces transcription, which is the wrong
  deliverable (a byte-faithful native function is still PSX-simulation). "Looks/works like the reference"
  + "the content still behaves" is the bar.
- **Content interface (the guest RAM the PSX AI/physics read) → MUST match the reference.** Where the
  engine writes state the PSX content consumes, those writes must be correct vs the reference, and a
  divergence there is a real bug (RAM/state diff via `tools/drive.py` + the Beetle oracle). NB the main-
  RAM A/B diff is BLIND to **scratchpad (0x1F800000)** and GTE regs — a wrong scratchpad write can
  corrupt the PSX content invisibly (later-158: the terrain clobbered scratchpad → broke collision).

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
- The PSX render (recomp/interp) is the **VISUAL ORACLE only** — "what should the frame look like" — never
  a byte-match target and never the source of draw order.
- The engine may WRITE the guest interface state the PSX content reads (e.g. terrain sway/world data),
  but those writes must match the reference (see the content-interface gate above); it must NOT
  gratuitously clobber other guest/scratchpad state.

## Hard rules
- **No bandaids / no magic constant offsets** — name the root cause; every lifted function / patched
  value is documented with the RE that justifies it (see global "No bandaids").
- **Single `main` branch**; verified milestones committed AND pushed to `origin`. (Remote is currently
  `github.com/SomeoneIsWorking/psxport.git`; will become Tomba2Engine + a psxport-framework submodule.)
- **Verification = observed behavior on real gameplay, cited** (RAM/state probes preferred over
  screenshots; diff against the oracle, never reason from the port alone).
- **Rendering bugs: render-level diff FIRST, eyeball LAST.** For any "renders wrong" issue the order is
  (1) `gpu_differ` — replay the captured GP0 stream through OUR rasterizer and Beetle and diff the VRAM
  (pure rasterizer diff, no state alignment); (2) GP0/state inspection (`PSXPORT_SCENEDUMP`/`PROVAT`/
  `GTEPROBE`); (3) eyeballing a screenshot / whole-frame pixel-diff ONLY as a last resort when stuck and
  a human judgment is genuinely needed. Stills hide bugs (water "looked fine" while broken). NEVER
  screenshot-compare port-vs-oracle by frame/latch — they drift. See `docs/gfx-debug.md`.
- **Improve the tools when they fall short — don't hand-grind or reinvent.** Grep `docs/gfx-debug.md` +
  `tools/` before building anything; extend the existing tool; update the doc + skill in the same change.
  (A session lost track of `gpu_differ` and built a worse screenshot tool — this rule prevents that.)
- Never commit CHDs or machine-specific paths. Disc: CLI arg > `PSXPORT_TOMBA2_DISC` (.env) > `*.chd`.
- Beetle changes live in the committed fork (`vendor/beetle-psx`), NOT out-of-tree .patch files.
