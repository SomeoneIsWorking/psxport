// Tomba!2-specific native overrides (per-game tier). Generic mechanisms live in timing.c /
// cd_override.c; this file holds glue tied to MAIN.EXE's own addresses.
//
// VBlank pacing: port the dwell to PC (don't dwell)
// ------------------------------------------------
// The StrPlayer main loop FUN_80050b08 paces each displayed frame with a busy-wait at
// 0x80050CE4:  DAT_800e809c = 0;  ... ;  do {} while (DAT_800e809c < DAT_1f800235);
// On hardware the VBlank IRQ bumps DAT_800e809c (0x800E809C, u16) until it reaches the
// per-frame quota DAT_1f800235 (scratchpad u8, =2 => the engine's 30 fps logic rate). This
// is pure frame-rate pacing. In a PC port frame pacing belongs to the host present loop, not
// a self-spinning counter, and we deliver no preemptive VBlank IRQ — so we make the loop NOT
// dwell: FUN_800788ac is the per-frame state update called exactly once per iteration (its
// only caller is the loop, right after the counter reset and before the dwell), so after its
// real body we set the display counter to the quota the dwell tests => the dwell falls
// through on its first check. This is exactly the state the real VBlank handler would have
// produced (the cb at 0x800506B4 only increments that counter), computed directly.
// (When a host present loop exists it will pace frames; this just removes the busy-wait.)
#include "core.h"
#include "game.h"   // Fps60::current_object (was g_current_object)
#include "cfg.h"
#include "mods.h"      // g_mods (fps60 persisted with the other user settings)
#include "margin_render.h"
#include "render.h"           // class Render — c->mRender->sceneNative()
#include "asset.h"      // PC-native asset-loading subsystem (extracted from this file)
#include "mathlib.h"    // PC-native math/PRNG leaf primitives (rand, trig LUTs, bit-test)
#include "cull.h"       // PC-native visibility cull / LOD subsystem
#include "collision.h"  // PC-native collision-grid subsystem
#include "entity.h"     // PC-native per-object entity state-machine subsystem
#include "script_vm.h"     // PC-native per-object script-VM subsystem
#include "animation.h"  // PC-native per-object animation-VM subsystem
#include "input.h"      // PC-native per-frame input/controller subsystem
#include "menu.h"       // PC-native in-game Options menu subsystem
#include "core/engine.h"  // class Engine — this file defines Engine::frameUpdate / Engine::drawOTag
#include <stdlib.h>
#include <stdio.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (super-call / A/B oracle)
void fps60_init(void);            // fps60: read PSXPORT_FPS60
// g_render_object retired 2026-07-03 — was defined + written by Cull::objectCull but never read; dead.
// g_fps60_on retired — read g_mods.fps60 (mods.h)
// SpuAudio methods are called via c->game->spu_audio (owner: Game, see runtime/recomp/spu_audio.h).
void rec_dispatch(Core*, uint32_t);  // hybrid call: recomp body if emitted, else interpret

#define DISPLAY_COUNTER 0x800E809Cu   // DAT_800e809c (u16) — the dwell's vblank counter
#define VBLANK_QUOTA    0x1F800235u   // DAT_1f800235 (u8)  — vblanks per displayed frame

// libsnd music-sequencer tick (RE: docs/journal.md later-53; SsSetTickMode = FUN_80090750).
// Tomba2 sequences its in-game/menu BGM with the libsnd sequencer, ticked from the VBlank IRQ
// (tick mode 5, RCnt3/vblank). The IRQ runs the tick wrapper FUN_800909c0, which chains the
// optional per-vblank user callback (DAT_800ac430) then the sequencer SsSeqCalled (DAT_800ac42c).
// The port delivers NO preemptive IRQ and collapses the pace-dwell the IRQ would fire in, so on
// hardware-faithful boot the sequencer never ticks -> zero per-note KON -> silent SPU (verified
// vs the oracle, later-53: the oracle writes KON from this very ISR while parked in the dwell).
// FIX (port the HW interrupt work to PC, per the busy-wait-porting rule — NOT simulate the IRQ):
// run the same tick wrapper natively once per vblank. The wrapper/sequencer are NOT emitted by
// the static recompiler (only reached via the IRQ callback pointer, never a direct jal), so we
// invoke them through rec_dispatch -> the hybrid interpreter (bit-identical to recomp); it runs
// FUN_800909c0 to its `jr ra` and returns. Caller-saved regs it clobbers are dead across the
// FUN_800788ac call site by MIPS convention, so this is safe to run right after the super-call.
#define SEQ_TICK_WRAPPER 0x800909C0u  // FUN_800909c0: per-vblank libsnd tick (user cb + SsSeqCalled)
#define SEQ_FUNC_PTR     0x800AC42Cu  // DAT_800ac42c: SsSeqCalled pointer (0 until SsStart inits)

#include "gpu_perf.h"   // per-frame CPU phase profiler (REPL `debug perf`), default off

// Per-frame engine tick. Called DIRECTLY (a plain C call) from native_step_frame (native_boot.cpp) —
// this is the PC-driven game loop's frame body. It runs the still-PSX per-frame update leaf, then OWNS
// the per-vblank audio, the fps60 commit, and present + pace (the things the now-removed override table
// used to attach). NOT an override anymore; not static so native_boot can call it top-down.
void Engine::frameUpdate() {
  Core* c = this->core;
  // Reset the fps60/objid billboard registry at the true frame boundary — BEFORE fieldFrame's substrate
  // render (in the scheduler step below) records this frame's billboard spans, and before drawOTag's OT
  // walk stamps them. (Was reset lazily at drawOTag's first rq push, which is AFTER fieldFrame recorded —
  // wiping the spans before the OT walk could use them; billboards were never re-anchored.)
  c->game->fps60.bbFrameReset();
  c->game->perf.phaseBegin(0);                               // perf: LOGIC = all guest interpreter work + render submit
  rec_dispatch(c, 0x800788ACu);                      // real per-frame state update (still-PSX leaf)
  c->game->perf.phaseEnd(0);
  // Per-VBLANK audio work. On hardware the libsnd sequencer ticks once per VBlank IRQ (60 Hz NTSC)
  // and the SPU plays in realtime. One ov_frame_update is one *logic frame*, which on hardware spans
  // DAT_1f800235 (=quota) VBlanks (=2 => Tomba2's 30 fps). So the per-vblank work — the sequencer
  // tick AND the SPU's 1/60 s field advance (spu_audio_frame) — must run `quota` times per logic
  // frame to stay at the hardware 60 Hz rate in real time. later-54 ran BOTH once (matching each
  // other but at half real-time); windowed that plays audio at HALF tempo — the user heard the
  // menu-cursor tick too slow (the headless WAV hid it: its timeline is field-count, not wall-clock,
  // so 1 tick/1 field there is still 60:60 = correct-sounding). Running both quota× fixes real-time
  // playback and keeps the WAV's tick:field ratio unchanged (just a longer, more correct duration).
  // Sequencer guard: pointer initialized + sane code address (never call through null pre-SsStart).
  // Opt out (A/B): PSXPORT_T2_NOSEQTICK. Adaptive: a true-60fps scene (quota=1) ticks once.
  // Dual-core diff mode: the per-frame state update above is the guest-state work we diff; everything
  // below is host OUTPUT (audio device feed, fps60, present, pace) — skip it so two cores can step in one
  // process without the shared SPU/VK singletons fighting. (The sequencer tick mutates guest libsnd state,
  // but its only purpose is audio playback we aren't producing here, so skip the whole audio block.)
  int quota = c->mem_r8(VBLANK_QUOTA); if (quota < 1) quota = 1;
  if (c->game->diff_mode) {
    // SBS/dual-core: skip host OUTPUT (device feed, fps60, present, pace) + the sequencer tick so two
    // cores step in one process without the shared output/VK singletons fighting. BUT game LOGIC can
    // block on an XA voice clip finishing — the intro-area cutscene wait `while(*(0x801fe0e0)!=0)` (the
    // XA voice task-2 state). That progress lives in spu_update->CDC_GetCDAudioSample, which the early
    // return used to skip entirely, so BOTH SBS cores froze in the field (the user bug, later-271).
    // Advance THIS core's per-instance XA stream logic-only (output discarded — no double audio); run it
    // `quota` times like the real path so the clip paces at the realtime-equivalent rate. No-op when no
    // clip is streaming.
    for (int v = 0; v < quota; v++) c->game->spu_audio.frameLogic();
    return;
  }
  uint32_t seqfn = c->mem_r32(SEQ_FUNC_PTR);
  int seq_ok = !cfg_on("PSXPORT_T2_NOSEQTICK") && c->game->native_gates.get("seqtick")
               && (seqfn & 0x1FFFFFFFu) >= 0x10000u && (seqfn & 0x1FFFFFFFu) < 0x200000u;
  c->game->perf.phaseBegin(1);                               // perf: AUDIO = per-vblank sequencer tick + SPU advance
  for (int v = 0; v < quota; v++) {                  // once per VBlank this logic frame spans
    if (seq_ok) rec_dispatch(c, SEQ_TICK_WRAPPER);   // libsnd per-vblank tick (user cb + SsSeqCalled)
    c->game->spu_audio.frame();                      // advance SPU one 1/60 s field + feed device
  }
  // (native field-BGM director REMOVED — it played a HARDCODED song over everything from the menu on.
  //  Music is the recompiled libsnd path above; no native music engine, no hardcoded song.)
  c->game->perf.phaseEnd(1);
  c->mem_w16(DISPLAY_COUNTER, c->mem_r8(VBLANK_QUOTA));    // satisfy the pacing dwell immediately
  // fps60 (when enabled) OWNS presentation: it presents the interpolated in-between + the real frame
  // (60 fps, 1 frame behind) and paces both halves — see fps60_present_vk. The faithful path
  // presents frame B once and paces a full frame.
  c->game->fps60.frame_commit(c);
  if (!g_mods.fps60) {
    c->game->perf.phaseBegin(2);                             // perf: PRESENT-cpu = VRAM mirror upload + VK record/submit
    gpu_present(c);
    c->game->perf.phaseEnd(2);                               // (pacing/vsync sleep below is excluded -> shows as idle/pace)
    gpu_pace_frame(c);
  }
}

// fps60 object tag: the universal per-object cull/LOD dispatcher (a0 = object*, once per logic
// frame for every live drawable). Every RTP op fired in its call tree is tagged with this object's
// stable pool-pointer id (the join key). Super-call the recomp body unchanged; clear on exit.
// PSXPORT_OBJLOG=1: dump every object the cull dispatcher visits (addr + type@+0xc +
// pos@+0x2e/32/36). Empirically maps the active-object pool/list for the native entity
// manager (Phase 1) — more reliable than static-tracing the overlay handler dispatch.
int gpu_gpu_wide_engine(void);   // gpu_gpu.c — genuine engine-wide active (PSXPORT_WIDE_ENGINE && aspect!=4:3)

// Native ownership of DrawOTag (libgpu FUN_80081560, the per-frame draw kick): the recomp body just
// programs the GPU linked-list DMA to walk the ordering table at a0 — which our renderer already does
// natively in gpu_dma2_linked_list (walk OT -> decode each primitive -> rasterize). Overriding it routes
// the draw straight through our native walk (synchronous), instead of the DMA-register emulation dance.
// This is the engine's draw submission, owned.
// g_render_psx retired — now Render::mode; g_ot_2d_only retired — now a param to gpu_dma2_linked_list.
void Engine::drawOTag(uint32_t otHead) {   // called directly from native_step_frame (PC-driven); NOT an override
  Core* c = this->core;
  // #7/#11 finish: while the DEMO/title front-end is still LOADING its assets (sub-SM task0+0x48 < 2, the
  // s4a load ramp), the title composites its menu/font over whatever VRAM the FMV/SCEA splash left — so the
  // SCEA text / FMV last-frame show through (the one-time hand-off clear can't cover the multi-frame ramp).
  // Blank the display FB to black BEFORE this frame's prims draw, every loading frame, so the title's
  // partial 2D layer always sits on opaque black. Once loaded (s48>=2) the title owns a full background and
  // this is a no-op-equivalent (its bg overwrites the black). Engine-owned, keyed on the stage's own signal.
  if (c->mem_r32(0x801FE00Cu) == 0x801062E4u && c->mem_r8(0x801FE048u) < 2) c->game->gpu.gpu_blank_display();
  // Engine-owned ordering (the one render path): owned world geometry was queued during submit; the OT
  // walk queues the guest 2D / un-owned submit variants (instead of drawing them inline); then the queue
  // drains in ENGINE order (layer: background < world < overlay < hud; depth within world). The guest OT
  // is read here ONLY to enumerate the leftover guest prims — its draw ORDER is discarded. M3 captures
  // those at submit time and retires this read. (PSXPORT_SBS debug compare keeps an inline path; its
  // queue stays empty so the flush is a no-op.)
  // DECOUPLED native scene render (one-native-render-path-decoupled). For the FIELD (GAME stage,
  // 0x801FE00C == 0x8010637C) the native path OWNS the render: it builds the world from GAME DATA (terrain +
  // entity lists + backdrop tilemap) with real depth, and the PSX OT walk is SKIPPED — this is what makes
  // dynamic objects (Tomba, NPCs, props) render at correct depth instead of the flat is3d=0 PSX prims that
  // the OT walk drops behind the terrain (the "invisible Tomba" regression). Other stages (DEMO/title/menus)
  // still walk the PSX OT (no native path yet). `renderpsx on` (g_render_psx) forces the PSX walk for A/B.
  // NOTE (frontier): scenenative skips ALL the PSX 2D for the field — once in-gameplay dialog/menus/item
  // bubbles appear they need native 2D (RQ_HUD/OVERLAY) or they'll be missing. Free-roam is correct now.
  bool field = (c->mem_r32(0x801FE00Cu) == 0x8010637Cu);
  // SOP INTRO NARRATION (GAME stage, SOP field mode sm[0x4a]==0; free-roam is sm[0x4a]>=1): this is NOT the
  // walkable 3D field — it is a 2D-composited cutscene (full-screen black/colour fills, semi-transparent
  // textured EFFECT quads, character sprites, and text) built entirely by the dispatched PSX SOP code into
  // the guest OT. The native field path (ov_scene_native) wrongly draws the walkable field's terrain/entity
  // world here (e.g. the SEA + characters during the dark "void" beat scene 0x800bf9b4==5) AND the 2D-only OT
  // filter DROPS the cutscene's own fills/effect quads (classified as backdrop/world) — so the void rendered
  // as the sea instead of black+effect, and the cliff water banded. The oracle (interpreter + software GPU,
  // docs/oracle.md) PROVES the PSX renders the whole cutscene from its GP0 stream. So for the narration we
  // walk the FULL guest OT (no native field render, no 2D-only filter) — reproducing the PSX cutscene exactly.
  // The SOP intro narration is active exactly when the loaded MODE overlay is the SOP one — the SAME check
  // the GAME submode-0 dispatcher uses to run ov_sop_field_mode (engine.cpp: *(0x80109450) is the
  // overlay's first instruction; the SOP overlay starts `lui v0,0x1f80` == 0x3C021F80). The walkable field
  // loads a different overlay (e.g. 0x801138A4), so this cleanly separates the cutscene from free-roam
  // (sm[0x4a] does NOT — free-roam settles back to sm[0x4a]==0 like the narration).
  bool sop_narration = field && c->mem_r32(0x80109450u) == 0x3C021F80u;
  // fps60 TRUE per-object tier: did the native scene render run this frame? The 60fps mid-present re-runs
  // sceneNative for the in-between only when it built THIS frame (field). Also arm mSceneTag around each
  // sceneNative() call so the prims it queues are tagged fps_scene=1 (rebuilt at midpoint) vs the OT-walk's
  // 2D/HUD/billboard prims (fps_scene=0, re-emitted). Fps60::sceneNativeTag RAII sets/clears it.
  bool sceneRan = false;
  struct SceneTagArm { Core* c; SceneTagArm(Core* c_):c(c_){ c->game->fps60.mSceneTag = true; }
                       ~SceneTagArm(){ c->game->fps60.mSceneTag = false; } };
  // FAIL-FAST guard (CLAUDE.md pc_render READ-ONLY OVERLAY invariant): arm DisplayPassGuard around
  // pc_render's OWN picture-producing calls only — sceneNative() + the native OT/queue draw below —
  // never around the substrate orchestrator (Render::frame/frameX are called elsewhere and legitimately
  // write the guest OT/packet-pool). Core::mem_w8/16/32 abort on any guest write while armed.
  if (sop_narration && !c->mRender->mode.psxRender()) {
    DisplayPassGuard displayPass(c->mRender->mode);
    // SOP narration render (oracle-derived, docs/oracle.md). The cutscene's full picture is built by the PSX
    // SOP code into the guest OT — full-screen fills, the semi-transparent textured EFFECT quads, character
    // sprites, the sea tiles, and text — so we walk the FULL OT (g_ot_2d_only=0), NOT the 2D-only filter that
    // dropped the fills/effect (which left the void's stale sea showing).
    // The 3D WORLD beats (village/letter == scene 0-3, the void->cliff transition == 6, cliff == 7) need the
    // native entity-list scene render (ov_scene_native: terrain + characters with real depth) — on the native
    // port this is the ONLY source of that 3D geometry (the native SOP submit tees to VK, it does not build
    // the geometry into the guest OT the way the real PSX does). The dark "VOID" swirl beat (SOP scene byte
    // 0x800bf9b4==5) is the one beat with NO 3D world (the oracle draws pure black + swirl effect + text);
    // running ov_scene_native there draws a stale field/sea behind the swirl (the original bug-2). Gate the
    // native 3D render off only for the void — the SOP scene byte is the game's per-beat state, not a magic
    // render constant. (Scene 6 IS a 3D beat: the cliff fading in — gating it off loses the cliff geometry.)
    if (c->mem_r8(0x800BF9B4u) != 5) { SceneTagArm t(c); c->mRender->sceneNative(); sceneRan = true; }
    gpu_dma2_linked_list(c, otHead, /*twoDOnly=*/false);   // full walk incl. cutscene fills/effect quads
  } else if (!c->mRender->mode.psxRender() && (field || cfg_dbg("scenenative"))) {
    DisplayPassGuard displayPass(c->mRender->mode);
    { SceneTagArm t(c); c->mRender->sceneNative(); sceneRan = true; }
    // The native field path owns the 3D world + backdrop, but the field still submits its 2D OVERLAY
    // through the PSX OT: the opening-cutscene narration glyphs, in-game dialog / item bubbles, menus,
    // HUD. Enumerate the OT in 2D-overlay-only mode so those 2D prims are queued as RQ_HUD on top of
    // the native world while the OT's 3D-world / backdrop prims are dropped (owned natively —
    // keeping them would double-draw the world). This is THE behavior, not a debug channel: without
    // it the opening story cutscene rendered nothing and the prior menu's stale VRAM showed through.
    // (scenenativehud kept as a DIAGNOSTIC: full walk incl. world, to A/B the native world render
    // against the PSX 2D-on-top composite.)
    gpu_dma2_linked_list(c, otHead, /*twoDOnly=*/!cfg_dbg("scenenativehud"));
  } else {
    gpu_dma2_linked_list(c, otHead, /*twoDOnly=*/false);
  }
  // ADDITIVE native render subsystem (game/render/) — the decoupled "native experience" pass. Fully
  // separate from the PSX path above: it builds the frame from native SCENE DATA (entity lists + camera)
  // with float transforms and real depth (NO OT/GP0/GTE). Gated behind the `rendernative` DIAGNOSTIC
  // channel (NOT an A/B behavior flag) so we can inspect it before it becomes the default; when off, the
  // pass is never invoked and the PSX-vanilla path is the only renderer. Emitted before rq_flush so its
  // world quads drain with this frame.
  if (cfg_dbg("rendernative")) c->mRender->mNativeScene.run();
  // fps60: record whether the native scene render ran this frame (gates the mid-present scene rebuild).
  c->game->fps60.mSceneRan = sceneRan;
  c->game->rq.flush(c);
}

void games_tomba2_init(void) {
  // ONE behavior = the PC game. The native engine path is registered UNCONDITIONALLY — no FAITHFUL master
  // switch, no per-override *_RECOMP / NO_* opt-outs (those were faithful-first A/B scaffolding; the user
  // directive is no gating + drive toward removing the interpreter entirely, so they are retired). Every
  // override below IS the behavior; the user verifies it via ./run.sh.
  // Hand-written native C++ for the boot→first-cutscene path (game_tomba2.cpp).
  // (games_native_path_init removed: native_misc.cpp was dead reference scaffolding — later-288)
  // OVERRIDE SYSTEM REMOVED (2026-06-22): the whole `_register()` scaffolding block used to install
  // rec_set_override() entries. The override table is gone; the per-subsystem register functions
  // (engine_math_register, save_register, sound_register, hud_register, actor_sm_24448_register, and
  // the beh_*_register siblings) were left as empty stubs "in case." Every stub had a zero- or single-
  // dead-line body — dead scaffolding — and got deleted. Direct-call wiring is the shape now.
  fps60_init();
  void render_observer_install();
  render_observer_install();   // read-only per-object depth tags at the substrate override choke point
  void perobj_dispatch_install();
  perobj_dispatch_install();   // FUN_8003CDD8/FUN_8003F698 substrate-mirror ownership (band 0x8003xxxx)
  void perobj_billboard_install();
  perobj_billboard_install();  // FUN_8003CCA4/C2D4/C464/C8F4 substrate-mirror ownership (band 0x8003xxxx)
  void render_walk_dispatch_install();
  render_walk_dispatch_install();  // FUN_8003C048 render-walk loop ownership (band 0x8003xxxx)
  void overlay_type_dispatch_install();
  overlay_type_dispatch_install();  // FUN_8003D0BC per-area-type overlay dispatch (band 0x8003xxxx)
  void gpu_dma_queue_install();
  gpu_dma_queue_install();     // FUN_80082D04/FB4/83364/82424 GPU-DMA completion-queue cluster
  void gpu_libgpu_leaves_install();
  gpu_libgpu_leaves_install(); // FUN_80080F6C/81458 DrawSync/ClearOTagR (libgpu GPU-sys jump table)
  void gpu_loadimage_streamer_install();
  gpu_loadimage_streamer_install(); // FUN_80082734 libgpu LoadImage() chunked GP0-FIFO pixel streamer
  void gpu_putdrawenv_install();
  gpu_putdrawenv_install();    // FUN_800815D0 PutDrawEnv + 4 DRAWENV field-word builders
  void font_wide_re_install();
  font_wide_re_install();      // FUN_80079374/80078CA8 Font::drawText/glyphEmit (hottest unowned leaves)
  void str_wide_re_install();
  str_wide_re_install();       // FUN_80079528 Str::length (generic strlen, hottest unowned leaf)  if (cfg_dbg("engine"))
    fprintf(stderr, "[engine] native object-list walk active (FUN_8007a904)\n");
}
