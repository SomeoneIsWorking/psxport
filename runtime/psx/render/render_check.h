// render_check.h — a null SDL_GPU handle the run depends on ends the run.
#pragma once
#include <SDL3/SDL.h>
#include <lucent/log.h>

#include <cstdlib>

#define GPUCHK(p, what)                                                                                                \
  do {                                                                                                                 \
    if (!(p)) {                                                                                                        \
      lucent::error("render", "{} failed: {}", what, SDL_GetError());                                                  \
      std::exit(2);                                                                                                    \
    }                                                                                                                  \
  } while (0)
