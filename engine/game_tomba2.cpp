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
  int quota = c->mem_r8(VBLANK_QUOTA); if (quota < 1) quota = 1;
  uint32_t seqfn = c->mem_r32(SEQ_FUNC_PTR);
  int seq_ok = !cfg_on("PSXPORT_T2_NOSEQTICK")
               && (seqfn & 0x1FFFFFFFu) >= 0x10000u && (seqfn & 0x1FFFFFFFu) < 0x200000u;
  perf_phase_begin(1);                               // perf: AUDIO = per-vblank sequencer tick + SPU advance
  for (int v = 0; v < quota; v++) {                  // once per VBlank this logic frame spans
    if (seq_ok) rec_dispatch(c, SEQ_TICK_WRAPPER);   // libsnd per-vblank tick (user cb + SsSeqCalled)
    spu_audio_frame();                               // advance SPU one 1/60 s field + feed device
  }
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
int gpu_vk_wide_engine(void);   // gpu_vk.c — genuine engine-wide active (PSXPORT_WIDE_ENGINE && aspect!=4:3)

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
static void ov_draw_otag(Core* c) {
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
  gpu_dma2_linked_list(c, c->r[4]);
  rq_flush(c);
}

void games_tomba2_init(void) {
  // ONE behavior = the PC game. The native engine path is registered UNCONDITIONALLY — no FAITHFUL master
  // switch, no per-override *_RECOMP / NO_* opt-outs (those were faithful-first A/B scaffolding; the user
  // directive is no gating + drive toward removing the interpreter entirely, so they are retired). Every
  // override below IS the behavior; the user verifies it via ./run.sh.
  // Hand-written native C++ for the boot→first-cutscene path (engine/native_path.cpp).
  void games_native_path_init(void);
  games_native_path_init();
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
  { void objbeh_739ac_register(void);  objbeh_739ac_register();  }
  { void objbeh_73cd8_register(void);  objbeh_73cd8_register();  }
  { void objbeh_741dc_register(void);  objbeh_741dc_register();  }
  { void inventory_register(void);     inventory_register();     }
  fps60_init();
  void engine_tomba2_init(void);
  engine_tomba2_init();                            // native engine layer (Phase 1: object-list walk)
}
