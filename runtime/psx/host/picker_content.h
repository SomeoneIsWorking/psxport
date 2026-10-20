#pragma once

#include "title_availability.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psx::host {

// The probed catalog and which of its available titles get a panel, in panel order: the one owner of
// the panel <-> title mapping.
class PickerContent {
public:
  // `panelSlugs` restricts which available titles get a panel; empty means all of them. It changes
  // what is shown, never what can be started: `pick <slug>` still resolves any provisioned title.
  explicit PickerContent(std::span<const TitleAvailability> titles, std::span<const std::string_view> panelSlugs = {});

  const TitleAvailability &title(int index) const;
  // The title behind panel `panel`; out_of_range outside the panels.
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
};

} // namespace psx::host
