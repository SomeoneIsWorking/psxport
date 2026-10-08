// gpu_vk_check.h — the one rule for an SDL call in the SDL_GPU backend that must not come back false.
//
// A checked handle is a HANDLE THE RUN DEPENDS ON: the window, the device, a sampler, a pipeline. SDL
// returning null there means the presentation surface is not the one the renderer was built for, and
// every frame after it would be black or undefined. So the check states what failed and ends the run
// rather than letting a null propagate into a draw call.
//
// The message is a log line, never a raw print: the backend's diagnostics all go through Lucent.
#pragma once
#include <SDL3/SDL.h>
#include <lucent/log.h>
#include <stdlib.h>

#define GPUCHK(p, what)                                                                                                \
  do {                                                                                                                 \
    if (!(p)) {                                                                                                        \
      lucent::error("gpu_vk", "{} failed: {}", what, SDL_GetError());                                                  \
      exit(2);                                                                                                         \
    }                                                                                                                  \
  } while (0)