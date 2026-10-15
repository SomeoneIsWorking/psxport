// display_presenter.h — the guest display to the screen: one present per completed frame, the
// in-betweens between them, and the per-present captures and counters.
#pragma once

#include "pixel_size.h"

#include <cstdint>
#include <string>
#include <vector>

class Core;

namespace psx::present {

// A main frame ends cold-init watchdog grace; a transition (movie teardown, a load's black) is only progress.
enum class PresentCompletion : std::uint8_t { MainFrame, Transition };

class DisplayPresenter {
public:
  explicit DisplayPresenter(Core &core);

  // The guest display rect through psx::gpu::present, then this present's diagnostics and counters.
  void present(PresentCompletion completion);
  // An interpolated frame between two presents: shown, counted apart, frame() does not move.
  void presentInBetween();
  // Re-show the last present without building anything (debug-server pause loop).
  void repaint();
  // A host RGBA8 picture (movie frame, splash) as the whole frame, letterboxed 4:3 and faded by `fade` (0..1).
  void presentHostPicture(const std::uint8_t *rgba, PixelSize size, float fade);
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

  // Rows presented for a decoded display height: GameRuntime::displayHeight when the title declares one.
  static int presentedHeight(Core &core, int displayHeight);

private:
  void presentDisplay();
  void logVramScan();
  void dumpGuestRam();
  void dumpDisplay();

  Core &core_;
  int frame_ = 0;
  int interpolated_ = 0;
  std::vector<std::uint32_t> shotFrames_;
  std::vector<std::uint32_t> presentShotFrames_;
  long ramDumpFrame_ = -1;
  std::string ramDumpPath_;
  std::string displayDumpDir_;
  int displayDumpEvery_ = 1;
};

} // namespace psx::present
