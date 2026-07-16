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
| 2 | TITLE screen (logo + New/Load menu + copyright) | `0x801FE00C == 0x801062E4` s2 | `titleNative()` (logo sprites + menu FT4 quads, decoded) | ✅ native — RMSE 0; geometry now RE-VERIFIED (see below) + input owned (`Demo::s2SubMachine`) |
| 0 | Task-switch handoff (scheduler state 3) | task0 `*(u16*)0x801FE000==3` | `renderLoading()` — black loader | ✅ native — RE'd from scheduler FUN_80051e60 (state 3 = entry reassigned, code not run → substate stale). Guards classifyScene from misreading the START→DEMO handoff's leftover sm[0x48] |
| 2a| DEMO attract (real field-engine demo, s7 after idle) | `0x801062E4` sm[0x48]==7 | reuse `sceneNative()` — it's the REAL field (Sop::fieldMode + AreaSlots::updateTail); NOT FMV | ⬜ blanks black now; wire s7→field producer once #3b lands |
| 2b| NEW GAME → s3 MENU page (from title, still DEMO task) | `0x801062E4` sm[0x48]==3 | `s3MenuNative()` (shared data-driven menu emitter — logos + FT4 templates 0x90/0x91 + cursor) | ✅ native — RMSE 0.000 vs reference. It is the s3 MENU (Demo::s3), NOT the SOP narration (that plays later at GAME stage); the SOP overlay is merely resident |
| 3 | Walkable field — WORLD | `0x801FE00C == 0x8010637C` | `sceneNative()` (terrain+entities+objects+backdrop, real depth) | ✅ native |
| 3b-A| Field free-roam blocker — UNTAGGED WORLD OBJECTS (not HUD) | same, `s_ot_2d_drawn>0` | own the object emit leaves so they register obj_depth / native-cover | ⛔ CRITICAL — blocks all gameplay in pc_render |
| 3b-B| Field genuine 2D HUD/dialog/text (interaction-triggered) | same | font glyphEmit dual-emit + panel.cpp (specs ready) | ⛔ (not in free-roam) |
| 4 | Hut/door interior authored sub-scene | field + `task-sm[0x4c]==3` | `fieldObjectsRender()` + `dialogTextNative()` (room obj 0x800FD850 + NPCs + Tomba + dialog text) | ✅ native — matches reference; dialog TEXT now native (panel bg still 2D pending) |
| 5 | SOP intro narration cutscene | field + overlay-sig `0x3C021F80` @ `0x80109450` | `sceneNative()` + void-beat guard + `narrationSwirlRender()` (native swirl — type-0x20 custom-fn node, mesh 0x8010CC08) | ✅ native — void beat coverage 59.1% vs ref 58.9%, RMSE 20 (accepted-3D band); caption text = 2D pending |

Stage constants: `0x8010649C` START.BIN · `0x801062E4` TITLE/DEMO · `0x8010637C` GAME field.

**#3b is TWO tracks (field-2D RE scout, 2026-07-15) — the free-roam blocker is NOT HUD:**
- **Track A (CRITICAL, blocks all gameplay in pc_render).** GROUND TRUTH (live probe of s_ot_2d_drawn++
  at true free-roam, overlay 0x801138A4 sm[0x4a]=1, 2026-07-15) — the counted prims are TWO groups, and
  op-0x5E is NOT among them (the line handler gpu_native.cpp:1184 has no counter):
  1. **~60 BILLBOARD polys** (op 0x34/0x3c/0x3e/0x2d, `is3d=1 billboard=1`, pool 0x800BFExx–0x800C0xxx).
     Counted at gpu_native.cpp:938 because the skip guard :936 (`!billboard && !ui`) doesn't skip
     billboards. But these are WORLD billboards WITH obj_depth that ARE emitted (:939) — a **poly-count
     FALSE POSITIVE** (or: they're only drawn via the 2d_only-walk TRANSCRIPTION, not natively — open Q:
     does billboardEmit 0x8003C8F4 push to the queue, or only guest packets + obj_depth?).
  2. **60 op-0x7C SPRITES** (RQ_HUD, objz=0, bg=0; contiguous pool run 0x800C5FF8 +0x10·n).
     **BUILDER RE'd (2026-07-16, docs/native-render-2d-tilegrid.md):** ONE scrolling 16×16
     TILE-GRID background layer — scroll-wrap helper 0x8011534C (dispatch label 0x80115364,
     case 0x8003D1C4 of overlayTypeDispatch) + tile-grid packet emitter 0x80115598 (nested W×H
     loop, 16-byte op-0x7C packets, atlas UV/CLUT from a tile-ID table at node+20, whole grid
     spliced into ONE background OT bucket). Native ownership = dual-emit port per the plan in
     that doc; the 0x801158E0 4-state driver is the remaining RE gap. CAVEAT: an earlier note
     here attributed the pool stores to 0x801401B8 — WRONG; that is OverlayGroundGt3Gt4::entityLoop
     (3D GT3/GT4, already owned) whose packets merely land in the same shared bump pool. A WWATCH
     on a pool ADDRESS RANGE names every emitter whose packets rotate through it, not "the" 2D
     builder — match on the packet's command byte, not the address, next time.
  So the free-roam blocker is NOT "own the 5 named substrate leaves" (the emit-leaf scout proved 4/5 are
  already obj_depth-covered by the installed billboardEmit/perObjRenderDispatch overrides). It is (1) a
  gate FALSE POSITIVE to fix (exclude covered billboards from :938) + (2) the 60 op-0x7C sprites to own.
  KEY architectural question this exposes: the field 2D layer is currently TRANSCRIBED via the 2d_only OT
  walk (emitOrQueue at :939/:1119 reads guest packets) — that IS the banned fallback. True nativeness
  needs native producers for the billboards + op-0x7C sprites + HUD/dialog, with the guest copies dropped
  by native-cover. The hut (#4) shares this. Fix design pending the diagnosis agent (fed this ground truth).
- **Track B (interaction-triggered HUD/dialog/text).** Font glyphs (glyphEmit dual-emit — the shared
  producer below), dialog panel (`panelBuild`/`panelFill`/`borderTiles` → `game/ui/panel.cpp`, spec in
  docs/native-render-2d-panel.md), `HudGaugeEmitter` (0x8004FD30, LIVE — confirm it dual-emits when a gauge
  is up). Retires the transitional `ui_span` observer hack on 0x8007D594.
  - **Dialog TEXT — LANDED (`Render::dialogTextNative`, render_walk.cpp).** Native producer mirroring the
    guest glyph emitter FUN_8007CC00 (Spec 3): reads the glyph list @0x800ECB88 (count=(s16)*0x1F80017E),
    emits op-0x65 font sprites (tpage 0x1F), CLUT=((char&0x7f)+0x1F0)<<6|0x3F. Wired into `renderField` +
    `renderHutInterior`; verified read-only (no DisplayPass guest-write, no crash) on hut-entry dialog
    (17-glyph bubble). DEFERRED: the highlight path (selected menu option → CLUT 0x7CBE) needs the
    DialogBox pointer the flat-list read lacks — lands with the panel/box native owner.
- **Completeness gate (self-completing).** Every owned 2D builder must (a) push its host quad to the queue
  AND (b) register its guest packet-pool span in a covered registry (`gpu_ui_span_add`). twoDOnly counts a
  prim iff RQ_HUD/OVERLAY AND not covered (obj_depth OR ui_span OR native_cover). Tighten the POLY path
  (gpu_native.cpp:938) to the sprite-path shape (:1118). `s_ot_2d_drawn>0 → abort` stays: an un-owned
  builder (or one that forgot span registration) keeps count>0 and crashes with the scene identity. No
  allowlist. guest-count − covered-count must reach 0, lowered only by genuine ownership.

**Shared foundation — font→queue producer.** #3b (field HUD/dialog text), #5 (SOP narration text), and
#2a text all use `Font::glyphEmit` (0x80078CA8, LIVE) which writes GUEST packets, not the queue. Add ONE
queue producer (dual-emit or a read glyph-run→push2dQuad, op-0x65 sprites, RQ_HUD) and it unblocks all
three. (The TITLE does NOT use glyphEmit — its labels are FT4 quads — so titleNative didn't need it.)
Build it from the field-2D scout's plan, verify on the field HUD, reuse for SOP + attract.

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

### VERIFIED plan (live otattr trace of the title, 2026-07-15) — read-only `titleNative()` producer

CORRECTIONS confirmed live: the title is NOT a MoveImage art-blit (the only vramcopy is a degenerate 2×1
no-op). `FUN_80092660` is a SOUND/SFX slot leaf (not a sprite builder). The panel family
(FUN_8005019C/4FFB4/7CC00, docs/native-render-2d-panel.md) is HUD/dialog, NOT the title. The title art is
in VRAM by s2 (uploaded by Demo::s0PreYield); the OP.STR movie is NOT a blocker (port-progress suspects
b/c falsified). The title picture = black fill + 2 logo sprites + 3 menu FT4 quads + native font text.

`titleNative()` (read-only producer, sibling of sceneNative; reads guest state, emits to RenderQueue only):
- **Backdrop**: full-screen black (gpu_blank_display or a black RQ_BACKGROUND quad).
- **2 logo sprites** (op 0x65, fn 0x8010696C — decoded packet constants, fixed layout):
  · sprite A: xy=(0,-8) wh=256×240 uv=(0,0) tpage 0x9A → tp=(640,256) mode=1(8bpp) clut=(640,511)
  · sprite B: xy=(256,-8) wh=64×240 uv=(0,0) tpage 0x9C → tp=(768,256) mode=1 clut=(640,511)
  → 2× push2dQuad(RQ_BACKGROUND, order_2d_fg=1, …), raw (op 0x65 = textured+raw, color ignored).
- **3 menu FT4 quads** (op 0x2C/0x2D, fn 0x8007E2F8 / callers 0x8007E1B8+0x8007E998) — e.g. quad0 xy0=(186,172)
  88×16, uv=(80,1)-(168,17), tpage 0x1D → tp=(832,256) mode=0(4bpp), clut=(880,510), color (0x50,0x50,0x50),
  opaque. quad2 xy0=(32,168) tpage samples clut=(480,247). Cursor-keyed color/selection: RE by agent (task).
  → push2dQuad(RQ_OVERLAY, …) opaque / emitOrQueue(semi) for op 0x2D. Reads menu cursor state (sm[0x68] /
  DAT_800bf808).
- **Text**: RESOLVED (verified live) — the title uses NO glyph-font path. `Font::glyphEmit` is called ZERO
  times during the title (both render modes). The "New Game"/"Load Game" labels ARE the 3 FT4 quads (pre-
  rendered menu-item text-images); the copyright line is baked into the logo sprite art. So titleNative =
  logo sprites + menu FT4 quads only — no separate font. (Font-to-queue is a FIELD-HUD/dialog concern, #3b.)

DONE: titleNative() implemented from the decoded packet constants — pixel-identical to the reference
(RMSE 0). STOPGAP: the menu selection is the boot-default snapshot (New Game raw/bright, Load Game
modulated-0x50/dim); live cursor keying via sm[0x68] is the refinement (the exact s2 row-layout caller
lives in the DEMO overlay 0x80106xxx and was not traced — confirm on a live dump before wiring dynamics).

drawOTag wiring: `demo = stage==0x801062E4`; `sm[0x48]==2 → titleNative()`, else → `gpu_blank_display()`
(black) — the DEMO loading ramp / OP.STR movie (s1) / attract (s7) all currently blank. s3 is a transient
title-menu cursor state that draws the TITLE family (logo+menu), NOT gameplay footage. #2a: the attract is
s7 = a REAL field scene (Sop::fieldMode + AreaSlots::updateTail, area-loaded); wire s7 → sceneNative (the
field producer) + field-2D once #3b lands — it's NOT the OP.STR FMV (that's s1, correctly black headless).

## Next
Build native producers down the backlog. Each removes one `abortUnimplemented`. Gate: the scene renders
in pc_render without crashing AND SBS-render (pane A pc_render vs pane B psx_render) reaches it.
Current frontier: #2 title — resolve port-progress.md "title renders BLACK" + own FUN_80092660 / UI-fill /
MoveImage builders. ALWAYS `codemap.py --addr` + grep port-progress.md/engine_re.md before any FUN RE.
