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
class Game;

class NodeXform {
public:
  Core* core = nullptr;

  // registerOverrides — dual-wire seedBlock/propagateRotmat/propagateAxis/buildAxis onto `game`
  // via the process-global override registry (overrides::install + shard_set_override). Called
  // once per Game — including SBS's own separately-constructed Games.
  static void registerOverrides(Game* game);

  // build (guest FUN_80051844): compose this node's world matrix at node+0x98 from its local
  // euler+translation, copy the world-pos triple, and propagate to children via propagate().
  void build(uint32_t node);

  // buildWithOffset (guest FUN_800518FC): sibling of build() used when the node has a LOCAL
  // ANCHOR OFFSET (an svec at node+0x88) that must be rotated by the composed matrix and then
  // added to the node's own world position. Same as build() through the matrix compose step
  // (node+0x98 = rot(node+0x54) × M(node+0xB8/BA/BC)), but the world-pos triple at node+0xAC/B0/B4
  // is computed as ApplyMatrixLV(node+0x98, node+0x88) + node+0x2E/32/36 instead of a straight
  // copy from +0x2E/32/36. Used by AI behaviour handlers to render nodes whose rendered pivot is
  // offset from the logical position in a rotating direction. Delegates to propagate() at the end.
  void buildWithOffset(uint32_t node);

  // propagate (guest FUN_80051128): per-object CHILD-NODE TRANSFORM loop — for each child on
  // node+0xC0 build the child's world matrix at child+0x18 and its world position at child+0x2C
  // by composing rotation × parent frame + accumulating parent translation. Root children
  // (sentinel child+6 == -1) reference this node; siblings reference node[0xC0 + sentinel*4].
  // Uses scratchpad 0x1F800000/20/40 as work areas. No render packets, no GTE ops.
  void propagate(uint32_t node);

  // seedBlock (guest FUN_800517BC): write an 8-word {x,0,y,0,z,0,0,0} block at `ptr` — the same
  // diagonal-seed shape build()/buildWithOffset build inline for their scratch source matrix.
  // Pure leaf, no scratchpad/GTE touch. Args sign-extended from int16 (matches the guest ABI).
  void seedBlock(uint32_t ptr, int16_t x, int16_t y, int16_t z);

  // propagateRotmat (guest FUN_80051300): sibling of propagate() — per child on node+0xC0, build
  // the child's rotation via a single Math::rotmat(child+8) call (NOT the identity+rotX/Y/Z
  // sequence propagateAxis/buildAxis use), compose against the parent frame (node+0x98 for a ROOT
  // child [child+6==-1], or the sibling's own frame at node[0xC0+sentinel*4]+0x18), and accumulate
  // world position into child+0x2C/30/34. Uses scratchpad 0x1F800000 (rotmat output) as the ONLY
  // work area — no intermediate 0x1F800040 buffer (unlike propagate()). Writes the child's world
  // matrix at child+0x18 (not child+0x24 — propagate()'s target). Called directly by the
  // recompiled GraphicsBind::renderUpdateBody body (FUN_800517F8) as its "downstream render setup".
  void propagateRotmat(uint32_t node);

  // propagateAxis (guest FUN_80051464): sibling of propagateRotmat() using the EXPLICIT
  // identity + rotX(child+8) + rotY(child+0xA) + rotZ(child+0xC) composition (three separate
  // int16 euler fields) instead of a single rotmat() call. Otherwise byte-identical control flow
  // (root/sibling frame compose, child+0x18 matrix, child+0x2C/30/34 world position). Tail call of
  // buildAxis(); also reached directly from AI behaviour handlers (beh_anim_trigger_gates).
  void propagateAxis(uint32_t node);

  // buildAxis (guest FUN_80051C8C): node-level sibling of build() — composes this node's OWN world
  // matrix at node+0x98 via identity + rotX(node+0x54)/rotY(node+0x56)/rotZ(node+0x58) (explicit
  // per-axis, matching propagateAxis's convention) instead of a single rotmat() call, copies the
  // raw local position (node+0x2E/32/36) straight into the world-pos triple (node+0xAC/B0/B4) with
  // NO rotation applied (unlike buildWithOffset), then tail-calls propagateAxis(node).
  void buildAxis(uint32_t node);

  // ------------------------------------------------------------------------------------------
  // UNWIRED DRAFTS (2026-07-08 wide-RE wave, region 0x80050000-0x8005FFFF). RE'd from
  // generated/shard_*.c ground truth (Ghidra's decompile mis-resolved a table base on one of
  // these — see buildFromChild — so the generated C, not Ghidra, is the source of truth per
  // CLAUDE.md). NOT registered anywhere (no overrides::install, no shard_set_override) and NOT
  // SBS-gated — dead code until a frontier pass wires + verifies them.
  // ------------------------------------------------------------------------------------------

  // copyMatrixBlock (guest FUN_80051B34): frameless leaf — copy a 5-word (20-byte) MATRIX block
  // (the packed GTE 3x3 rotation-matrix layout Math::rotmat/matMul use) from `src` to `dst`.
  void copyMatrixBlock(uint32_t src, uint32_t dst);

  // buildFromChild (guest FUN_80051614): a THIRD node-build variant, sibling of build()/
  // buildWithOffset(). Composes this node's world matrix at node+0x98 from either a straight
  // rotmat(node+0x54) [mode==0] or a rotmat×scale compose using node+0xB8/BA/BC as the diagonal
  // scale [mode!=0], multiplies against a PARENT frame read from
  // *(ActorTomba::G_ADDR + tableIdx*4 + 0xC0) (Ghidra mislabeled this as a "DAT_800e7f40" table;
  // the generated C proves it is Tomba's own child-record table, same +0xC0 array shape as
  // NodeXform::propagate's node+0xC0), then applies that composed matrix to `inVec` to produce
  // node's world position (node+0xAC/B0/B4, mirrored down to the int16 node+0x2E/32/36 slot).
  // Tail-dispatches to propagateRotmat(node) [mode==0] or propagate(node) [mode!=0] — SAME
  // register-faithfulness requirement as build()/buildWithOffset (the nested call's own frame
  // spills whatever is currently in r16..r23/r19..r20, so this method sets the callee-saved
  // registers the recomp has live at that point; see .cpp for the exact trace against
  // generated/shard_3.c gen_func_80051614).
  void buildFromChild(uint32_t node, uint32_t inVec, uint32_t tableIdx, uint32_t mode);

  // worldPosFromLocal (guest FUN_80051D90): out[0..2] += node's LOCAL-frame world position
  // (node+0x2C/30/34), after transforming `inVec` by node's LOCAL matrix (node+0x18) via the
  // not-yet-owned libgte leaf FUN_800844C0 (ApplyMatrixLV variant that returns a packed SVECTOR,
  // not a VECTOR — distinct from the already-native Math::applyMatrixLV). Routed via
  // rec_dispatch since FUN_800844C0 (0x800844C0) is outside this region's ownership.
  void worldPosFromLocal(uint32_t node, uint32_t inVec, uint32_t outVec);

  // worldPosFromComposed (guest FUN_80051D20): sibling of worldPosFromLocal() using node's
  // COMPOSED world matrix (node+0x98) and world-space position (node+0xAC/B0/B4) instead of the
  // local ones. Same FUN_800844C0 dependency.
  void worldPosFromComposed(uint32_t node, uint32_t inVec, uint32_t outVec);
};
