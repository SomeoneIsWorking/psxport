// psx::ui::MenuTabBar — the row of `<tab>`s, and which one is selected.
//
// The tab bar owns its tabs and selection. Selecting a tab runs that tab's callback rather than the
// tab bar reaching across the document to show a pane. `MenuTab` is private to this ownership
// boundary because a tab has no life outside its bar.
//
// WHAT IT REPLACES: a heap `std::vector<std::unique_ptr<TabClick>>` behind a `void*` member of the
// overlay, with one `TabClick` listener class per tab holding a raw `RmlOverlay*` and an index, and
// no `RemoveEventListener` anywhere. Selection lived in an `int mActiveTab` on the overlay and was
// applied by re-querying the document for every `<tab>` and every `<pane>` on each change.
#ifndef PSXPORT_UI_MENU_TAB_BAR_H
#define PSXPORT_UI_MENU_TAB_BAR_H

#include "ui_component.h"

#include <functional>
#include <vector>

namespace psx::ui {

class MenuTab : public Component {
public:
  MenuTab(Rml::Element *root, std::function<void()> on_click);
};

class MenuTabBar : public Component {
public:
  // `root` is the authored `<tab-bar>`; `on_select` is called with the new index whenever the
  // selection changes, including the initial one.
  MenuTabBar(Rml::Element *root, std::function<void(int)> on_select);

  int count() const {
    return (int)mTabs.size();
  }
  int active() const {
    return mActive;
  }

  // Select `index`, wrapping at both ends (so Left from the first tab reaches the last). No-op
  // when there are no tabs.
  void select(int index);

private:
  std::vector<MenuTab *> mTabs;
  std::function<void(int)> mOnSelect;
  int mActive = 0;
};

} // namespace psx::ui

#endif
