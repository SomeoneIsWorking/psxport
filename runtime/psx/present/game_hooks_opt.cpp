// game_hooks_opt.cpp — impl. See game_hooks_opt.h for why optional hooks are never called directly.
#include "game_hooks_opt.h"
#include "core.h"
#include "game_iface.h"

FadeState game_render_fade_state(Core *c, const GameHooks *hooks) {
  FadeState state{};
  if (hooks && hooks->renderFadeState) {
    hooks->renderFadeState(c, &state);
  }
  return state;
}
