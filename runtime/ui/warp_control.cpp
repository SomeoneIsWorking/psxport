#include "warp_control.h"

#include "game.h"

#include <lucent/log.h>

#include <cstdio>

namespace psx::ui {

int WarpControl::areaCount() const {
    if (!mGame || !mGame->core.hooks || !mGame->core.hooks->devAreaCount) return 0;
    return mGame->core.hooks->devAreaCount(&mGame->core);
}

std::string WarpControl::areaLabel(int area) const {
    const char* nm = (mGame && mGame->core.hooks && mGame->core.hooks->devAreaName)
                         ? mGame->core.hooks->devAreaName(&mGame->core, area)
                         : "";
    char b[96];
    if (nm && *nm) snprintf(b, sizeof b, "%d - %s", area, nm);
    else           snprintf(b, sizeof b, "Area %d", area);
    return b;
}

void WarpControl::adjust(int dir) {
    const int n = areaCount();
    if (n <= 0) return;
    mArea = (mArea + dir) % n;
    if (mArea < 0) mArea += n;   // wrap both ways, so Left from 0 reaches the last area
}

std::string WarpControl::arm() {
    if (!mGame || !mGame->core.hooks) return "warp unavailable";
    if (areaCount() <= 0) return "warp unavailable";
    if (mGame->core.hooks->devWarpAllowed && !mGame->core.hooks->devWarpAllowed(&mGame->core))
        return "not in the field - reach gameplay first";
    mGame->repl.warpDest  = (uint32_t)mArea;
    mGame->repl.warpSub   = 0;
    mGame->repl.warpArmed = 1;
    lucent::info("rmlui", "warp: armed area {} from the menu", mArea);
    return "warping to " + areaLabel(mArea);
}

}  // namespace psx::ui
