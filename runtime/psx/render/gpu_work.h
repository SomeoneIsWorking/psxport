// gpu_work.h — the GPU work a present's record rasterization submitted, counted on the CPU as it is recorded.
// Every count follows from the record, the internal resolution and the canvas, never from the host.
#pragma once

#include <cstdint>

namespace psx::render {

struct GpuWork {
  std::uint64_t draws = 0;
  std::uint64_t pipelineBinds = 0;
  std::uint64_t renderPasses = 0;
  std::uint64_t textureCopies = 0;
  std::uint64_t triangles = 0;
  std::uint64_t pixels = 0; // covered by the draws' shapes at the internal resolution, once per draw
  // The same through the full texel, blend and mask shader: every draw but a depth or Model mark.
  std::uint64_t shadedPixels = 0;
  std::uint64_t copyPixels = 0; // texture copies at the internal resolution
  std::uint64_t uploadBytes = 0;
};

// Diagnostic `gputime`: the GPU time one submit label was waited for, measured on the host, unlike GpuWork.
struct SubmitTime {
  const char *where = nullptr;
  std::uint64_t ns = 0;
  std::uint64_t submits = 0;
};

} // namespace psx::render
