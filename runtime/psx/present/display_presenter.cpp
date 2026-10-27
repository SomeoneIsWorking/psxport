// display_presenter.cpp — see display_presenter.h.
#include "core.h"
#include "watchdog.h"

#include "diagnostic_config.h"
#include "display_presenter.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_device.h"
#include "host_clock.h"
#include "image_writer.h"
#include "number_text.h"
#include "picture_announce.h"
#include "render_state.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace psx::present {
namespace {

constexpr lucent::Channel presentSequenceChannel{"presentseq"};

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

} // namespace

DisplayPresenter::DisplayPresenter(Core &core)
    : core_(core), shotFrames_(psx::config::gpu_shot_frames()), presentShotFrames_(psx::config::present_shot_frames()) {
  parseDumpDir(psx::config::gpu_dump_dir(), displayDumpDir_, displayDumpEvery_);
}

gpu::VramRect DisplayPresenter::presentedRect(Core &core) {
  const gpu::DisplayArea area = core.gpuDevice().displayArea();
  const int declared = core.runtime.displayHeight();
  return {area.x, area.y, area.width, declared > 0 ? declared : area.height};
}

void DisplayPresenter::buildDisplay() {
  const gpu::VramRect rect = presentedRect(core_);
  core_.game->rml_overlay.newFrame();
  psx::picture::announceOnChange(core_, rect.width);
  core_.game->render().buildDisplay(rect);
  core_.game->render().preseqCapture();
}

void DisplayPresenter::build(PresentCompletion completion) {
  finishShow();
  logVramScan();
  buildDisplay();
  dumpDisplay();
  frame_++;
  pendingCompletion_ = completion;
}

void DisplayPresenter::buildInBetween() {
  finishShow();
  buildDisplay();
  interpolated_++;
}

void DisplayPresenter::buildHostPicture(const std::uint8_t *rgba, PixelSize size, float fade) {
  finishShow();
  core_.game->render().buildImage(rgba, size, fade);
}

void DisplayPresenter::buildPanes(std::span<const render::Pane> panes) {
  finishShow();
  core_.game->rml_overlay.newFrame();
  core_.game->render().buildPanes(panes);
}

void DisplayPresenter::show() {
  finishShow();
  showBuilt();
}

void DisplayPresenter::showAt(std::optional<double> dueMs, const PresentNote &note) {
  finishShow();
  scheduled_ = ScheduledShow{dueMs.value_or(psx::platform::hostNowMs()), note};
  showIfDue();
}

void DisplayPresenter::showIfDue() {
  if (scheduled_ && psx::platform::hostNowMs() >= scheduled_->dueMs) {
    showScheduled();
  }
}

void DisplayPresenter::finishShow() {
  if (!scheduled_) {
    return;
  }
  psx::platform::sleepUntilMs(scheduled_->dueMs);
  showScheduled();
}

void DisplayPresenter::showScheduled() {
  const ScheduledShow scheduled = *scheduled_;
  scheduled_.reset();
  showBuilt();
  const PresentNote &note = scheduled.note;
  lucent::debug(presentSequenceChannel,
                "at={:.3f}ms fence={} shown={} from={} t={} n={} fields={}/{} composed={} cut={} logic={} due={:.3f}ms",
                psx::platform::hostNowMs(),
                note.fence,
                note.shown ? static_cast<long long>(*note.shown) : -1LL,
                note.from ? static_cast<long long>(*note.from) : -1LL,
                note.t,
                note.interval,
                note.fields,
                note.parts,
                note.composed ? 1 : 0,
                note.cut ? 1 : 0,
                note.logic,
                scheduled.dueMs);
}

void DisplayPresenter::showBuilt() {
  core_.game->render().show();
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

// The overlay stays interactive while the game is frozen.
void DisplayPresenter::repaint() {
  finishShow();
  core_.game->rml_overlay.newFrame();
  core_.game->render().repaint();
}

void DisplayPresenter::shot(const char *path) {
  core_.game->render().shot(path);
}

void DisplayPresenter::captureFrame() {
  const auto frame = static_cast<std::uint32_t>(frame_);
  if (shotFrames_.contains(frame)) {
    char path[128];
    std::snprintf(path, sizeof path, "scratch/screenshots/shot_%u.ppm", frame);
    lucent::info("shot", "present {} -> arming display capture to {}", frame, path);
    core_.game->render().shot(path);
  }
  if (presentShotFrames_.contains(frame)) {
    char path[128];
    std::snprintf(path, sizeof path, "scratch/screenshots/present_%u.png", frame);
    core_.game->render().presentShot(path);
  }
}

// GUARD KEPT: a full VRAM sweep, not a log call.
void DisplayPresenter::logVramScan() {
  if (!lucent::channel_on("vramscan")) {
    return;
  }
  const std::span<const std::uint16_t> vram = core_.gpuDevice().vram();
  int minX = gpu::kVramWidth;
  int minY = gpu::kVramHeight;
  int maxX = -1;
  int maxY = -1;
  long lit = 0;
  for (int y = 0; y < gpu::kVramHeight; y++) {
    for (int x = 0; x < gpu::kVramWidth; x++) {
      if ((vram[static_cast<std::size_t>(y) * gpu::kVramWidth + static_cast<std::size_t>(x)] & 0x7FFF) == 0) {
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
  const gpu::Rgb24Picture picture =
      core_.gpuDevice().picture(presentedRect(core_), core_.gpuDevice().displayArea().depth24);
  if (!psx::debug::writeImageRgb24(path, picture.pixels.data(), picture.width, picture.height)) {
    lucent::error("gpudump", "f{} could not write {}", frame_, path);
  }
}

} // namespace psx::present
