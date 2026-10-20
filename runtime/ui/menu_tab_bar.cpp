#include "menu_tab_bar.h"

#include "wrap.h"

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
  index = wrap(index, 0, n);
  mActive = index;
  // No early-out on index == mActive: the construction-time select(0) must still style the tab.
  for (int i = 0; i < n; i++) {
    mTabs[i]->set_selected(i == index);
  }
  if (mOnSelect) {
    mOnSelect(index);
  }
}

} // namespace psx::ui
