// class Engine — the PC-native GAME/STAGE driver of Tomba! 2.
//
// PROPER OOP: one instance per Core (embedded as `Core::engine`). Callers use it as
// `c->engine.method()` — non-static instance methods, back-pointer to Core stored once at Core
// construction time (same pattern as `Core::screenFade`). No `extern "C"` shims.
//
// SCOPE: the GAME-stage state machine (sm[0x48] handlers) and its per-frame field driver — the
// top of the field spine that was previously exposed as the free functions `ov_game_stage_prologue`
// / `ov_game_frame` / `ov_field_frame` / `ov_field_frame_x` in engine_stage.cpp. Those free
// functions are the implementation; this class is the public interface (called by the scheduler
// and by the state-machine handlers themselves once fully migrated).
//
// Migration is progressive: methods are added here as pieces of engine_stage.cpp are class-ified.
// The existing static helpers in engine_stage.cpp keep working meanwhile — an Engine method may
// currently be a thin forwarder to a static ov_* function in that TU. As each is class-ified in
// place, the forwarder collapses.
#pragma once
#include "scene_transition.h"    // Engine owns the SceneTransition subsystem instance
#include "transition_state3.h"   // Engine owns the TransitionState3 walker instance
class Core;

class Engine {
public:
  // Back-pointer set once by Core's constructor (same pattern as ScreenFade::core).
  Core* core = nullptr;

  // ── Scene subsystem instances owned by Engine ─────────────────────────────────────
  // Callers reach them as `c->engine.sceneTransition.method(args)`.
  SceneTransition sceneTransition;   // area-mask trigger + sub-scene swap handshake
  TransitionState3 transitionState3;  // mid-transition entity walker (guest FUN_8007B04C)

  // ── GAME-stage entry points (called by the scheduler each frame) ────────────────────────────
  // stagePrologue: one-time prologue that runs when the GAME task enters — task-slot setup, first
  //   field-mode load, etc. (was `ov_game_stage_prologue`).
  // frame: one frame of the GAME stage — the sm[0x48] dispatcher + tail. Returns 0 if the current
  //   sm[0x48] state isn't owned natively yet (scheduler falls back to substrate); non-zero if
  //   handled here (was `ov_game_frame`).
  void stagePrologue();
  int  frame();

  // ── ov_field_frame direct children (progressive class-ification) ──────────────────────────
  // areaModeDispatch: the 22-way area-mode jump-table dispatcher at guest 0x8001CAC0. Reads the
  //   area RENDER-MODE byte at 0x800BF870 and dispatches to the overlay handler that owns that
  //   mode (each mode = an entry in the resident table at 0x80010000; 10 of 22 slots are the
  //   "no-op default" stub 0x8001CB98, the other 12 jal one specific overlay leaf then return).
  //   Replaces `d0(c, 0x8001cac0u)` in the field-frame body.
  void areaModeDispatch();

  // sceneEventFifo: the field EVENT/COMMAND-QUEUE state machine at guest 0x80025588 (struct
  //   @0x800ed058). 3-state top switch on base[2]: state 0 arms + falls through to state 1
  //   (active body drains a small FIFO); state >=2 no-op. Was `d0(c, 0x80025588)`.
  void sceneEventFifo();

  // sceneRenderListBuilder: 2-phase scene/render-list builder driver at guest 0x8004FE84 (struct
  //   @0x800bf548). Phase 0 arms it (snapshot list ptr 0x800ecf64 into base+0x2b0..0x2b8, advance
  //   phase to 1); phase 1 dispatches a sub-state handler (base[1] 0..3 -> distinct overlays);
  //   flag @0x800bf822 bit 0 latched from (base[1]!=0 || base[0x0a]!=0). Was `d0(c, 0x8004fe84)`.
  void sceneRenderListBuilder();

  // sceneStateStep: the per-frame area SCENE-INIT / SCENE-RUN state machine at guest 0x80050DE4.
  //   Two overlay-handler tables of 22 entries each (indexed by the same render-mode byte
  //   0x800BF870 that areaModeDispatch uses), plus a scene-phase byte at 0x800F2418:
  //     phase == 0 -> INIT: call the INIT handler for the current area mode, set phase=1.
  //     phase == 1 -> RUN:  call the RUN handler for the current area mode (per-frame update).
  //     phase < 0 or >= 2 -> no-op.
  //   Both handler tables extracted verbatim from MAIN.EXE .text (@0x80015A40 init, @0x80015A98 run,
  //   21 overlay leaves + 1 default no-op each). Handlers take a0 = 0x800F2418 (the scene-state
  //   base). Replaces `d0(c, 0x80050de4u)` in the field-frame body.
  void sceneStateStep();

  // modePerFrameDispatch: the second per-frame render-mode dispatcher at guest 0x80022A80 (ov_field_frame
  //   calls it right after the object dispatcher). Keyed by the same render-mode byte 0x800BF870;
  //   reads a 24-entry function-pointer table at 0x8009D1D4 (MAIN.EXE .rodata, one entry per area
  //   mode, all pointing at overlay leaves in the 0x8010xxxx..0x80117xxx range that the currently-
  //   loaded overlay owns). Mode 3 (A00 fisherman village) is explicitly SKIPPED before the table
  //   read — the only special case. Replaces `d0(c, 0x80022a80u)` in the field-frame body.
  void modePerFrameDispatch();

  // postRenderTick: small 3-state machine on byte 0x800BF842 at guest 0x80077D8C, called after the
  //   per-frame render submit. `b42 & 0x7F` selects: 1 = trigger FX 41 then set b42=0x87; 2 = trigger
  //   FX 42 then clear b42; otherwise = decrement b42. Trigger call = FUN_80074590(id, 2, -65) — a
  //   sound/vibration fx queue leaf, still substrate. Replaces `d0(c, 0x80077d8cu)` in ov_field_frame.
  void postRenderTick();

  // frameStartTick: the first call in ov_field_frame's gameplay block (guest 0x80059D28). A per-frame
  //   prologue that (a) decrements the frame counter at 0x800BF819 and masks two 12-bit heading fields
  //   when it fires, (b) zeroes a bank of frame-scoped flags at G+0x177..0x17B and 0x1F80027A,
  //   (c) increments the per-frame stamp at 0x1F800247, (d) if 0x800BF841 == 0 dispatches a mode-keyed
  //   per-frame handler (modes 2/3/7/20 -> overlay-specific, else 0x8005950C) with G as a0 and clears
  //   0x1F800230, (e) seeds the master position + heading (G+0x2E/0x32/0x36/0x56/0x58) into scratchpad
  //   0x1F800160..0x1F80016A for the projection to read, (f) latches 0x800BF81E=1 when 0x800BF9C3&0x80,
  //   (g) ticks the sub-counter at G+0x180 when 0x1F800137 (pause flag) is 0, (h) advances the LFSR
  //   rand at 0x8009A450 every frame. Replaces `d0(c, 0x80059d28u)` in ov_field_frame.
  void frameStartTick();

  // areaUpdateTail: the last direct child of ov_field_frame's gameplay block — per-frame area-slot
  //   state machine at guest 0x80075A80. Iterates a 24-entry × 12-byte slot table at 0x800BE238
  //   keyed by the counter at 0x800BED78; per slot the kind byte drives one of three arms:
  //     kind == 0    -> skip to the buf[slot] post-check (nothing to do this frame).
  //     kind == 0xFF -> fire the action leaf FUN_80092660(slot_s16, hword_g, sub1, sub2, [3 stacked
  //                     bytes]) using either the g_a4f7e hword (top bit of sub2 set, arm-hi) or
  //                     the 0xBED84 hword (arm-lo), clear the "armed" bit in the 24-bit mask at
  //                     0x800BE358, then decrement kind.
  //     other        -> if slot[7] == 4, decrement kind; if it went to 0, SET the bit in 0x800BE358
  //                     and zero slot[1]/slot[2]. Else just decrement kind.
  //   Then a per-slot post-check reads buf[slot] filled by FUN_800998e4 at entry (0=zero slot[1];
  //   3=fall through unchanged; other=fall through). Tail: if 0x800BE358 nonzero call FUN_80098F90(0)
  //   + clear it; then FUN_80075824(0x800BE1F8), FUN_80099490(0x800BE1F8); if key2 = mem_r16s(0x800BED80)
  //   != -1, clear 0x800BE1F8, look up hword table[key2].w0 at 0x800BE368+key2*8, call FUN_8008E0C0
  //   with that hword and 0 — if it returns nonzero on the low16, fall to epilogue; else read the
  //   sub-object id at 0x800BE22A, if zero call FUN_80074E48 else call FUN_80074BF8(id) and clear
  //   both 0x800BED80 and 0x800BE22A. All 8 callees stay substrate. Replaces `d0(c, 0x80075a80u)`.
  void areaUpdateTail();
};
