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
};
