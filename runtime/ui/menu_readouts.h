// psx::ui::MenuReadouts — the menu's live status lines: the video line, the debug status line and the
// readouts the title's debug menu declares.
//
// A readout is a `<div id="…">` the document holds; an absent one is skipped, and the construction report
// says how many were found.
#ifndef PSXPORT_UI_MENU_READOUTS_H
#define PSXPORT_UI_MENU_READOUTS_H

#include "ui_component.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

class Game;

namespace psx::ui {

class MenuReadouts : public Component {
public:
  // `root` is the document body; `game` may be null (the readouts then show their static text).
  MenuReadouts(Rml::Element *root, Game *game);

  // Refresh the video line and the title's readouts from live state. Called by the base's per-frame walk.
  void update() override;

  // The Debug tab's status line: the last verb's reply.
  void set_status(const std::string &text);

  int found() const {
    return mFound;
  }

private:
  Game *mGame = nullptr;
  Rml::Element *mVideo = nullptr;
  Rml::Element *mStatus = nullptr;
  // Each title readout's element and its index in the debug menu.
  std::vector<std::pair<Rml::Element *, std::size_t>> mTitle;
  int mFound = 0;
};

} // namespace psx::ui

#endif
