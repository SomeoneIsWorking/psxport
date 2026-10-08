#pragma once
#include <SDL3/SDL_gpu.h>

#include <lucent/log.h>

// ---- "May the sink WAIT for the window?" ------------------------------------------------------------
//
// No. gpu_vk_present_mode.h closed the first form of this: a VSYNC sink stalls the caller ~16.7 ms per
// field, and the caller is the guest thread — the CD pump, MDEC, DMA completion and the guest itself all
// run there. A blocking ACQUIRE is the second form, and it is worse, because it depends on the
// compositor rather than on the driver: a window nobody is currently showing gets no frame callbacks, so
// waiting for one never returns.
//
// MEASURED 2026-09-14 on a windowed Wayland sink whose present mode was MAILBOX, so the VSYNC leg was
// NOT in play: ~1036 presents into the run the guest thread parked at `show_present_image+0xd9` — the
// SDL_WaitAndAcquireGPUSwapchainTexture call — and never came back. With no present for three seconds
// the frame watchdog reported STUCK and the process aborted (SIGABRT, exit 134) while still on the title
// screen, i.e. before the game could be played. A full-screen terminal in front of the game window
// reproduces it every time.
//
// SDL's own contract supplies the fix: SDL_AcquireGPUSwapchainTexture fills the handle with NULL when no
// image is ready and calls that "not an error". So an unavailable image is an IDLE SINK — skip the blit
// and keep executing the guest — which is the branch the minimized-window case already took.
//
// The accounting is a latch over a per-field event, not a per-field log: the sink is called once per
// field, so sixty identical lines a second are noise, while the TRANSITION (idle entered / idle left) is
// the signal, and it carries the counts on both sides. `observe` is pure, so a test drives both
// directions with no GPU, no window and no disc.
struct SinkIdleState {
  long skipped = 0;   // fields whose sink was skipped: no swapchain image was available
  long presented = 0; // fields that reached the window
  bool idle = false;  // true while skipping, so only crossings are reported

  // Record one sink attempt. Returns true when the idle state CHANGED (the caller announces it).
  bool observe(bool image_available) {
    if (image_available) {
      presented++;
      if (!idle) {
        return false;
      }
      idle = false;
      return true;
    }
    skipped++;
    if (idle) {
      return false;
    }
    idle = true;
    return true;
  }
};

// Acquire the swapchain image WITHOUT waiting for the window, accounting an idle field. Returns NULL when
// no image is ready, and the caller skips the sink for this field. `w`/`h` are SDL's own answer and are
// left untouched on failure.
inline SDL_GPUTexture *
sink_acquire(SinkIdleState &s, SDL_GPUCommandBuffer *cmd, SDL_Window *win, Uint32 *w, Uint32 *h) {
  SDL_GPUTexture *tex = nullptr;
  const bool available = SDL_AcquireGPUSwapchainTexture(cmd, win, &tex, w, h) && tex != nullptr;
  if (s.observe(available)) {
    if (available) {
      lucent::info("sinkidle", "sink resumed (skipped {} field(s), presented {})", s.skipped, s.presented);
    } else {
      // The message is printed at the transition only, so it can afford SDL's own words; this branch is
      // also reached for "too many frames in flight", which SDL likewise calls a normal state.
      lucent::info("sinkidle",
                   "sink idle — no swapchain image (skipped {}, presented {}) — the guest keeps running; SDL: {}",
                   s.skipped,
                   s.presented,
                   SDL_GetError());
    }
  }
  return available ? tex : nullptr;
}
