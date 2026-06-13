// Tomba! 2 (SCUS-94454) game module: per-game hooks, overrides and pokes.
#pragma once

#include <cstdint>

/// Registers PC hooks/overrides. Call once after the game is loaded.
void Tomba2_Install();

/// Called once per frame with main RAM; returns pad-1 button bits
/// (RETRO_DEVICE_ID_JOYPAD_* mask) to inject this frame.
uint16_t Tomba2_FrameTick(uint8_t* ram);

/// True while the game is in a load-mask segment (BIOS/logos) AND the user is
/// holding the skip button (skip_held). The logos are load masks, not skippable
/// segments (verified: input can't jump past the load) — so "skip" means
/// fast-forward through them on demand, never automatically.
bool Tomba2_WantTurbo(const uint8_t* ram, unsigned frame, bool skip_held);

/// Debug heartbeat: render-dispatch hook hits since the last call.
uint32_t Tomba2_GetAndResetRenderHits();
