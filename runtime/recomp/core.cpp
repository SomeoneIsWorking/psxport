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
#include "render/render.h"     // full defn of Render (fwd-declared in core.h)

Core::Core() {
  memset((R3000*)this, 0, sizeof(R3000));
  memset(ram, 0, sizeof(ram));
  memset(scratch, 0, sizeof(scratch));
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
  rng.core        = this;
  inventory.core  = this;
  // Render umbrella (owned by pointer): allocate, wire its back-pointer + each embedded sub-subsystem.
  mRender = new Render();
  mRender->mCore = this;
  mRender->mNodeXform.core = this;
}

Core::~Core() {
  delete mRender;
  mRender = nullptr;
}
