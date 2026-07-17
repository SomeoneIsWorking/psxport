// game_hooks.cpp — the Tomba!2 GameHooks instance (game_iface.h seam).
//
// Each hook is a thin function-pointer impl that reaches the game's per-Core Engine aggregate
// (`c->engine.*`). The framework substrate (runtime/recomp/*) calls these as `c->hooks->fn(c)` in
// place of the direct `c->engine.X()` calls it used to bake in, so the framework no longer names any
// game type. Installed alongside the GameConfig via tomba_install_game_config() (game_config.cpp),
// before any Game/Core is constructed, so Core's ctor snapshots a non-null c->hooks.
#include "game_iface.h"
#include "core.h"
#include "engine.h"

static void tomba_frameUpdate(Core* c)                { c->engine.frameUpdate(); }
static void tomba_drawOTag(Core* c, uint32_t otHead)  { c->engine.drawOTag(otHead); }
static void tomba_musicCoordTick(Core* c)             { c->engine.musicCoord.tick(); }
static bool tomba_cdDialogToneActive(Core* c)         { return c->engine.musicCoord.dialogToneActive(); }
static void tomba_cdMusicFadeIn(Core* c)              { c->engine.musicCoord.musicFadeIn(); }

// registerOverrides installs ALL the game's override clusters into the process-global registry.
// Body lives in register_overrides.cpp (moved out of framework boot.cpp); declared here so the hook
// table names it. Takes Game* (not Core*): the clusters register per-Game.
extern void register_engine_overrides(Game*);

// extern-visible: game_config.cpp names it in the install call. A namespace-scope `const` object
// has INTERNAL linkage by default in C++, so `extern` is required to export the symbol. One
// process-global instance; both SBS cores snapshot the same pointer.
extern const GameHooks g_tomba_hooks = {
  /* frameUpdate        */ tomba_frameUpdate,
  /* drawOTag           */ tomba_drawOTag,
  /* musicCoordTick     */ tomba_musicCoordTick,
  /* cdDialogToneActive */ tomba_cdDialogToneActive,
  /* cdMusicFadeIn      */ tomba_cdMusicFadeIn,
  /* registerOverrides  */ register_engine_overrides,
};
