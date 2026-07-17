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
#include "game_ctx.h"
#include "game.h"   // Fps60::current_object (was g_current_object)
#include "cfg.h"
#include "mods.h"      // g_mods (fps60 persisted with the other user settings)
#include "margin_render.h"
#include "render.h"           // class Render — rend(c)->sceneNative()
#include "asset.h"      // PC-native asset-loading subsystem (extracted from this file)
#include "mathlib.h"    // PC-native math/PRNG leaf primitives (rand, trig LUTs, bit-test)
#include "cull.h"       // PC-native visibility cull / LOD subsystem
#include "screen_fade.h"   // ScreenFade::installLeafTap — FUN_8007E9C8 global fade-leaf ownership
#include "ui/panel.h"      // Panel::install — FUN_8004FFB4/8005019C global panel-leaf ownership
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
  if (!c->game->mods.fps60) {
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
int gpu_gpu_wide_engine(Core*);   // gpu_gpu.c — genuine engine-wide active (PSXPORT_WIDE_ENGINE && aspect!=4:3)

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

  // ============================================================================================
  // REFERENCE RENDERER (psx_render) — the substrate guest-OT walk. This is now the ONLY caller of
  // gpu_dma2_linked_list. It is not a shipped render behavior: it is the byte-exact PSX reference
  // used by SBS core B, the `renderpsx` REPL toggle, and to DRIVE into scenes whose native producer
  // isn't built yet (PSXPORT_RENDER_PSX=1). The pc_render path below never walks the guest OT.
  // ============================================================================================
  if (c->rsub.mode.psxRender()) {
    gpu_dma2_linked_list(c, otHead, /*twoDOnly=*/false);
    if (cfg_dbg("rendernative")) rend(c)->mNativeScene.run();
    c->game->rq.flush(c);
    return;
  }

  // ============================================================================================
  // THE ONE NATIVE RENDERER (pc_render) — the picture comes from GAME STATE with real depth, NEVER from
  // transcribing the guest OT/GP0 (USER 2026-07-15 "break and rebuild"). Render::renderScene() classifies
  // the scene and dispatches to its per-scene native producer (renderField/renderHutInterior/… in
  // render_walk.cpp); a stage with no producer aborts. The 2D layer comes from its own native producers
  // emitting to the queue during execution — until each is rebuilt, that 2D is visibly ABSENT (the honest
  // break), never transcribed. The substrate render orchestrator (Render::frame/frameX) still runs
  // underneath (guest OT/packet pool = part of the byte-exact state); we simply do not read it here.
  // ============================================================================================
  rend(c)->renderScene();
  // ADDITIVE native render subsystem (game/render/mNativeScene) — the decoupled "native experience" pass,
  // gated behind the `rendernative` DIAGNOSTIC channel (off by default). Builds from native scene data.
  if (cfg_dbg("rendernative")) rend(c)->mNativeScene.run();
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
  void render_observer_install();
  render_observer_install();   // read-only per-object depth tags at the substrate override choke point
  void perobj_dispatch_install();
  perobj_dispatch_install();   // FUN_8003CDD8/FUN_8003F698 substrate-mirror ownership (band 0x8003xxxx)
  void perobj_billboard_install();
  perobj_billboard_install();  // FUN_8003CCA4/C2D4/C464/C8F4 substrate-mirror ownership (band 0x8003xxxx)
  void text_label_install();
  text_label_install();        // FUN_80039F4C text-label renderer (Render::textLabelEmit + WqRec capture)
  void render_walk_dispatch_install();
  render_walk_dispatch_install();  // FUN_8003C048 render-walk loop ownership (band 0x8003xxxx)
  void overlay_type_dispatch_install();
  overlay_type_dispatch_install();  // FUN_8003D0BC per-area-type overlay dispatch (band 0x8003xxxx)
  void objlist_walk_install();
  objlist_walk_install();      // FUN_8003BB50/BCF4/BED8/BF00/EEC0 object-list walkers (band 0x8003xxxx)
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
  str_wide_re_install();       // FUN_80079528 Str::length (generic strlen, hottest unowned leaf)
  ScreenFade::installLeafTap();   // FUN_8007E9C8 fade leaf: gen body + host-state mirror (fixes #63)
  Panel::install();               // FUN_8004FFB4/8005019C/8007CC00 panel + dialog-glyph taps
  void pad_edge_fence_install();
  pad_edge_fence_install();       // FUN_800788AC per-frame input-edge fence (banked draft, §9-verified)
  void guest_memset_install();
  guest_memset_install();         // FUN_8009A420 psyq memset (banked draft; n<=0 return-0 bug fixed at §9)
  cfg_logf("engine", "native object-list walk active (FUN_8007a904)");
}
