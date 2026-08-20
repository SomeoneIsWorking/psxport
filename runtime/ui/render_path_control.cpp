#include "render_path_control.h"

#include "config_vars.h"
#include "game.h"

#include <lucent/log.h>

namespace psx::ui {

RenderPath player_render_path_next(RenderPath current) {
  return current == RenderPath::Native ? RenderPath::Gte : RenderPath::Native;
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
  if (!mGame) {
    return;
  }
  if (mGame->oracle || mGame->sbs) {
    lucent::warn("render",
                 "RmlUi renderer change REFUSED: this run is {} and exists to be the reference — "
                 "changing the renderer mid-run would invalidate it",
                 mGame->oracle ? "ORACLE" : "an SBS compare");
    return;
  }

  Core &core = mGame->core;
  const RenderPath next = player_render_path_next(core.rsub.mode.path());
  core.rsub.mode.setPath(next);
  psx::config::cv_render_path.set(psx::config::Layer::Runtime, render_path_name(next));
  lucent::info("render",
               "RmlUi -> render path = {} — PC enhancements {}",
               render_path_name(next),
               core.rsub.mode.enhancementsAllowed() ? "ALLOWED" : "locked out");
}

} // namespace psx::ui
