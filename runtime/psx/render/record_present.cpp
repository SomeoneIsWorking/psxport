// record_present.cpp — RenderPath::Record's picture: the current frame record through RecordRasterizer.
#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "guest_widescreen_projection.h"
#include "image_writer.h"
#include "present_plan.h"
#include "record_geometry.h"
#include "render.h"
#include "render_device.h"
#include "render_state.h"
#include "vram_pixel.h"

#include <lucent/log.h>

#include <vector>

namespace psx::render {

void RenderState::presentRecord(present::PresentInputs &inputs) {
  presentsRecord_ = false;
  Core &core = game_.core;
  const gpu::DisplayArea area = core.gpuDevice().displayArea();
  // 24bpp scanout reads VRAM bytes, which the scaled image does not hold; the device picture stands.
  if (core.rsub.mode.path() != present::RenderPath::Record || area.depth24) {
    return;
  }
  const int scale = videoStatus(core).ires;
  if (!record_) {
    record_ = std::make_unique<RecordRasterizer>(device_);
  }
  const gpu::RecordRect display = gpu::clampedVramRect({inputs.sx, inputs.sy, inputs.native_w, inputs.disp_h});
  const present::GuestProjectionPlan plan =
      present::latchRecordDisplay(core, {display.x1 - display.x0, display.y1 - display.y0});
  const RecordView view{display, plan.presentationHorizontalMargin};
  const present::FrameRecord &record = game_.presentation.currentRecord();
  const present::FrameRecord *composed = game_.presentation.composedRecord();
  const bool advances = game_.presentation.composedAdvances();
  const bool deviceAhead = core.gpuDevice().hasUnsealedWork();
  SDL_GPUCommandBuffer *cmd = device_.acquire();
  presentsRecord_ = true;
  recordFade_[0] = inputs.fade_mode;
  recordFade_[1] = inputs.fade_r;
  recordFade_[2] = inputs.fade_g;
  recordFade_[3] = inputs.fade_b;
  const bool drewComposed =
      record_->show(cmd, record, composed, advances, deviceAhead, core.gpuDevice().vram(), scale, view);
  // The display, or its wide canvas, at the 4:3 display's aspect per column.
  const gpu::RecordRect shown = record_->presented().rect;
  inputs.sx = shown.x0;
  inputs.sy = shown.y0;
  inputs.disp_w = shown.x1 - shown.x0;
  inputs.disp_h = shown.y1 - shown.y0;
  inputs.content_w = 0;
  inputs.present_ires = scale > 1 ? scale : 0;
  lucent::debug("recordcheck",
                "composed seq={} over current seq={} final={} drawn={}",
                composed != nullptr ? composed->sequence() : 0,
                record.sequence(),
                advances ? 1 : 0,
                drewComposed ? 1 : 0);
  device_.submit(cmd, drewComposed ? "record composed" : "record");
  // An in-between has no device picture to match; the t = 1 composed frame at 4:3 must match it.
  if ((drewComposed && (!advances || view.margin > 0)) || !lucent::channel_on("recordcheck")) {
    return;
  }
  checkRecord(record, drewComposed, deviceAhead, scale);
}

// The record image against the device's VRAM over the display. The host picture is not dithered and the
// device's is, so dithered draws mismatch by design.
void RenderState::checkRecord(const present::FrameRecord &record, bool drewComposed, bool deviceAhead, int scale) {
  const gpu::DisplayArea area = game_.core.gpuDevice().displayArea();
  const gpu::RecordRect rect = gpu::clampedVramRect({area.x, area.y, area.width, area.height});
  const MappedTransfer image = record_->download({drewComposed ? record_->woven().texture : record_->image(), rect});
  if (!image) {
    return;
  }
  const auto *ours = reinterpret_cast<const std::uint16_t *>(image.bytes());
  const std::span<const std::uint16_t> vram = game_.core.gpuDevice().vram();
  const int width = rect.x1 - rect.x0;
  long mismatched = 0;
  int firstX = -1;
  int firstY = -1;
  std::uint16_t firstImage = 0;
  std::uint16_t firstDevice = 0;
  for (int row = 0; row < rect.y1 - rect.y0; row++) {
    for (int column = 0; column < width; column++) {
      const std::uint16_t mine = ours[static_cast<std::size_t>(row * scale) * static_cast<std::size_t>(width * scale) +
                                      static_cast<std::size_t>(column * scale)];
      const std::uint16_t device =
          vram[static_cast<std::size_t>(rect.y0 + row) * gpu::kVramWidth + static_cast<std::size_t>(rect.x0 + column)];
      if (mine != device && mismatched++ == 0) {
        firstX = rect.x0 + column;
        firstY = rect.y0 + row;
        firstImage = mine;
        firstDevice = device;
      }
    }
  }
  lucent::debug("recordcheck",
                "seq={} composed={} complete={} entries={} ahead={} replayed={} resynced={} scale={} undrawn={} "
                "display {}x{}@{},{} mismatched={} first=({},{}) record={:#06x} device={:#06x}",
                record.sequence(),
                drewComposed ? 1 : 0,
                record.complete() ? 1 : 0,
                record.entries().size(),
                deviceAhead ? 1 : 0,
                record_->replayed(),
                record_->resynced(),
                scale,
                record.undrawnRowParity().value_or(-1),
                width,
                rect.y1 - rect.y0,
                rect.x0,
                rect.y0,
                mismatched,
                firstX,
                firstY,
                firstImage,
                firstDevice);
}

bool RenderState::recordShot(const char *path) {
  const RecordPicture picture = record_ ? record_->presented() : RecordPicture{};
  if (picture.texture == nullptr) {
    lucent::error("shot", "no record picture has been presented, nothing written to {}", path);
    return false;
  }
  const MappedTransfer image = record_->download(picture);
  if (!image) {
    return false;
  }
  const int scale = record_->scale();
  const int width = (picture.rect.x1 - picture.rect.x0) * scale;
  const int height = (picture.rect.y1 - picture.rect.y0) * scale;
  const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  const auto *words = reinterpret_cast<const std::uint16_t *>(image.bytes());
  std::vector<std::uint8_t> rgb(pixels * 3);
  for (std::size_t i = 0; i < pixels; i++) {
    const gpu::VramPixel pixel = gpu::decodeVramPixel(words[i]);
    const present::Rgb8 colour = present::fadeRgb(
        recordFade_, {gpu::scale5to8(pixel.red), gpu::scale5to8(pixel.green), gpu::scale5to8(pixel.blue)});
    rgb[i * 3] = static_cast<std::uint8_t>(colour.red);
    rgb[i * 3 + 1] = static_cast<std::uint8_t>(colour.green);
    rgb[i * 3 + 2] = static_cast<std::uint8_t>(colour.blue);
  }
  if (!debug::writeImageRgb24(path, rgb.data(), width, height)) {
    lucent::error("shot", "record picture not written to {}", path);
    return false;
  }
  lucent::info("shot",
               "record picture -> {} ({}x{} at {}x, from {},{})",
               path,
               width,
               height,
               scale,
               picture.rect.x0,
               picture.rect.y0);
  return true;
}

} // namespace psx::render
