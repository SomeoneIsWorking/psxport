#include "picker_session.h"

#include "panel_sessions.h"
#include "title_session.h"

#include "cfg.h"
#include "core.h"
#include "dbg_server.h"
#include "field_turn.h"
#include "frame_pacer.h"
#include "game.h"
#include "gpu_vk.h"
#include "gpu_vk_device.h"
#include "machine.h"

#include <lucent/log.h>

#include <cstdlib>
#include <optional>
#include <string>

namespace psx::host {
namespace {

// Active-low pad bits, as Pad::buttons carries them.
constexpr std::uint16_t kPadLeft = 0x0100u;
constexpr std::uint16_t kPadRight = 0x0200u;
constexpr std::uint16_t kPadStart = 0x0008u;
constexpr std::uint16_t kPadCross = 0x4000u;
constexpr std::uint16_t kPadConfirm = kPadCross | kPadStart;

// Ratios, not three numbers: PickerLayout normalises by the total.
constexpr float kSelectedShare = 0.5f;
constexpr float kUnselectedShare = 0.25f;
// The lean is a horizontal displacement, so it is a fraction of the width.
constexpr float kSlantPerWidth = 0.1f;
// A quarter of what is left per frame: long enough to read as movement, short enough that the
// panel is where the player left it before they can press again.
constexpr float kWidthResponse = 0.25f;
// Bounded, because an unbounded boot would freeze the screen on the panel the player is leaving.
constexpr int kBootStepBudget = 6;
} // namespace

PickerSession::PickerSession(PickerRuntime &runtime,
                             const TitleCatalog &catalog,
                             const PickerContent &content,
                             int frameCap,
                             GpuDevice &presentation)
    : runtime_(runtime), catalog_(catalog), content_(content), frameCap_(frameCap), presentation_(presentation),
      layout_(content.panelCount(), kSelectedShare, kUnselectedShare, kSlantPerWidth, kWidthResponse) {
  if (content.panelCount() > PickerLayout::maxPanels()) {
    lucent::error("picker",
                  "{} titles are available and the picker lays out at most {} panels",
                  content.panelCount(),
                  PickerLayout::maxPanels());
    std::abort();
  }
  panels_ = std::make_unique<PanelSessions>(catalog_, content_, presentation_);
  runtime_.bind(&content_, nullptr);
}

PickerSession::~PickerSession() = default;

int PickerSession::serviceInput(Game &host) {
  // The host's pad is the only pad that sees the player, so a Left tap moves one selection
  // instead of three demos at once.
  int panel = panels_->selected();
  const int count = panels_->count();
  if (count > 0) {
    if (host.pad.pressedButton(kPadLeft)) {
      panel = panel - 1 >= 0 ? panel - 1 : count - 1;
    }
    if (host.pad.pressedButton(kPadRight)) {
      panel = panel + 1 < count ? panel + 1 : 0;
    }
    if (std::optional<int> requested = runtime_.takeSelection()) {
      panel = *requested;
    }
    panels_->select(panel);
  }
  runtime_.setSelection(panels_->selected());

  int chosen = -1;
  if (host.pad.pressedButton(kPadConfirm) && panels_->count() > 0) {
    chosen = panels_->selected();
  }
  if (std::optional<std::string> slug = runtime_.takePick()) {
    std::string refusal;
    if (const TitleAvailability *title = content_.findAvailable(*slug, refusal)) {
      const int requested = panels_->panelOf(title->identity->slug);
      if (requested >= 0) {
        panels_->select(requested);
        chosen = requested;
      }
    } else {
      lucent::warn("picker", "pick refused: {}", refusal);
    }
  }
  return chosen;
}

PickerSession::Result PickerSession::run() {
  psxport_install_game(runtime_);
  // No guest runs here, but the control endpoint, the pad and the screen text all need an owner,
  // and no panel session may have them.
  auto host = std::make_unique<Game>(presentation_);
  Core &core = host->core;
  host->pad.useLiveInputOnly(); // a selector must not open or rotate the player's pad recording
  // The same arming call the product spine makes in `TitleSession::boot`, so the selector's channel
  // is not reachable only through a hand-written attach of its own.
  const std::uint32_t frameCap = psx::Machine{*host}.attachControlChannel(frameCap_);
  gpu_vk_ensure_device(&core);

  composite_ = std::make_unique<PickerComposite>(presentation_, &core);
  runtime_.bind(&content_, composite_.get());

  // Each panel is a live session's own presented frame plus that title's own logo read from its own
  // disc, with no overlay screen in between.

  // The same four services a title's field owes: the player is read, panels advance and the
  // composite presents.
  const psx::frame::FieldTurn fieldTurn;
  int frame = 0;
  for (; frameCap == 0 || static_cast<std::uint32_t>(frame) < frameCap; ++frame) {
    fieldTurn.beginField(core);
    host->pad.serviceFrame();

    int sinkWidth = 0, sinkHeight = 0;
    gpu_vk_present_sink_size(&sinkWidth, &sinkHeight);
    layout_.setSurface(sinkWidth, sinkHeight);
    layout_.setSelection(panels_->selected());
    composite_->setSurface(layout_, sinkWidth, sinkHeight);
    // Every panel session presents into the composite, never into the window: the window belongs to
    // this host, and each session's picture is a fraction of it.
    for (int panel = 0; panel < panels_->count(); ++panel) {
      if (TitleSession *session = panels_->session(panel)) {
        session->presentToPane(composite_->paneImageWidth(), composite_->paneImageHeight());
      }
    }

    const int chosen = serviceInput(*host);
    if (chosen >= 0 && panels_->count() > 0) {
      std::unique_ptr<TitleSession> session = panels_->confirm();
      if (session != nullptr) {
        // No press is replayed into the new session: `boot()` has not run, so a tap held over from
        // the selector's own pad would arrive as a phantom press in the title's first frame.
        lucent::info("picker",
                     "{} confirmed at panel {} — it boots fresh into the window",
                     content_.panelTitle(chosen).identity->slug,
                     chosen);
        composite_.reset();
        return Result{Outcome::Chosen, std::move(session)};
      }
    }

    // The panel animation runs on the same frame, so the width change the player just
    // asked for is drawn from its first frame.
    panels_->advance(cfg_int("PSXPORT_PICKER_BOOT_STEPS", kBootStepBudget));
    layout_.advance();
    panels_->report();

    sources_.clear();
    for (int panel = 0; panel < panels_->count(); ++panel) {
      PanelSource source;
      source.session = panels_->core(panel);
      // So the panel cover-crops to the shape the picture really is and leaves no band
      // of background.
      source.pictureWidth = panels_->pictureWidth(panel);
      source.pictureHeight = panels_->pictureHeight(panel);
      source.logo = panels_->logo(panel);
      sources_.push_back(source);
    }
    composite_->present(layout_, sources_, panels_->selected());
    core.game->framePacer.hostScreenPace(core);
    fieldTurn.endField(core, static_cast<std::uint32_t>(frame));
  }
  lucent::info("picker", "selector ended after {} frame(s) with no choice", frame);
  return {};
}

} // namespace psx::host
