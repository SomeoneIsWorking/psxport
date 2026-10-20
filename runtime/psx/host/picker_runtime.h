#pragma once

#include "game_runtime.h"
#include "picker_content.h"

#include <optional>
#include <string>

namespace psx::host {

// The selector host's runtime, in which no guest runs: its control channel is the headless route to
// every decision the pad makes in the window. Commands only record a request; the selector applies it.
class PickerRuntime final : public psx::game::GameRuntime {
public:
  PickerRuntime() = default;

  void bind(const PickerContent *content) {
    content_ = content;
    pendingSlug_.reset();
    pendingPanel_.reset();
    pendingStep_ = 0;
  }
  // The slug `pick` accepted since the last call: the title to start.
  std::optional<std::string> takePick() {
    std::optional<std::string> pick = std::move(pendingSlug_);
    pendingSlug_.reset();
    return pick;
  }
  // The panel `select <slug>` named since the last call.
  std::optional<int> takePanel() {
    std::optional<int> panel = pendingPanel_;
    pendingPanel_.reset();
    return panel;
  }
  // The steps `select left|right` asked for since the last call.
  int takeStep() {
    const int step = pendingStep_;
    pendingStep_ = 0;
    return step;
  }

  void registerOverrides(Game &) override {}
  bool titleCommand(Core &core, const psx::game::TitleCommandLine &input, FILE *out) override;

private:
  const PickerContent *content_ = nullptr;
  std::optional<std::string> pendingSlug_;
  std::optional<int> pendingPanel_;
  int pendingStep_ = 0;
};

} // namespace psx::host
