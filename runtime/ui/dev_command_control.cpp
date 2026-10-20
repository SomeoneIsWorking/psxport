#include "dev_command_control.h"

#include "command_args.h"
#include "game.h"
#include "title_command.h"
#include "wrap.h"

#include <lucent/log.h>

namespace psx::ui {

std::string DevCommandControl::send(const std::string &line) const {
  if (!mGame) {
    return "not available";
  }
  const psx::game::TitleCommandReply reply = psx::game::runTitleCommand(mGame->core, psx::debug::CommandArgs(line));
  lucent::info("rmlui", "dev command '{}': {}", line, reply.handled ? reply.text : "not handled by this game");
  return reply.handled ? reply.text : "not supported by this game";
}

std::string DevCommandControl::warp(int area) const {
  return send("warp " + std::to_string(area) + " " + std::to_string(mEntry));
}

std::string DevCommandControl::grantAllItems() const {
  return send("items all");
}

std::string DevCommandControl::readFlag() const {
  return send("flag get " + std::to_string(mFlagIndex));
}

std::string DevCommandControl::writeFlag() const {
  return send("flag set " + std::to_string(mFlagIndex) + " " + std::to_string(mFlagValue));
}

void DevCommandControl::adjustEntry(int dir) {
  mEntry = wrap(mEntry, dir, kMaxEntry + 1);
}

void DevCommandControl::adjustFlagIndex(int dir) {
  mFlagIndex = wrap(mFlagIndex, dir, kMaxByte + 1);
}

void DevCommandControl::adjustFlagValue(int dir) {
  mFlagValue = wrap(mFlagValue, dir, kMaxByte + 1);
}

} // namespace psx::ui
