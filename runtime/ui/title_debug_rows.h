// psx::ui::TitleDebugRows — the Debug and About tabs built from the title's debug menu: a selection per verb
// parameter, a run row per verb, and the About lines. Running a verb sends the line its selections spell
// through the same verb table the control channel uses.
#ifndef PSXPORT_UI_TITLE_DEBUG_ROWS_H
#define PSXPORT_UI_TITLE_DEBUG_ROWS_H

#include "title_debug_menu.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Game;

namespace psx::ui {

class RowBinding;

class TitleDebugRows {
public:
  // `status` receives each run's reply.
  TitleDebugRows(Game *game, std::function<void(const std::string &)> status);

  // The rows for `#title_debug`: section headings, readout lines, parameter and run rows.
  std::string debugMarkup() const;
  // The lines for `#about`: product, title, serial, build, then the title's own lines.
  std::string aboutMarkup() const;
  // The binding for a `debug:` row id; null when the id is not one of these rows.
  std::unique_ptr<RowBinding> bind(std::string_view id);

  // The element id of readout `index`.
  static std::string readoutId(std::size_t index);

private:
  // One parameter's selection; absent only for an optional parameter.
  struct Selection {
    std::int64_t value = 0;
    bool present = true;
  };
  using VerbSelections = std::array<Selection, psx::game::kMaxDebugParams>;

  friend class DebugParamBinding;
  friend class DebugRunBinding;

  const psx::game::TitleDebugMenu *menu() const;
  std::string valueText(std::size_t verb, std::size_t param) const;
  void step(std::size_t verb, std::size_t param, int dir);
  // The control line the verb's selections spell; parameters after an absent one are left out.
  std::string line(std::size_t verb) const;
  void run(std::size_t verb);
  bool available(std::size_t verb) const;

  Game *mGame = nullptr;
  std::function<void(const std::string &)> mStatus;
  std::vector<VerbSelections> mSelections;
};

} // namespace psx::ui

#endif
