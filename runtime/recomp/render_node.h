// runtime/recomp/render_node.h — cur_render_node: the current per-instance render node (framework-side).
//
// Extracted from game/render/render_internal.h so the framework (ot_attr.cpp) can read the render-walk's
// current node without pulling in the whole game render-internals header (which drags in render.h, game.h,
// render_queue.h, …). Depends only on Core (RenderSubstrate::diag + guest RAM read) — no game types.
// game/render/render_internal.h #includes this so the game render path keeps cur_render_node as before.
#pragma once
#include "core.h"   // Core::rsub (RenderSubstrate::diag.currentNode), Core::mem_r32

// The real per-instance render object: the walk's node when set, else the guest "current render object"
// scratch (0x1F80028C). Prefer the native walk's node — 0x28C is shared/stale for some billboard paths.
static inline uint32_t cur_render_node(Core* c) {
  return c->rsub.diag.currentNode() ? c->rsub.diag.currentNode() : c->mem_r32(0x1F80028Cu);
}
