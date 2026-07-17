// game_ctx.cpp — the Tomba!2 per-Core subsystem-aggregate (TombaCtx) lifecycle.
//
// The framework allocates/frees the game's opaque per-Core aggregate through the GameHooks
// ctxCreate/ctxDestroy pointers (game_iface.h); Core::gameCtx holds the resulting void*. The
// back-pointer wiring below moved VERBATIM out of Core::Core() (runtime/recomp/core.cpp) when the
// 9 game subsystems were pulled OFF Core into TombaCtx — each `this` there is `c` here, each member
// prefixed with `ctx->`. No wiring line was dropped.
#include "game_ctx.h"

void* tomba_ctx_create(Core* c) {
  TombaCtx* ctx = new TombaCtx();
  // Wire up owned subsystems' back-pointers so their methods can reach this Core's guest memory.
  ctx->screenFade.core = c;
  ctx->engine.core     = c;
  ctx->engine.sceneTransition.core = c;   // Engine-owned scene subsystem
  ctx->engine.transitionState3.core = c;  // Engine-owned mid-transition walker
  ctx->engine.objectList.core = c;        // Engine-owned entity-list walkers
  ctx->engine.array8Dispatch.core = c;    // Engine-owned 8-slot fixed-array dispatcher
  ctx->engine.objectTable.core = c;       // Engine-owned 40-slot object-table dispatcher
  ctx->engine.demo.core = c;              // Engine-owned front-end DEMO / MENU stage
  ctx->engine.sop.core = c;               // Engine-owned SOP intro-cutscene FIELD stage
  ctx->engine.bgSceneTransitionSm.core = c;  // Engine-owned BG scene-transition fade manager
  ctx->engine.parallaxBg.core = c;        // Engine-owned SOP parallax-BG state machine
  ctx->engine.pool.core = c;              // Engine-owned per-area init subsystem
  ctx->engine.placement.core = c;         // Engine-owned field-placement driver
  ctx->engine.graphicsBind.core = c;      // Engine-owned per-object render-bind subsystem
  ctx->engine.font.core = c;              // Engine-owned boot-time font/text init subsystem
  ctx->engine.animation.core = c;         // Engine-owned per-object animation-VM stepper
  ctx->engine.asset.core = c;             // Engine-owned asset loader subsystem
  ctx->engine.musicCoord.core = c;        // Engine-owned dialog↔music coordination
  ctx->engine.collision.core = c;         // Engine-owned collision-grid subsystem
  ctx->engine.bit.core = c;               // Engine-owned game-flag bitmap bit-test subsystem
  ctx->engine.spawn.core = c;             // Engine-owned entity spawn/despawn subsystem
  ctx->engine.behaviors.core = c;         // Engine-owned per-object behavior dispatcher
  ctx->engine.cull.core = c;              // Engine-owned visibility cull subsystem
  ctx->engine.sceneEvents.core = c;       // Engine-owned scene-event arm subsystem
  ctx->engine.sfx.core = c;               // Engine-owned sound-FX trigger dispatcher
  ctx->engine.audioDispatch.core = c;     // Engine-owned field-audio dispatch/settle cluster
  ctx->engine.sequencer.core = c;         // Engine-owned libsnd per-VBlank tick wrapper (wide-RE draft, unwired)
  ctx->engine.areaSlots.core = c;         // Engine-owned area-slot table state machine
  ctx->engine.modeStateArm.core = c;      // Engine-owned mode-state arm primitive pair
  ctx->engine.script.core = c;            // Engine-owned cutscene bytecode dispatcher
  ctx->engine.actorTomba.core = c;        // Engine-owned Tomba per-frame logic (G-block owner)
  ctx->engine.attackOrbit.core = c;       // Engine-owned A00 attack-orbit sub-behaviors
  ctx->engine.releaseTriggerMotion.core = c;  // Engine-owned release-trigger sub-motion cluster
  ctx->engine.actorMeleeEngage.core = c;      // Engine-owned melee-engage/reposition/arm leaf
  ctx->engine.meleeProximity.core = c;        // Engine-owned melee-proximity/approach-anchor leaf
  ctx->rng.core        = c;
  ctx->trig.core       = c;
  ctx->math.core       = c;
  ctx->mtx.core        = c;
  ctx->inventory.core  = c;
  ctx->saveMenu.core   = c;
  // Game-owned audio: MusicList reaches the disc backend (c->game->disc) + its sibling native_music
  // (gctx(c)->native_music) through this Core at call time. It holds a Core* (not a Game*) because
  // c->game is not yet wired when ctxCreate runs (Core's ctor precedes Game's ctor body); c->game is
  // valid by the time any MusicList method is called. NativeMusic needs no back-pointer (self-contained).
  ctx->music_list.core = c;
  // Render umbrella (owned by pointer): allocate, wire its back-pointer + each embedded sub-subsystem.
  ctx->mRender = new Render();
  ctx->mRender->mCore = c;
  ctx->mRender->mNodeXform.core = c;
  ctx->mRender->mNativeScene.mCore = c;
  return ctx;
}

void tomba_ctx_destroy(void* p) {
  TombaCtx* ctx = (TombaCtx*)p;
  if (ctx) {
    delete ctx->mRender;
    delete ctx;
  }
}
