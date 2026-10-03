#pragma once
#include <SDL3/SDL_gpu.h>

// ---- "Which swapchain present mode may the SINK use?" -----------------------------------------------
//
// The sink is the ONE place a windowed run and a headless run are allowed to differ (headless reads the
// composite back, windowed hands it to a swapchain). Everything before it is one code path. This header
// exists because that difference had a property nobody chose: a BLOCKING sink.
//
// psxport never called SDL_SetGPUSwapchainParameters, so a claimed window kept SDL's DEFAULT present
// mode, VSYNC. Under VSYNC, SDL_WaitAndAcquireGPUSwapchainTexture blocks the CALLING thread until the
// next vblank — and the caller is GpuVkState::show_present_image, reached from vblank_advance -> gpu_present
// on the GUEST thread. There is no I/O thread: the CD pump, MDEC, DMA completion and the guest itself
// all run there. So the sink was spending ~16.7 ms of every guest field asleep inside an ioctl, and the
// guest got almost no CPU. Headless never blocked (null window, acquire fails instantly), which is why
// headless looked healthy while the window showed black. MEASURED, one build, one variable, ~4100
// presents: headless vram_writes=12812 / rebuild_geom=1511; windowed vram_writes=0 / rebuild_geom=0.
//
// A sink that stalls the thread that produces the frames is not an equivalent sink. So the sink asks for
// a NON-BLOCKING present mode and the preference order is:
//
//   MAILBOX    the driver keeps the newest finished image and discards older ones. Acquire returns
//              without waiting for a vblank, and the display still never tears. First choice.
//   IMMEDIATE  no queue, no wait; the image is scanned out when it arrives, so it can tear. Still
//              strictly better than stalling the only thread the game runs on.
//   VSYNC      always supported (the SDL/Vulkan spec guarantees FIFO), and always blocking. This is
//              the honest fallback, NOT a target: reaching it means the driver offers nothing else,
//              and the caller is expected to say so in the log rather than hide it.
//
// Pure function of two booleans so it is testable with no GPU, no window and no disc — the caller feeds
// it SDL_WindowSupportsGPUPresentMode's answers. It deliberately does NOT take the window or the device:
// the POLICY is what is worth pinning down, and a policy that needs a live swapchain to check is a
// policy nobody checks.
static inline SDL_GPUPresentMode preferred_present_mode(bool mailbox_ok, bool immediate_ok) {
  if (mailbox_ok) {
    return SDL_GPU_PRESENTMODE_MAILBOX;
  }
  if (immediate_ok) {
    return SDL_GPU_PRESENTMODE_IMMEDIATE;
  }
  return SDL_GPU_PRESENTMODE_VSYNC;
}

// Does this mode stall the caller until a vblank? The one property the guest thread cares about, named
// so a log line (and the test) can talk about it without re-deriving the enum's semantics.
static inline bool present_mode_blocks_caller(SDL_GPUPresentMode m) {
  return m == SDL_GPU_PRESENTMODE_VSYNC;
}

// Name for the log. Never returns null — a null const char* is undefined behaviour for std::format,
// and this feeds a lucent::info on the boot path.
static inline const char *present_mode_name(SDL_GPUPresentMode m) {
  switch (m) {
  case SDL_GPU_PRESENTMODE_VSYNC:
    return "VSYNC";
  case SDL_GPU_PRESENTMODE_IMMEDIATE:
    return "IMMEDIATE";
  case SDL_GPU_PRESENTMODE_MAILBOX:
    return "MAILBOX";
  }
  return "(unknown)";
}
