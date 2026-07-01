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
#include "game.h"   // Fps60State::current_object (was g_current_object)
#include "cfg.h"
#include "margin_render.hpp"
#include "asset.h"      // PC-native asset-loading subsystem (extracted from this file)
#include "mathlib.h"    // PC-native math/PRNG leaf primitives (rand, trig LUTs, bit-test)
#include "cull.h"       // PC-native visibility cull / LOD subsystem
#include "collision.h"  // PC-native collision-grid subsystem
#include "entity.h"     // PC-native per-object entity state-machine subsystem
#include "script.h"     // PC-native per-object script-VM subsystem
#include "animation.h"  // PC-native per-object animation-VM subsystem
#include "input.h"      // PC-native per-frame input/controller subsystem
#include "menu.h"       // PC-native in-game Options menu subsystem
#include <stdlib.h>
#include <stdio.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (super-call / A/B oracle)
void fps60_init(void);            // fps60: read PSXPORT_FPS60
// geomblk capture probe (engine_submit.c): the LAST object the cull ran on. Unlike g_current_object this is
// NOT restored on return — a handler calls its cull (FUN_8007712c) then immediately submits its geometry, so
// across that submit g_render_object identifies the rendering object. Pure probe key; no gameplay effect.
uint32_t g_render_object = 0;
extern int g_fps60_on;            // fps60: capture enabled (PSXPORT_FPS60)
extern "C" void spu_audio_frame(void);        // SPU: advance the mixer one frame + feed the audio device
extern "C" void spu_audio_frame_logic(void);  // SPU: advance XA stream for game logic only (SBS diff_mode)
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

// gpu_perf.cpp — per-frame CPU phase profiler (REPL `debug perf`), default off. PH_LOGIC=0/AUDIO=1/PRESENT=2.
extern "C" void perf_phase_begin(int), perf_phase_end(int);

// ---- NATIVE field BGM director ------------------------------------------------------------------
// The libsnd sequencer is SILENT in the port (every voice keyon binds VAB id = -1; later-218), so the
// field plays no music despite libsnd ticking. We drive the PC-native synth (engine/audio/) from the
// LIVE area bundle (guest 0x80182000: 10 SEPs + the field VAB @+0x26b4) instead. This director runs
// once per logic frame: while the GAME stage (the field, 0x8010637C) is active AND the area bundle is
// present, start the area BGM natively (once, on entry) and keep it playing; stop it when leaving the
// field. The played sequence defaults to the field theme; override with PSXPORT_FIELD_SONG=<0..9> for
// auditioning (the USER confirms the right one by ear). This is a real PC-game feature replacing a
// broken subsystem, not an A/B behavior gate.
extern "C" int  music_list_play_area(const uint8_t* bundle, long len, int song);
extern "C" void music_list_stop(void);
extern "C" int  music_list_now_playing(void);

#define GAME_STAGE_ADDR 0x801062ECu   // current-stage cell (c->mem_r32) — 0x8010637C while in the field
#define AREA_BUNDLE     0x182000u     // guest 0x80182000 -> RAM offset

extern "C" int native_gate(const char* name);   // native_boot.cpp — REPL `native <name> on|off`

static void field_bgm_director(Core* c) {
  if (!native_gate("music")) return;   // gated off -> recomp libsnd is the (oracle) music path
  // Are we in the field (GAME stage running)? The stage cell holds the active stage's task-0 entry.
  uint32_t stage = c->mem_r32(0x801fe00c);
  int in_field = (stage == 0x8010637Cu);
  static int s_started = 0;
  if (!in_field) {
    if (s_started) { music_list_stop(); s_started = 0; }
    return;
  }
  if (s_started) {
    // Restart if the song fully drained (one-shot tail) so the field stays scored.
    if (music_list_now_playing() < 0) s_started = 0; else return;
  }
  // Validate the bundle is loaded before starting (area data may not be in yet right after a load).
  const uint8_t* b = c->ram + AREA_BUNDLE;
  if (memcmp(b + 0x30, "pQES", 4) || memcmp(b + 0x26b4, "pBAV", 4)) return;
  int song = 8;                                   // default field theme (longest area seq)
  if (const char* s = cfg_str("PSXPORT_FIELD_SONG")) { int v = atoi(s); if (v >= 0 && v < 10) song = v; }
  if (music_list_play_area(b, 0x50000, song) == 0) s_started = 1;
}

// Per-frame engine tick. Called DIRECTLY (a plain C call) from native_step_frame (native_boot.cpp) —
// this is the PC-driven game loop's frame body. It runs the still-PSX per-frame update leaf, then OWNS
// the per-vblank audio, the fps60 commit, and present + pace (the things the now-removed override table
// used to attach). NOT an override anymore; not static so native_boot can call it top-down.
void ov_frame_update(Core* c) {
  perf_phase_begin(0);                               // perf: LOGIC = all guest interpreter work + render submit
  rec_dispatch(c, 0x800788ACu);                      // real per-frame state update (still-PSX leaf)
  perf_phase_end(0);
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
    for (int v = 0; v < quota; v++) spu_audio_frame_logic();
    return;
  }
  uint32_t seqfn = c->mem_r32(SEQ_FUNC_PTR);
  int seq_ok = !cfg_on("PSXPORT_T2_NOSEQTICK") && native_gate("seqtick")
               && (seqfn & 0x1FFFFFFFu) >= 0x10000u && (seqfn & 0x1FFFFFFFu) < 0x200000u;
  perf_phase_begin(1);                               // perf: AUDIO = per-vblank sequencer tick + SPU advance
  for (int v = 0; v < quota; v++) {                  // once per VBlank this logic frame spans
    if (seq_ok) rec_dispatch(c, SEQ_TICK_WRAPPER);   // libsnd per-vblank tick (user cb + SsSeqCalled)
    spu_audio_frame();                               // advance SPU one 1/60 s field + feed device
  }
  // (native field-BGM director REMOVED — it played a HARDCODED song over everything from the menu on.
  //  Music is the recompiled libsnd path above; no native music engine, no hardcoded song.)
  perf_phase_end(1);
  c->mem_w16(DISPLAY_COUNTER, c->mem_r8(VBLANK_QUOTA));    // satisfy the pacing dwell immediately
  // fps60 (when enabled) OWNS presentation: it presents the interpolated in-between + the real frame
  // (60 fps, 1 frame behind) and paces both halves — see fps60_present_vk. The faithful path
  // presents frame B once and paces a full frame.
  fps60_frame_commit(c);
  if (!g_fps60_on) {
    perf_phase_begin(2);                             // perf: PRESENT-cpu = VRAM mirror upload + VK record/submit
    gpu_present(c);
    perf_phase_end(2);                               // (pacing/vsync sleep below is excluded -> shows as idle/pace)
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

// --- Native ownership of the GTE projection setters (libgte) -------------------------------------
// The engine configures its projection via libgte SetGeomOffset/SetGeomScreen (RE: docs/engine_re.md,
// FUN_800509B4 -> screen center (160,120), focal length H=350). We reimplement them in native C
// (byte-identical to gen_func_800846D0/800846F0) so the PROJECTION is ours: this is the genuine
// widescreen FOV lever (widen OFX + the draw-env clip; no squish, no renderer re-center) and the
// reference point the 60fps/hi-res paths build on. Owned unconditionally (no gating). Was the
// recomp bodies for A/B. A one-time log prints the configured projection to confirm equivalence.
static void ov_set_geom_offset(Core* c) {       // SetGeomOffset(ofx, ofy)
  // FAITHFUL: write the game's exact projection offset. We do NOT widen OFX here anymore — CR24 is
  // SHARED GTE state that the GAME's OWN logic reads back (its on-screen tests / placement run RTPS and
  // consume the projected SXY). Widening it globally shifted those read-backs and corrupted the game.
  // Genuine widescreen now happens ONLY inside our owned render submit (engine_submit.c), isolated from
  // this guest-visible state — the PC engine drives the wide render; the game's logic stays untouched.
  uint32_t ofx = c->r[4], ofy = c->r[5];
  gte_write_ctrl(24, ofx << 16);                 // OFX
  gte_write_ctrl(25, ofy << 16);                 // OFY
  static int logged = 0;
  if (!logged++) fprintf(stderr, "[geom] native SetGeomOffset OFX=%u OFY=%u (CR24=%08X CR25=%08X)\n",
                         ofx, ofy, gte_read_ctrl(24), gte_read_ctrl(25));
}
static void ov_set_geom_screen(Core* c) {       // SetGeomScreen(h) — projection-plane distance (FOV)
  gte_write_ctrl(26, c->r[4]);                   // H
  static int logged = 0;
  if (!logged++) fprintf(stderr, "[geom] native SetGeomScreen H=%u (CR26=%08X)\n", c->r[4], gte_read_ctrl(26));
}

// Native ownership of DrawOTag (libgpu FUN_80081560, the per-frame draw kick): the recomp body just
// programs the GPU linked-list DMA to walk the ordering table at a0 — which our renderer already does
// natively in gpu_dma2_linked_list (walk OT -> decode each primitive -> rasterize). Overriding it routes
// the draw straight through our native walk (synchronous), instead of the DMA-register emulation dance.
// This is the engine's draw submission, owned.
void gpu_blank_display(Core* core);
extern "C" int g_render_psx;   // engine_render.cpp — A/B compare switch (forces the PSX OT walk)
extern int g_ot_2d_only;       // gpu_native.cpp — OT walk queues ONLY 2D HUD prims (world/bg owned natively)
void ov_draw_otag(Core* c) {   // called directly from native_step_frame (PC-driven); NOT an override
  // #7/#11 finish: while the DEMO/title front-end is still LOADING its assets (sub-SM task0+0x48 < 2, the
  // s4a load ramp), the title composites its menu/font over whatever VRAM the FMV/SCEA splash left — so the
  // SCEA text / FMV last-frame show through (the one-time hand-off clear can't cover the multi-frame ramp).
  // Blank the display FB to black BEFORE this frame's prims draw, every loading frame, so the title's
  // partial 2D layer always sits on opaque black. Once loaded (s48>=2) the title owns a full background and
  // this is a no-op-equivalent (its bg overwrites the black). Engine-owned, keyed on the stage's own signal.
  if (c->mem_r32(0x801FE00Cu) == 0x801062E4u && c->mem_r8(0x801FE048u) < 2) gpu_blank_display(c);
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
  // the GAME submode-0 dispatcher uses to run ov_sop_field_mode (engine_stage.cpp: *(0x80109450) is the
  // overlay's first instruction; the SOP overlay starts `lui v0,0x1f80` == 0x3C021F80). The walkable field
  // loads a different overlay (e.g. 0x801138A4), so this cleanly separates the cutscene from free-roam
  // (sm[0x4a] does NOT — free-roam settles back to sm[0x4a]==0 like the narration).
  bool sop_narration = field && c->mem_r32(0x80109450u) == 0x3C021F80u;
  if (sop_narration && !g_render_psx) {
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
    if (c->mem_r8(0x800BF9B4u) != 5) { void ov_scene_native(Core*); ov_scene_native(c); }
    gpu_dma2_linked_list(c, c->r[4]);
  } else if (!g_render_psx && (field || cfg_dbg("scenenative"))) {
    void ov_scene_native(Core*); ov_scene_native(c);
    // The native field path owns the 3D world + backdrop, but the field still submits its 2D OVERLAY
    // through the PSX OT: the opening-cutscene narration glyphs, in-game dialog / item bubbles, menus,
    // HUD. Enumerate the OT in 2D-overlay-only mode (g_ot_2d_only) so those 2D prims are queued as
    // RQ_HUD on top of the native world while the OT's 3D-world / backdrop prims are dropped (owned
    // natively — keeping them would double-draw the world). This is THE behavior, not a debug channel:
    // without it the opening story cutscene rendered nothing and the prior menu's stale VRAM showed
    // through. (scenenativehud kept as a DIAGNOSTIC: full walk incl. world, to A/B the native world
    // render against the PSX 2D-on-top composite.)
    g_ot_2d_only = cfg_dbg("scenenativehud") ? 0 : 1;
    gpu_dma2_linked_list(c, c->r[4]);
    g_ot_2d_only = 0;
  } else {
    gpu_dma2_linked_list(c, c->r[4]);
  }
  // ADDITIVE native render subsystem (game/render/) — the decoupled "native experience" pass. Fully
  // separate from the PSX path above: it builds the frame from native SCENE DATA (entity lists + camera)
  // with float transforms and real depth (NO OT/GP0/GTE). Gated behind the `rendernative` DIAGNOSTIC
  // channel (NOT an A/B behavior flag) so we can inspect it before it becomes the default; when off, the
  // pass is never invoked and the PSX-vanilla path is the only renderer. Emitted before rq_flush so its
  // world quads drain with this frame.
  if (cfg_dbg("rendernative")) { void render_scene_native(Core*); render_scene_native(c); }
  rq_flush(c);
}

void games_tomba2_init(void) {
  // ONE behavior = the PC game. The native engine path is registered UNCONDITIONALLY — no FAITHFUL master
  // switch, no per-override *_RECOMP / NO_* opt-outs (those were faithful-first A/B scaffolding; the user
  // directive is no gating + drive toward removing the interpreter entirely, so they are retired). Every
  // override below IS the behavior; the user verifies it via ./run.sh.
  // Hand-written native C++ for the boot→first-cutscene path (engine/native_path.cpp).
  // (games_native_path_init removed: native_misc.cpp was dead reference scaffolding — later-288)
  // OVERRIDE SYSTEM REMOVED (2026-06-22): every rec_set_override(...) registration that used to live in
  // this init was deleted. The ov_* native fns are KEPT (in native_path.cpp / engine_submit.cpp / etc.)
  // as future DIRECT-CALL targets, wired top-down as each parent is owned. The *_register() helpers below
  // are plain C calls into the subsystem init (now no-ops where their bodies only registered overrides),
  // left in place so re-introducing direct wiring per subsystem is a one-line change.
  { void engine_camera_register(void); engine_camera_register(); }
  { void engine_math_register(void);   engine_math_register();   }
  { void save_register(void);          save_register();          }
  { void sound_register(void);         sound_register();         }
  { void hud_register(void);           hud_register();           }
  { void engine_submit_register_autodetect(void); engine_submit_register_autodetect(); }
  { void entity_spawn_register(void);  entity_spawn_register();  }
  { void actor_sm_24448_register(void); actor_sm_24448_register(); }
  { void beh_scene_ui_trigger_register(void);  beh_scene_ui_trigger_register();  }
  { void beh_typed_init_scene_trigger_register(void);  beh_typed_init_scene_trigger_register();  }
  { void beh_pickup_collect_trigger_register(void);  beh_pickup_collect_trigger_register();  }
  { void inventory_register(void);     inventory_register();     }
  fps60_init();
  void engine_tomba2_init(void);
  engine_tomba2_init();                            // native engine layer (Phase 1: object-list walk)
}
