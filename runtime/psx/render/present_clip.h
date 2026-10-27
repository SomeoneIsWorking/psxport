// present_clip.h — PresentClip: the last second of a Game's window presents, kept downscaled on the GPU for a
// bug report's motion clip. Fixed size, allocated once; recording a present is one GPU blit.
#pragma once
#include "pixel_size.h"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <chrono>
#include <cstdint>

namespace psx::render {

class RenderDevice;
using present::PixelSize;

class PresentClip {
public:
  // About one second of presents at 60 Hz, at 960x540.
  inline static constexpr int kFrames = 60;
  inline static constexpr PixelSize kSize{960, 540};

  explicit PresentClip(RenderDevice &device) : device_(device) {}
  ~PresentClip();
  PresentClip(const PresentClip &) = delete;
  PresentClip &operator=(const PresentClip &) = delete;

  // `image` (`size`) as the newest frame, letterboxed into the clip; recorded into `cmd`.
  void record(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *image, PixelSize size);
  // The held frames oldest first as an animated PNG at `path`; false when none are held or the write failed.
  bool write(const char *path);

private:
  RenderDevice &device_;
  SDL_GPUTexture *frames_ = nullptr; // a 2D array, one layer per frame
  int next_ = 0;
  int held_ = 0;
  std::array<std::chrono::steady_clock::time_point, kFrames> times_{};
};

} // namespace psx::render
