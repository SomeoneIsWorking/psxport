#include "picker_runtime.h"

#include <lucent/log.h>

#include <array>
#include <string_view>

namespace psx::host {
namespace {

inline constexpr std::array<std::string_view, 2> kSteps{"left", "right"};

} // namespace

PickerRuntime::Menu::Menu(PickerRuntime &picker) {
  using psx::game::DebugCall;
  using psx::game::DebugNames;
  using psx::game::DebugParam;
  section("Title Selection");
  const DebugNames panels{
      [&picker](Core &) {
        return picker.panelCount();
      },
      [&picker](Core &, int panel) {
        return picker.panelSlug(panel);
      },
  };
  verb({
      .name = "picker",
      .summary = "List the titles",
      .run =
          [&picker](const DebugCall &call) {
            call.reply = picker.listing();
          },
      .inMenu = false,
  });
  verb({
      .name = "pick",
      .summary = "Start a title",
      .params = {DebugParam::word("title", panels)},
      .run =
          [&picker](const DebugCall &call) {
            call.reply = picker.pick(call.values.word(0));
          },
  });
  verb({
      .name = "select",
      .summary = "Move the selection",
      .params = {DebugParam::word("panel",
                                  {
                                      [&picker](Core &) {
                                        return static_cast<int>(kSteps.size()) + picker.panelCount();
                                      },
                                      [&picker](Core &, int index) {
                                        return index < static_cast<int>(kSteps.size())
                                                   ? std::string(kSteps[index])
                                                   : picker.panelSlug(index - static_cast<int>(kSteps.size()));
                                      },
                                  })},
      .run =
          [&picker](const DebugCall &call) {
            call.reply = picker.select(call.values.word(0));
          },
      .inMenu = false,
  });
}

PickerRuntime::PickerRuntime() = default;

int PickerRuntime::panelCount() const {
  return content_ != nullptr ? content_->panelCount() : 0;
}

std::string PickerRuntime::panelSlug(int panel) const {
  return panel >= 0 && panel < panelCount() ? std::string(content_->panelTitle(panel).identity->slug) : std::string();
}

std::string PickerRuntime::listing() const {
  return content_ != nullptr ? content_->listing() : std::string("refused: the selector is not up");
}

std::string PickerRuntime::pick(std::string_view slug) {
  std::string refusal;
  if (content_ == nullptr) {
    return "refused: the selector is not up";
  }
  if (const TitleAvailability *title = content_->findAvailable(slug, refusal)) {
    pendingSlug_ = std::string(title->identity->slug);
    return lucent::format("ok: starting {}", *pendingSlug_);
  }
  return lucent::format("refused: {}", refusal);
}

std::string PickerRuntime::select(std::string_view panel) {
  if (panelCount() <= 0) {
    return "refused: there are no panels to select";
  }
  if (panel == kSteps[0] || panel == kSteps[1]) {
    pendingStep_ += panel == kSteps[1] ? 1 : -1;
    return lucent::format("ok: step {}", panel);
  }
  const int index = content_->panelOf(panel);
  pendingPanel_ = index;
  return lucent::format("ok: panel {} ({})", index, panel);
}

} // namespace psx::host
