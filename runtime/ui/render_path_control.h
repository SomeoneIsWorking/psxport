// psx::ui::RenderPathControl — the RmlUi renderer selector's live state and action.
#ifndef PSXPORT_UI_RENDER_PATH_CONTROL_H
#define PSXPORT_UI_RENDER_PATH_CONTROL_H

#include <string>

class Game;

namespace psx::ui {

class RenderPathControl {
public:
  explicit RenderPathControl(Game *game) : mGame(game) {}

  std::string currentLabel() const;
  void cycle();

private:
  Game *mGame = nullptr;
};

} // namespace psx::ui

#endif
