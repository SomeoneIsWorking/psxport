// psx::ui::ChoiceContent — a choose-one list's heading, hint and entries. Pure (no RmlUi).
#ifndef PSXPORT_UI_CHOICE_CONTENT_H
#define PSXPORT_UI_CHOICE_CONTENT_H

#include "choice_navigator.h"

#include <string>
#include <vector>

namespace psx::ui {

struct ChoiceContent {
  std::string heading;
  std::string hint;
  std::vector<ChoiceEntry> entries;
};

} // namespace psx::ui

#endif
