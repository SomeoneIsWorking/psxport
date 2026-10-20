#include "dev_command_control.h"

#include "command_args.h"
#include "game.h"
#include "title_command.h"

#include <lucent/log.h>

namespace psx::ui {
namespace {

template <int Max> int wrap(int value, int dir) {
  constexpr int span = Max + 1;
  return ((value + dir) % span + span) % span;
}

} // namespace

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
  mEntry = wrap<kMaxEntry>(mEntry, dir);
}

void DevCommandControl::adjustFlagIndex(int dir) {
  mFlagIndex = wrap<kMaxByte>(mFlagIndex, dir);
}

void DevCommandControl::adjustFlagValue(int dir) {
  mFlagValue = wrap<kMaxByte>(mFlagValue, dir);
}

} // namespace psx::ui
