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

void writeGpuSection(Game &game, psx::platform::BlobWriter &out) {
  out.u8(kGpuLayoutVersion);
  const std::vector<std::uint8_t> device = game.core.gpuDevice().saveState();
  out.blob(device);
}

bool readGpuSection(Game &game, psx::platform::BlobReader &in, std::string &error) {
  if (!in.expectLayout("gpu", kGpuLayoutVersion, error)) {
    return false;
  }
  std::vector<std::uint8_t> device;
  if (!in.blob(device)) {
    return in.decoded("gpu", error);
  }
  if (!game.core.gpuDevice().loadState(device)) {
    error = "the GPU device did not accept its " + std::to_string(device.size()) + "-byte state stream";
    return false;
  }
  return true;
}

} // namespace psx::state
