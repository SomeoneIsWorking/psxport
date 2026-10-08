// gpu_vk_record_present.cpp — RenderPath::Record's picture: the current frame record through RecordRasterizer.
#include "core.h"
#include "game.h"
#include "gpu_vk.h"
#include "gpu_vk_check.h"
#include "gpu_vk_device_handles.h"
#include "gpu_vk_internal.h"
#include "image_writer.h"
#include "present_plan.h"
#include "vram_pixel.h"

#include <lucent/log.h>

#include <vector>

void GpuVkState::present_record(PresentInputs &inputs) {
  s_present_record = false;
  Core &core = game->core;
  const psx::gpu::DisplayArea area = core.gpuDevice.displayArea();
  // 24bpp scanout reads VRAM bytes, which the scaled image does not hold; the device picture stands.
  if (core.rsub.mode.path() != RenderPath::Record || area.depth24) {
    return;
  }
  int scale = 1;
  gpu_vk_video_status(&core, nullptr, &scale, nullptr, nullptr, nullptr, nullptr, nullptr);
  if (!s_record) {
    s_record = std::make_unique<psx::gpu::RecordRasterizer>(s_dev);
  }
  const psx::gpu::RecordRect display = psx::gpu::clampedVramRect(inputs.sx, inputs.sy, inputs.native_w, inputs.disp_h);
  const GuestProjectionPlan plan =
      gpu_vk_latch_record_display(core, {display.x1 - display.x0, display.y1 - display.y0});
  const psx::gpu::RecordView view{display, plan.presentationHorizontalMargin};
  const psx::present::FrameRecord &record = game->presentation.currentRecord();
  const psx::present::FrameRecord *composed = game->presentation.composedRecord();
  const bool advances = game->presentation.composedAdvances();
  const bool deviceAhead = core.gpuDevice.hasUnsealedWork();
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_dev);
  GPUCHK(cmd, "record command buffer");
  s_present_record = true;
  s_present_ires = scale > 1 ? scale : 0;
  s_present_fade[0] = inputs.fade_mode;
  s_present_fade[1] = inputs.fade_r;
  s_present_fade[2] = inputs.fade_g;
  s_present_fade[3] = inputs.fade_b;
  const bool drewComposed =
      s_record->show(cmd, record, composed, advances, deviceAhead, core.gpuDevice.vram(), scale, view);
  // The display, or its wide canvas, at the 4:3 display's aspect per column.
  const psx::gpu::RecordRect shown = s_record->presented().rect;
  inputs.sx = shown.x0;
  inputs.sy = shown.y0;
  inputs.disp_w = shown.x1 - shown.x0;
  inputs.disp_h = shown.y1 - shown.y0;
  inputs.content_w = 0;
  inputs.present_ires = s_present_ires;
  if (composed != nullptr) {
    lucent::debug("recordcheck",
                  "composed seq={} over current seq={} final={} drawn={}",
                  composed->sequence(),
                  record.sequence(),
                  advances ? 1 : 0,
                  drewComposed ? 1 : 0);
  }
  // An in-between has no device picture to match; the t = 1 composed frame at 4:3 must match it.
  if ((drewComposed && (!advances || view.margin > 0)) || !lucent::channel_on("recordcheck")) {
    gpu_submit(cmd, drewComposed ? "record composed" : "record");
    return;
  }
  const psx::gpu::RecordRect rect = psx::gpu::clampedVramRect(area.x, area.y, area.width, area.height);
  s_record->download(cmd, {drewComposed ? s_record->presented().texture : s_record->image(), rect});
  if (!gpu_submit_and_wait(cmd, "record check")) {
    return;
  }
  const std::vector<std::uint16_t> image = s_record->downloaded();
  const std::span<const std::uint16_t> vram = core.gpuDevice.vram();
  const int width = rect.x1 - rect.x0;
  long mismatched = 0;
  int firstX = -1;
  int firstY = -1;
  std::uint16_t firstImage = 0;
  std::uint16_t firstDevice = 0;
  for (int row = 0; row < rect.y1 - rect.y0; row++) {
    for (int column = 0; column < width; column++) {
      const std::uint16_t ours = image[static_cast<std::size_t>(row * scale) * static_cast<std::size_t>(width * scale) +
                                       static_cast<std::size_t>(column * scale)];
      const std::uint16_t device = vram[static_cast<std::size_t>(rect.y0 + row) * psx::gpu::kRecordVramWidth +
                                        static_cast<std::size_t>(rect.x0 + column)];
      if (ours != device && mismatched++ == 0) {
        firstX = rect.x0 + column;
        firstY = rect.y0 + row;
        firstImage = ours;
        firstDevice = device;
      }
    }
  }
  lucent::debug("recordcheck",
                "seq={} composed={} complete={} entries={} ahead={} replayed={} resynced={} scale={} "
                "display {}x{}@{},{} mismatched={} first=({},{}) record={:#06x} device={:#06x}",
                record.sequence(),
                drewComposed ? 1 : 0,
                record.complete() ? 1 : 0,
                record.entries().size(),
                deviceAhead ? 1 : 0,
                s_record->replayed(),
                s_record->resynced(),
                scale,
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

bool GpuVkState::record_shot(const char *path) {
  const psx::gpu::RecordPicture picture = s_record ? s_record->presented() : psx::gpu::RecordPicture{};
  if (picture.texture == nullptr) {
    lucent::error("shot", "no record picture has been presented, nothing written to {}", path ? path : "(null)");
    return false;
  }
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_dev);
  GPUCHK(cmd, "record shot command buffer");
  s_record->download(cmd, picture);
  if (!gpu_submit_and_wait(cmd, "record shot")) {
    return false;
  }
  const std::vector<std::uint16_t> image = s_record->downloaded();
  const int scale = s_record->scale();
  const int width = (picture.rect.x1 - picture.rect.x0) * scale;
  const int height = (picture.rect.y1 - picture.rect.y0) * scale;
  std::vector<std::uint8_t> rgb(image.size() * 3);
  for (std::size_t i = 0; i < image.size(); i++) {
    const psx::gpu::VramPixel pixel = psx::gpu::decodeVramPixel(image[i]);
    int r = psx::gpu::scale5to8(pixel.red);
    int g = psx::gpu::scale5to8(pixel.green);
    int b = psx::gpu::scale5to8(pixel.blue);
    present_fade_rgb(s_present_fade, r, g, b);
    rgb[i * 3] = static_cast<std::uint8_t>(r);
    rgb[i * 3 + 1] = static_cast<std::uint8_t>(g);
    rgb[i * 3 + 2] = static_cast<std::uint8_t>(b);
  }
  if (!image_write_rgb24(path, rgb.data(), width, height)) {
    lucent::error("shot", "record picture not written to {}", path ? path : "(null)");
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
