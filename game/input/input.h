// game/input/input.h — PC-native per-frame INPUT/controller subsystem.
// The per-frame input / controller-state processor (FUN_800931C0) — extracted from game_tomba2.cpp into
// its own module (PC-game code structure). This module also owns the one-shot voice/channel-table init
// FUN_80093650 (SPU-voice + controller-channel allocation tables at 0x80105xxx), installed by address
// into the ONE override registry via Input::registerOverrides().
#ifndef ENGINE_INPUT_H
#define ENGINE_INPUT_H
#include <cstdint>
struct Core;
class Game;

// class Input — stateless owner of the input/voice-table primitives (methods over Core*). Registered
// by guest address into the override registry; no per-Core instance state, so the entry points are
// static and reached through the eov_* guest-ABI thunks in input.cpp.
class Input {
public:
  // FUN_80093650 — one-shot voice/channel-table init. Zeroes the SPU-voice allocation table and the
  // controller-channel state block at base 0x80100000 + ~21688..24000, clamps the active-voice count
  // (arg0, r4, an int8 cap) to <=24, then per-voice seeds the per-voice record (default gain 255,
  // pan/env fields) and calls the SPU key-off / channel-reset helpers (func_80099970/func_80094B50).
  // ready-FRAME: mirrors the gen 112-byte stack frame (spills r16..r21,r31, marshals a per-voice
  // struct at sp+16 for func_80099970). Byte-faithful to gen_func_80093650 (tools/port_check.py gate).
  static void voiceTableInit(Core* c);   // guest ABI: voice-count cap (int8) in r4

  // registerOverrides(): install voiceTableInit by guest address into the ONE override registry.
  static void registerOverrides(Game* game);
};
#endif
