// psx::ui::MenuRow — one focusable `<select-button>` row of the overlay menu and the binding it shows and steps.
#ifndef PSXPORT_UI_MENU_ROW_H
#define PSXPORT_UI_MENU_ROW_H

#include "ui_component.h"

#include <functional>
#include <memory>
#include <string>

class Mods;
namespace psx::game {
struct TitleIntSetting;
}

namespace psx::ui {

class WarpControl;
class DevCommandControl;

// What a row is wired to: a Mods toggle, a title setting, a Debug-tab selection or an action.
class RowBinding {
public:
  virtual ~RowBinding() = default;

  // False removes the authored row from both layout and navigation. This is a declared capability
  // absence, distinct from a null/unknown binding, which remains visible and is reported as inert.
  virtual bool available() const {
    return true;
  }

  // Current display text; false keeps the authored `<value>`, as an action row does.
  virtual bool text(std::string &out) const {
    (void)out;
    return false;
  }

  // Enter / A / click use dir = +1; Left and Right use -1 / +1 on a row that steps with arrows.
  virtual void step(int dir) = 0;

  // False lets Left/Right fall through to the tab bar, as a toggle row's arrows do.
  virtual bool steps_with_arrows() const {
    return false;
  }
};

std::unique_ptr<RowBinding> make_mod_toggle_binding(Mods *mods, std::string id);
std::unique_ptr<RowBinding> make_warp_area_binding(WarpControl *warp);
// A title's declared integer setting: shows the value and unit, steps within the declared range and persists.
std::unique_ptr<RowBinding> make_title_setting_binding(const psx::game::TitleIntSetting *setting, Mods *mods);
// `warp_entry`, `flag_index` and `flag_value`: the numeric selections behind the developer commands.
std::unique_ptr<RowBinding> make_dev_command_binding(DevCommandControl *commands, std::string id);
// `available` (optional) is asked when the row is built: false removes the row, as a declared capability
// absence (a product with no title selector has no "return to selection" row).
std::unique_ptr<RowBinding> make_action_binding(std::function<void()> action, std::function<bool()> available = {});

class MenuRow : public Component {
public:
  // `root` is the authored `<select-button>`; `on_click` is invoked before the binding steps, so
  // the document can move focus to the clicked row exactly as the keyboard path does.
  MenuRow(Rml::Element *root, std::unique_ptr<RowBinding> binding, std::function<void(MenuRow &)> on_click);

  // Refresh the `<value>` text from the binding. Called by the base's per-frame walk.
  void update() override;

  void step(int dir);
  bool steps_with_arrows() const {
    return mBinding && mBinding->steps_with_arrows();
  }

  // The row as it stands: authored kind and id, label, and the decoded text in the DOM.
  std::string describe() const;

private:
  std::unique_ptr<RowBinding> mBinding;
  std::function<void(MenuRow &)> mOnClick;
  Rml::Element *mValue = nullptr;
};

} // namespace psx::ui

#endif
