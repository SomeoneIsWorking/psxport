#include "warp_control.h"

#include "dev_command_control.h"
#include "game.h"

#include <cstdio>

namespace psx::ui {

psx::game::TitleDevWarp *WarpControl::titleWarp() const {
  return mGame ? mGame->runtime.devWarp(mGame->core) : nullptr;
}

int WarpControl::areaCount() const {
  const psx::game::TitleDevWarp *warp = titleWarp();
  return warp ? warp->areaCount() : 0;
}

std::string WarpControl::areaLabel(int area) const {
  const psx::game::TitleDevWarp *warp = titleWarp();
  const char *nm = warp ? warp->areaName(area) : nullptr;
  char b[96];
  if (nm && *nm) {
    snprintf(b, sizeof b, "%d - %s", area, nm);
  } else {
    snprintf(b, sizeof b, "Area %d", area);
  }
  return b;
}

void WarpControl::adjust(int dir) {
  const int n = areaCount();
  if (n <= 0) {
    return;
  }
  mArea = (mArea + dir) % n;
  if (mArea < 0) {
    mArea += n; // wrap both ways, so Left from 0 reaches the last area
  }
}

std::string WarpControl::arm() {
  const psx::game::TitleDevWarp *warp = titleWarp();
  if (!warp || warp->areaCount() <= 0) {
    return "warp unavailable";
  }
  if (!warp->warpAllowed()) {
    return "not in the field - reach gameplay first";
  }
  return mCommands->warp(mArea);
}

} // namespace psx::ui
