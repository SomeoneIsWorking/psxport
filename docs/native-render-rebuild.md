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

## The 2D-composite path — OWN THE BUILDERS (per the existing RE map, not a new subsystem)

CONSULT FIRST every time (this section replaces an earlier from-scratch RE that mislabeled owned functions):
`tools/codemap.py --addr <hex>`, `docs/port-progress.md` (CURRENT FRONTIER), `docs/engine_re.md`.

Non-field scenes draw 2D via the game's own builders (sprite/tile/rect/flat/line + font glyphs). The
established native plan (engine_re.md "Native ownership plan"; NOTE its `NativePrim`/`native_dl` naming is
RETIRED — the live infra is submit.cpp + projprim depth + gp0_exec tee) is to OWN each builder so it emits
into the native render queue, exactly like the 3D geometry submit. Already native: `Font::glyphEmit`
(0x80078CA8), `Font::glyphClassFill` (0x800752b4), `GpuState::ui_span_add` (0x8007D594), the pause/options
menu (`game/ui/menu.cpp`), dialog text drafted (`DialogTextStream`). Unowned 2D builders remaining:
`FUN_80092660` (slot-sprite), `FUN_8005019C`+`FUN_8004FFB4` (UI corner sprites + FT4 fills), `FUN_8007CC00`
(dialog border tiles), and the `MoveImage`/vramcopy that blits pre-rendered art (engine_re.md plan #3).

## Removed in this milestone
- fps60 `captureSubscene`/`mSubsceneCur` present-captured-queue shortcut (the hut-interior flicker hack).
- Every OT-walk fallback branch in `drawOTag`'s pc_render path (hut, SOP, field-2D, non-field else).

## #2 TITLE — CORRECTED state (2026-07-15, after consulting codemap + RE map)

CORRECTION of an earlier from-scratch RE in this doc: `0x80075a80` is `AreaSlots::updateTail` (a FIELD-
frame child, also the demo tail — the RE map labels `0x80106658→0x80075A80` "attract render"), NOT a
bespoke "title sprite engine". `0x80075824` is `MusicCoord::voiceMixTick` (AUDIO voice-volume mixer);
`0x800BE1F8` is a VOICE channel, not a sprite — the "sprite fade" reading was wrong. Both were already
LIVE/owned; I mislabeled them by skipping `codemap.py --addr`.

This is a TRACKED FRONTIER: **port-progress.md "NEW FRONTIER (1) — the title/front-end renders BLACK"**.
s2 runs the attract render (`0x80075A80`) every frame; the documented suspects are (a) the attract
render's display list isn't reaching the native VK present, (b) the FMV-teardown shortcut (skipping the
OP.STR movie frame-decode states) left the title without the background/camera it expects, (c) the title
genuinely needs the movie's last frame. Under the reference renderer (`PSXPORT_RENDER_PSX=1`) the title
DOES render (logo + New/Load menu). Under pc_render it now aborts (native-or-crash).

Correct next steps (work FROM the maps, per docs/gfx-debug.md):
1. Resolve the tracked frontier's suspects on the LIVE port (scene/submit/vkvram probes) — is the title's
   draw content even being produced in the native path, or is it the FMV-teardown/background gap?
2. Own the remaining 2D builders so pc_render produces the title without the OT walk: `FUN_80092660`
   (slot-sprite), `FUN_8005019C`+`FUN_8004FFB4` (UI corner sprites + FT4 fills), and the art `MoveImage`.
   Font/menu/dialog are already (partly) native. RE each via codemap.py --addr FIRST, then own it to emit
   into the native queue (submit.cpp pattern), NOT via a new "spriteNative reads a state table".

## Next
Build native producers down the backlog. Each removes one `abortUnimplemented`. Gate: the scene renders
in pc_render without crashing AND SBS-render (pane A pc_render vs pane B psx_render) reaches it.
Current frontier: #2 title — resolve port-progress.md "title renders BLACK" + own FUN_80092660 / UI-fill /
MoveImage builders. ALWAYS `codemap.py --addr` + grep port-progress.md/engine_re.md before any FUN RE.
