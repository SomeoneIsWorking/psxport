// psx::debug::BugReportSession — the in-app bug report, from the B key to a saved report folder.
//
// The report itself (folder, report.json, README.md, the RmlUi form) is the shared bug-report
// library's (shared/bug-report). This owner is the PSX half: WHEN a report is taken, WHAT it
// carries, and holding the game still while the player writes it.
//
// One report is three frame boundaries, so the screenshot and the reference picture are the same
// frame:
//   1. `request()` — the B key (HostInput) or the REPL. Nothing happens mid-frame.
//   2. The next field begins: the session arms `capturingFrame()`, the frame runs, and the title's
//      ordering-table kick calls `captureReferenceFrame` with the table it drew; the GPU device's
//      drawing of that table is the reference picture.
//   3. The field after that begins: that frame is on screen, so the session captures the presented
//      picture, the pad recording up to it and the card it started from, opens the form and holds
//      the field until the player saves or cancels.
//
// A title that never calls `captureReferenceFrame` still gets a report; it just carries no reference
// picture and says so in its facts.
#pragma once

#include "gpu_device.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

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

  // True for the one frame whose ordering table the reference picture must come from.
  bool capturingFrame() const {
    return mState == State::Capturing;
  }
  // Called by a title's ordering-table kick while capturingFrame(), after its own draw has run. On a
  // native path the device never saw the table, so it is drawn here and the device restored after.
  void captureReferenceFrame(Core &core, std::uint32_t orderingTableHead);

  // The frame-boundary half, called at the start of every field (FieldTurn::beginField).
  void atFieldBoundary(Core &core);

private:
  enum class State : std::uint8_t { Idle, Requested, Capturing };

  void report(Core &core);
  std::optional<std::filesystem::path> reportRoot(Core &core) const;
  void attachCaptures(Core &core, bug_report::Draft &draft);
  bool holdForm(Core &core, bug_report::Draft &draft);

  State mState = State::Idle;
  bool mPruned = false;
  std::optional<std::string> mScriptedSummary;
  std::optional<psx::gpu::Rgb24Picture> mReference;
};

} // namespace psx::debug
