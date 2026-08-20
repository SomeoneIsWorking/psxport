// psx::ui::MenuRow — ONE focusable `<select-button>` row of the overlay menu, and the bindings
// that say what it is wired to.
//
// SHAPE FROM DUSKLIGHT (CC0), `src/dusk/ui/select_button.{hpp,cpp}`: a row is a component that owns
// its `<key>`/`<value>` elements and its own listener, and it asks a BINDING for its display text
// rather than holding the state itself (their `ControlledSelectButton` takes a `getValue` /
// `isDisabled` / `isModified` triple for exactly this reason). Ours is a small virtual instead of
// three `std::function`s because our four row kinds are a closed set that lives in this file, not
// an open extension point callers configure.
//
// WHY A BINDING AT ALL, rather than a `kind` enum and a switch: the four kinds have four different
// MODELS behind them — a `Mods` toggle, a `Mods` float, the `WarpControl` area selector, and a
// one-shot action routed back to the document. A switch would put all four models' includes into
// the row widget and reintroduce exactly the ladder this split removes.
#ifndef PSXPORT_UI_MENU_ROW_H
#define PSXPORT_UI_MENU_ROW_H

#include "mod_row_model.h"
#include "ui_component.h"

#include <functional>
#include <memory>
#include <string>

class Mods;

namespace psx::ui {

class WarpControl;
class RenderPathControl;

// ---- what a row is wired to -----------------------------------------------------------------------
class RowBinding {
public:
  virtual ~RowBinding() = default;

  // Current display text. `false` means "leave the authored placeholder alone" — an action row
  // has no state to show, and its `<value>` in menu.rml is a glyph the document chose.
  virtual bool text(std::string &out) const {
    (void)out;
    return false;
  }

  // Enter / A / click use dir = +1; Left and Right use -1 / +1 on a row that steps with arrows.
  virtual void step(int dir) = 0;

  // True when Left/Right should step this row. False rows let Left/Right fall through to the tab
  // bar, which is how a toggle row's arrows change TAB rather than the value — existing
  // behaviour, preserved deliberately.
  virtual bool steps_with_arrows() const {
    return false;
  }
};

std::unique_ptr<RowBinding> make_mod_toggle_binding(Mods *mods, std::string id);
std::unique_ptr<RowBinding> make_mod_adjust_binding(Mods *mods, std::string id);
std::unique_ptr<RowBinding> make_warp_area_binding(WarpControl *warp);
std::unique_ptr<RowBinding> make_render_path_binding(RenderPathControl *render_path);
std::unique_ptr<RowBinding> make_action_binding(std::function<void()> action);

// ---- the row widget -------------------------------------------------------------------------------
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

  // One line describing this row AS IT CURRENTLY IS — its authored kind/id, its label, and the
  // text actually in the DOM (decoded, i.e. what the font would draw). This is what makes the
  // menu enumerable from the REPL without a window, so "did the restructure lose a row?" is a
  // question a headless run answers rather than one a human answers by reading two files.
  std::string describe() const;

private:
  std::unique_ptr<RowBinding> mBinding;
  std::function<void(MenuRow &)> mOnClick;
  Rml::Element *mValue = nullptr;
};

} // namespace psx::ui

#endif
