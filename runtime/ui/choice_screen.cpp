#include "choice_screen.h"

#include <RmlUi/Core.h>
#include <lucent/log.h>

#include <utility>

namespace psx::ui {

namespace {
Rml::Element *appendChild(Rml::Element *parent, const char *tag) {
  Rml::ElementPtr child = parent->GetOwnerDocument()->CreateElement(tag);
  return parent->AppendChild(std::move(child));
}
} // namespace

ChoiceScreen::ChoiceScreen(Rml::Context *ctx, Rml::ElementDocument *doc, ChoiceContent content)
    : Component(doc), mCtx(ctx), mDoc(doc), mNavigator(std::move(content.entries)) {
  set_text(doc->GetElementById("heading"), content.heading);
  set_text(doc->GetElementById("hint"), content.hint);
  Rml::Element *list = doc->GetElementById("list");
  if (list == nullptr) {
    lucent::error("rmlui", "choice.rml has no #list element; the choice screen will be empty");
    return;
  }
  const std::vector<ChoiceEntry> &entries = mNavigator.entries();
  for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
    Rml::Element *row = appendChild(list, "entry");
    Rml::Element *name = appendChild(row, "name");
    Rml::Element *note = appendChild(row, "note");
    set_text(name, entries[i].label);
    set_text(note, entries[i].enabled ? entries[i].detail : entries[i].reason);
    if (!entries[i].enabled) {
      row->SetPseudoClass("disabled", true);
    }
    listen(row, Rml::EventId::Click, [this, i](Rml::Event &) {
      if (mNavigator.select(i)) {
        mClicked = i;
        refreshSelection();
      }
    });
    mRows.push_back(row);
  }
  refreshSelection();
}

ChoiceScreen::~ChoiceScreen() = default;

void ChoiceScreen::show() {
  mVisible = true;
  mDoc->Show();
  mCtx->Update();
}

void ChoiceScreen::hide() {
  mVisible = false;
  mDoc->Hide();
}

bool ChoiceScreen::move(int direction) {
  const bool moved = mNavigator.move(direction);
  if (moved) {
    refreshSelection();
  }
  return moved;
}

std::optional<int> ChoiceScreen::takeClick() {
  return std::exchange(mClicked, std::nullopt);
}

void ChoiceScreen::refreshSelection() {
  for (int i = 0; i < static_cast<int>(mRows.size()); ++i) {
    mRows[i]->SetPseudoClass("selected", i == mNavigator.selected());
  }
}

} // namespace psx::ui
