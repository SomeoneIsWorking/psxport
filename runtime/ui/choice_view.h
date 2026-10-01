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
  // The highlighted entry when it may be taken.
  virtual std::optional<int> activate() const = 0;
  virtual int selectedIndex() const = 0;
  // An entry the pointer clicked since the last call, already highlighted. A disabled entry yields nothing.
  virtual std::optional<int> takeClick() = 0;
};

} // namespace psx::ui

#endif
