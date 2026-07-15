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
| 1 | START.BIN boot | `0x801FE00C == 0x8010649C` | none | ⛔ crashes first |
| 2 | Title / DEMO attract / SCEA / FMV | `0x801FE00C == 0x801062E4` | none | ⛔ |
| 3 | Walkable field — WORLD | `0x801FE00C == 0x8010637C` | `sceneNative()` (terrain+entities+objects+backdrop, real depth) | ✅ native |
| 3b| Walkable field — 2D OVERLAY (HUD/dialog/item-bubble/menu/text) | same, `s_ot_2d_drawn>0` | none | ⛔ crashes when any overlay prim present |
| 4 | Hut/door interior authored sub-scene | field + `task-sm[0x4c]==3` | none (objects share HEADS[0..1] via TransitionState3; room geo + interior camera unbuilt) | ⛔ |
| 5 | SOP intro narration cutscene | field + overlay-sig `0x3C021F80` @ `0x80109450` | partial (`sceneNative` for 3D beats) — 2D composite unbuilt | ⛔ |

Stage constants: `0x8010649C` START.BIN · `0x801062E4` TITLE/DEMO load · `0x8010637C` GAME field.

## Removed in this milestone
- fps60 `captureSubscene`/`mSubsceneCur` present-captured-queue shortcut (the hut-interior flicker hack).
- Every OT-walk fallback branch in `drawOTag`'s pc_render path (hut, SOP, field-2D, non-field else).

## Next
Build native producers down the backlog. Each removes one `abortUnimplemented`. Gate: the scene renders
in pc_render without crashing AND SBS-render (pane A pc_render vs pane B psx_render) reaches it.
