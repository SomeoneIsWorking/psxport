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

static void ov_frame_update(Core* c) {
  rec_super_call(c, 0x800788ACu);                    // real per-frame state update
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
  for (int v = 0; v < quota; v++) {                  // once per VBlank this logic frame spans
    if (seq_ok) rec_dispatch(c, SEQ_TICK_WRAPPER);   // libsnd per-vblank tick (user cb + SsSeqCalled)
    spu_audio_frame();                               // advance SPU one 1/60 s field + feed device
  }
  c->mem_w16(DISPLAY_COUNTER, c->mem_r8(VBLANK_QUOTA));    // satisfy the pacing dwell immediately
  // fps60 (when enabled) OWNS presentation: it presents the previous real frame + the interpolated
  // frame (60 fps, 1 frame behind) and paces both halves — see fps60_present. The faithful path
  // presents frame B once and paces a full frame.
  fps60_frame_commit(c);
  if (!g_fps60_on) { gpu_present(c); gpu_pace_frame(c); }
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
  rec_set_override(0x800788ACu, ov_frame_update);    // per-frame state update + present + audio
  // Replace the game's in-game Options menu with our overlay. The override falls back to the real menu
  // (super-call) when the overlay isn't up (headless / PSXPORT_UI=0), so it self-activates with the overlay.
  rec_set_override(0x8007B45Cu, ov_options_menu);
  // Own the GTE projection setup natively.
  rec_set_override(0x800846D0u, ov_set_geom_offset);
  rec_set_override(0x800846F0u, ov_set_geom_screen);
  rec_set_override(0x80081560u, ov_draw_otag);       // own DrawOTag (the per-frame draw kick) natively
  { void engine_camera_register(void); engine_camera_register(); }   // per-frame camera X/Z follow native
  { void engine_math_register(void);   engine_math_register();   }   // hot libgte-style math leaves (isqrt)
  { void save_register(void);          save_register();          }   // save/load FLOW dispatcher (engine/save.cpp)
  { void sound_register(void);         sound_register();         }   // PC-native sound front-end (SFX/BGM trigger API, engine/sound.cpp)
  // PC-owned asset codecs.
  rec_set_override(0x80044D8Cu, ov_lz_decompress);   // LZ image decompressor
  rec_set_override(0x80044F58u, ov_load_texgroup);   // texture-group LOADER orchestration (header+archive+unpack)
  rec_set_override(0x80044E84u, ov_unpack_group);    // texture-group unpacker (drives the above)
  rec_set_override(0x80081218u, ov_upload_image);    // PC-native CPU->VRAM upload (libgs upload lib)
  // Own the geometry submit path natively.
  {
    void ov_submit_poly_gt3(Core*), ov_submit_poly_gt4(Core*), ov_submit_poly_gt4_bp(Core*),
         engine_submit_register_autodetect(void);
    rec_set_override(0x8007FDB0u, ov_submit_poly_gt3);   // POLY_GT3 (gouraud-textured triangle) submit
    rec_set_override(0x8008007Cu, ov_submit_poly_gt4);   // POLY_GT4 (gouraud-textured quad) submit
    rec_set_override(0x80027768u, ov_submit_poly_gt4_bp);// byte-packed POLY_GT4 (field's dominant emitter)
    engine_submit_register_autodetect();                 // + own the same library in runtime-loaded overlays
  }
  // Own the PER-OBJECT render flush natively (gen_func_8003CDD8 — the world/margin render submission):
  // compose the camera×object transform + dispatch the geomblk to the native submitter, with NO guest
  // render code (later-133).
  {
    void ov_perobj_flush(Core*), ov_perobj_render(Core*), ov_render_walk(Core*), ov_terrain(Core*);
    void ov_render_walk_snapshot(Core*);
    rec_set_override(0x8003CDD8u, ov_perobj_flush);
    rec_set_override(0x8003CCA4u, ov_perobj_render);   // per-object render dispatch
    rec_set_override(0x8003C048u, ov_render_walk);     // phase-2 render-list walk
    rec_set_override(0x8003BB50u, ov_render_walk_snapshot);  // snapshot-queue object walk + world-pos depth
    void ov_collectable_quad(Core*);
    rec_set_override(0x8003C8F4u, ov_collectable_quad);  // collectable billboard-quad drawer: world-pos depth
    rec_set_override(0x8002AB5Cu, ov_terrain);         // field terrain renderer (float transform + real depth)
    void ov_build_xform(Core*);
    rec_set_override(0x80051C8Cu, ov_build_xform);     // per-object transform builder
    void ov_xform_propagate(Core*);
    rec_set_override(0x80051464u, ov_xform_propagate); // child-node transform propagation (orchestrates owned prims)
    void ov_xform51128(Core*);
    rec_set_override(0x80051128u, ov_xform51128);      // per-object child-node transform loop (sibling of xform_propagate; orchestrates owned prims)
    void ov_orch597AC(Core*);
    rec_set_override(0x800597ACu, ov_orch597AC);       // per-object world-transform orchestrator (build matrix + secondary + child propagate)
    { void ov_cone_cull_2b278(Core*);
      rec_set_override(0x8002B278u, ov_cone_cull_2b278); }  // view-cone cull (lazy conecull gate)
    { void ov_rand(Core*); rec_set_override(0x8009A450u, ov_rand); }   // platform PRNG (rand LCG)
    { void ov_bittest_4d7ec(Core*); rec_set_override(0x8004D7ECu, ov_bittest_4d7ec); }  // bitmap bit-test
    { void ov_trig_sin(Core*), ov_trig_cos(Core*), ov_trig_lut(Core*);
      rec_set_override(0x80083E80u, ov_trig_sin);                      // sin LUT
      rec_set_override(0x80083F50u, ov_trig_cos);                      // cos LUT
      rec_set_override(0x80083EBCu, ov_trig_lut); }                    // sin-quadrant lookup
    { void ov_cull_wrap_77acc(Core*); rec_set_override(0x80077ACCu, ov_cull_wrap_77acc); }  // cull wrapper variant (flags 1/4)
    { void ov_list_scan_31780(Core*); rec_set_override(0x80031780u, ov_list_scan_31780); }  // list-tail resolver/reset
    { void ov_grid_setup_49968(Core*); rec_set_override(0x80049968u, ov_grid_setup_49968); }  // collision-grid row-ptr setup
    { void ov_grid_query_47cbc(Core*); rec_set_override(0x80047CBCu, ov_grid_query_47cbc); }  // collision-grid cell query/walk
    { void ov_grid_resolve_498c8(Core*); rec_set_override(0x800498C8u, ov_grid_resolve_498c8); }  // collision-grid resolve loop (control flow owned)
    { void ov_grid_step_4798c(Core*); rec_set_override(0x8004798Cu, ov_grid_step_4798c); }  // collision-grid per-step origin/index setup (the last dispatched grid callee)
    { void ov_script_vm_4ce14(Core*); rec_set_override(0x8004CE14u, ov_script_vm_4ce14); }  // per-object script-VM tick (control flow owned; sub-behaviors dispatched)
    { void ov_input_dispatch_931c0(Core*); rec_set_override(0x800931C0u, ov_input_dispatch_931c0); }  // per-frame input/controller-state processor (control flow owned)
    // PC-native PLAYER velocity-integrate handler (engine/engine_player.cpp): FUN_80056B48 integrates
    // speed×dir into the master position (16.16 X/Y/Z at 0x800E7EAC/B0/B4). CONTENT (post-boundary), owned
    // native; `playerverify` full RAM+scratchpad A/B gate. The settle helper 0x80054650 stays dispatched.
    { void ov_player_move(Core*); rec_set_override(0x80056B48u, ov_player_move); }
    { void ov_anim_vm_76d68(Core*); rec_set_override(0x80076D68u, ov_anim_vm_76d68); }  // per-object animation-sequence VM stepper (control flow owned; 3 frame sub-fns dispatched)
    { void ov_child_spawn_40410(Core*); rec_set_override(0x80040410u, ov_child_spawn_40410); }  // per-object child-node spawn/sub-object builder (control flow owned; allocator+setup dispatched)
    { void ov_sm40558(Core*); rec_set_override(0x80040558u, ov_sm40558); }  // per-object state-machine head (control flow owned; all sub-behaviors dispatched)
    { void ov_osc_fd10(Core*); rec_set_override(0x8003FD10u, ov_osc_fd10); }  // sm40558 STATE-1 obj[5]=0 oscillate/frame-toggle handler (control flow owned; ov_rand dispatched)
    { void ov_disp_26c88(Core*); rec_set_override(0x80026C88u, ov_disp_26c88); }  // per-object dispatcher loop over 0x800ec188 table (control flow owned; handlers dispatched)
    { void ov_bav_load(Core*); rec_set_override(0x80096590u, ov_bav_load); }  // per-area BAV effect-cel LOADER (engine_bav.cpp): slot alloc + cel/UV parse + global latch owned; VRAM alloc/upload callback dispatched
  }
  // PC-native LEVEL/STAGE LOADER (engine/engine_level.cpp): the engine's overlay loader FUN_800450bc —
  // load a stage's overlay off the disc + set its entry, synchronous (no PSX CD-wait yield).
  { void ov_load_stage(Core*); rec_set_override(0x800450BCu, ov_load_stage); }
  // PC-native STAGE TRANSITION (engine/engine_level.cpp): FUN_80052078 — load the next stage + restart the
  // task at its new entry. Thread plumbing replaced by the native scheduler (state=3 == restart); the
  // terminal yield is the existing ov_switch. Exercised at the START->DEMO->GAME boot transitions.
  { void ov_stage_transition(Core*); rec_set_override(0x80052078u, ov_stage_transition); }
  // PC-native task-0 BOOTSTRAP (engine/engine_level.cpp): FUN_800499e8 — resolve \BIN\START.BIN, record its
  // (LBA,size) in the stage table, transition to stage 0. CD-directory lookup stays the platform mechanism.
  { void ov_task0_boot(Core*); rec_set_override(0x800499E8u, ov_task0_boot); }
  // PC-native FONT/TEXT init (engine/engine_font.cpp): FUN_80075130 is called directly from native_boot,
  // but register the orchestrator + its 3 owned engine callees so any other dispatcher to them uses native
  // (the 8 libgpu/sound callees stay PSX, dispatched in-context). later (font frontier).
  { void ov_font_init(Core*), ov_font_bank_select(Core*), ov_font_bank2_store(Core*), ov_font_glyphclass_fill(Core*);
    rec_set_override(0x80075130u, ov_font_init);
    rec_set_override(0x800963a0u, ov_font_bank_select);
    rec_set_override(0x80096370u, ov_font_bank2_store);
    rec_set_override(0x800752b4u, ov_font_glyphclass_fill); }
  fps60_init();
  // cull tap: genuine-wide is the default wide path and the overlay can toggle aspect LIVE, so the
  // widened-frustum re-include is always available. ov_object_cull is a faithful super-call + a wide-only
  // re-include (no-op at 4:3).
  rec_set_override(0x8007712Cu, ov_object_cull);
  rec_set_override(0x8007778Cu, ov_cull_wrapper);    // camera-relative delta + flag reset → dispatch the cull body
  // PC-native per-object DEPTH at the render-command dispatcher (the universal chokepoint): every queued
  // render command passes through 0x8003F698 with the composed camera×object transform live in the GTE, so
  // ov_render_cmd computes the object's world-position view-depth there and tags the command's packet-pool
  // span → a 2D billboard prim occludes by real world depth, not sprite order. (Also carries the rcmd debug
  // dump when PSXPORT_DEBUG=rcmd.) Always on — one behavior. later-130/this session.
  { void ov_render_cmd(Core*); rec_set_override(0x8003F698u, ov_render_cmd); }
  // Enqueue tap (PSXPORT_DEBUG=enq): the render-command push, called per-object in phase 1 → g_current_object
  // names the source object, the attribution the downstream oracle lacks. later-131. Gated (super-call).
  if (cfg_dbg("enq")) { void ov_enqueue_probe(Core*); rec_set_override(0x80077EBCu, ov_enqueue_probe); }
  // Flush tap (PSXPORT_DEBUG=flush): dump the command-struct addresses (list+0xc0[i]) the flush drains, to
  // trace the still-open render-command enqueue. later-131. Gated (super-call).
  if (cfg_dbg("flush")) { void ov_flush_probe(Core*); rec_set_override(0x8003F174u, ov_flush_probe); }
  // Major flush tap (PSXPORT_DEBUG=flush2): the world/margin flush gen_func_8003CDD8 (later-133). Gated.
  if (cfg_dbg("flush2")) { void ov_flush2_probe(Core*); rec_set_override(0x8003CDD8u, ov_flush2_probe); }
  // Command-enqueue tap (PSXPORT_DEBUG=cmdenq): gen_func_80051B70, validates obj/(group,sub)→geomblk. later-132.
  if (cfg_dbg("cmdenq")) { void ov_cmdenq_probe(Core*); rec_set_override(0x80051B04u, ov_cmdenq_probe); }
  // Submitter call-counter (PSXPORT_DEBUG=subcnt): which un-owned submit variants fire per scene. Gated.
  if (cfg_dbg("subcnt")) { void ov_subcnt_b320(Core*), ov_subcnt_c8f4(Core*);
    rec_set_override(0x8003B320u, ov_subcnt_b320); rec_set_override(0x8003C8F4u, ov_subcnt_c8f4); }
  // Per-object dispatch case histogram (PSXPORT_DEBUG=ccase): which gen_func_8003CCA4 cases fire. Gated.
  if (cfg_dbg("ccase")) { void ov_ccase_probe(Core*); rec_set_override(0x8003CCA4u, ov_ccase_probe); }
  // Phase-2 render-walk caller counter (PSXPORT_DEBUG=rwalk): which orchestrator drives 8003CCA4. Gated.
  if (cfg_dbg("rwalk")) {
    void ov_rwalk_b588(Core*), ov_rwalk_bb50(Core*), ov_rwalk_bcf4(Core*),
         ov_rwalk_bf00(Core*), ov_rwalk_c048(Core*), ov_rwalk_eec0(Core*);
    rec_set_override(0x8003B588u, ov_rwalk_b588); rec_set_override(0x8003BB50u, ov_rwalk_bb50);
    rec_set_override(0x8003BCF4u, ov_rwalk_bcf4); rec_set_override(0x8003BF00u, ov_rwalk_bf00);
    rec_set_override(0x8003C048u, ov_rwalk_c048); rec_set_override(0x8003EEC0u, ov_rwalk_eec0);
  }
  // Render-list node-type dump (PSXPORT_DEBUG=rlist): the full type set 8003C048 must handle. Gated.
  if (cfg_dbg("rlist")) { void ov_rlist_probe(Core*); rec_set_override(0x8003C048u, ov_rlist_probe); }
  // Issue #4: own the AUXILIARY render walks PC-native (engine/engine_submit.cpp) so flame/rope/effect
  // billboards get a real WORLD-POSITION depth (gpu_obj_depth_add) instead of falling to the flat 2D band
  // and drawing over occluding foliage. Faithful per-node lift of each recomp body + per-node depth tag,
  // mirroring ov_render_walk_snapshot. Skip when PSXPORT_DEBUG=rwalk is on (the diagnostic counters above
  // own these addresses then; the override table is last-registration-wins, so this guard avoids a clash).
  if (!cfg_dbg("rwalk")) {
    void ov_rwalk_aux_bcf4(Core*), ov_rwalk_aux_bf00(Core*), ov_rwalk_aux_eec0(Core*);
    rec_set_override(0x8003BCF4u, ov_rwalk_aux_bcf4);
    rec_set_override(0x8003BF00u, ov_rwalk_aux_bf00);
    rec_set_override(0x8003EEC0u, ov_rwalk_aux_eec0);
  }
  void engine_tomba2_init(void);
  engine_tomba2_init();                            // native engine layer (Phase 1: object-list walk)
}
