// class ObjectList — the PC-native per-frame ENTITY-LIST WALKERS.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::objectList`. Back-pointer wired
// once by Core's constructor. Callers reach the walkers via the object graph:
//
//     c->engine.objectList.walkAll();     // main per-frame walk (was ov_objwalk / FUN_8007A904)
//     c->engine.objectList.walkAux();     // aux list walk       (was ov_list_walk_69b28 / FUN_80069B28)
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as
// SceneTransition, TransitionState3, NodeXform, ScreenFade.
//
// == walkAll (guest FUN_8007A904) ==
//   The two doubly-linked entity lists (heads T2_OBJLIST_HEAD_1 / T2_OBJLIST_HEAD_2, node.next at
//   +0x24), walked in order; per node clears the per-frame render flag @+1, then dispatches the
//   handler @+0x1c(node) with the node in a0. The `next` pointer is read BEFORE the handler runs
//   (a handler may unlink/free its own node) and held in a host local. After both walks, calls
//   `margin_render_flush` (widescreen margin re-inclusion pass, later-133).
//
// == walkAux (guest FUN_80069B28) ==
//   A separate list head at 0x800F2738 (method ptr @+0x1c, next @+0x24). Unlike walkAll, does NOT
//   clear the render flag. Same dispatch path (native beh if owned, substrate otherwise).
#pragma once
#include <cstdint>
class Core;

class ObjectList {
public:
  Core* core = nullptr;

  // Auxiliary-list head (guest FUN_80069B28 walks this one only).
  static constexpr uint32_t AUX_LIST_HEAD = 0x800F2738u;

  void walkAll();
  void walkAux();
};
