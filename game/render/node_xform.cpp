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
#include "actor_tomba.h"        // ActorTomba::G_ADDR — buildFromChild's parent-table base (UNWIRED draft)


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

// Guest-stack frame mirrors — RE'd from the generated prologues (gen_func_<addr>, see each
// function's own comment block above for the exact `sw` sequence). None of these 6 functions
// read/write r29 in their own C++ body (all locals are named C++ variables, not register-mapped
// stack slots), but the RECOMP side descends a real frame and spills whatever the CALLER currently
// has in r16../ra at the RE'd offsets, then restores those exact values on return — a net-zero r29
// move that nonetheless writes real, comparable bytes into guest RAM for the frame's lifetime.
// Omitting this (as this file did until now) leaves that stack region untouched on the native side
// while the recomp side's spills/restores run, so whatever OTHER call last wrote there shows through
// instead — a real, reproducible SBS residual (see docs/findings/render.md / the f117-class
// NodeXform residual). Mirrored here per the "MIRROR THE GUEST STACK" directive
// (docs/faithful-execution.md; same pattern as Cull::wrapFrame/performBaseCullFramed and
// Render::perObjRenderDispatch's GuestFrame): spill the LIVE c->r[..] values (whatever they happen
// to hold — that's what the substrate's own callee-save spill captures too, since it has no idea
// what those registers "mean" to the caller) into the RE'd offsets, run the body, then restore.
//
// REGISTER FAITHFULNESS (2026-07-08, the f117 residual root cause): frame-descent alone is NOT
// enough. These functions load `node` (and scratch-base pointers) into CALLEE-SAVED registers
// (`r16=r4`, `r17=r16+152`, …) and then TAIL-CALL a nested NodeXform function (build/buildWithOffset
// -> propagate, buildAxis -> propagateAxis) whose OWN prologue spills the caller's live r16..r23 into
// ITS frame. For those spilled bytes to byte-match the substrate, the caller's c->r[16..] must hold
// the SAME values the recomp computed — but the native C++ body uses local variables and never
// touches c->r[16..], leaving them stale (a real reproducible SBS diff at 0x801FE8E8: the recomp
// spilled r16=node=0x800FD010 while the native spilled a stale 0x1000). So each OUTER function that
// makes a nested NodeXform call sets its callee-saved node/scratch registers to the RE'd recomp
// values after the frame descent (the nested callee then spills the right bytes; the frame RAII
// still restores the caller's own incoming values on exit). The Math leaves (rotmat/matMul/rotX/…,
// game/math/gte_math.cpp) are FRAMELESS and touch only r2..r15/r24/r25, so they never spill a
// callee-saved register — the ONLY nested spills that matter are the NodeXform->NodeXform tail calls.
namespace {
// -32: +16 r16, +20 r17, +24 ra   (build 0x80051844, buildWithOffset 0x800518FC, buildAxis 0x80051C8C
// share this shape except buildAxis's own offsets — see BuildAxisFrame below for its distinct layout)
struct BuildFrame {
  Core* c; uint32_t s16, s17, s18, sra;
  explicit BuildFrame(Core* c_) : c(c_), s16(c_->r[16]), s17(c_->r[17]), s18(c_->r[18]), sra(c_->r[31]) {
    c->r[29] -= 32;
    c->mem_w32(c->r[29] + 20, s17);
    c->mem_w32(c->r[29] + 24, s18);
    c->mem_w32(c->r[29] + 28, sra);
    c->mem_w32(c->r[29] + 16, s16);
  }
  ~BuildFrame() {
    c->r[31] = c->mem_r32(c->r[29] + 28);
    c->r[18] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 32;
  }
};
// -32: +16 r16, +20 r17, +24 ra   (buildAxis 0x80051C8C)
struct BuildAxisFrame {
  Core* c; uint32_t s16, s17, sra;
  explicit BuildAxisFrame(Core* c_) : c(c_), s16(c_->r[16]), s17(c_->r[17]), sra(c_->r[31]) {
    c->r[29] -= 32;
    c->mem_w32(c->r[29] + 16, s16);
    c->mem_w32(c->r[29] + 20, s17);
    c->mem_w32(c->r[29] + 24, sra);
  }
  ~BuildAxisFrame() {
    c->r[31] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 32;
  }
};
// -40: +16 r16, +20 r17, +24 r18, +28 r19, +32 r20, +36 ra   (propagateRotmat 0x80051300)
struct PropagateRotmatFrame {
  Core* c; uint32_t s16, s17, s18, s19, s20, sra;
  explicit PropagateRotmatFrame(Core* c_) : c(c_), s16(c_->r[16]), s17(c_->r[17]), s18(c_->r[18]),
      s19(c_->r[19]), s20(c_->r[20]), sra(c_->r[31]) {
    c->r[29] -= 40;
    c->mem_w32(c->r[29] + 28, s19);
    c->mem_w32(c->r[29] + 36, sra);
    c->mem_w32(c->r[29] + 32, s20);
    c->mem_w32(c->r[29] + 24, s18);
    c->mem_w32(c->r[29] + 20, s17);
    c->mem_w32(c->r[29] + 16, s16);
  }
  ~PropagateRotmatFrame() {
    c->r[31] = c->mem_r32(c->r[29] + 36);
    c->r[20] = c->mem_r32(c->r[29] + 32);
    c->r[19] = c->mem_r32(c->r[29] + 28);
    c->r[18] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 40;
  }
};
// -48: +16 r16, +20 r17, +24 r18, +28 r19, +32 r20, +36 r21, +40 r22, +44 ra   (propagateAxis 0x80051464)
struct PropagateAxisFrame {
  Core* c; uint32_t s16, s17, s18, s19, s20, s21, s22, sra;
  explicit PropagateAxisFrame(Core* c_) : c(c_), s16(c_->r[16]), s17(c_->r[17]), s18(c_->r[18]),
      s19(c_->r[19]), s20(c_->r[20]), s21(c_->r[21]), s22(c_->r[22]), sra(c_->r[31]) {
    c->r[29] -= 48;
    c->mem_w32(c->r[29] + 32, s20);
    c->mem_w32(c->r[29] + 44, sra);
    c->mem_w32(c->r[29] + 40, s22);
    c->mem_w32(c->r[29] + 36, s21);
    c->mem_w32(c->r[29] + 28, s19);
    c->mem_w32(c->r[29] + 24, s18);
    c->mem_w32(c->r[29] + 20, s17);
    c->mem_w32(c->r[29] + 16, s16);
  }
  ~PropagateAxisFrame() {
    c->r[31] = c->mem_r32(c->r[29] + 44);
    c->r[22] = c->mem_r32(c->r[29] + 40);
    c->r[21] = c->mem_r32(c->r[29] + 36);
    c->r[20] = c->mem_r32(c->r[29] + 32);
    c->r[19] = c->mem_r32(c->r[29] + 28);
    c->r[18] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 48;
  }
};
// -56: +16 r16, +20 r17, +24 r18, +28 r19, +32 r20, +36 r21, +40 r22, +44 r23, +48 ra (propagate 0x80051128)
struct PropagateFrame {
  Core* c; uint32_t s16, s17, s18, s19, s20, s21, s22, s23, sra;
  explicit PropagateFrame(Core* c_) : c(c_), s16(c_->r[16]), s17(c_->r[17]), s18(c_->r[18]),
      s19(c_->r[19]), s20(c_->r[20]), s21(c_->r[21]), s22(c_->r[22]), s23(c_->r[23]), sra(c_->r[31]) {
    c->r[29] -= 56;
    c->mem_w32(c->r[29] + 28, s19);
    c->mem_w32(c->r[29] + 48, sra);
    c->mem_w32(c->r[29] + 44, s23);
    c->mem_w32(c->r[29] + 40, s22);
    c->mem_w32(c->r[29] + 36, s21);
    c->mem_w32(c->r[29] + 32, s20);
    c->mem_w32(c->r[29] + 24, s18);
    c->mem_w32(c->r[29] + 20, s17);
    c->mem_w32(c->r[29] + 16, s16);
  }
  ~PropagateFrame() {
    c->r[31] = c->mem_r32(c->r[29] + 48);
    c->r[23] = c->mem_r32(c->r[29] + 44);
    c->r[22] = c->mem_r32(c->r[29] + 40);
    c->r[21] = c->mem_r32(c->r[29] + 36);
    c->r[20] = c->mem_r32(c->r[29] + 32);
    c->r[19] = c->mem_r32(c->r[29] + 28);
    c->r[18] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 56;
  }
};
}  // namespace

void NodeXform::build(uint32_t node) {
  Core* c = core;
  BuildFrame frame(c);
  const uint32_t SCR_M = 0x1F800000u;   // 8-word source matrix
  const uint32_t SCR_R = 0x1F800020u;   // rot output
  // register faithfulness for the nested propagate() spill (gen_func_80051844: r16=SCR_M, r17=node,
  // r18=SCR_R at the func_80051128 tail call).
  c->r[16] = SCR_M; c->r[17] = node; c->r[18] = SCR_R;
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
  BuildFrame frame(c);
  // register faithfulness for the nested propagate() spill (gen_func_800518FC: same r16/r17/r18 shape
  // as build — r16=SCR_M, r17=node, r18=SCR_R at the func_80051128 tail call).
  c->r[16] = 0x1F800000u; c->r[17] = node; c->r[18] = 0x1F800020u;
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
  PropagateFrame frame(c);
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
  PropagateRotmatFrame frame(c);
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
  PropagateAxisFrame frame(c);
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
  BuildAxisFrame frame(c);
  // register faithfulness for the nested propagateAxis() spill (gen_func_80051C8C: r16=node,
  // r17=node+152 at the func_80051464 tail call — the exact f117 residual writer).
  c->r[16] = node; c->r[17] = node + 152u;
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

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// UNWIRED DRAFTS (2026-07-08 wide-RE wave, region 0x80050000-0x8005FFFF). See node_xform.h for the
// per-method summary. Not registered anywhere; dead code until a frontier pass wires + SBS-gates.
// ═════════════════════════════════════════════════════════════════════════════════════════════════

// FUN_80051B34 — frameless leaf, verbatim from generated/shard_3.c gen_func_80051B34 (no r29
// change, pure 5-word copy: 5x `lw`/`sw` pairs, no branches). Copies a packed GTE MATRIX (5 words
// = 3x3 int16, same layout Math::rotmat/matMul/NodeXform produce).
void NodeXform::copyMatrixBlock(uint32_t src, uint32_t dst) {
  Core* c = core;
  for (int i = 0; i < 5; i++) c->mem_w32(dst + (uint32_t)i * 4, c->mem_r32(src + (uint32_t)i * 4));
}

namespace {
// -48: +16 r16, +20 r17, +24 r18, +28 r19, +32 r20, +36 r21, +40 ra. Confirmed verbatim against
// generated/shard_3.c gen_func_80051614 lines 13334-13425 (2026-07-08 frontier wiring pass — the
// prior draft had every register shifted +4 off these real offsets).
struct BuildFromChildFrame {
  Core* c; uint32_t s16, s17, s18, s19, s20, s21, sra;
  explicit BuildFromChildFrame(Core* c_) : c(c_), s16(c_->r[16]), s17(c_->r[17]), s18(c_->r[18]),
      s19(c_->r[19]), s20(c_->r[20]), s21(c_->r[21]), sra(c_->r[31]) {
    c->r[29] -= 48;
    c->mem_w32(c->r[29] + 36, s21);
    c->mem_w32(c->r[29] + 32, s20);
    c->mem_w32(c->r[29] + 28, s19);
    c->mem_w32(c->r[29] + 24, s18);
    c->mem_w32(c->r[29] + 20, s17);
    c->mem_w32(c->r[29] + 16, s16);
    c->mem_w32(c->r[29] + 40, sra);
  }
  ~BuildFromChildFrame() {
    c->r[31] = c->mem_r32(c->r[29] + 40);
    c->r[21] = c->mem_r32(c->r[29] + 36);
    c->r[20] = c->mem_r32(c->r[29] + 32);
    c->r[19] = c->mem_r32(c->r[29] + 28);
    c->r[18] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 48;
  }
};
}  // namespace

// FUN_80051614 — RE'd from generated/shard_3.c gen_func_80051614 (ground truth; Ghidra's decompile
// mislabeled the parent-table read as "(&DAT_800e7f40)[tableIdx]" — the generated C computes the
// base as literal 0x800E7E80, which IS ActorTomba::G_ADDR, so this reads *(G_ADDR + tableIdx*4 +
// 0xC0): one of Tomba's own child-record slots, not a separate global table):
//   parent = *(u32*)(G_ADDR + tableIdx*4 + 0xC0)
//   mode==0:  M = rotmat(node+0x54)                                    [scratch 0x1F800000]
//   mode!=0:  Mscale = diag(sx16(node+0xB8/BA/BC)); Mrot = rotmat(node+0x54);
//             M = Mrot × Mscale                                        [0x1F800020, 0x1F800040 -> 0x1F800000]
//   node+0x98 = parent[+0x18] × M
//   node+0xAC/B0/B4 = ApplyMatlv(inVec) via the CR loaded by the matMul above (reads parent[+0x18]
//     as the effective rotation — same CR-coupling idiom as NodeXform::propagate), THEN += parent's
//     own world translation (parent+0x2C/30/34, 32-bit add) — same accumulate-parent-translation
//     idiom as NodeXform::propagate/propagateRotmat's root-child case. (2026-07-08 fix: this draft
//     was missing the += entirely — a real bug, not just an unwired leaf.)
//   node+0x2E/32/36 = (int16)node+0xAC/B0/B4                            (mirror down, AFTER the add)
//   mode==0: propagateRotmat(node)     mode!=0: propagate(node)
// REGISTER FAITHFULNESS (traced against generated/shard_3.c lines 13334-13425 — the callee-saved
// registers propagate()/propagateRotmat()'s OWN frame will spill at the tail-call site): r16 is
// ALWAYS 0x1F800000 (the scratch M) on both paths; r17 is 0x1F800020 ONLY on the mode!=0 path — on
// mode==0 the recomp NEVER touches r17, so it carries whatever buildFromChild's OWN caller left
// there (this port matches that by simply not writing c->r[17] on the mode==0 path); r18=node,
// r19=parent, r20=mode, r21=inVec on BOTH paths (propagateRotmat's own frame only spills r16-r20;
// propagate's spills r16-r23, so r22/r23 are never touched here either way — same "leave alone"
// argument applies to them). The tail-call ra (0x80051760 for propagate / 0x80051770 for
// propagateRotmat) IS mirrored into c->r[31] here — every `jal` overwrites $ra unconditionally, so
// (unlike the OUTER-caller case where a still-substrate caller's own compiled body already sets
// c->r[31] before reaching an EngineOverrides-intercepted rec_dispatch) an internal tail-call inside
// an already-native function must set it itself, or the nested frame's ra spill diverges.
void NodeXform::buildFromChild(uint32_t node, uint32_t inVec, uint32_t tableIdx, uint32_t mode) {
  Core* c = core;
  BuildFromChildFrame frame(c);
  const uint32_t SCR_M = 0x1F800000u, SCR_ROT = 0x1F800020u, SCR_SCALE = 0x1F800040u;
  uint32_t parent = c->mem_r32(ActorTomba::G_ADDR + tableIdx * 4u + 0xC0u);
  c->r[18] = node; c->r[19] = parent; c->r[20] = mode; c->r[21] = inVec;
  if (mode == 0) {
    c->r[16] = SCR_M;
    c->math.rotmat(node + 0x54, SCR_M);
  } else {
    c->r[16] = SCR_M; c->r[17] = SCR_ROT;
    c->mem_w32(SCR_SCALE +  4, 0); c->mem_w32(SCR_SCALE + 12, 0); c->mem_w32(SCR_SCALE + 20, 0);
    c->mem_w32(SCR_SCALE + 24, 0); c->mem_w32(SCR_SCALE + 28, 0);
    c->mem_w32(SCR_SCALE +  0, (uint32_t)(int32_t)c->mem_r16s(node + 0xB8));
    c->mem_w32(SCR_SCALE +  8, (uint32_t)(int32_t)c->mem_r16s(node + 0xBA));
    c->mem_w32(SCR_SCALE + 16, (uint32_t)(int32_t)c->mem_r16s(node + 0xBC));
    c->math.rotmat(node + 0x54, SCR_ROT);
    c->math.matMul(SCR_ROT, SCR_SCALE, SCR_M);
  }
  c->math.matMul(parent + 0x18, SCR_M, node + 0x98);
  c->math.applyMatlv(inVec, node + 0xAC);
  c->mem_w32(node + 0xAC, c->mem_r32(node + 0xAC) + c->mem_r32(parent + 0x2C));
  c->mem_w32(node + 0xB0, c->mem_r32(node + 0xB0) + c->mem_r32(parent + 0x30));
  c->mem_w32(node + 0xB4, c->mem_r32(node + 0xB4) + c->mem_r32(parent + 0x34));
  c->mem_w16(node + 0x2E, c->mem_r16(node + 0xAC));
  c->mem_w16(node + 0x32, c->mem_r16(node + 0xB0));
  c->mem_w16(node + 0x36, c->mem_r16(node + 0xB4));
  if (mode == 0) { c->r[31] = 0x80051770u; propagateRotmat(node); }
  else            { c->r[31] = 0x80051760u; propagate(node); }
}

namespace {
// -32: +16 r16, +20 r17, +24 ra (worldPosFromLocal 0x80051D90, worldPosFromComposed 0x80051D20 --
// both confirmed against generated/shard_7.c gen_func_80051D90 / generated/shard_6.c gen_func_80051D20:
// identical frame shape, r17=node, r16=outVec).
struct WorldPosFrame {
  Core* c; uint32_t s16, s17, sra;
  explicit WorldPosFrame(Core* c_) : c(c_), s16(c_->r[16]), s17(c_->r[17]), sra(c_->r[31]) {
    c->r[29] -= 32;
    c->mem_w32(c->r[29] + 20, s17);
    c->mem_w32(c->r[29] + 16, s16);
    c->mem_w32(c->r[29] + 24, sra);
  }
  ~WorldPosFrame() {
    c->r[31] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 32;
  }
};
}  // namespace

// FUN_80051D90 — RE'd from generated/shard_7.c gen_func_80051D90 (frame: addiu sp,-0x20; spill
// r17(node)=sp+20, r16(outVec)=sp+16, ra=sp+24 -- was missing here, added 2026-07-08). The recomp
// calls FUN_800844C0 with ONLY r4 (=node+0x18) explicitly loaded — r5/r6 pass through UNCHANGED from
// this function's OWN incoming r5 (inVec)/r6 (outVec), i.e. FUN_800844C0(matrix=node+0x18, in=inVec,
// out=outVec) is a 3-arg libgte "ApplyMatrixLV, packed-SVECTOR-out" leaf distinct from the already-
// native Math::applyMatrixLV (which writes unclamped 32-bit MACs, not a packed int16 triple — see
// gen_func_800844C0 vs gen_func_80084470). FUN_800844C0 is OUTSIDE this region (0x800844C0) and
// UNOWNED (frameless, confirmed via generated/shard_3.c — no register-faithfulness consequence, but
// ra=0x80051DB0u mirrored anyway per ground truth). After that call, outVec holds the transformed
// local-space vector; this function adds node's LOCAL position (node+0x2C/30/34) on top.
void NodeXform::worldPosFromLocal(uint32_t node, uint32_t inVec, uint32_t outVec) {
  Core* c = core;
  WorldPosFrame frame(c);
  c->r[17] = node; c->r[16] = outVec;
  c->r[4] = node + 0x18; c->r[5] = inVec; c->r[6] = outVec;
  c->r[31] = 0x80051DB0u;
  rec_dispatch(c, 0x800844C0u);
  c->mem_w16(outVec + 0, (uint16_t)(c->mem_r16(outVec + 0) + (uint16_t)c->mem_r16s(node + 0x2C)));
  c->mem_w16(outVec + 2, (uint16_t)(c->mem_r16(outVec + 2) + (uint16_t)c->mem_r16s(node + 0x30)));
  c->mem_w16(outVec + 4, (uint16_t)(c->mem_r16(outVec + 4) + (uint16_t)c->mem_r16s(node + 0x34)));
}

// FUN_80051D20 — sibling of worldPosFromLocal() using node's COMPOSED world matrix (node+0x98) and
// world-space position (node+0xAC/B0/B4). RE'd from generated/shard_6.c gen_func_80051D20 (same
// frame shape as worldPosFromLocal, ra=0x80051D40u before the FUN_800844C0 call).
void NodeXform::worldPosFromComposed(uint32_t node, uint32_t inVec, uint32_t outVec) {
  Core* c = core;
  WorldPosFrame frame(c);
  c->r[17] = node; c->r[16] = outVec;
  c->r[4] = node + 0x98; c->r[5] = inVec; c->r[6] = outVec;
  c->r[31] = 0x80051D40u;
  rec_dispatch(c, 0x800844C0u);
  c->mem_w16(outVec + 0, (uint16_t)(c->mem_r16(outVec + 0) + (uint16_t)c->mem_r16s(node + 0xAC)));
  c->mem_w16(outVec + 2, (uint16_t)(c->mem_r16(outVec + 2) + (uint16_t)c->mem_r16s(node + 0xB0)));
  c->mem_w16(outVec + 4, (uint16_t)(c->mem_r16(outVec + 4) + (uint16_t)c->mem_r16s(node + 0xB4)));
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
// buildAxis (0x80051C8C): the native NodeXform::buildAxis body already mirrors the substrate's
// 32-byte frame internally (BuildAxisFrame above, verified matching the abi_extract contract).
// Adding a trampoline-level GuestFrame here would create a DOUBLE frame (pushing sp 32 bytes too
// deep), which shifts all downstream callee spills to wrong addresses — confirmed: it moved the
// SBS diverge from f389 to f117. The MIRROR_VERIFY failure on 0x80051C8C is from the internal
// frame's register setup, NOT a missing trampoline frame. Left bare.
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

// --- WIDE-RE DRAFT wiring (2026-07-08 frontier pass) --- copyMatrixBlock / buildFromChild /
// worldPosFromLocal / worldPosFromComposed. copyMatrixBlock (0x80051B34) and buildFromChild
// (0x80051614) have direct same-module callers (confirmed via generated/shard_6.c, shard_5.c,
// shard_1.c) -> dual-wired. worldPosFromLocal (0x80051D90) / worldPosFromComposed (0x80051D20)
// have NO direct same-module caller (every reference is rec_dispatch from an AI overlay) ->
// EngineOverrides only, same as buildAxis above.
extern void gen_func_80051B34(Core*);
extern void gen_func_80051614(Core*);
static void eov_copyMatrixBlock(Core* c) {
  c->mRender->mNodeXform.copyMatrixBlock(c->r[4], c->r[5]);
}
static void eov_buildFromChild(Core* c) {
  c->mRender->mNodeXform.buildFromChild(c->r[4], c->r[5], c->r[6], c->r[7]);
  c->r[2] = 0;   // v0 always 0 at return -- tail-dispatches into propagate()/propagateRotmat(),
                 // both of which structurally always leave v0==0 (see the note above this block).
}
static void eov_worldPosFromLocal(Core* c) {
  c->mRender->mNodeXform.worldPosFromLocal(c->r[4], c->r[5], c->r[6]);
}
static void eov_worldPosFromComposed(Core* c) {
  c->mRender->mNodeXform.worldPosFromComposed(c->r[4], c->r[5], c->r[6]);
}
static void gov_copyMatrixBlock(Core* c) {
  if (c->game->psx_fallback) { gen_func_80051B34(c); return; }
  c->game->engine_overrides.traceHit(c, 0x80051B34u); eov_copyMatrixBlock(c);
}
static void gov_buildFromChild(Core* c) {
  if (c->game->psx_fallback) { gen_func_80051614(c); return; }
  c->game->engine_overrides.traceHit(c, 0x80051614u); eov_buildFromChild(c);
}

void NodeXform::registerOverrides(Game* game) {
  EngineOverrides& ov = game->engine_overrides;
  ov.register_(0x800517BCu, "NodeXform::seedBlock",        eov_seedBlock);
  ov.register_(0x80051300u, "NodeXform::propagateRotmat",  eov_propagateRotmat);
  ov.register_(0x80051464u, "NodeXform::propagateAxis",    eov_propagateAxis);
  ov.register_(0x80051C8Cu, "NodeXform::buildAxis",        eov_buildAxis);
  ov.register_(0x80051B34u, "NodeXform::copyMatrixBlock",  eov_copyMatrixBlock);
  ov.register_(0x80051614u, "NodeXform::buildFromChild",   eov_buildFromChild);
  ov.register_(0x80051D90u, "NodeXform::worldPosFromLocal",    eov_worldPosFromLocal);
  ov.register_(0x80051D20u, "NodeXform::worldPosFromComposed", eov_worldPosFromComposed);

  shard_set_override(0x800517BCu, gov_seedBlock);
  shard_set_override(0x80051300u, gov_propagateRotmat);
  shard_set_override(0x80051464u, gov_propagateAxis);
  shard_set_override(0x80051B34u, gov_copyMatrixBlock);
  shard_set_override(0x80051614u, gov_buildFromChild);
  // 0x80051C8C / 0x80051D90 / 0x80051D20 have no direct same-module (func_<addr>(c)) caller — every
  // reference is rec_dispatch(c, addr) from an overlay, so EngineOverrides alone covers them.
}
