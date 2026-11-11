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
  std::uint64_t pixels = 0;     // covered by the draws' quads at the internal resolution, once per draw
  std::uint64_t copyPixels = 0; // texture copies at the internal resolution
  std::uint64_t uploadBytes = 0;
};

} // namespace psx::render
