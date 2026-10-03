#include "choice_screen.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/StringUtilities.h>
#include <lucent/log.h>

#include <utility>

namespace psx::ui {

namespace {
Rml::Element *appendChild(Rml::Element *parent, const char *tag) {
  Rml::ElementPtr child = parent->GetOwnerDocument()->CreateElement(tag);
  return parent->AppendChild(std::move(child));
}

// A property value written in the stylesheet's own syntax.
void set_property(Rml::Element *element, const char *name, const Rml::String &value) {
  element->SetProperty(name, value);
}

// A length in RmlUi's pixel unit. "dp" is device pixels — the sink's own coordinate space, which is
// what a host's band is expressed in and what stays true whatever the font scale is.
Rml::String bandPixels(int value) {
  Rml::String out;
  Rml::FormatString(out, "%ddp", value);
  return out;
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
  mList = list;
  mBand = doc->GetElementById("panel");
  // A screen is the WHOLE picture unless its host says otherwise: opaque backdrop, list shown.
  setBackdropOpaque(true);
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

bool ChoiceScreen::select(int index) {
  if (!mNavigator.select(index)) {
    return false;
  }
  refreshSelection();
  return true;
}

void ChoiceScreen::setHeading(const std::string &text) {
  set_text(mDoc->GetElementById("heading"), text);
  mCtx->Update();
}

void ChoiceScreen::setEntriesVisible(bool visible) {
  if (mList == nullptr) {
    return;
  }
  // A pseudo-class rather than a direct style write: the shape of this screen lives in its
  // stylesheet, where every other rule about it lives, and the two states are two readable rules.
  mList->SetPseudoClass("hidden", !visible);
  mCtx->Update();
}

void ChoiceScreen::setBackdropOpaque(bool opaque) {
  // <body> is the document's own root element and carries the id the stylesheet's `body` rules match.
  if (Rml::Element *body = mDoc->GetElementById("body")) {
    body->SetPseudoClass("opaque", opaque);
    mCtx->Update();
  }
}

void ChoiceScreen::setCaptionBand(int x, int y, int width, int height) {
  // The <panel> that holds the heading and the hint is positioned, not restyled: the host knows the
  // band's pixels and they change every frame of a width animation, so a stylesheet cannot own this.
  // The `banded` pseudo-class is what shrinks the type — a caption is a label under a picture, not a
  // title over one — and the two states stay two readable rules in choice.rcss.
  if (mBand == nullptr || width <= 0 || height <= 0) {
    return;
  }
  mBand->SetPseudoClass("banded", true);
  mBand->SetProperty("position", "absolute");
  // "dp" is RmlUi's pixel unit: it is the sink's own coordinate space, which is what the host's
  // band is expressed in, and it is not affected by any font-size scaling.
  set_property(mBand, "left", bandPixels(x));
  set_property(mBand, "top", bandPixels(y));
  set_property(mBand, "width", bandPixels(width));
  mCtx->Update();
}

void ChoiceScreen::refreshSelection() {
  for (int i = 0; i < static_cast<int>(mRows.size()); ++i) {
    mRows[i]->SetPseudoClass("selected", i == mNavigator.selected());
  }
}

} // namespace psx::ui
