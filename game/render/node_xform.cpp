// NodeXform::build — PC-native reimpl of guest FUN_80051844.
//
// RE'd 1:1 from gen_func_80051844 (generated/shard_7.c) / disas 0x80051844:
//   scratchpad @0x1F800000 = { sx16(node+184), 0, sx16(node+186), 0, sx16(node+188), 0, 0, 0 }
//     (8 int32 words — 3 trans lanes at +0/+8/+16, zeros elsewhere)
//   ov_rotmat(a0=node+84 euler, a1=0x1F800020)              // libgte RotMatrix at 0x80085480
//   ov_mat_mul(a0=0x1F800020 rot, a1=0x1F800000 mat, a2=node+152)
//                                                           // node.mat152 = rot × mat   (0x80084110)
//   node+172 = sx16(node+46)                                // world-space position copy
//   node+176 = sx16(node+50)
//   node+180 = sx16(node+54)
//   ov_xform51128(node)                                     // propagate to children     (0x80051128)
//
// All 3 callees are already native (ov_rotmat, ov_mat_mul, ov_xform51128), so this is pure
// scratchpad seeding + native-call orchestration. No rec_dispatch needed.
#include "node_xform.h"
#include "core.h"
#include "gte_math.h"     // Math::rotmat, Math::matMul (static)
#include "game.h"
#include "engine_overrides.h"   // class EngineOverrides — global dispatch table
#include "render.h"             // full Render definition — c->mRender->mNodeXform

// Dual-wiring plumbing (same pattern as Math::registerOverrides / ActorReward::registerOverrides):
// (1) EngineOverrides for callers reaching these via an explicit rec_dispatch(c, addr) (overlay
// cross-module calls always route this way), and (2) shard_set_override for the recompiler's OWN
// g_override[] table, which is what MAIN's direct intra-shard `func_<addr>(c)` call sites consult
// (confirmed via generated/shard_1.c, shard_2.c, shard_0.c, shard_3.c, shard_5.c, shard_6.c —
// 0x80051300/0x80051464/0x800517BC each have a direct same-module caller). 0x80051C8C has NO direct
// same-module caller (every reference found is `rec_dispatch(c, 0x80051C8Cu)` from an overlay), so
// it only needs the EngineOverrides registration — but is still safe to leave un-dual-wired since
// no g_override[] slot would ever be consulted for it.
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_80051300(Core*);
extern void gen_func_80051464(Core*);
extern void gen_func_800517BC(Core*);

void NodeXform::build(uint32_t node) {
  Core* c = core;
  const uint32_t SCR_M = 0x1F800000u;   // 8-word source matrix
  const uint32_t SCR_R = 0x1F800020u;   // rot output
  c->mem_w32(SCR_M +  4, 0);
  c->mem_w32(SCR_M + 12, 0);
  c->mem_w32(SCR_M + 20, 0);
  c->mem_w32(SCR_M + 24, 0);
  c->mem_w32(SCR_M + 28, 0);
  c->mem_w32(SCR_M +  0, (uint32_t)c->mem_r16s(node + 184));
  c->mem_w32(SCR_M +  8, (uint32_t)c->mem_r16s(node + 186));
  c->mem_w32(SCR_M + 16, (uint32_t)c->mem_r16s(node + 188));
  c->math.rotmat(node + 84, SCR_R);                            // libgte RotMatrix at 0x80085480
  c->math.matMul(SCR_R, SCR_M, node + 152);                    // node.mat152 = rot × mat 0x80084110
  c->mem_w32(node + 172, (uint32_t)c->mem_r16s(node + 46));
  c->mem_w32(node + 176, (uint32_t)c->mem_r16s(node + 50));
  c->mem_w32(node + 180, (uint32_t)c->mem_r16s(node + 54));
  propagate(node);
}

// NodeXform::buildWithOffset — PC-native reimpl of guest FUN_800518FC.
//
// RE'd from disas 0x800518FC. Sibling of NodeXform::build (FUN_80051844): identical scratchpad
// seeding + rotmat + matMul (node+0x98 = rot(node+0x54 euler) × M(sx16(node+0xB8/BA/BC) on the
// diagonal of an 8-word source matrix)). The two diverge at the world-position step:
//   - build():           node[+0xAC/B0/B4] = sx16(node[+0x2E/32/36])                       [direct]
//   - buildWithOffset(): node[+0xAC/B0/B4] = ApplyMatrixLV(node+0x98, node+0x88)
//                                            + sx16(node[+0x2E/32/36])                    [rotated]
// i.e. the local anchor svec at node+0x88 is rotated by the composed matrix and the node's own
// world position (node+0x2E/32/36) is then added on top. Ends with propagate(node) — same as
// build(). All callees already native (Math::rotmat/matMul/applyMatrixLV, NodeXform::propagate).
//
// Callsites (5, all AI behaviour handlers): beh_cull_substate_orchestrator, beh_area_event_dispatch,
// beh_id_compare_motion_dispatch (×3).
void NodeXform::buildWithOffset(uint32_t node) {
  Core* c = core;
  const uint32_t SCR_M = 0x1F800000u;   // 8-word source matrix
  const uint32_t SCR_R = 0x1F800020u;   // rot output
  c->mem_w32(SCR_M +  4, 0);
  c->mem_w32(SCR_M + 12, 0);
  c->mem_w32(SCR_M + 20, 0);
  c->mem_w32(SCR_M + 24, 0);
  c->mem_w32(SCR_M + 28, 0);
  c->mem_w32(SCR_M +  0, (uint32_t)c->mem_r16s(node + 0xB8));
  c->mem_w32(SCR_M +  8, (uint32_t)c->mem_r16s(node + 0xBA));
  c->mem_w32(SCR_M + 16, (uint32_t)c->mem_r16s(node + 0xBC));
  c->math.rotmat(node + 0x54, SCR_R);
  c->math.matMul(SCR_R, SCR_M, node + 0x98);
  c->math.applyMatrixLV(node + 0x98, node + 0x88, node + 0xAC);
  // pos += own world position (int16 sign-extended). Order matches disas (X, then Y+Z paired).
  c->mem_w32(node + 0xAC, c->mem_r32(node + 0xAC) + (uint32_t)(int32_t)c->mem_r16s(node + 0x2E));
  c->mem_w32(node + 0xB0, c->mem_r32(node + 0xB0) + (uint32_t)(int32_t)c->mem_r16s(node + 0x32));
  c->mem_w32(node + 0xB4, c->mem_r32(node + 0xB4) + (uint32_t)(int32_t)c->mem_r16s(node + 0x36));
  propagate(node);
}

// FUN_80051128 — per-object CHILD-NODE TRANSFORM loop. RE'd from disas:
//   guard: if node[9]==0 -> return
//   loop s2 in [0, node[8]) with continue-bound node[9] (dual-bound idiom):
//     child = node[0xC0 + 4*s2]
//     seed scratchpad @0x1F800000: diagonal { child[56], 0, child[58], 0, child[60], 0, 0, 0 }
//     rotmat(child+8 euler → 0x1F800020) ; matMul(rot, work → 0x1F800040)
//     sentinel = child+6 (s16)
//     ROOT (sentinel==-1): matMul(node+152, 0x1F800040 → child+24); applyMatlv(child → child+44);
//       child[0x2C/0x30/0x34] += node[0xAC/0xB0/0xB4]
//     SIBLING: p = node[0xC0 + 4*sentinel]; matMul(p+24, 0x1F800040 → child+24); applyMatlv(...)
//       child[0x2C/0x30/0x34] += p[0x2C/0x30/0x34]
// (GOTCHAS: +56/58/60 are sign-extended lhu+sll16+sra16; sentinel is sll'd by 2 before the branch
// so in the sibling path it's already a byte offset. NO render packets, NO GTE ops.)
void NodeXform::propagate(uint32_t node) {
  Core* c = core;
  if (c->mem_r8(node + 9) == 0) return;
  int i = 0;
  while (i < (int)(uint8_t)c->mem_r8(node + 8)) {
    uint32_t child = c->mem_r32(node + 0xC0 + 4u * (uint32_t)i);
    c->mem_w32(0x1F800004u, 0); c->mem_w32(0x1F80000Cu, 0); c->mem_w32(0x1F800014u, 0);
    c->mem_w32(0x1F800018u, 0); c->mem_w32(0x1F80001Cu, 0);
    c->mem_w32(0x1F800000u, (uint32_t)c->mem_r16s(child + 56));
    c->mem_w32(0x1F800008u, (uint32_t)c->mem_r16s(child + 58));
    c->mem_w32(0x1F800010u, (uint32_t)c->mem_r16s(child + 60));
    int16_t sentinel = c->mem_r16s(child + 6);
    c->math.rotmat(child + 8, 0x1F800020u);
    c->math.matMul(0x1F800020u, 0x1F800000u, 0x1F800040u);
    if (sentinel == -1) {
      c->math.matMul(node + 152, 0x1F800040u, child + 24);
      c->math.applyMatlv(child, child + 44);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(node + 0xAC));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(node + 0xB0));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(node + 0xB4));
    } else {
      uint32_t p = c->mem_r32(node + 0xC0 + 4u * (uint32_t)(int)sentinel);
      c->math.matMul(p + 24, 0x1F800040u, child + 24);
      c->math.applyMatlv(child, child + 44);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(p + 0x2C));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(p + 0x30));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(p + 0x34));
    }
    i++;
    if (!(i < (int)(uint8_t)c->mem_r8(node + 9))) break;
  }
}

// FUN_800517BC — trivial 8-word block seeder: {x,0,y,0,z,0,0,0}. RE'd + cross-checked verbatim
// against generated/shard_5.c gen_func_800517BC (sign-extends a1-a3 from int16 first).
void NodeXform::seedBlock(uint32_t ptr, int16_t x, int16_t y, int16_t z) {
  Core* c = core;
  c->mem_w32(ptr +  0, (uint32_t)(int32_t)x);
  c->mem_w32(ptr +  4, 0);
  c->mem_w32(ptr +  8, (uint32_t)(int32_t)y);
  c->mem_w32(ptr + 12, 0);
  c->mem_w32(ptr + 16, (uint32_t)(int32_t)z);
  c->mem_w32(ptr + 20, 0);
  c->mem_w32(ptr + 24, 0);
  c->mem_w32(ptr + 28, 0);
}

// FUN_80051300 — per-object CHILD-NODE TRANSFORM loop, rotmat-single-call variant. RE'd +
// cross-checked verbatim against generated/shard_1.c gen_func_80051300 (ground truth for the
// register-level shape; this port keeps the same control flow, using the already-native
// Math::rotmat/matMul/applyMatlv leaves in place of func_80085480/80084110/80084220):
//   guard: if node[9]==0 -> return
//   loop i in [0, node[8]) with continue-bound node[9] (dual-bound idiom, same as propagate()):
//     child = node[0xC0 + 4*i]
//     Math::rotmat(child+8 euler-triple -> scratch 0x1F800000)
//     sentinel = (s16)child[6]
//     ROOT (sentinel==-1): Math::matMul(node+0x98, scratch, child+0x18)
//     SIBLING:             p = node[0xC0 + 4*sentinel]; Math::matMul(p+0x18, scratch, child+0x18)
//     Math::applyMatlv(child, child+0x2C)                  // reads the matrix matMul just loaded
//                                                            // into GTE CR via its CTC2 side effect
//     child[0x2C/0x30/0x34] += (ROOT: node[0xAC/B0/B4]) or (SIBLING: p[0x2C/0x30/0x34])
// Called directly (native C++ call, no rec_dispatch) by GraphicsBind::renderUpdateBody
// (FUN_800517F8) — its "downstream render setup" step.
void NodeXform::propagateRotmat(uint32_t node) {
  Core* c = core;
  if (c->mem_r8(node + 9) == 0) return;
  int i = 0;
  while (i < (int)(uint8_t)c->mem_r8(node + 8)) {
    uint32_t child = c->mem_r32(node + 0xC0 + 4u * (uint32_t)i);
    int16_t sentinel = c->mem_r16s(child + 6);
    c->math.rotmat(child + 8, 0x1F800000u);
    if (sentinel == -1) {
      c->math.matMul(node + 0x98, 0x1F800000u, child + 0x18);
      c->math.applyMatlv(child, child + 0x2C);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(node + 0xAC));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(node + 0xB0));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(node + 0xB4));
    } else {
      uint32_t p = c->mem_r32(node + 0xC0 + 4u * (uint32_t)(int)sentinel);
      c->math.matMul(p + 0x18, 0x1F800000u, child + 0x18);
      c->math.applyMatlv(child, child + 0x2C);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(p + 0x2C));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(p + 0x30));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(p + 0x34));
    }
    i++;
    if (!(i < (int)(uint8_t)c->mem_r8(node + 9))) break;
  }
}

// FUN_80051464 — sibling of propagateRotmat(): identical control flow, but the child's rotation is
// built by an EXPLICIT identity + rotX(child+8)/rotY(child+0xA)/rotZ(child+0xC) composition instead
// of a single Math::rotmat() call. RE'd + cross-checked verbatim against generated/shard_2.c
// gen_func_80051464. Tail call of buildAxis(); also reached directly from AI behaviour handlers
// (beh_anim_trigger_gates.cpp, via rec_dispatch — that caller is an overlay, so rec_dispatch is the
// only way to reach MAIN from it).
void NodeXform::propagateAxis(uint32_t node) {
  Core* c = core;
  if (c->mem_r8(node + 9) == 0) return;
  int i = 0;
  while (i < (int)(uint8_t)c->mem_r8(node + 8)) {
    uint32_t child = c->mem_r32(node + 0xC0 + 4u * (uint32_t)i);
    const uint32_t SCR = 0x1F800000u;
    c->mem_w32(SCR +  0, 0x1000); c->mem_w32(SCR +  4, 0);
    c->mem_w32(SCR +  8, 0x1000); c->mem_w32(SCR + 12, 0);
    c->mem_w32(SCR + 16, 0x1000); c->mem_w32(SCR + 20, 0);
    c->mem_w32(SCR + 24, 0);      c->mem_w32(SCR + 28, 0);
    c->math.rotX((int16_t)c->mem_r16s(child + 0x8), SCR);
    c->math.rotY((int16_t)c->mem_r16s(child + 0xA), SCR);
    c->math.rotZ((int16_t)c->mem_r16s(child + 0xC), SCR);
    int16_t sentinel = c->mem_r16s(child + 6);
    if (sentinel == -1) {
      c->math.matMul(node + 0x98, SCR, child + 0x18);
      c->math.applyMatlv(child, child + 0x2C);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(node + 0xAC));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(node + 0xB0));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(node + 0xB4));
    } else {
      uint32_t p = c->mem_r32(node + 0xC0 + 4u * (uint32_t)(int)sentinel);
      c->math.matMul(p + 0x18, SCR, child + 0x18);
      c->math.applyMatlv(child, child + 0x2C);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(p + 0x2C));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(p + 0x30));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(p + 0x34));
    }
    i++;
    if (!(i < (int)(uint8_t)c->mem_r8(node + 9))) break;
  }
}

// FUN_80051C8C — node-level sibling of build(): composes THIS node's own world matrix at node+0x98
// via identity + rotX(node+0x54)/rotY(node+0x56)/rotZ(node+0x58) (explicit per-axis, matching
// propagateAxis's convention — NOT a single rotmat() call), copies the raw local position
// (node+0x2E/32/36) straight into the world-pos triple (node+0xAC/B0/B4, NO rotation applied,
// unlike buildWithOffset), then tail-calls propagateAxis(node). RE'd + cross-checked verbatim
// against generated/shard_5.c gen_func_80051C8C.
void NodeXform::buildAxis(uint32_t node) {
  Core* c = core;
  const uint32_t M = node + 0x98;
  c->mem_w32(M +  0, 0x1000); c->mem_w32(M +  4, 0);
  c->mem_w32(M +  8, 0x1000); c->mem_w32(M + 12, 0);
  c->mem_w32(M + 16, 0x1000); c->mem_w32(M + 20, 0);
  c->mem_w32(M + 24, 0);      c->mem_w32(M + 28, 0);
  c->math.rotX((int16_t)c->mem_r16s(node + 0x54), M);
  c->math.rotY((int16_t)c->mem_r16s(node + 0x56), M);
  c->math.rotZ((int16_t)c->mem_r16s(node + 0x58), M);
  c->mem_w32(node + 0xAC, (uint32_t)(int32_t)c->mem_r16s(node + 0x2E));
  c->mem_w32(node + 0xB0, (uint32_t)(int32_t)c->mem_r16s(node + 0x32));
  c->mem_w32(node + 0xB4, (uint32_t)(int32_t)c->mem_r16s(node + 0x36));
  propagateAxis(node);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// Wiring — same dual pattern as Math::registerOverrides (game/math/gte_math.cpp) /
// ActorReward::registerOverrides (game/object/actor_sm_reward.cpp).
//
// v0 (c->r[2]) note: propagateRotmat/propagateAxis structurally ALWAYS leave v0==0 at return — the
// guest body's own loop-guard idiom (`r2 = node[9]; if (r2==0) return;` and the per-iteration bound
// check `r2 = (i < node[9]) ? 1 : 0`) means every return path, including the early guard, sets r2 to
// exactly 0 right before returning (confirmed against generated/shard_1.c gen_func_80051300 and
// generated/shard_2.c gen_func_80051464 — no other write to r2 exists between the last such compare
// and the function's `return`). GraphicsBind::renderUpdateBody propagates this v0 as its OWN return
// value (`return c->r[2];` right after `rec_dispatch(c, 0x80051300u);`), so the trampolines set it
// explicitly rather than leaving it as an accidental leftover. buildAxis/seedBlock are void guest
// leaves with no caller observed reading v0 afterward; left unset (matches build()/propagate()).
static void eov_seedBlock(Core* c) {
  c->mRender->mNodeXform.seedBlock(c->r[4], (int16_t)c->r[5], (int16_t)c->r[6], (int16_t)c->r[7]);
}
static void eov_propagateRotmat(Core* c) {
  c->mRender->mNodeXform.propagateRotmat(c->r[4]);
  c->r[2] = 0;
}
static void eov_propagateAxis(Core* c) {
  c->mRender->mNodeXform.propagateAxis(c->r[4]);
  c->r[2] = 0;
}
static void eov_buildAxis(Core* c) {
  c->mRender->mNodeXform.buildAxis(c->r[4]);
}

// psx_fallback-gated trampolines for shard_set_override (core B must stay pure substrate).
static void gov_seedBlock(Core* c) {
  if (c->game->psx_fallback) { gen_func_800517BC(c); return; } eov_seedBlock(c);
}
static void gov_propagateRotmat(Core* c) {
  if (c->game->psx_fallback) { gen_func_80051300(c); return; } eov_propagateRotmat(c);
}
static void gov_propagateAxis(Core* c) {
  if (c->game->psx_fallback) { gen_func_80051464(c); return; } eov_propagateAxis(c);
}

void NodeXform::registerOverrides(Game* game) {
  EngineOverrides& ov = game->engine_overrides;
  ov.register_(0x800517BCu, "NodeXform::seedBlock",        eov_seedBlock);
  ov.register_(0x80051300u, "NodeXform::propagateRotmat",  eov_propagateRotmat);
  ov.register_(0x80051464u, "NodeXform::propagateAxis",    eov_propagateAxis);
  ov.register_(0x80051C8Cu, "NodeXform::buildAxis",        eov_buildAxis);

  shard_set_override(0x800517BCu, gov_seedBlock);
  shard_set_override(0x80051300u, gov_propagateRotmat);
  shard_set_override(0x80051464u, gov_propagateAxis);
  // 0x80051C8C has no direct same-module (func_<addr>(c)) caller — every reference is
  // rec_dispatch(c, 0x80051C8Cu) from an overlay, so EngineOverrides alone covers it.
}
