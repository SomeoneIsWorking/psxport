#pragma once

#include "game_runtime.h"
#include "picker_content.h"

#include <optional>
#include <string>

namespace psx::host {

class PickerComposite;

// The host's own session, in which no guest runs: the control channel is its job, which is the
// headless route to every decision the pad makes in the window. It also owns the screen's Game,
// the one that holds the control endpoint, reads the pad and records the screen text.
class PickerRuntime final : public GameRuntime {
public:
  PickerRuntime() = default;

  void bind(const PickerContent *content, PickerComposite *composite) {
    content_ = content;
    composite_ = composite;
    pendingSlug_.reset();
    pendingSelection_.reset();
  }
  // The slug `pick` accepted since the last call: the title to start, as opposed to the panel to
  // show.
  std::optional<std::string> takePick() {
    std::optional<std::string> pick = std::move(pendingSlug_);
    pendingSlug_.reset();
    return pick;
  }
  // The panel index is into the panels, which are the available titles in catalog order.
  std::optional<int> takeSelection() {
    std::optional<int> selection = pendingSelection_;
    pendingSelection_.reset();
    return selection;
  }

  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  // `line` is the whole command line, as the framework passes it.
  bool controlCommand(Core &core, const char *cmd, const char *line, FILE *out) override;

  // The command logic itself, which needs no Core: a hermetic test drives it directly.
  bool handle(const char *cmd, const char *line, FILE *out);
  // The panel count is the content's own, so `select left|right` cannot resolve a panel that does
  // not exist.
  void setSelection(int selected);

private:
  const PickerContent *content_ = nullptr;
  PickerComposite *composite_ = nullptr;
  std::optional<std::string> pendingSlug_;
  std::optional<int> pendingSelection_;
  int selected_ = 0;
};

} // namespace psx::host
