// present_mode.h — the swapchain present mode the sink asks for.
#pragma once
#include <SDL3/SDL_gpu.h>

namespace psx::render {

// MAILBOX, then IMMEDIATE: a blocking present stalls the guest thread.
inline SDL_GPUPresentMode preferredPresentMode(bool mailboxOk, bool immediateOk) {
  if (mailboxOk) {
    return SDL_GPU_PRESENTMODE_MAILBOX;
  }
  if (immediateOk) {
    return SDL_GPU_PRESENTMODE_IMMEDIATE;
  }
  return SDL_GPU_PRESENTMODE_VSYNC;
}

inline bool presentModeBlocksCaller(SDL_GPUPresentMode mode) {
  return mode == SDL_GPU_PRESENTMODE_VSYNC;
}

inline const char *presentModeName(SDL_GPUPresentMode mode) {
  switch (mode) {
  case SDL_GPU_PRESENTMODE_VSYNC:
    return "VSYNC";
  case SDL_GPU_PRESENTMODE_IMMEDIATE:
    return "IMMEDIATE";
  case SDL_GPU_PRESENTMODE_MAILBOX:
    return "MAILBOX";
  }
  return "(unknown)";
}

} // namespace psx::render
