// psx::ui::ChoiceView — what a host needs from a choice screen: its content and the four questions it asks
// each frame. Pure (no RmlUi), so a title's host code can drive a screen without including the UI library.
#ifndef PSXPORT_UI_CHOICE_VIEW_H
#define PSXPORT_UI_CHOICE_VIEW_H

#include "choice_navigator.h"

#include <optional>
#include <string>
#include <vector>

namespace psx::ui {

struct ChoiceContent {
  std::string heading;
  std::string hint;
  std::vector<ChoiceEntry> entries;
};

class ChoiceView {
public:
  virtual ~ChoiceView() = default;

  // Step the highlight to the next (+1) or previous (-1) enabled entry; whether it moved.
  virtual bool move(int direction) = 0;
  // Highlight `index` directly, refusing an out-of-range or disabled one. This is how a host that moved
  // the selection ITSELF — through its control channel, or a pointer over a picture instead of a row —
  // tells the screen where it is, so the screen's own highlight and the host's never disagree.
  virtual bool select(int index) = 0;
  // The highlighted entry when it may be taken.
  virtual std::optional<int> activate() const = 0;
  virtual int selectedIndex() const = 0;
  // An entry the pointer clicked since the last call, already highlighted. A disabled entry yields nothing.
  virtual std::optional<int> takeClick() = 0;
  // The shape of the screen, for a host whose entries are pictures rather than a text list: the heading
  // follows the selection, the list can be dropped, and the backdrop can go transparent so the picture
  // the host composited behind this screen is visible. A screen that IS the whole picture keeps all
  // three as they are.
  virtual void setHeading(const std::string &heading) = 0;
  virtual void setEntriesVisible(bool visible) = 0;
  virtual void setBackdropOpaque(bool opaque) = 0;
  // Move the heading and the hint into a BAND — a rectangle in surface pixels, usually the bottom of
  // whatever the host composited behind this screen — instead of the middle of the screen.
  //
  // A host whose entries are PICTURES (a title picker whose panels are live sessions) has a heading
  // that covers the game when it is centred: the text lands on the middle of every panel. In a band
  // under its own panel it names the picture it belongs to and leaves the game visible, and because
  // the band is given in pixels per frame it follows the panel while the widths animate. A screen that
  // IS the whole picture keeps the centred layout.
  virtual void setCaptionBand(int x, int y, int width, int height) = 0;
};

} // namespace psx::ui

#endif
