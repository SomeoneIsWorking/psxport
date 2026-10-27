// psx::debug::BugReportSession — the in-app bug report, from the B key to a saved report folder.
//
// The report itself (folder, report.json, README.md, the RmlUi form) is the shared bug-report
// library's (shared/bug-report). This owner is the PSX half: WHEN a report is taken, WHAT it
// carries, and holding the game still while the player writes it.
//
// A report is taken across frame boundaries, so the screenshot and the reference picture are the same frame:
//   1. `request()` — the B key (HostInput) or the REPL. Nothing happens mid-frame.
//   2. The next field begins and the session captures: the next logic frame the presenter seals
//      (FramePresenter::commit) hands it the device's drawing of that frame, the reference picture.
//   3. The field after the seal begins: that frame is on screen, so the session captures the presented
//      picture, the last second of presents as a clip, the pad recording up to it and the card it
//      started from, opens the form and holds the field until the player saves or cancels.
//
// A field run that seals no frame within a few fields still gets a report, without a reference picture.
#pragma once

#include "rgb24_picture.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

class Core;

namespace bug_report {
class Draft;
}

namespace psx::debug {

class BugReportSession {
public:
  // Ask for a report at the next frame boundary. Repeated requests before it opens are one report.
  void request();
  // The same, but the form is filled and saved without a player: the REPL's `bugreport <summary>`,
  // so a headless run exercises the whole path.
  void requestScripted(std::string summary);

  // A logic frame was sealed: the device has drawn it into its draw area.
  void frameSealed(Core &core);

  // The frame-boundary half, called at the start of every field (FieldTurn::beginField).
  void atFieldBoundary(Core &core);

private:
  enum class State : std::uint8_t { Idle, Requested, Capturing };

  void report(Core &core);
  std::optional<std::filesystem::path> reportRoot(Core &core) const;
  void attachCaptures(Core &core, bug_report::Draft &draft);
  bool holdForm(Core &core, bug_report::Draft &draft);

  State mState = State::Idle;
  int mFieldsCapturing = 0;
  bool mPruned = false;
  std::optional<std::string> mScriptedSummary;
  std::optional<psx::gpu::Rgb24Picture> mReference;
};

} // namespace psx::debug
