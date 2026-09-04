#include "render_path_control.h"

#include "game.h"
#include "game_runtime.h"

#include <lucent/log.h>

namespace psx::ui {

RenderPath player_render_path_next(RenderPath current, const RenderCapabilities &capabilities) {
  return render_path_next_supported(current, capabilities, RenderPathAudience::Player);
}

bool RenderPathControl::available() const {
  return mGame && mGame->runtime && mGame->runtime->renderCapabilities().playerPathCount() > 1;
}

std::string RenderPathControl::currentLabel() const {
  if (!mGame) {
    return "Unavailable";
  }
  switch (mGame->core.rsub.mode.path()) {
  case RenderPath::Native:
    return "Native / PC";
  case RenderPath::Gte:
    return "GTE / PC";
  case RenderPath::Psx:
    return "Diagnostic / PSX";
  }
  return "Unknown";
}

void RenderPathControl::cycle() {
  if (!mGame || !mGame->runtime) {
    return;
  }

  Core &core = mGame->core;
  const RenderCapabilities capabilities = mGame->runtime->renderCapabilities();
  const RenderPath next = player_render_path_next(core.rsub.mode.path(), capabilities);
  const RenderPathSelectionResult result = render_path_apply(*mGame, next, RenderPathAudience::Player);
  if (result == RenderPathSelectionResult::Unsupported) {
    lucent::warn("render",
                 "RmlUi renderer change REFUSED: this title does not expose render path '{}' to players",
                 render_path_name(next));
    return;
  }
  lucent::info("render",
               "RmlUi -> render path = {} — PC enhancements {}",
               render_path_name(next),
               core.rsub.mode.enhancementsAllowed() ? "ALLOWED" : "locked out");
}

} // namespace psx::ui
