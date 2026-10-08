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
  bool select(int index) override;
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

  // Re-word the screen's heading. A host whose screen shows what is currently SELECTED (a picker
  // whose rows are pictures rather than a text list) needs the heading to follow the selection, and
  // the heading is written once in the constructor.
  void setHeading(const std::string &text) override;
  // Show or hide the ENTRY LIST. A host whose entries are pictures — a selector that composites each
  // title's own attract demo into a panel (psxport::PaneCompositor) — keeps the heading and the
  // hint and drops the rows, which would otherwise repeat the panels as text over them.
  void setEntriesVisible(bool visible) override;
  // Whether the screen paints its own opaque backdrop. A screen drawn over a host-composited picture
  // must not: the picture behind it IS the content, and an opaque body would hide every panel while
  // still being the correct answer for a screen that is the whole picture on its own.
  void setBackdropOpaque(bool opaque) override;
  // Move the heading and the hint into a band of these surface pixels. See ChoiceView::setCaptionBand.
  void setCaptionBand(int x, int y, int width, int height) override;

private:
  void refreshSelection();

  Rml::Context *mCtx = nullptr;
  Rml::ElementDocument *mDoc = nullptr;
  Rml::Element *mList = nullptr; // the #list, kept so its visibility is a host decision
  Rml::Element *mBand = nullptr; // the #panel, repositioned into a caption band when the host asks
  ChoiceNavigator mNavigator;
  std::vector<Rml::Element *> mRows;
  std::optional<int> mClicked;
  bool mVisible = false;
};

} // namespace psx::ui

#endif
