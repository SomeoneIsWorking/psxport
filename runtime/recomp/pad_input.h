// pad_input.h — class Pad — native controller input subsystem, owned by Game (c->game->pad).
// Carries the current host button state + REPL drive control + all the pad_* behavior (host poll,
// per-VBlank fill buffer, REPL hold/tap/release). Implemented in pad_input.cpp. (Test-hook /
// config-cache statics inside the SDL poll path stay shared per the plan policy — those are
// host-wide, not per-Core.)
#pragma once
#include <cstdint>
class Game;

class Pad {
public:
  Game* game = nullptr;
  uint16_t buttons    = 0xFFFF;  // current host button state, active-low (0 bit = pressed) (was s_buttons)
  uint16_t repl_hold  = 0xFFFF;  // REPL: bits cleared = held down (was s_repl_hold)
  uint16_t repl_tap   = 0xFFFF;  // REPL: active-low mask pressed for repl_tap_n frames (was s_repl_tap)
  int      repl_tap_n = 0;       // REPL: tap countdown frames (was s_repl_tap_n)
  int      repl_on    = 0;       // REPL drive active (was s_repl_on)

  void init();                              // was pad_init(Core*)
  void setButtons(uint16_t mask);           // was pad_set_buttons(Core*, mask) — feed the active-low mask
  void fillBuffer(uint8_t* buf);            // was pad_fill_buffer(Core*, buf) — per-VBlank guest read pad
  void pollSdl();                           // was pad_poll_sdl(Core*) — host SDL controller poll
  void overridesInit();                     // was pad_overrides_init(Core*) — install per-VBlank pad-read override
  void driveHold(uint16_t activeLowMask);   // was pad_repl_hold(c, mask) — REPL: hold down these bits
  void driveTap(uint16_t activeLowMask, int nframes);  // was pad_repl_tap(c, mask, n) — press for n frames
  void driveRelease();                      // was pad_repl_release(c) — clear REPL drive
  void serviceFrame();                      // was pad_service_frame(c) — per-frame native pad service
};
