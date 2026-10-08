#include "bug_report_session.h"

#include "c_subsys.h" // watchdog_suspend / watchdog_resume — writing a report is intentional idle
#include "config_vars.h"
#include "core.h"
#include "game.h"
#include "gpu_vk.h"       // gpu_vk_present_shot
#include "image_writer.h" // image_write_rgb24
#include "ordering_table.h"
#include "psxport_build_id.h"
#include "render_mode.h"

#include <bug_report/draft.h>
#include <bug_report/form.h>
#include <lucent/log.h>
#include <lucent/platform.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>

namespace psx::debug {

namespace {

// The report folder's file names. The reproduction command below names them, so they are spelled once.
constexpr const char *kScreenshotFile = "screen.png";
constexpr const char *kReferenceFile = "psx.png";
constexpr const char *kRecordingFile = "repro.pad";
constexpr const char *kCardFile = "memcard.mcr";

// The directory name under the user-data root when a title declares none: the framework's own, so
// every report still lands in one known place.
constexpr const char *kFallbackUserDataName = "psxport";

bool writeBytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

void attachOrWarn(bug_report::Draft &draft, const char *file, bug_report::AttachmentRole role, const char *label) {
  std::string error;
  if (!draft.attach(file, role, label, error)) {
    lucent::warn("bugreport", "{} is not in the report: {}", file, error);
  }
}

} // namespace

void BugReportSession::request() {
  if (mState == State::Idle) {
    mState = State::Requested;
  }
}

void BugReportSession::requestScripted(std::string summary) {
  mScriptedSummary = std::move(summary);
  request();
}

void BugReportSession::captureReferenceFrame(Core &core, std::uint32_t orderingTableHead) {
  psx::gpu::GpuDevice &device = core.gpuDevice;
  const bool kickDrewIt = core.rsub.mode.psxRender();
  std::vector<std::uint8_t> before;
  if (!kickDrewIt) {
    before = device.saveState();
    psx::gpu::submitOrderingTable(core, device, orderingTableHead);
  }
  // The table draws into the draw area; the display still shows the previous frame.
  const psx::gpu::DisplayArea display = device.displayArea();
  const psx::gpu::VramPoint origin = device.drawAreaOrigin();
  mReference = device.picture(origin.x, origin.y, display.width, display.height, false);
  if (!kickDrewIt && !device.loadState(before)) {
    lucent::error("bugreport", "the GPU device refused its own state after drawing the reference frame");
    std::abort();
  }
}

void BugReportSession::atFieldBoundary(Core &core) {
  if (!mPruned) {
    // Once per process, before this process can have a draft of its own: a draft folder left by a
    // run that died mid-report is removed.
    mPruned = true;
    if (const std::optional<std::filesystem::path> root = reportRoot(core)) {
      bug_report::Draft::pruneAbandoned(*root);
    }
  }
  switch (mState) {
  case State::Idle:
    return;
  case State::Requested:
    mReference.reset();
    mState = State::Capturing;
    return;
  case State::Capturing:
    report(core);
    mState = State::Idle;
    mReference.reset();
    mScriptedSummary.reset();
    return;
  }
}

std::optional<std::filesystem::path> BugReportSession::reportRoot(Core &core) const {
  const std::string &configured = psx::config::cv_bug_report_dir.get();
  if (!configured.empty()) {
    return std::filesystem::path(configured);
  }
  const char *declared = core.game->hostIdentity().userDataName;
  const std::optional<std::filesystem::path> userData =
      lucent::platform::user_data_directory(declared != nullptr ? declared : kFallbackUserDataName);
  if (!userData) {
    return std::nullopt;
  }
  return *userData / "bug-reports";
}

void BugReportSession::report(Core &core) {
  Game &game = *core.game;
  const std::optional<std::filesystem::path> root = reportRoot(core);
  if (!root) {
    lucent::error("bugreport", "no user-data directory on this host; set PSXPORT_BUG_REPORT_DIR");
    return;
  }
  const HostIdentity identity = game.hostIdentity();
  const char *application = identity.windowTitle != nullptr    ? identity.windowTitle
                            : identity.userDataName != nullptr ? identity.userDataName
                                                               : kFallbackUserDataName;
  std::string error;
  std::optional<bug_report::Draft> draft =
      bug_report::Draft::open(*root, application, std::chrono::system_clock::now(), error);
  if (!draft) {
    lucent::error("bugreport", "cannot start a report under {}: {}", root->string(), error);
    return;
  }
  attachCaptures(core, *draft);

  watchdog_suspend(); // the player may write for as long as they like
  const bool saved = holdForm(core, *draft);
  watchdog_resume();
  if (!saved) {
    lucent::info("bugreport", "report cancelled; nothing saved");
  }
}

void BugReportSession::attachCaptures(Core &core, bug_report::Draft &draft) {
  using bug_report::AttachmentRole;
  Game &game = *core.game;
  std::string error;

  if (const auto path = draft.pathFor(kScreenshotFile, error)) {
    gpu_vk_present_shot(&core, path->string().c_str());
    std::error_code exists;
    if (std::filesystem::exists(*path, exists)) {
      attachOrWarn(draft, kScreenshotFile, AttachmentRole::Screenshot, "On screen");
    }
  }

  if (mReference && !mReference->pixels.empty()) {
    if (const auto path = draft.pathFor(kReferenceFile, error);
        path &&
        image_write_rgb24(path->string().c_str(), mReference->pixels.data(), mReference->width, mReference->height)) {
      attachOrWarn(draft, kReferenceFile, AttachmentRole::Reference, "GPU device render of the same frame");
    }
  } else {
    draft.addFact("Device render", "not captured (the title's draw kick does not take a reference frame)");
  }

  const std::size_t padFrames = game.pad.recordedFrames();
  const std::uint32_t reportedFrame = padFrames > 0 ? static_cast<std::uint32_t>(padFrames - 1) : 0u;
  bool recorded = false;
  if (const auto path = draft.pathFor(kRecordingFile, error);
      path && game.pad.saveRecording(path->string().c_str(), 0)) {
    attachOrWarn(draft, kRecordingFile, AttachmentRole::Reproduction, "Pad recording from boot");
    recorded = true;
  }
  const std::vector<std::uint8_t> &card = game.pad.recordingStartCard();
  if (const auto path = draft.pathFor(kCardFile, error); path && !card.empty() && writeBytes(*path, card)) {
    attachOrWarn(draft, kCardFile, AttachmentRole::Reproduction, "Memory card the recording started from");
  }

  draft.addFact("pad frame", std::to_string(reportedFrame));
  draft.addFact("present frame", std::to_string(game.gpu.gpu_frame_no()));
  draft.addFact("render path", render_path_name(core.rsub.mode.path()));
  draft.addFact("build", PSXPORT_BUILD_ID_COMPOSITE);

  if (recorded) {
    const HostIdentity identity = game.hostIdentity();
    const std::string cardVar = identity.cardEnvVar != nullptr ? identity.cardEnvVar : "PSXPORT_CARD";
    draft.setReproduction(bug_report::Reproduction{
        .summary = "Replay the pad recording from boot on the memory card it started from; pad frame " +
                   std::to_string(reportedFrame) + " is the reported frame.",
        .steps =
            {
                "Copy memcard.mcr to a writable place outside the report: the game writes to its card.",
                "Run the title with the variables below. The replay runs at real speed and screenshots the "
                "reported frame to scratch/screenshots/padshot_" +
                    std::to_string(reportedFrame) + ".ppm.",
            },
        .command = cardVar + "=<copy of memcard.mcr> PSXPORT_PAD_REPLAY=<report>/" + kRecordingFile +
                   " PSXPORT_PAD_SHOT_AT=" + std::to_string(reportedFrame),
    });
  }
}

bool BugReportSession::holdForm(Core &core, bug_report::Draft &draft) {
  Game &game = *core.game;
  bug_report::Form *form = game.rml_overlay.showBugReport(draft);
  if (form == nullptr) {
    lucent::error("bugreport", "the overlay cannot show the report form; nothing saved");
    return false;
  }
  if (mScriptedSummary) {
    form->fill(bug_report::PlayerText{*mScriptedSummary, "Filed by the REPL `bugreport` command."});
    form->requestSave();
  }
  bool saved = false;
  for (;;) {
    const bug_report::Form::Outcome outcome = form->outcome();
    if (outcome == bug_report::Form::Outcome::Cancelled) {
      break;
    }
    if (outcome == bug_report::Form::Outcome::SaveRequested) {
      std::string error;
      if (const std::optional<std::filesystem::path> folder = draft.commit(form->text(), error)) {
        lucent::info("bugreport", "report saved: {}", folder->string());
        saved = true;
        break;
      }
      lucent::warn("bugreport", "save refused: {}", error);
      form->rejectSave(error);
      if (mScriptedSummary) {
        form->requestCancel(); // no player to correct it
      }
    }
    game.dbg_server.idleFrame(&core);
  }
  game.rml_overlay.closeBugReport();
  return saved;
}

} // namespace psx::debug
