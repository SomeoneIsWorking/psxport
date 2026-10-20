// display_presenter.cpp — see display_presenter.h.
#include "core.h"
#include "watchdog.h"

#include "diagnostic_config.h"
#include "display_presenter.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_device.h"
#include "gpu_vk.h"
#include "gpu_vk_internal.h"
#include "image_writer.h"
#include "number_text.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace psx::present {
namespace {

std::vector<std::uint32_t> parseFrameList(const std::string &text) {
  std::vector<std::uint32_t> frames;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t end = std::min(text.find(',', begin), text.size());
    const std::string item = text.substr(begin, end - begin);
    if (!item.empty()) {
      frames.push_back(static_cast<std::uint32_t>(std::strtoul(item.c_str(), nullptr, 0)));
    }
    begin = end + 1;
  }
  return frames;
}

// "frame:path"; an unparsable value leaves the frame at -1.
void parseFrameAndPath(const std::string &text, long &frame, std::string &path) {
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos) {
    return;
  }
  frame = std::strtol(text.c_str(), nullptr, 10);
  path = text.substr(colon + 1);
}

// "dir[:every]": a trailing all-digit component is the interval, anything else is part of the path.
void parseDumpDir(const std::string &text, std::string &dir, int &every) {
  dir = text;
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon + 1 >= text.size()) {
    return;
  }
  const std::string_view tail = std::string_view(text).substr(colon + 1);
  if (!std::all_of(tail.begin(), tail.end(), [](char c) {
        return c >= '0' && c <= '9';
      })) {
    return;
  }
  const int n = psx::config::parseInt(tail).value_or(0);
  if (n > 0) {
    every = n;
    dir = text.substr(0, colon);
  }
}

bool contains(const std::vector<std::uint32_t> &frames, std::uint32_t frame) {
  return std::find(frames.begin(), frames.end(), frame) != frames.end();
}

} // namespace

DisplayPresenter::DisplayPresenter(Core &core)
    : core_(core), shotFrames_(parseFrameList(psx::config::gpu_shot_frames())),
      presentShotFrames_(parseFrameList(psx::config::present_shot_frames())) {
  parseFrameAndPath(psx::config::gpu_ram_dump(), ramDumpFrame_, ramDumpPath_);
  parseDumpDir(psx::config::gpu_dump_dir(), displayDumpDir_, displayDumpEvery_);
}

int DisplayPresenter::presentedHeight(Core &core, int displayHeight) {
  const int declared = core.runtime.displayHeight();
  return declared > 0 ? declared : displayHeight;
}

void DisplayPresenter::buildDisplay() {
  const psx::gpu::DisplayArea area = core_.gpuDevice().displayArea();
  psx::gpu::buildDisplay(core_, area.x, area.y, area.width, presentedHeight(core_, area.height));
  core_.game->gpuVk().preseq_capture();
}

void DisplayPresenter::build(PresentCompletion completion) {
  logVramScan();
  buildDisplay();
  dumpGuestRam();
  dumpDisplay();
  frame_++;
  pendingCompletion_ = completion;
}

void DisplayPresenter::buildInBetween() {
  buildDisplay();
  interpolated_++;
}

void DisplayPresenter::buildHostPicture(const std::uint8_t *rgba, PixelSize size, float fade) {
  core_.game->gpuVk().build_image(rgba, size, fade);
}

void DisplayPresenter::show() {
  core_.game->gpuVk().show();
  if (!pendingCompletion_) {
    return;
  }
  if (*pendingCompletion_ == PresentCompletion::MainFrame) {
    psx::platform::Watchdog::mainPresentComplete();
  } else {
    psx::platform::Watchdog::progress();
  }
  pendingCompletion_.reset();
}

void DisplayPresenter::repaint() {
  psx::gpu::repaint(core_);
}

void DisplayPresenter::shot(const char *path) {
  core_.game->gpuVk().shot(path);
}

void DisplayPresenter::captureFrame() {
  const auto frame = static_cast<std::uint32_t>(frame_);
  if (contains(shotFrames_, frame)) {
    char path[128];
    std::snprintf(path, sizeof path, "scratch/screenshots/shot_%u.ppm", frame);
    lucent::info("shot", "present {} -> arming display capture to {}", frame, path);
    core_.game->gpuVk().shot(path);
  }
  if (contains(presentShotFrames_, frame)) {
    char path[128];
    std::snprintf(path, sizeof path, "scratch/screenshots/present_%u.png", frame);
    core_.game->gpuVk().present_shot(path);
  }
}

// GUARD KEPT: a full VRAM sweep, not a log call.
void DisplayPresenter::logVramScan() {
  if (!lucent::channel_on("vramscan")) {
    return;
  }
  const std::span<const std::uint16_t> vram = core_.gpuDevice().vram();
  int minX = psx::gpu::kDeviceVramWidth;
  int minY = psx::gpu::kDeviceVramHeight;
  int maxX = -1;
  int maxY = -1;
  long lit = 0;
  for (int y = 0; y < psx::gpu::kDeviceVramHeight; y++) {
    for (int x = 0; x < psx::gpu::kDeviceVramWidth; x++) {
      if ((vram[static_cast<std::size_t>(y) * psx::gpu::kDeviceVramWidth + static_cast<std::size_t>(x)] & 0x7FFF) ==
          0) {
        continue;
      }
      lit++;
      minX = std::min(minX, x);
      maxX = std::max(maxX, x);
      minY = std::min(minY, y);
      maxY = std::max(maxY, y);
    }
  }
  const psx::gpu::DisplayArea area = core_.gpuDevice().displayArea();
  lucent::debug("vramscan",
                "f{} disp@({},{}) {}x{}  nonblack={} bbox=({},{})-({},{})",
                frame_,
                area.x,
                area.y,
                area.width,
                area.height,
                lit,
                minX,
                minY,
                maxX,
                maxY);
}

// PSXPORT_GRAMDUMP="frame:path": the 2 MB of guest RAM at one present.
void DisplayPresenter::dumpGuestRam() {
  if (ramDumpFrame_ < 0 || frame_ != ramDumpFrame_) {
    return;
  }
  std::FILE *file = std::fopen(ramDumpPath_.c_str(), "wb");
  if (!file) {
    lucent::warn("gramdump", "f{} could not open {}", frame_, ramDumpPath_);
    return;
  }
  const std::size_t written = std::fwrite(core_.ram, 1, 0x200000, file);
  std::fclose(file);
  lucent::info("gramdump", "f{} -> {} ({} bytes of 2097152)", frame_, ramDumpPath_, written);
}

// PSXPORT_GPU_DUMP=dir[:every]: the device's displayed picture as f<NNNNN>.ppm.
void DisplayPresenter::dumpDisplay() {
  if (displayDumpDir_.empty() || (frame_ % displayDumpEvery_) != 0) {
    return;
  }
  char path[512];
  std::snprintf(path, sizeof path, "%s/f%05d.ppm", displayDumpDir_.c_str(), frame_);
  if (!Fs::ensureParentDirs(path)) {
    lucent::error("gpudump", "cannot create the parent directory of {}", path);
    return;
  }
  const psx::gpu::DisplayArea area = core_.gpuDevice().displayArea();
  const psx::gpu::Rgb24Picture picture =
      core_.gpuDevice().picture({area.x, area.y, area.width, presentedHeight(core_, area.height)}, area.depth24);
  if (!psx::debug::writeImageRgb24(path, picture.pixels.data(), picture.width, picture.height)) {
    lucent::error("gpudump", "f{} could not write {}", frame_, path);
  }
}

} // namespace psx::present
