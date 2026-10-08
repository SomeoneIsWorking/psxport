// present_fade_state.cpp — impl. See present_fade_state.h.
#include "present_fade_state.h"

#include "core.h"
#include "game.h"
#include "game_hooks_opt.h" // game_render_fade_state — the title's live level, the endpoint source
#include "legacy_game_hooks.h"

FadeState present_fade_state(Core *core) {
  const FadeState live = game_render_fade_state(core, core ? core->hooks : nullptr);
  if (!core || !core->game) {
    return live;
  }
  return core->game->presentFade.resolveNow(live);
}
