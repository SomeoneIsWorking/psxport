// psx::ui::WarpControl — the Debug tab's dev AREA WARP: destination selection + arming.
//
// The framework knows NOTHING about areas. How many there are, what they are called, and whether a
// warp is legal right now all come from the game through GameHooks (`devAreaCount` / `devAreaName`
// / `devWarpAllowed`); this class holds only the current selection, which is deliberately not
// persisted. Arming sends the same control-channel `warp <area> <entry>` line a client sends, through
// DevCommandControl, so there is one warp mechanism, not a second one hiding behind a button.
//
// NAMING RULE (docs/areas.md): an area INDEX is a fact, an area NAME is a claim that needs a
// source. An area the game has no name for is shown as "Area 12", never as a guess.
#ifndef PSXPORT_UI_WARP_CONTROL_H
#define PSXPORT_UI_WARP_CONTROL_H

#include <string>

class Game;

namespace psx::ui {
class DevCommandControl;
}

namespace psx::ui {

class WarpControl {
public:
  WarpControl(Game *game, const DevCommandControl *commands) : mGame(game), mCommands(commands) {}

  int areaCount() const;
  std::string areaLabel(int area) const;
  std::string currentLabel() const {
    return areaLabel(mArea);
  }
  void adjust(int dir);

  // Arm the warp and return the status line for the readout. The GAME decides whether a warp is
  // legal: it is only legal from the field, because the warp runs the game's OWN door transition
  // (fade, teardown, CD settle, reload) and that needs a running field machine to carry it out.
  std::string arm();

private:
  Game *mGame = nullptr;
  const DevCommandControl *mCommands = nullptr;
  int mArea = 0;
};

} // namespace psx::ui

#endif
