// psx::ui::RenderPathControl — the RmlUi renderer selector's live state and action.
#ifndef PSXPORT_UI_RENDER_PATH_CONTROL_H
#define PSXPORT_UI_RENDER_PATH_CONTROL_H

#include "render_mode.h"

#include <string>

class Game;

namespace psx::ui {

// The player menu deliberately excludes RenderPath::Psx. That software-rasterized path is retained
// for oracle and diagnostic runs, but it is not a supported live gameplay renderer. If a diagnostic
// run opens the menu while already on it, the next activation returns to the shipping renderer.
RenderPath player_render_path_next(RenderPath current);

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
