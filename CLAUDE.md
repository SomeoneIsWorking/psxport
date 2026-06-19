# Tomba2Engine — a PC-native reimplementation of Tomba! 2's engine

**Goal (2026-06-16):** turn Tomba! 2 into a real **PC-native engine port** — Tomba! 2's *own*
game engine, reimplemented in native C, running the real game content. Not an emulator, not a
recompiled-MIPS blob with I/O bolted on. Reference/oracle for every lift = the recompiled MIPS body.

**Scope boundary (important):** the **gameplay logic** (per-entity AI, physics, game rules) STAYS as
recompiled PSX code running in **PSX guest memory** — we do NOT reimplement it. We reimplement the
**engine layer** in native C: the main loop, entity-list iteration, per-object cull/LOD, render
submission, camera, and the 60fps interpolation + widescreen. The native engine **reads the entity
structs out of guest RAM and calls into PSX gameplay code** where needed. This is the layer that makes
object interpolation correct (we own the real entity list → no GTE-transform-fingerprint guessing) and
it drops any "lift the whole game" phase.

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

## Methodology — oracle-validated incremental lift
Extend the existing override pattern inward from I/O to the engine: `rec_set_override(addr, ov_fn)`
keeps the recomp body callable as the oracle/super-call; A/B toggle. Every lifted function is gated on
**RAM/state equivalence vs the recomp body on real gameplay** (diff via `tools/drive.py` + the Beetle
oracle), never "looks right". Determinism check first. The game stays playable at every step (un-lifted
functions keep running as recomp).

### RENDER is the EXCEPTION — render PC-native, do NOT transcribe the PSX pipeline (USER DIRECTIVE, anchor)
The RAM/byte-equivalence gate above is for **gameplay logic**. For the **render submission** it is the
WRONG gate — it forces you to replicate PSX behavior in C. **We are porting the game to PC, not replaying
PSX on PC.** What the tree currently calls "native" render (`engine_submit.cpp` `submit_terrain`,
`ov_submit_poly_gt4_bp`, `native_dl`) is in fact a **transcription of the PSX render path**: it reproduces
the GTE matrix compose (MVMVA columns + CR0-7 packing), drives `gte_op` (emulated PSX GTE), and fills
GP0 packet words "byte-identical to the guest packet." That replication IS the source of the render
glitches (the terrain's tiny composed matrix → water garbage; mangled 2D/UI quads). **Do not chase those
by patching the transcription.** Instead:
- Render the game's CONTENT with a PC renderer: read the SCENE DATA the game already computes — camera/
  view, per-object transform (node euler + translation + scale), model GEOMETRY (geomblk prim-lists:
  positions/UVs/colors) — transform with **float matrices**, project, draw textured tris/quads via
  `gpu_vk_draw_*_f` with a **real depth buffer**. NO GTE compose, NO `gte_op` for render, NO byte-packed
  PSX packet.
- The PSX render (recomp/interp) is the **VISUAL ORACLE only** — "what should the frame look like" — never
  a byte-match target. Gate render work on the picture matching the oracle.
- Still READ gameplay/scene state from guest RAM; never WRITE guest gameplay state (host-memory rule).
- Applies to the whole render path (terrain, per-object, 2D/UI). First target: PC-native terrain
  (`run.sh` ships `PSXPORT_NO_TERRAIN=1` as a stopgap until it works).

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
