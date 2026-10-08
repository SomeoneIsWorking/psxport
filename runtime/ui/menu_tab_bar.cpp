#include "menu_tab_bar.h"

#include <utility>

namespace psx::ui {

MenuTab::MenuTab(Rml::Element *root, std::function<void()> on_click) : Component(root) {
  listen(mRoot, Rml::EventId::Click, [cb = std::move(on_click)](Rml::Event &) {
    if (cb) {
      cb();
    }
  });
}

MenuTabBar::MenuTabBar(Rml::Element *root, std::function<void(int)> on_select)
    : Component(root), mOnSelect(std::move(on_select)) {
  if (!mRoot) {
    return;
  }
  Rml::ElementList tabs;
  mRoot->GetElementsByTagName(tabs, "tab");
  for (int i = 0; i < (int)tabs.size(); i++) {
    MenuTab &tab = adopt<MenuTab>(tabs[i], [this, i] {
      select(i);
    });
    mTabs.push_back(&tab);
  }
}

void MenuTabBar::select(int index) {
  const int n = count();
  if (n == 0) {
    return;
  }
  if (index < 0) {
    index = n - 1;
  } else if (index >= n) {
    index = 0;
  }
  mActive = index;
  // Deliberately NOT guarded on `index == mActive`. This is the single place tab state is
  // applied, so the very first call — select(0) at construction, where mActive is already 0 —
  // must still stamp the pseudo-class and run the callback. An early-out here would leave the
  // opening tab styled as unselected and its pane hidden, which is precisely the bug an
  // "optimisation" of this shape would introduce.
  for (int i = 0; i < n; i++) {
    mTabs[i]->set_selected(i == index);
  }
  if (mOnSelect) {
    mOnSelect(index);
  }
}

} // namespace psx::ui
