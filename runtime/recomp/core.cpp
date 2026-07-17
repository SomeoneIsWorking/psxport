// Core::Core / Core::~Core — the per-instance R3000 machine's constructor and destructor.
//
// Zero the R3000 register bank + main-RAM + scratchpad, allocate the render subsystem umbrella
// (`class Render` — game/render/render.h), and wire every owned subsystem's back-pointer to `this`
// so its methods can reach this Core's guest memory. Callers access subsystems as:
//     c->screenFade.method(args)   // embedded-value subsystems
//     c->mRender->mNodeXform.method(args)   // pointer-to-umbrella subsystem
//
// Lived in mem.cpp historically (right next to the memory-access primitives) — moved out into its
// own file so Core lifetime concerns aren't tangled with the memory-window helpers.
#include <cstring>
#include "core.h"
#include "render.h"     // full defn of Render (fwd-declared in core.h)

Core::Core() {
  memset((R3000*)this, 0, sizeof(R3000));
  memset(ram, 0, sizeof(ram));
  memset(scratch, 0, sizeof(scratch));
  // Snapshot the game seam the game installed at startup (game_iface.h). nullptr until installed —
  // harmless while no framework literal/call-site has been converted to read cfg/hooks yet.
  cfg   = psxport_game_config();
  hooks = psxport_game_hooks();
  // Wire up owned subsystems' back-pointers so their methods can reach this Core's guest memory.
  screenFade.core = this;
  engine.core     = this;
  engine.sceneTransition.core = this;   // Engine-owned scene subsystem
  engine.transitionState3.core = this;  // Engine-owned mid-transition walker
  engine.objectList.core = this;        // Engine-owned entity-list walkers
  engine.array8Dispatch.core = this;    // Engine-owned 8-slot fixed-array dispatcher
  engine.objectTable.core = this;       // Engine-owned 40-slot object-table dispatcher
  engine.demo.core = this;              // Engine-owned front-end DEMO / MENU stage
  engine.sop.core = this;               // Engine-owned SOP intro-cutscene FIELD stage
  engine.bgSceneTransitionSm.core = this;  // Engine-owned BG scene-transition fade manager
  engine.parallaxBg.core = this;        // Engine-owned SOP parallax-BG state machine
  engine.pool.core = this;              // Engine-owned per-area init subsystem
  engine.placement.core = this;         // Engine-owned field-placement driver
  engine.graphicsBind.core = this;      // Engine-owned per-object render-bind subsystem
  engine.font.core = this;              // Engine-owned boot-time font/text init subsystem
  engine.animation.core = this;         // Engine-owned per-object animation-VM stepper
  engine.asset.core = this;             // Engine-owned asset loader subsystem
  engine.musicCoord.core = this;        // Engine-owned dialog↔music coordination
  engine.collision.core = this;         // Engine-owned collision-grid subsystem
  engine.bit.core = this;               // Engine-owned game-flag bitmap bit-test subsystem
  engine.spawn.core = this;             // Engine-owned entity spawn/despawn subsystem
  engine.behaviors.core = this;         // Engine-owned per-object behavior dispatcher
  engine.cull.core = this;              // Engine-owned visibility cull subsystem
  engine.sceneEvents.core = this;       // Engine-owned scene-event arm subsystem
  engine.sfx.core = this;               // Engine-owned sound-FX trigger dispatcher
  engine.audioDispatch.core = this;     // Engine-owned field-audio dispatch/settle cluster
  engine.sequencer.core = this;         // Engine-owned libsnd per-VBlank tick wrapper (wide-RE draft, unwired)
  engine.areaSlots.core = this;         // Engine-owned area-slot table state machine
  engine.modeStateArm.core = this;      // Engine-owned mode-state arm primitive pair
  engine.script.core = this;            // Engine-owned cutscene bytecode dispatcher
  engine.actorTomba.core = this;        // Engine-owned Tomba per-frame logic (G-block owner)
  engine.attackOrbit.core = this;       // Engine-owned A00 attack-orbit sub-behaviors
  engine.releaseTriggerMotion.core = this;  // Engine-owned release-trigger sub-motion cluster
  engine.actorMeleeEngage.core = this;      // Engine-owned melee-engage/reposition/arm leaf
  engine.meleeProximity.core = this;        // Engine-owned melee-proximity/approach-anchor leaf
  rng.core        = this;
  trig.core       = this;
  math.core       = this;
  mtx.core        = this;
  inventory.core  = this;
  saveMenu.core   = this;
  // Render umbrella (owned by pointer): allocate, wire its back-pointer + each embedded sub-subsystem.
  mRender = new Render();
  mRender->mCore = this;
  mRender->mNodeXform.core = this;
  mRender->mNativeScene.mCore = this;
}

Core::~Core() {
  delete mRender;
  mRender = nullptr;
}
