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

#include <optional>
#include <string>
#include <vector>

namespace psx::host {
namespace {

constexpr std::uint16_t kConfirm = psx::input::kButtonCross | psx::input::kButtonStart;

} // namespace

PickerSession::PickerSession(PickerRuntime &runtime,
                             const TitleCatalog &catalog,
                             const PickerContent &content,
                             PanelSessions &stage,
                             SessionServices &services)
    : runtime_(runtime), catalog_(catalog), content_(content), services_(services), panels_(&stage),
      sides_(stage.count()) {
  runtime_.bind(&content_);
  // The title the player left (or the first) stands selected.
  sides_.show(panels_->focused());
}

PickerSession::~PickerSession() = default;

void PickerSession::build(const PickerLayout &layout) {
  sources_.clear();
  for (int side = 0; side < sides_.sides(); ++side) {
    const int panel = sides_.title(side);
    sources_.push_back({.picture = panels_->picture(panel), .logo = panels_->logo(panel)});
  }
  host_->display.buildPanes(panes_.build(layout, sources_, sides_.selectedSide(), services_.presentation));
  built_ = true;
}

// The focused panel's presents drive the selector. The layout lives in run(), which the hosted
// panel's presents happen inside.
void PickerSession::attachPaneHost(const PickerLayout &layout) {
  for (int panel = 0; panel < panels_->count(); ++panel) {
    if (TitleSession *session = panels_->session(panel)) {
      if (panel == hostedPanel_) {
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

void PickerSession::applyMove(StageSides::Move move, int dir, PickerLayout &layout) {
  if (!move.changed) {
    return;
  }
  if (move.refilled) {
    // The selected title stepped across to the other side at its width; the edge eases on from there.
    layout.snap(dir > 0 ? 0 : 1);
  }
  layout.setSelection(sides_.selectedSide());
  panels_->focus(sides_.selectedTitle());
  host_->rml_overlay.selectStageTitle(sides_.selectedTitle());
}

bool PickerSession::serviceInput(PickerLayout &layout) {
  if (panels_->count() == 0) {
    return false;
  }
  // The host's pad is the only one that sees the player, so a tap moves one selection.
  const int step = runtime_.takeStep() + (host_->pad.pressedButton(psx::input::kButtonRight) ? 1 : 0) -
                   (host_->pad.pressedButton(psx::input::kButtonLeft) ? 1 : 0);
  if (step != 0) {
    const int dir = step > 0 ? 1 : -1;
    applyMove(sides_.step(dir), dir, layout);
  }
  if (std::optional<int> requested = runtime_.takePanel()) {
    applyMove(sides_.show(*requested), 0, layout);
  }

  bool picked = host_->pad.pressedButton(kConfirm);
  if (std::optional<std::string> slug = runtime_.takePick()) {
    std::string refusal;
    if (const TitleAvailability *title = content_.findAvailable(*slug, refusal)) {
      const int requested = content_.panelOf(title->identity->slug);
      if (requested >= 0) {
        applyMove(sides_.show(requested), 0, layout);
        picked = true;
      }
    } else {
      lucent::warn("picker", "pick refused: {}", refusal);
    }
  }
  if (hostedPanel_ != panels_->focused()) {
    hostedPanel_ = panels_->focused();
    attachPaneHost(layout);
  }
  return picked;
}

PickerSession::Result PickerSession::run() {
  host_ = std::make_unique<Game>(runtime_, services_.presentation, services_.input, services_.config);
  host_->productName = catalog_.productName();
  Core &core = host_->core;
  PickerLayout layout(sides_.sides(),
                      PickerLayout::Motion{.selectedShare = host_->mods.pickerSelectedShare,
                                           .slantPerWidth = host_->mods.pickerSlantPerWidth,
                                           .responsePerFrame = host_->mods.pickerWidthResponse});
  layout.snap(sides_.selectedSide());
  host_->pad.useLiveInputOnly(); // a selector must not open or rotate the player's pad recording
  // No guest runs here, so the channel needs no store observer. Claimed before the stage boots, so
  // the selector holds it.
  host_->dbg_server.start();
  panels_->bootAll();
  host_->dbg_server.routeMachineVerbsToPanel(panels_->focusedMachine());
  // The overlay exists before the first build.
  if (services_.presentation.enabled()) {
    host_->render().bringUp();
  }
  std::vector<std::string> names;
  for (int panel = 0; panel < panels_->count(); ++panel) {
    names.emplace_back(content_.panelTitle(panel).identity->displayName);
  }
  host_->rml_overlay.showStageBand(names);
  host_->rml_overlay.selectStageTitle(sides_.selectedTitle());
  lucent::info("picker", "on {}", content_.panelTitle(panels_->focused()).identity->displayName);

  // The same services a title's field owes: the player is read, panels advance, the screen presents.
  const psx::frame::FieldTurn fieldTurn;
  bool picking = false;
  int frame = 0;
  for (; !services_.budget.exhausted(); ++frame) {
    if (services_.presentation.faulted()) {
      return Result{Outcome::DrawFault, nullptr};
    }
    if (services_.quit.take() || host_->dbg_server.takeQuitRequest()) {
      lucent::info("picker", "selector asked to quit after {} frame(s)", frame);
      return {};
    }
    fieldTurn.beginField(core);
    host_->pad.serviceFrame();
    // Reset before anything can present, so it asks whether a panel presented this iteration.
    built_ = false;

    const psx::present::PixelSize sink = services_.presentation.sinkSize();
    layout.setSurface(sink.width, sink.height);

    if (!picking && serviceInput(layout) && panels_->session(panels_->focused()) != nullptr) {
      picking = true;
      layout.pick(sides_.selectedSide());
      host_->rml_overlay.hideStageBand();
      lucent::info("picker", "picking {}", content_.panelTitle(panels_->focused()).identity->displayName);
    }
    // The focused title builds its picture at the size its side is shown at; the window is the host's.
    if (TitleSession *session = panels_->session(panels_->focused())) {
      session->presentToPane(layout.imageSize(sides_.selectedSide()));
    }

    // The width animation runs on the same frame, so a selection change is drawn from its first frame.
    const unsigned long long pacedFields = panels_->advance();
    const bool settling = layout.advance();
    lucent::debug("layout",
                  "side widths {} {} of {}",
                  layout.panel(0).bounds.w,
                  layout.panel(1).bounds.w,
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
    // The channel's guest verbs (`rw`, `regs`, `shot`) read the panel the player is on.
    host_->dbg_server.routeMachineVerbsToPanel(panels_->focusedMachine());
    fieldTurn.endField(core, static_cast<std::uint32_t>(frame));

    if (picking && !settling) {
      // The edge has left the window: the picked title takes it and plays on.
      host_.reset();
      return Result{Outcome::Chosen, panels_->handOver()};
    }
  }
  lucent::info("picker", "selector ended after {} frame(s) with no choice; {}", frame, panels_->summary());
  return {};
}

} // namespace psx::host
