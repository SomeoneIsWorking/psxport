// class ScreenFade — the PC-native screen-fade subsystem.
//
// PROPER OOP: an instance per Core (embedded as `Core::screenFade`). Callers use it as
// `c->screenFade.method(args)` — non-static instance methods, back-pointer to Core stored once at
// Game construction time (like the other subsystem back-pointers in game.h). State lives in guest
// memory at ENGINE_OWNED_BSS 0x800E7DE0 so it's SBS-clean per Core and still-recomp code can read
// it if it ever needs to.
//
// Design (2026-07-01, RE'd against scratch/decomp/field2/8002655c.c bg_scene_transition_sm +
// scratch/decomp/game/transition.c ov_transition_main + sop/80109450.c ov_sop_field_mode):
//
//   PSX behaviour: fade is a semi rect drawn each frame at OT slot 4 by FUN_8007e9c8(color, mode, 4).
//   The OT resets per frame — a frame with no fade call = no rect = scene visible. Every SM that owns
//   an active fade state calls FUN_8007e9c8 EVERY frame it wants a fade (ramp AND "hold black/white"
//   are both per-frame calls). "No fade" isn't a persistent state, it's the absence of a call.
//
//   PC-native model: this class holds ONE frame's worth of fade state and RESETS at the top of every
//   logic frame (frameStart, called from ov_game_frame). Every fade caller MUST be NATIVE — it calls
//   set() / applyLeafCall() directly. Still-recomp fade callers do NOT reach this class (the recomp
//   substrate FUN_8007e9c8 body just writes guest OT data that our renderer no longer draws); each
//   one is a top-down port task, tracked in docs/port-progress.md. Overrides are NOT the answer —
//   registering ov_8007E9C8 as an override would violate the top-down ownership rule (overrides are
//   for async-to-sync platform HLE only). The class exposes applyLeafCall() as the (color, a1)
//   caller-shape so native ports of a fade caller stay 1:1 with the guest RE.
//
//   The class layers a "held fully-faded" latch on top of per-frame reset (see FULLY_FADED_THRESHOLD
//   below): when a fade span reaches full black/white, subsequent admin frames with no caller stay
//   at black/white until a fresh set() lands below the threshold and releases the hold. This is what
//   keeps the transition period (fade-out complete -> load-and-swap admin frames -> fade-in start)
//   from flashing bright between the two ramps.
#pragma once
#include <cstdint>
class Core;

class ScreenFade {
public:
  enum Mode : uint8_t {
    NONE        = 0,   // no fade this frame -> scene renders normally
    ADDITIVE    = 1,   // dst += (r,g,b) clamped -> fade to/from white
    SUBTRACTIVE = 2,   // dst -= (r,g,b) clamped -> fade to/from black
  };

  struct State { Mode mode; uint8_t r, g, b; };

  // "Fully faded" threshold. When the last fade rect the game drew was at or above this in every
  // channel (subtractive => screen was fully black; additive => fully white), the class latches a
  // HOLD. On subsequent frames where no caller sets a fade (mode NONE), the held state is what the
  // renderer sees, so scene content that's freshly loaded during those admin frames doesn't leak
  // through as bright flashes between fade-out and the next fade-in. The hold releases as soon as
  // any caller sets a fade below this threshold in ANY channel — i.e. the game has started ramping
  // back toward "scene visible", so we're leaving the fully-faded span.
  static constexpr uint8_t FULLY_FADED_THRESHOLD = 0xE0;

  // Guest-memory layout at ENGINE_OWNED_BSS 0x800E7DE0 (8 bytes):
  //   +0..+3  frame-scoped fade   {mode, r, g, b}       reset by frameStart, set by set/applyLeafCall
  //   +4..+7  held fully-faded    {mode, r, g, b}       persists across frames; latched/released by set
  static constexpr uint32_t GUEST_ADDR = 0x800E7DE0u;

  // Back-pointer set once by Game's constructor (same pattern as GpuGpuState::game etc.).
  // Not owned; ScreenFade never outlives its Core.
  Core* core = nullptr;

  // Called ONCE at the top of each logic frame. Resets the FRAME-SCOPED state to NONE. Does NOT
  // touch the held fully-faded state — that persists across admin frames.
  void frameStart();

  // Set the fade for THIS FRAME. Called by NATIVE fade callers (SMs owned in game/scene/*.cpp).
  // Still-recomp SMs that call the guest FUN_8007e9c8 leaf do NOT reach this class — porting them
  // native is the top-down task that closes the coverage gap. Last write wins in the same frame.
  // If (mode, r, g, b) is at or above FULLY_FADED_THRESHOLD in every channel the HOLD is latched;
  // if the fade is below the threshold in any channel the hold is released (fade span has left the
  // fully-faded portion).
  void set(Mode mode, uint8_t r, uint8_t g, uint8_t b);

  // FUN_8007e9c8 caller-shape convenience: `color` packed as 0x00RRGGBB in the a0 register, `a1`
  // selects blend (a1!=0 => ADDITIVE / white, a1==0 => SUBTRACTIVE / black).
  void applyLeafCall(uint32_t color, uint32_t a1);

  // Read this frame's effective state (native renderer's present prologue). Returns the frame-scoped
  // state if a caller set it this frame; otherwise returns the held fully-faded state (if latched),
  // otherwise NONE.
  State get() const;
};
