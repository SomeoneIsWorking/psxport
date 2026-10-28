#include "picker_session.h"

#include "panel_sessions.h"
#include "title_session.h"

#include "core.h"
#include "dbg_server.h"
#include "field_turn.h"
#include "frame_pacer.h"
#include "game.h"
#include "host_input.h"
#include "render_device.h"
#include "render_state.h"

#include <lucent/log.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>

namespace psx::host {
namespace {

constexpr std::uint16_t kConfirm = psx::input::kButtonCross | psx::input::kButtonStart;

} // namespace

PickerSession::PickerSession(PickerRuntime &runtime,
                             const TitleCatalog &catalog,
                             const PickerContent &content,
                             SessionServices &services)
    : runtime_(runtime), catalog_(catalog), content_(content), services_(services) {
  if (content.panelCount() > PickerLayout::maxPanels()) {
    lucent::error("picker",
                  "{} titles are available and the picker lays out at most {} panels",
                  content.panelCount(),
                  PickerLayout::maxPanels());
    std::abort();
  }
  panels_ = std::make_unique<PanelSessions>(catalog_, content_, services_);
  runtime_.bind(&content_);
}

PickerSession::~PickerSession() = default;

void PickerSession::build(const PickerLayout &layout) {
  sources_.clear();
  for (int panel = 0; panel < panels_->count(); ++panel) {
    sources_.push_back({.picture = panels_->picture(panel), .logo = panels_->logo(panel)});
  }
  host_->display.buildPanes(panes_.build(layout, sources_, panels_->selected(), services_.presentation));
  built_ = true;
}

// The settled selected panel's presents drive the selector; a pre-rolling one gets no host.
// The layout lives in run(), which the hosted panel's presents happen inside.
void PickerSession::attachPaneHost(const PickerLayout &layout) {
  for (int panel = 0; panel < panels_->count(); ++panel) {
    if (TitleSession *session = panels_->session(panel)) {
      if (panel == hostedPanel_ && panels_->panelSettled(hostedPanel_)) {
        session->setPaneHost({.built =
                                  [this, &layout] {
                                    build(layout);
                                  },
                              .show =
                                  [this] {
                                    host_->display.show();
                                  }});
      } else {
        session->setPaneHost({});
      }
    }
  }
}

int PickerSession::serviceInput(PickerLayout &layout) {
  const int before = panels_->selected();
  // The host's pad is the only one that sees the player, so a tap moves one selection.
  if (panels_->count() > 0) {
    const int step = runtime_.takeStep() + (host_->pad.pressedButton(psx::input::kButtonRight) ? 1 : 0) -
                     (host_->pad.pressedButton(psx::input::kButtonLeft) ? 1 : 0);
    if (step != 0) {
      panels_->step(step);
    }
    if (std::optional<int> requested = runtime_.takePanel()) {
      panels_->select(*requested);
    }
  }

  int chosen = -1;
  if (host_->pad.pressedButton(kConfirm) && panels_->count() > 0) {
    chosen = panels_->selected();
  }
  if (std::optional<std::string> slug = runtime_.takePick()) {
    std::string refusal;
    if (const TitleAvailability *title = content_.findAvailable(*slug, refusal)) {
      const int requested = content_.panelOf(title->identity->slug);
      if (requested >= 0) {
        panels_->select(requested);
        chosen = requested;
      }
    } else {
      lucent::warn("picker", "pick refused: {}", refusal);
    }
  }
  if (panels_->selected() != before) {
    layout.setSelection(panels_->selected());
  }
  // The host follows the selected panel and is attached once that panel has settled.
  const bool settled = panels_->panelSettled(panels_->selected());
  if (hostedPanel_ != panels_->selected() || hostedSettled_ != settled) {
    hostedPanel_ = panels_->selected();
    hostedSettled_ = settled;
    attachPaneHost(layout);
  }
  return chosen;
}

PickerSession::Result PickerSession::run() {
  host_ = std::make_unique<Game>(runtime_, services_.presentation, services_.input, services_.config);
  host_->productName = catalog_.productName();
  Core &core = host_->core;
  PickerLayout layout(content_.panelCount(),
                      PickerLayout::Motion{.selectedShare = host_->mods.pickerSelectedShare,
                                           .slantPerWidth = host_->mods.pickerSlantPerWidth,
                                           .responsePerFrame = host_->mods.pickerWidthResponse});
  host_->pad.useLiveInputOnly(); // a selector must not open or rotate the player's pad recording
  // No guest runs here, so the channel needs no store observer.
  host_->dbg_server.start();
  // The overlay exists before the first build.
  if (services_.presentation.enabled()) {
    host_->render().bringUp();
  }

  // The same services a title's field owes: the player is read, panels advance, the screen presents.
  const psx::frame::FieldTurn fieldTurn;
  int frame = 0;
  const auto fieldInterval =
      std::chrono::duration_cast<PreRollBudget::Clock::duration>(std::chrono::duration<double, std::milli>(
          psx::frame::fieldIntervalMs(psx::frame::kHostScreenFieldRateMilliHz, 1, 1)));
  for (; !services_.budget.exhausted(); ++frame) {
    if (services_.presentation.faulted()) {
      return Result{Outcome::DrawFault, nullptr};
    }
    if (services_.quit.take() || host_->dbg_server.takeQuitRequest()) {
      lucent::info("picker", "selector asked to quit after {} frame(s)", frame);
      return {};
    }
    const PreRollBudget preRoll(PreRollBudget::Clock::now() + fieldInterval);
    fieldTurn.beginField(core);
    host_->pad.serviceFrame();
    // Reset before anything can present, so it asks whether a panel presented this iteration.
    built_ = false;

    const psx::present::PixelSize sink = services_.presentation.sinkSize();
    layout.setSurface(sink.width, sink.height);

    const int chosen = serviceInput(layout);
    // Each panel builds its own picture at the size it is shown at; the window is the host's.
    for (int panel = 0; panel < panels_->count(); ++panel) {
      if (TitleSession *session = panels_->session(panel)) {
        session->presentToPane(layout.imageSize(panel));
      }
    }
    if (chosen >= 0 && panels_->count() > 0) {
      std::unique_ptr<TitleSession> session = panels_->confirm();
      if (session != nullptr) {
        // `boot()` has not run, so no press held over from the selector reaches the title.
        lucent::info("picker",
                     "{} confirmed at panel {}; it boots fresh into the window",
                     content_.panelTitle(chosen).identity->slug,
                     chosen);
        host_.reset();
        return Result{Outcome::Chosen, std::move(session)};
      }
    }

    // The width animation runs on the same frame, so a selection change is drawn from its first frame.
    const unsigned long long pacedFields = panels_->advance(preRoll);
    layout.advance();
    lucent::debug("layout",
                  "panel widths {} {} {} of {}",
                  layout.panel(0).bounds.w,
                  layout.panel(1).bounds.w,
                  layout.panel(2).bounds.w,
                  sink.width);
    panels_->report();

    // A frame in which no panel presented still owes the window one: build, pace unless the panel's
    // step already did, show.
    if (!built_) {
      build(layout);
      if (pacedFields == 0) {
        host_->framePacer.hostScreenPace(core);
      }
      host_->display.show();
    }
    services_.budget.step();
    fieldTurn.endField(core, static_cast<std::uint32_t>(frame));
  }
  lucent::info("picker", "selector ended after {} frame(s) with no choice; {}", frame, panels_->summary());
  return {};
}

} // namespace psx::host
