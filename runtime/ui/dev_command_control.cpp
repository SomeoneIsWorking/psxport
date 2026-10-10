#include "dev_command_control.h"

#include "game.h"

#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>

namespace psx::ui {
namespace {

int wrap(int value, int dir, int max) {
  const int span = max + 1;
  return ((value + dir) % span + span) % span;
}

} // namespace

std::string DevCommandControl::send(const std::string &line) const {
  if (!mGame || !mGame->runtime) {
    return "not available";
  }
  const std::string verb = line.substr(0, line.find(' '));
  char *buffer = nullptr;
  size_t length = 0;
  FILE *out = open_memstream(&buffer, &length);
  if (!out) {
    return "not available";
  }
  const bool handled = mGame->runtime->controlCommand(mGame->core, verb.c_str(), line.c_str(), out);
  fclose(out);
  std::string reply(buffer ? buffer : "", length);
  free(buffer);
  while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) {
    reply.pop_back();
  }
  lucent::info("rmlui", "dev command '{}': {}", line, handled ? reply : "not handled by this game");
  return handled ? reply : "not supported by this game";
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
  mEntry = wrap(mEntry, dir, kMaxEntry);
}

void DevCommandControl::adjustFlagIndex(int dir) {
  mFlagIndex = wrap(mFlagIndex, dir, kMaxByte);
}

void DevCommandControl::adjustFlagValue(int dir) {
  mFlagValue = wrap(mFlagValue, dir, kMaxByte);
}

} // namespace psx::ui
