// psx::debug::BugReportSession — the in-app bug report, from the B key to a saved report folder.
//
// The report itself (folder, report.json, README.md, the RmlUi form) is the shared bug-report
// library's (shared/bug-report). This owner is the PSX half: WHEN a report is taken, WHAT it
// carries, and holding the game still while the player writes it.
//
// A report is taken across frame boundaries, so the screenshot and the reference picture are the same frame:
//   1. `request()` — the B key (HostInput) or the REPL. Nothing happens mid-frame.
//   2. The next field begins and the session opens its draft: the next logic frame the presenter seals
//      (FramePresenter::commit) hands it the device's drawing of that frame, the reference picture.
//   3. The first present that shows that frame whole (no in-between) is the screenshot.
//   4. The field after both begins: the session captures the last second of presents as a clip, the pad
//      recording up to it, the card it started from and the state, opens the form and holds the field
//      until the player saves or cancels.
//
// A field run that seals or shows no frame within a few fields still gets a report: the newest present as the
// screenshot, and a fact saying which picture is missing.
#pragma once

#include "rgb24_picture.h"

#include <bug_report/draft.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

class Core;

namespace psx::present {
struct PresentNote;
}

namespace psx::debug {

class BugReportSession {
public:
  // Ask for a report at the next frame boundary. Repeated requests before it opens are one report.
  void request();

  // Logic frame `sequence` was sealed: the device has drawn it into its draw area.
  void frameSealed(Core &core, std::uint64_t sequence);

  // A present described by `note` is on screen.
  void presented(Core &core, const psx::present::PresentNote &note);

  // The frame-boundary half, called at the start of every field (FieldTurn::beginField).
  void atFieldBoundary(Core &core);

private:
  enum class State : std::uint8_t { Idle, Requested, Capturing };

  void report(Core &core, bug_report::Draft &draft);
  std::optional<std::filesystem::path> reportRoot(Core &core) const;
  std::optional<bug_report::Draft> openDraft(Core &core) const;
  // The newest present into the draft's screenshot.
  void shootScreen(Core &core);
  void attachCaptures(Core &core, bug_report::Draft &draft);
  bool holdForm(Core &core, bug_report::Draft &draft);

  State mState = State::Idle;
  int mFieldsCapturing = 0;
  bool mPruned = false;
  std::optional<bug_report::Draft> mDraft;
  std::optional<psx::gpu::Rgb24Picture> mReference;
  std::uint64_t mSealed = 0; // the reference picture's record
  bool mScreenShot = false;
  bool mScreenMatches = false; // the screenshot shows the reference's frame
};

} // namespace psx::debug
