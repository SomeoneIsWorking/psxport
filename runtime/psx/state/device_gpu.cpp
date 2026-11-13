// device_gpu.cpp — the GPU section: the GPU device's own Beetle state stream, the VRAM and registers
// the guest observes, the host packets of tables built but not yet walked, and the record canvases'
// margins, which hold the picture beside the displayed buffers that VRAM does not. The present
// counters are run-local and not captured.
#include "device_sections.h"

#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "render_state.h"
#include "vram_rect.h"

#include <optional>
#include <vector>

namespace psx::state {
namespace {

constexpr std::uint8_t kGpuLayoutVersion = 6;

void writeMargins(psx::platform::BlobWriter &out, const present::HorizontalMargins &margins) {
  out.i32(margins.left);
  out.i32(margins.right);
}

present::HorizontalMargins readMargins(psx::platform::BlobReader &in) {
  const int left = in.i32();
  const int right = in.i32();
  return {left, right};
}

void writeCanvas(psx::platform::BlobWriter &out, const render::CanvasMargins &canvas) {
  out.i32(canvas.buffer.x0);
  out.i32(canvas.buffer.y0);
  out.i32(canvas.buffer.x1);
  out.i32(canvas.buffer.y1);
  writeMargins(out, canvas.view);
  writeMargins(out, canvas.drawn);
  out.array(canvas.left.data(), canvas.left.size());
  out.array(canvas.right.data(), canvas.right.size());
}

bool readColumns(psx::platform::BlobReader &in, int columns, int rows, std::vector<std::uint16_t> &pixels) {
  pixels.resize(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
  return in.array(pixels.data(), pixels.size());
}

bool readCanvas(psx::platform::BlobReader &in, render::CanvasMargins &canvas, std::string &error) {
  canvas.buffer.x0 = in.i32();
  canvas.buffer.y0 = in.i32();
  canvas.buffer.x1 = in.i32();
  canvas.buffer.y1 = in.i32();
  canvas.view = readMargins(in);
  canvas.drawn = readMargins(in);
  const int rows = canvas.buffer.y1 - canvas.buffer.y0;
  if (!in.ok() || rows <= 0 || rows > gpu::kVramHeight || canvas.drawn.left < 0 || canvas.drawn.right < 0 ||
      canvas.drawn.left > gpu::kVramWidth || canvas.drawn.right > gpu::kVramWidth) {
    error = "gpu section: a record canvas's buffer or drawn margins do not decode";
    return false;
  }
  if (!readColumns(in, canvas.drawn.left, rows, canvas.left) ||
      !readColumns(in, canvas.drawn.right, rows, canvas.right)) {
    error = "gpu section: a record canvas's margin pixels are not its drawn columns by its rows";
    return false;
  }
  return true;
}

} // namespace

bool writeGpuSection(Game &game, psx::platform::BlobWriter &out, std::string &error) {
  const std::optional<std::vector<render::CanvasMargins>> canvases = game.render().canvasMargins();
  if (!canvases) {
    error = "the record canvases' margins could not be downloaded from the GPU";
    return false;
  }
  out.u8(kGpuLayoutVersion);
  const std::vector<std::uint8_t> device = game.core.gpuDevice().saveState();
  out.blob(device);
  game.core.hostPackets.save(out);
  out.u8(static_cast<std::uint8_t>(canvases->size()));
  for (const render::CanvasMargins &canvas : *canvases) {
    writeCanvas(out, canvas);
  }
  return true;
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
  if (!game.core.hostPackets.load(in, error)) {
    return false;
  }
  std::vector<render::CanvasMargins> canvases(in.u8());
  for (render::CanvasMargins &canvas : canvases) {
    if (!readCanvas(in, canvas, error)) {
      return false;
    }
  }
  if (!in.ok()) {
    return in.decoded("gpu", error);
  }
  game.render().restoreCanvasMargins(std::move(canvases));
  return true;
}

} // namespace psx::state
