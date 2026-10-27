// psx::ui::MenuDocument — the overlay menu as a component tree, and the only thing that knows the
// DOM.
//
// A document object owns the loaded `ElementDocument`, holds the components built over it, and answers
// show/hide/update/focus/nav — so the code that manages RmlUi's LIFETIME (contexts, interfaces,
// fonts) never touches elements, and the code that manages ELEMENTS never touches RmlUi's lifetime.
//
// It adopts the shell `assets/rml/menu.rml` authors (Display, Debug, Game, About) and fills the Debug and About
// tabs from the title's debug menu (TitleDebugRows), so nothing here names a title's rows.
#ifndef PSXPORT_UI_MENU_DOCUMENT_H
#define PSXPORT_UI_MENU_DOCUMENT_H

#include "menu_pane.h"
#include "menu_readouts.h"
#include "menu_row.h"
#include "menu_tab_bar.h"
#include "title_debug_rows.h"
#include "ui_component.h"

#include <SDL3/SDL_keycode.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

class Game;
namespace psx::game {
struct TitleIntSetting;
}

namespace Rml {
class Context;
class ElementDocument;
} // namespace Rml

namespace psx::ui {

class MenuDocument : public Component, private RowBuilder {
public:
  // `ctx` and `doc` are owned by the caller (RmlOverlay); this object owns only the components it
  // builds over them, and must be destroyed before `Rml::Shutdown()`.
  MenuDocument(Rml::Context *ctx, Rml::ElementDocument *doc, Game *game);
  ~MenuDocument() override;

  void show();
  void hide();
  bool visible() const {
    return mVisible;
  }

  // Per-frame CPU step; refreshes nothing while hidden.
  void update() override;

  // Handle one menu key. Returns false when the key is not ours, so the caller can fall through
  // to RmlUi's SDL input translation (hover, text, wheel).
  bool handle_key(SDL_Keycode key);

  // ---- headless driving surface ---------------------------------------------------------------
  // AGENTS MAY NOT RUN WINDOWED (docs/workspace/PROTOCOL.md), and the menu is driven by SDL keyboard events
  // that do not exist without a window. Without these the entire UI is unreachable by every
  // instrument this project actually uses — which is the same class of blindness as the
  // windowed-only init that was already removed. They are a DRIVING surface, like the REPL's
  // press/tap: host UI state only, no guest state, no behaviour switch.
  void select_tab(int index) {
    if (mTabBar) {
      mTabBar->select(index);
    }
  }
  // Enumerate the whole menu as it currently stands: every tab, every pane, every row with its
  // kind, id, label and the text actually in the DOM. This is the instrument that answers "did
  // the restructure lose a row?" by MEASURING rather than by counting two files by hand.
  void dump() const;

  // ---- census, for the load-time report and for tests -----------------------------------------
  int tab_count() const {
    return mTabBar ? mTabBar->count() : 0;
  }
  int pane_count() const {
    return (int)mPanes.size();
  }
  int row_count() const;
  int readout_count() const {
    return mReadouts ? mReadouts->found() : 0;
  }
  int unknown_row_count() const {
    return mUnknownRows;
  }

private:
  // ---- RowBuilder ------------------------------------------------------------------------------
  std::unique_ptr<RowBinding> bind_row(Rml::Element *row) override;
  void on_row_clicked(MenuRow &row) override;

  void add_title_setting_rows(); // the title's declared settings, under #title_settings
  const psx::game::TitleIntSetting *find_title_setting(std::string_view id) const;
  void on_tab_selected(int index); // MenuTabBar's callback; call mTabBar->select() to change tab
  void focus_step(int dir);        // Down/Up, via RmlUi's own TAB navigation
  void activate_focused(int dir);
  MenuRow *focused_row() const;
  void run_action(const std::string &id);

  Rml::Context *mCtx = nullptr;
  Rml::ElementDocument *mDoc = nullptr;
  Game *mGame = nullptr;

  TitleDebugRows mDebugRows;
  MenuTabBar *mTabBar = nullptr;     // owned by Component::mChildren
  MenuReadouts *mReadouts = nullptr; // owned by Component::mChildren
  std::vector<MenuPane *> mPanes;    // owned by Component::mChildren
  bool mVisible = false;
  int mUnknownRows = 0;
};

} // namespace psx::ui

#endif
