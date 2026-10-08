#pragma once

#include "title_availability.h"

#include <choice_view.h>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace psx::host {

// Entry i of `content()` is catalog entry i of `titles`; nothing else about a label is an
// identity. The panels are the catalog index of each available title, in panel order.
class PickerContent {
public:
  // `panelSlugs` restricts which available titles get a panel; empty means all of them. It changes
  // what is shown, never what can be started: `pick <slug>` still resolves any provisioned title.
  // `heading` is the product's name.
  PickerContent(std::string_view heading,
                std::span<const TitleAvailability> titles,
                std::span<const std::string> panelSlugs = {});

  const psx::ui::ChoiceContent &content() const {
    return content_;
  }
  const TitleAvailability &title(int index) const;
  // The title behind panel `panel`, or the same out_of_range refusal `title` gives.
  const TitleAvailability &panelTitle(int panel) const;
  // The panel showing this slug, or -1 when the slug names no panel (unknown, or not startable).
  int panelOf(std::string_view slug) const;
  int panelCount() const {
    return static_cast<int>(panels_.size());
  }
  // The available title with this slug, for `pick <slug>`.
  const TitleAvailability *findAvailable(std::string_view slug, std::string &refusal) const;
  std::string listing() const;

private:
  std::span<const TitleAvailability> titles_;
  std::vector<int> panels_; // catalog index of each panel, in panel order
  psx::ui::ChoiceContent content_;
};

} // namespace psx::host
