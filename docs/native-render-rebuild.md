# Native render rebuild — one PC-game renderer, no OT/GP0 fallback

USER directive (2026-07-15): "rebuild the renderer as I asked, one real PC game renderer, then fix your
SBS tooling then you can work on the bugs" + "Drop the render shortcut and other fallbacks, if something
lacks native rendering then it shouldn't be rendered at all and better yet, it should crash the game with
a message saying unimplemented rendering." Scope chosen: **Everything, crash at boot.**

## The contract

`Engine::drawOTag` (game/game_tomba2.cpp) has exactly two paths:

- **psx_render** (`PSXPORT_RENDER_PSX=1` / SBS core B / `renderpsx` REPL): the substrate guest-OT walk
  (`gpu_dma2_linked_list`). This is the ONLY OT-walk caller left. It is the byte-exact PSX **reference**,
  not a shipped render behavior — used to drive into scenes whose native producer isn't built yet.
- **pc_render** (default): THE one native renderer. The picture comes from GAME STATE with real depth.
  It NEVER transcribes the guest OT/GP0. A scene — or a scene LAYER — without a native producer calls
  `Render::abortUnimplemented(scene)` (render_walk.cpp), which prints `unimplemented native rendering: …`
  with the scene identity (stage / sm[0x4a] / sm[0x4c] / overlay-sig) and `abort()`s. No fallback, no env
  escape hatch. The crash sequence IS this backlog.

The substrate render orchestrator (`Render::frame`/`frameX`) still runs underneath every frame on the
faithful task's own call path — it builds the guest OT/packet pool (part of the byte-exact state). We
simply do not read it for the picture in pc_render.

## Consequence: build TOP-DOWN from boot

pc_render aborts on the FIRST boot frame (verified: stage `0x8010649C`, START.BIN). Because SBS core A =
pc_render boots from scratch (no savestates, no per-stage render-mode bridge — that would be a banned
fallback), native rendering must be built boot→gameplay in order. Until a stage is native, drive/verify
it with the psx_render reference (and SBS core B).

## Backlog (pc_render crash order)

| # | Scene | stage / selector | native producer today | status |
|---|-------|------------------|------------------------|--------|
| 1 | START.BIN boot (black loader) | `0x801FE00C == 0x8010649C` | black frame (gpu_blank_display) | ✅ native |
| 2 | TITLE screen (logo + New/Load menu + copyright, animated bg) | `0x801FE00C == 0x801062E4` | none | ⛔ crashes here now |
| 2a| DEMO attract (gameplay footage after idle) | `0x801062E4`, attract sub-state | none | ⛔ |
| 3 | Walkable field — WORLD | `0x801FE00C == 0x8010637C` | `sceneNative()` (terrain+entities+objects+backdrop, real depth) | ✅ native |
| 3b| Walkable field — 2D OVERLAY (HUD/dialog/item-bubble/menu/text) | same, `s_ot_2d_drawn>0` | none | ⛔ crashes when any overlay prim present |
| 4 | Hut/door interior authored sub-scene | field + `task-sm[0x4c]==3` | none (objects share HEADS[0..1] via TransitionState3; room geo + interior camera unbuilt) | ⛔ |
| 5 | SOP intro narration cutscene | field + overlay-sig `0x3C021F80` @ `0x80109450` | partial (`sceneNative` for 3D beats) — 2D composite unbuilt | ⛔ |

Stage constants: `0x8010649C` START.BIN · `0x801062E4` TITLE/DEMO · `0x8010637C` GAME field.

## The 2D-composite pattern (title, menus, HUD, dialog, text)

Most non-field scenes are 2D composites (sprites + text over a background). "Native" for these = rebuild
from the overlay's OWN sprite/text DATA (its sprite table, string list, menu cursor state) drawn through a
native 2D sprite+font+menu subsystem — NOT reading the guest OT/GP0 packet stream. This is the 2D twin of
sceneNative reading entity lists. Each such scene needs its draw-data RE'd first (Ghidra), then a native
producer. The TITLE screen (#2) is the first and establishes this subsystem.

## Removed in this milestone
- fps60 `captureSubscene`/`mSubsceneCur` present-captured-queue shortcut (the hut-interior flicker hack).
- Every OT-walk fallback branch in `drawOTag`'s pc_render path (hut, SOP, field-2D, non-field else).

## #2 TITLE — investigation state (2026-07-15)

- Stage `0x801062E4` (labelled DEMO): frames 0-4 black (START handoff), then the TITLE screen: TOMBA! 2
  logo + "New Game"/"Load Game" menu + copyright over an animated character/castle background.
- Drawn as **textured quads** (gp0 op 0x2c/0x2d, tp=(832,256)/(384,0)), NOT SPRT sprites — the logo/bg/
  menu are POLY_FT4s sampling VRAM sprite patterns that `Engine::uploadModeSprites` (FUN_80067DA8) uploads
  each frame. So the native 2D subsystem is a **textured-quad + font compositor**, keyed by texpage/uv.
- The Demo class (game/scene/demo.h) owns the SUBSTATE machine only; the RENDER stays substrate. The
  render tail is `TAIL_REND = 0x80106658` → **`FUN_80075a80`** (title/attract render, Ghidra-decompiled).
- **FUN_80075a80** iterates a 24-entry display list at `0x800be238` (stride 0xc) and calls the game's
  2D SPRITE ENGINE per active entry: **`FUN_80092660`** (activate/pose a sprite → fills a prototype
  POLY packet `DAT_80105cf8..d10` [tag 0x21, color, uv, tpage, clut, xy] + a sprite state table at
  `0x801054d8` [stride 0x1c]) → `FUN_80092fd0` (pattern/pose state) → `FUN_8009440c`/`FUN_80094c10`.
  Pattern def table `PTR_80105cdc`, pose/frame table `PTR_80105ce8` (stride 0x20).
- BUT the live title OT is MINIMAL (scene dump f130: OT1 fill=1 env=6; OT2 poly=3 rect=2 **vramcopy=1**
  env=2). So the title picture = a **pre-rendered VRAM art image blitted by a vramcopy** into the FB +
  a fill + ~5 menu/logo overlay prims — NOT dozens of per-frame sprite quads. The sprite engine drives
  only the few overlay/animated bits.
### Architecture decision: `spriteNative()` reads sprite STATE, draws natively (the sceneNative pattern)

Do NOT port the whole sprite engine. Let the substrate sprite engine keep running (it computes sprite
state into guest tables — part of the byte-exact state), and write a READ-ONLY `Render::spriteNative()`
that walks the sprite state table and draws each active sprite as a native textured quad (RQ_HUD) — the
2D twin of how `sceneNative()` reads the 3D entity lists. No OT read, no GP0 replay; reads guest sprite
state, writes host VK only. This one producer renders title/menus/HUD/dialog (all use this engine).

Sprite state = an array of structs, base ≈ `0x801054c8`, **stride 0x1c**, 24 slots. Fields derived from
FUN_80092660's setup writes (SoA accessors, base+i*0x1c):
- `+0x00` = 0x21 packet tag (active marker)      · `+0x02` = pose/flip (DAT_80105cff)
- `+0x04` = pattern/pose index (param_3)          · `+0x06` = frame (DAT_80105d04)
- `+0x08` = screen Y (param_2, short)             · `+0x0e` (0x801054c8+i*1c-0x10 → +0x00? recheck)
- `+0x26` = brightness/scale (DAT_80105cfc)
Pattern-def table `PTR_80105cdc` (uv/size, stride 0x10), pose/frame table `PTR_80105ce8` (stride 0x20,
holds tpage @+0x10, clut/flags @+0x12) — the emit (FUN_80092fd0) reads these to build the final quad's
tex-rect. RE TODO before coding spriteNative: (1) confirm the full field map incl. screen X + W/H +
u/v + tp + clut + color, from the emit/flush geometry (FUN_80092fd0 + the per-frame flush that walks the
table into the OT); (2) find that flush (the POLY_FT4 builder) — its geometry math is what spriteNative
reproduces natively from the state. Then spriteNative walks slots, active (`+0x00==0x21`), draws quads.

## Next
Build native producers down the backlog. Each removes one `abortUnimplemented`. Gate: the scene renders
in pc_render without crashing AND SBS-render (pane A pc_render vs pane B psx_render) reaches it.
Current frontier: #2 title — RE the shared 2D-quad submit primitive, own it natively.
