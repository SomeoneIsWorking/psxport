// device_gpu.cpp — the GPU section: the GPU device's own Beetle state stream, the VRAM and registers
// the guest observes. The present counters are run-local and not captured.
#include "device_sections.h"

#include "game.h"
#include "gpu_device.h"

#include <vector>

namespace psx::state {
namespace {

constexpr std::uint8_t kGpuLayoutVersion = 3;

} // namespace

void writeGpuSection(Game &game, BlobWriter &out) {
  out.u8(kGpuLayoutVersion);
  const std::vector<std::uint8_t> device = game.core.gpuDevice().saveState();
  out.blob(device);
}

bool readGpuSection(Game &game, BlobReader &in, std::string &error) {
  const std::uint8_t layout = in.u8();
  if (layout != kGpuLayoutVersion) {
    error = "gpu section layout " + std::to_string(layout) + ", this build writes " + std::to_string(kGpuLayoutVersion);
    return false;
  }
  std::vector<std::uint8_t> device;
  in.blob(device);
  if (!game.core.gpuDevice().loadState(device)) {
    error = "the GPU device did not accept its " + std::to_string(device.size()) + "-byte state stream";
    return false;
  }
  return true;
}

} // namespace psx::state
