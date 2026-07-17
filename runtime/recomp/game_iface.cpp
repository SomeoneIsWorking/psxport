// game_iface.cpp — framework-side storage for the game seam (game_iface.h). Holds the process-global
// GameConfig/GameHooks the game installs at startup. No game types referenced (game-agnostic).
#include "game_iface.h"

static const GameConfig* g_cfg   = nullptr;
static const GameHooks*  g_hooks = nullptr;

void psxport_install_game(const GameConfig* cfg, const GameHooks* hooks) {
  g_cfg   = cfg;
  g_hooks = hooks;
}
const GameConfig* psxport_game_config() { return g_cfg; }
const GameHooks*  psxport_game_hooks()  { return g_hooks; }
