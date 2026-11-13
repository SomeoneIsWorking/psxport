// device_gpu.cpp — the GPU section: the GPU device's own Beetle state stream and scanline clock, the VRAM and registers
// the guest observes, the host packets of tables built but not yet walked, the record tap's record in progress, the
// record canvases' windows and the presenter's records, which a load draws again at scale so the first presents show
// what the saving run's would have. The present counters are run-local and not captured.
#include "device_sections.h"

#include "core.h"
#include "frame_presenter.h"
#include "game.h"
#include "gpu_device.h"
#include "render_state.h"
#include "vram_rect.h"

#include <optional>
#include <vector>

namespace psx::state {
namespace {

constexpr std::uint8_t kGpuLayoutVersion = 9;

void writeMargins(psx::platform::BlobWriter &out, const present::HorizontalMargins &margins) {
  out.i32(margins.left);
  out.i32(margins.right);
}

present::HorizontalMargins readMargins(psx::platform::BlobReader &in) {
  const int left = in.i32();
  const int right = in.i32();
  return {left, right};
}

void writeCanvas(psx::platform::BlobWriter &out, const render::CanvasWindows &canvas) {
  out.i32(canvas.buffer.x0);
  out.i32(canvas.buffer.y0);
  out.i32(canvas.buffer.x1);
  out.i32(canvas.buffer.y1);
  writeMargins(out, canvas.view);
  writeMargins(out, canvas.drawn);
}

bool readCanvas(psx::platform::BlobReader &in, render::CanvasWindows &canvas, std::string &error) {
  canvas.buffer.x0 = in.i32();
  canvas.buffer.y0 = in.i32();
  canvas.buffer.x1 = in.i32();
  canvas.buffer.y1 = in.i32();
  canvas.view = readMargins(in);
  canvas.drawn = readMargins(in);
  const gpu::RecordRect &buffer = canvas.buffer;
  if (!in.ok() || buffer.x0 < 0 || buffer.y0 < 0 || buffer.x1 <= buffer.x0 || buffer.y1 <= buffer.y0 ||
      buffer.x1 > gpu::kVramWidth || buffer.y1 > gpu::kVramHeight || canvas.drawn.left < 0 || canvas.drawn.right < 0) {
    error = "gpu section: a record canvas's buffer or windows do not decode";
    return false;
  }
  return true;
}

} // namespace

bool writeGpuSection(Game &game, psx::platform::BlobWriter &out, std::string &) {
  out.u8(kGpuLayoutVersion);
  const std::vector<std::uint8_t> device = game.core.gpuDevice().saveState();
  out.blob(device);
  const std::optional<std::uint64_t> clock = game.core.gpuDevice().clockTicks();
  out.boolean(clock.has_value());
  out.u64(clock.value_or(0));
  game.core.hostPackets.save(out);
  game.core.gpuDevice().saveRecord(out);
  const std::vector<render::CanvasWindows> canvases = game.render().canvasWindows();
  out.u8(static_cast<std::uint8_t>(canvases.size()));
  for (const render::CanvasWindows &canvas : canvases) {
    writeCanvas(out, canvas);
  }
  game.presentation.save(out);
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
  const bool clocked = in.boolean();
  const std::uint64_t clockTicks = in.u64();
  if (!in.ok()) {
    return in.decoded("gpu", error);
  }
  if (!game.core.gpuDevice().loadState(device, clocked ? std::optional<std::uint64_t>(clockTicks) : std::nullopt)) {
    error = "the GPU device did not accept its " + std::to_string(device.size()) + "-byte state stream";
    return false;
  }
  if (!game.core.hostPackets.load(in, error)) {
    return false;
  }
  gpu::TapResume tap;
  if (!gpu::Gp0RecordTap::read(in, tap, error)) {
    return false;
  }
  game.core.gpuDevice().resumeRecord(std::move(tap));
  std::vector<render::CanvasWindows> canvases(in.u8());
  for (render::CanvasWindows &canvas : canvases) {
    if (!readCanvas(in, canvas, error)) {
      return false;
    }
  }
  if (!game.presentation.load(in, error)) {
    return false;
  }
  if (!in.ok()) {
    return in.decoded("gpu", error);
  }
  std::vector<present::FrameRecord> records;
  for (const present::FrameRecord *record : game.presentation.heldRecords()) {
    records.push_back(*record);
  }
  game.render().restoreRecords(std::move(canvases), std::move(records));
  return true;
}

} // namespace psx::state
