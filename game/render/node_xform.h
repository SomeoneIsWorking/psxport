// class NodeXform — PC-native scene-node WORLD-TRANSFORM builder.
//
// PROPER OOP: one instance per Core (embedded as `Core::nodeXform`), back-pointer to Core wired in
// Core::Core(). Callers use it as `c->nodeXform.build(node)`. No `extern "C"` shim, no free-function
// wrapper. Same pattern as `Rng`, `Inventory`, `ScreenFade`, `Engine`.
//
// Reimplements guest FUN_80051844: given a scene-node record it composes the object's world matrix
// at node+0x98 (152) from the local euler-angle triple at node+84 and the local translation triple at
// node+184/186/188, then copies the local-space position (node+46/50/54) into the world-pos slot
// (node+172/176/180) and delegates to ov_xform51128 to propagate the composed matrix down to child
// sub-nodes. Uses scratchpad 0x1F800000 (source matrix, 8 words) and 0x1F800020 (rot output).
#pragma once
#include <cstdint>
class Core;

class NodeXform {
public:
  Core* core = nullptr;

  // build (guest FUN_80051844): compose this node's world matrix at node+0x98 from its local
  // euler+translation, copy the world-pos triple, and propagate to children via propagate().
  void build(uint32_t node);

  // propagate (guest FUN_80051128): per-object CHILD-NODE TRANSFORM loop — for each child on
  // node+0xC0 build the child's world matrix at child+0x18 and its world position at child+0x2C
  // by composing rotation × parent frame + accumulating parent translation. Root children
  // (sentinel child+6 == -1) reference this node; siblings reference node[0xC0 + sentinel*4].
  // Uses scratchpad 0x1F800000/20/40 as work areas. No render packets, no GTE ops.
  void propagate(uint32_t node);
};
