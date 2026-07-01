// class ScreenFade — implementation. See screen_fade.h for the architecture note.
#include "screen_fade.h"
#include "core.h"

void ScreenFade::frameStart() {
  // Reset only the frame-scoped state. The held fully-faded state (+4..+7) persists across frames.
  core->mem_w8(GUEST_ADDR + 0, (uint8_t)NONE);
  core->mem_w8(GUEST_ADDR + 1, 0);
  core->mem_w8(GUEST_ADDR + 2, 0);
  core->mem_w8(GUEST_ADDR + 3, 0);
}

void ScreenFade::set(Mode mode, uint8_t r, uint8_t g, uint8_t b) {
  // Frame-scoped state.
  core->mem_w8(GUEST_ADDR + 0, (uint8_t)mode);
  core->mem_w8(GUEST_ADDR + 1, r);
  core->mem_w8(GUEST_ADDR + 2, g);
  core->mem_w8(GUEST_ADDR + 3, b);
  // Held fully-faded latch. Latched only when the fade is at or above the threshold in every channel
  // (screen is essentially fully black for SUBTRACTIVE or fully white for ADDITIVE). Any set() below
  // the threshold in any channel — i.e. the game has started ramping back toward "scene visible" —
  // releases the hold.
  if (mode != NONE && r >= FULLY_FADED_THRESHOLD && g >= FULLY_FADED_THRESHOLD && b >= FULLY_FADED_THRESHOLD) {
    core->mem_w8(GUEST_ADDR + 4, (uint8_t)mode);
    core->mem_w8(GUEST_ADDR + 5, r);
    core->mem_w8(GUEST_ADDR + 6, g);
    core->mem_w8(GUEST_ADDR + 7, b);
  } else {
    core->mem_w8(GUEST_ADDR + 4, (uint8_t)NONE);
    core->mem_w8(GUEST_ADDR + 5, 0);
    core->mem_w8(GUEST_ADDR + 6, 0);
    core->mem_w8(GUEST_ADDR + 7, 0);
  }
}

void ScreenFade::applyLeafCall(uint32_t color, uint32_t a1) {
  // Guest ABI: color is 0x00RRGGBB in a0. a1 selects blend: !=0 additive, ==0 subtractive.
  set((a1 != 0u) ? ADDITIVE : SUBTRACTIVE,
      (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color);
}

ScreenFade::State ScreenFade::get() const {
  Mode frame_mode = (Mode)core->mem_r8(GUEST_ADDR + 0);
  if (frame_mode != NONE) {
    return State{
      frame_mode,
      core->mem_r8(GUEST_ADDR + 1),
      core->mem_r8(GUEST_ADDR + 2),
      core->mem_r8(GUEST_ADDR + 3),
    };
  }
  // No caller set fade this frame — return the held fully-faded state if any.
  return State{
    (Mode)core->mem_r8(GUEST_ADDR + 4),
    core->mem_r8(GUEST_ADDR + 5),
    core->mem_r8(GUEST_ADDR + 6),
    core->mem_r8(GUEST_ADDR + 7),
  };
}
