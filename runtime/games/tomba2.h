// Tomba! 2 (SCUS-94454) game module: per-game hooks, overrides and pokes.
#pragma once

#include <cstdint>

/// Registers PC hooks/overrides. Call once after the game is loaded.
void Tomba2_Install();

/// Called once per frame with main RAM; returns pad-1 button bits
/// (RETRO_DEVICE_ID_JOYPAD_* mask) to inject this frame.
uint16_t Tomba2_FrameTick(uint8_t* ram);

/// Latch the host "skip intro" input (Start) for this frame so the PC-native
/// intro-skip overrides can read it from inside the interpreter. Holding Start
/// natively skips the SCEA + Whoopee Camp logos via each logo's own advance
/// path (runtime/games/tomba2.cpp; RE: docs/tomba2-intro.md).
void Tomba2_SetSkipHeld(bool held);

/// Debug heartbeat: render-dispatch hook hits since the last call.
uint32_t Tomba2_GetAndResetRenderHits();
