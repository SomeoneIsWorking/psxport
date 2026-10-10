// psx::ui::DevCommandControl — the Debug tab's developer commands: warp entry, grant all items, event flags.
//
// The framework knows nothing about any title's warp, items or flags. Each command is a line on the title's
// control channel (GameRuntime::controlCommand, the same surface a client sends `warp 3 2` to); the title
// parses it, arms it, and writes it at its next frame boundary. This class holds only the current selection,
// which is deliberately not persisted, and returns the title's reply for the readout.
#ifndef PSXPORT_UI_DEV_COMMAND_CONTROL_H
#define PSXPORT_UI_DEV_COMMAND_CONTROL_H

#include <string>

class Game;

namespace psx::ui {

class DevCommandControl {
public:
  inline static constexpr int kMaxEntry = 63;
  inline static constexpr int kMaxByte = 255;

  explicit DevCommandControl(Game *game) : mGame(game) {}

  // Sends one control-channel line and returns the title's one-line reply.
  std::string send(const std::string &line) const;

  std::string warp(int area) const;
  std::string grantAllItems() const;
  std::string readFlag() const;
  std::string writeFlag() const;

  int entry() const {
    return mEntry;
  }
  int flagIndex() const {
    return mFlagIndex;
  }
  int flagValue() const {
    return mFlagValue;
  }
  void adjustEntry(int dir);
  void adjustFlagIndex(int dir);
  void adjustFlagValue(int dir);

private:
  Game *mGame = nullptr;
  int mEntry = 0;
  int mFlagIndex = 0;
  int mFlagValue = 0;
};

} // namespace psx::ui

#endif
