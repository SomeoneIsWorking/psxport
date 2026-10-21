// display_presenter.h — the guest display to the screen: one present per completed frame, the
// in-betweens between them, and the per-present captures and counters. Each present is a build, then a show,
// either now or at a deadline while guest work runs.
#pragma once

#include "frame_schedule.h"
#include "pixel_size.h"
#include "render_pane.h"
#include "vram_rect.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

class Core;

namespace psx::present {

// A main frame ends cold-init watchdog grace; a transition (movie teardown, a load's black) is only progress.
enum class PresentCompletion : std::uint8_t { MainFrame, Transition };

// The record path's account of a present, logged on the presentseq channel when it is shown.
struct PresentNote {
  std::uint64_t fence = 0;
  std::optional<std::uint64_t> shown; // the record whose picture it shows; none for the displayed VRAM as it stands
  std::optional<std::uint64_t> from;  // an in-between's earlier record
  float t = 1.0f;
  int fields = 0;
  int parts = 1;
  bool composed = false;
  bool cut = false;
  std::uint32_t logic = 0;
};

class DisplayPresenter {
public:
  explicit DisplayPresenter(Core &core);

  // Builds the presented display rect, then this present's diagnostics and counters.
  // `completion` is reported to the watchdog when the build is shown.
  void build(PresentCompletion completion);
  // Builds an interpolated frame between two presents: counted apart, frame() does not move.
  void buildInBetween();
  // Builds a host RGBA8 picture (movie frame, splash) as the whole frame, letterboxed 4:3 and faded by `fade` (0..1).
  void buildHostPicture(const std::uint8_t *rgba, PixelSize size, float fade);
  // Builds a host screen (the title selector) from panes, under the same overlay as a guest frame.
  void buildPanes(std::span<const render::Pane> panes);
  // Shows the last build in the window now.
  void show();
  // Shows the last build at host time `dueMs` (now when unset): at the first executor boundary past it, or
  // before anything else is built, shown or paced, whichever comes first. Guest work runs while it waits.
  void showAt(std::optional<double> dueMs, const PresentNote &note);
  // At an executor boundary: shows the scheduled build once its deadline has passed.
  void showIfDue();
  // Waits for the scheduled build's deadline and shows it.
  void finishShow();
  // Re-show the last present without building anything (debug-server pause loop).
  void repaint();
  // The displayed picture: the record image on RenderPath::Record, the device's VRAM otherwise.
  void shot(const char *path);
  // PSXPORT_SHOT_AT / PSXPORT_PRESENT_SHOT_AT, once the frame's presentation is complete.
  void captureFrame();

  int frame() const {
    return frame_;
  }
  int interpolatedFrames() const {
    return interpolated_;
  }

  // The guest display rect in VRAM, its height GameRuntime::displayHeight when the title declares one.
  static gpu::VramRect presentedRect(Core &core);

private:
  void buildDisplay();
  void showBuilt();
  void showScheduled();
  void logVramScan();
  void dumpDisplay();

  Core &core_;
  int frame_ = 0;
  int interpolated_ = 0;
  debug::FrameSchedule shotFrames_;
  debug::FrameSchedule presentShotFrames_;
  std::string displayDumpDir_;
  int displayDumpEvery_ = 1;
  std::optional<PresentCompletion> pendingCompletion_; // the last build's, until it is shown
  struct ScheduledShow {
    double dueMs = 0.0;
    PresentNote note;
  };
  std::optional<ScheduledShow> scheduled_;
};

} // namespace psx::present
