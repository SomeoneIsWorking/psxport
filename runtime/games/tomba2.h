// Tomba! 2 (SCUS-94454) game module: per-game hooks, overrides and pokes.
#pragma once

#include <cstdint>

/// Registers PC hooks/overrides. Call once after the game is loaded.
void Tomba2_Install();

/// Called once per frame with main RAM; returns pad-1 button bits
/// (RETRO_DEVICE_ID_JOYPAD_* mask) to inject this frame.
uint16_t Tomba2_FrameTick(uint8_t* ram);

/// True while the game is in a load-mask segment (BIOS/logos) that should be
/// fast-forwarded automatically in play mode.
bool Tomba2_WantTurbo(const uint8_t* ram, unsigned frame);

/// Debug heartbeat: render-dispatch hook hits since the last call.
uint32_t Tomba2_GetAndResetRenderHits();
