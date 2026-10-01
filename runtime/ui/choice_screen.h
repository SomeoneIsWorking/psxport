// psx::ui::ChoiceScreen — a full-window "choose one of N" screen over assets/rml/choice.rml.
//
// Title-neutral: the host supplies the heading, the entries and a hint line, and maps the returned
// index back to whatever it means. This is a SCREEN, not the ESC overlay: it is the picture of a
// product state in which no guest is running (a title picker), so it is recorded into the present
// image where a headless capture can see it, not over the window.
#ifndef PSXPORT_UI_CHOICE_SCREEN_H
#define PSXPORT_UI_CHOICE_SCREEN_H

#include "choice_navigator.h"
#include "choice_view.h"
#include "ui_component.h"

#include <optional>
#include <string>
#include <vector>

namespace Rml {
class Context;
class ElementDocument;
} // namespace Rml

namespace psx::ui {

class ChoiceScreen : public Component, public ChoiceView {
public:
  // `ctx` and `doc` are owned by the caller (RmlOverlay); destroy this before Rml::Shutdown().
  ChoiceScreen(Rml::Context *ctx, Rml::ElementDocument *doc, ChoiceContent content);
  ~ChoiceScreen() override;

  void show();
  void hide();
  bool visible() const {
    return mVisible;
  }

  bool move(int direction) override;
  std::optional<int> activate() const override {
    return mNavigator.activate();
  }
  int selectedIndex() const override {
    return mNavigator.selected();
  }
  const ChoiceNavigator &navigator() const {
    return mNavigator;
  }
  // An entry the pointer clicked since the last call, already highlighted. Clicking a disabled entry
  // yields nothing.
  std::optional<int> takeClick() override;

private:
  void refreshSelection();

  Rml::Context *mCtx = nullptr;
  Rml::ElementDocument *mDoc = nullptr;
  ChoiceNavigator mNavigator;
  std::vector<Rml::Element *> mRows;
  std::optional<int> mClicked;
  bool mVisible = false;
};

} // namespace psx::ui

#endif
