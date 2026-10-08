// psx::ui::ChoiceNavigator — which entry of a "choose one" list is highlighted, and whether it may be
// taken. No RmlUi, no SDL, no Game: the rules a title-selection screen must obey, reachable from a
// hermetic test.
//
// An entry is DISABLED when the product cannot honour it (a title whose executable is not provisioned,
// say). A disabled entry stays visible, carrying its reason, because hiding it would answer "why can I
// not pick Spyro 2?" with silence. It is never highlighted by navigation and never activatable.
#ifndef PSXPORT_UI_CHOICE_NAVIGATOR_H
#define PSXPORT_UI_CHOICE_NAVIGATOR_H

#include <optional>
#include <string>
#include <vector>

namespace psx::ui {

struct ChoiceEntry {
  std::string label;  // what the player reads; NEVER an identity — the host maps an index back to one
  std::string detail; // secondary line (e.g. the serial) shown dimmed
  bool enabled = true;
  std::string reason; // why it is disabled; shown instead of `detail` when !enabled
};

class ChoiceNavigator {
public:
  explicit ChoiceNavigator(std::vector<ChoiceEntry> entries);

  // The highlighted index, or -1 when no entry is enabled.
  int selected() const {
    return mSelected;
  }
  int enabledCount() const;
  const std::vector<ChoiceEntry> &entries() const {
    return mEntries;
  }

  // Step to the next (+1) or previous (-1) ENABLED entry, wrapping. Returns whether the highlight moved.
  bool move(int direction);
  // Highlight `index` directly (pointer click). Refuses an out-of-range or disabled index.
  bool select(int index);
  // The highlighted index when it may be taken, otherwise nothing.
  std::optional<int> activate() const;

private:
  std::vector<ChoiceEntry> mEntries;
  int mSelected = -1;
};

} // namespace psx::ui

#endif
