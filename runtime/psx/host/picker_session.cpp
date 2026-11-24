#include "picker_session.h"

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

#include <algorithm>
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
                             std::span<const PanelArt *const> art,
                             int selected,
                             SessionServices &services)
    : runtime_(runtime), catalog_(catalog), content_(content), art_(art), services_(services),
      selected_(std::clamp(selected, 0, std::max(content.panelCount() - 1, 0))) {
  runtime_.bind(&content_);
}

PickerSession::~PickerSession() = default;

void PickerSession::select(int panel, PickerLayout &layout) {
  if (panel == selected_) {
    return;
  }
  selected_ = panel;
  layout.setSelection(selected_);
  host_->rml_overlay.selectStageTitle(selected_);
}

bool PickerSession::serviceInput(PickerLayout &layout) {
  const int count = content_.panelCount();
  if (count == 0) {
    return false;
  }
  // The panels are a ring: stepping past either end comes round to the other.
  const int step = runtime_.takeStep() + (host_->pad.pressedButton(psx::input::kButtonRight) ? 1 : 0) -
                   (host_->pad.pressedButton(psx::input::kButtonLeft) ? 1 : 0);
  if (step != 0) {
    select((((selected_ + step) % count) + count) % count, layout);
  }
  if (std::optional<int> requested = runtime_.takePanel()) {
    select(*requested, layout);
  }

  bool picked = host_->pad.pressedButton(kConfirm);
  if (std::optional<std::string> slug = runtime_.takePick()) {
    std::string refusal;
    if (const TitleAvailability *title = content_.findAvailable(*slug, refusal)) {
      const int requested = content_.panelOf(title->identity->slug);
      if (requested >= 0) {
        select(requested, layout);
        picked = true;
      }
    } else {
      lucent::warn("picker", "pick refused: {}", refusal);
    }
  }
  return picked;
}

PickerSession::Result PickerSession::run() {
  host_ = std::make_unique<Game>(runtime_, services_.presentation, services_.input, services_.config);
  host_->productName = catalog_.productName();
  Core &core = host_->core;
  PickerLayout layout(content_.panelCount(),
                      PickerLayout::Motion{.selectedShare = host_->mods.pickerSelectedShare,
                                           .slantPerWidth = host_->mods.pickerSlantPerWidth,
                                           .responsePerFrame = host_->mods.pickerWidthResponse});
  layout.snap(selected_);
  host_->pad.useLiveInputOnly(); // a selector must not open or rotate the player's pad recording
  // No guest runs here, so the channel needs no store observer.
  host_->dbg_server.start();
  // The overlay exists before the first build.
  if (services_.presentation.enabled()) {
    host_->render().bringUp();
  }
  std::vector<std::string> names;
  for (int panel = 0; panel < content_.panelCount(); ++panel) {
    names.emplace_back(content_.panelTitle(panel).identity->displayName);
  }
  host_->rml_overlay.showStageBand(names);
  host_->rml_overlay.selectStageTitle(selected_);
  lucent::info("picker", "on {}", names.empty() ? std::string("no title") : names[static_cast<std::size_t>(selected_)]);

  // The same services a title's field owes: the player is read and the screen presents.
  const psx::frame::FieldTurn fieldTurn;
  bool picking = false;
  int frame = 0;
  for (; !services_.budget.exhausted(); ++frame) {
    if (services_.presentation.faulted()) {
      return Result{Outcome::DrawFault};
    }
    if (services_.quit.take() || host_->dbg_server.takeQuitRequest()) {
      lucent::info("picker", "selector asked to quit after {} frame(s)", frame);
      return {};
    }
    fieldTurn.beginField(core);
    host_->pad.serviceFrame();
    const psx::present::PixelSize sink = services_.presentation.sinkSize();
    layout.setSurface(sink.width, sink.height);

    if (!picking && serviceInput(layout)) {
      picking = true;
      layout.pick(selected_);
      host_->rml_overlay.hideStageBand();
      lucent::info("picker", "picking {}", content_.panelTitle(selected_).identity->displayName);
    }
    // The width animation runs on the same frame, so a selection change is drawn from its first frame.
    const bool settling = layout.advance();
    host_->display.buildPanes(panes_.build(layout, art_, selected_, services_.presentation));
    host_->framePacer.hostScreenPace(core);
    host_->display.show();
    services_.budget.step();
    fieldTurn.endField(core, static_cast<std::uint32_t>(frame));

    if (picking && !settling) {
      // The seams have left the window: the picked title takes it.
      host_.reset();
      return Result{Outcome::Chosen, selected_};
    }
  }
  lucent::info("picker", "selector ended after {} frame(s) with no choice", frame);
  return {};
}

} // namespace psx::host
