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
class Core;

class Engine {
public:
  // Back-pointer set once by Core's constructor (same pattern as ScreenFade::core).
  Core* core = nullptr;

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
};
