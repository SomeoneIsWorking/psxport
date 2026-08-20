#include "menu_pane.h"

namespace psx::ui {

MenuPane::MenuPane(Rml::Element *root, RowBuilder &builder) : Component(root) {
  if (!mRoot) {
    return;
  }
  Rml::ElementList rows;
  mRoot->GetElementsByTagName(rows, "select-button");
  for (Rml::Element *r : rows) {
    MenuRow &row = adopt<MenuRow>(r, builder.bind_row(r), [&builder](MenuRow &clicked) {
      builder.on_row_clicked(clicked);
    });
    mRows.push_back(&row);
  }
}

void MenuPane::update() {
  if (!shown()) {
    return;
  }
  Component::update();
}

void MenuPane::set_shown(bool shown) {
  if (!mRoot || mRoot->IsPseudoClassSet("shown") == shown) {
    return;
  }
  mRoot->SetPseudoClass("shown", shown);
}

bool MenuPane::focus_first_row() {
  for (MenuRow *r : mRows) {
    if (r->focus()) {
      return true;
    }
  }
  return false;
}

MenuRow *MenuPane::row_containing(Rml::Element *element) const {
  if (!element) {
    return nullptr;
  }
  for (MenuRow *r : mRows) {
    if (r->contains(element)) {
      return r;
    }
  }
  return nullptr;
}

} // namespace psx::ui
