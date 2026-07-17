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
#include "game_ctx.h"
#include "core.h"
#include "gte_math.h"     // Math::rotmat, Math::matMul (static)
#include "game.h"
#include "override_registry.h"   // overrides::install — the one native-override registry
#include "render.h"             // full Render definition — rend(c)->mNodeXform
#include "actor_tomba.h"        // ActorTomba::G_ADDR — buildFromChild's parent-table base (UNWIRED draft)
#include "guest_abi.h"          // GuestFrame/GuestReg/guest_fn — ABI vocabulary (2026-07-14 readability pass)

// Override wiring (see registerOverrides below): 0x80051300/0x80051464/0x800517BC (+ copyMatrixBlock/
// buildFromChild/buildWithOffset) have a direct same-module `func_<addr>(c)` caller (confirmed via
// generated/shard_0/1/2/3/5/6.c), so they install with the shard_set_override thunk to intercept those
// too. 0x80051C8C/0x80051D90/0x80051D20 are only reached via rec_dispatch(c, addr) from an overlay, so
// they wire rec_dispatch-only (install's setter omitted).
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_80051300(Core*);
extern void gen_func_80051464(Core*);
extern void gen_func_800517BC(Core*);
void rec_dispatch(Core*, uint32_t);

namespace {

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// Node — typed lens over a scene-node record. Same guest addresses as before (this is a faithful
// port: SBS byte-compares the underlying RAM), just named field accessors instead of scattered
// `c->mem_rXX(node + 0xNN)`. A "node" plays THREE roles at different call sites, each with its own
// field group at the SAME struct base — this lens exposes all three under one type since a caller
// passes whichever record (self/child/parent) is relevant and the offsets are identical either way:
//   SELF  — a node building its OWN world transform (build/buildWithOffset/buildAxis/buildFromChild):
//           localEuler(0x54), localScale(0xB8), localPos16(0x2E), anchorLocal(0x88),
//           worldMatrix(0x98), worldPos(0xAC).
//   CHILD — an entry in a parent's node+0xC0 child-pointer array, being positioned by the parent's
//           transform loop (propagate/propagateRotmat/propagateAxis): sentinel(0x06),
//           childEuler(0x08), childScale(0x38), frameMatrix(0x18), framePos(0x2C).
//   PARENT/CONTAINER — a node addressing its own children: childCount(0x08)/childCountGuard(0x09)
//           (yes, same bytes as childEuler's low half when that SAME node is later walked as
//           someone else's child — that's the guest struct's own field reuse, not a naming choice
//           here), childPtr(i) (0xC0 + 4*i).
// frameMatrix(0x18)/framePos(0x2C) are also what worldPosFromLocal/worldPosFromComposed read — see
// their own comments for the int16-vs-int32 width note.
struct Node {
  Core* c;
  uint32_t base;

  // --- SELF: this node's own world-build fields ---
  uint32_t localEulerPtr() const   { return base + 0x54u; }   // 3x s16, rotmat/rotX/Y/Z input
  int16_t  localEulerX() const     { return c->mem_r16s(base + 0x54u); }
  int16_t  localEulerY() const     { return c->mem_r16s(base + 0x56u); }
  int16_t  localEulerZ() const     { return c->mem_r16s(base + 0x58u); }
  int16_t  localScaleX() const     { return c->mem_r16s(base + 0xB8u); }
  int16_t  localScaleY() const     { return c->mem_r16s(base + 0xBAu); }
  int16_t  localScaleZ() const     { return c->mem_r16s(base + 0xBCu); }
  int16_t  localPosX16() const     { return c->mem_r16s(base + 0x2Eu); }
  int16_t  localPosY16() const     { return c->mem_r16s(base + 0x32u); }
  int16_t  localPosZ16() const     { return c->mem_r16s(base + 0x36u); }
  void     setLocalPosX16(int16_t v) { c->mem_w16(base + 0x2Eu, (uint16_t)v); }
  void     setLocalPosY16(int16_t v) { c->mem_w16(base + 0x32u, (uint16_t)v); }
  void     setLocalPosZ16(int16_t v) { c->mem_w16(base + 0x36u, (uint16_t)v); }
  uint32_t anchorLocalPtr() const  { return base + 0x88u; }   // svec, buildWithOffset's local anchor
  uint32_t worldMatrixPtr() const  { return base + 0x98u; }   // composed world matrix (5-word GTE layout)
  uint32_t worldPosPtr() const     { return base + 0xACu; }
  int32_t  worldPosX() const       { return (int32_t)c->mem_r32(base + 0xACu); }
  int32_t  worldPosY() const       { return (int32_t)c->mem_r32(base + 0xB0u); }
  int32_t  worldPosZ() const       { return (int32_t)c->mem_r32(base + 0xB4u); }
  void     setWorldPosX(int32_t v) { c->mem_w32(base + 0xACu, (uint32_t)v); }
  void     setWorldPosY(int32_t v) { c->mem_w32(base + 0xB0u, (uint32_t)v); }
  void     setWorldPosZ(int32_t v) { c->mem_w32(base + 0xB4u, (uint32_t)v); }

  // --- CHILD: fields written/read when this node is positioned via a parent's node+0xC0 array ---
  int16_t  sentinel() const        { return c->mem_r16s(base + 0x06u); }  // -1=root, else sibling idx
  int16_t  childEulerX() const     { return c->mem_r16s(base + 0x08u); }
  int16_t  childEulerY() const     { return c->mem_r16s(base + 0x0Au); }
  int16_t  childEulerZ() const     { return c->mem_r16s(base + 0x0Cu); }
  int16_t  childScaleX() const     { return c->mem_r16s(base + 0x38u); }
  int16_t  childScaleY() const     { return c->mem_r16s(base + 0x3Au); }
  int16_t  childScaleZ() const     { return c->mem_r16s(base + 0x3Cu); }
  uint32_t frameMatrixPtr() const  { return base + 0x18u; }   // this node's own local/frame matrix
  uint32_t childEulerPtr() const   { return base + 0x08u; }   // 3x s16 euler triple (rotmat input)
  uint32_t framePosPtr() const     { return base + 0x2Cu; }
  int32_t  framePosX32() const     { return (int32_t)c->mem_r32(base + 0x2Cu); }
  int32_t  framePosY32() const     { return (int32_t)c->mem_r32(base + 0x30u); }
  int32_t  framePosZ32() const     { return (int32_t)c->mem_r32(base + 0x34u); }
  void     setFramePosX32(int32_t v) { c->mem_w32(base + 0x2Cu, (uint32_t)v); }
  void     setFramePosY32(int32_t v) { c->mem_w32(base + 0x30u, (uint32_t)v); }
  void     setFramePosZ32(int32_t v) { c->mem_w32(base + 0x34u, (uint32_t)v); }
  void     addFramePos32(int32_t dx, int32_t dy, int32_t dz) {
    setFramePosX32(framePosX32() + dx); setFramePosY32(framePosY32() + dy); setFramePosZ32(framePosZ32() + dz);
  }
  // Low-16 view of the SAME bytes as framePosX32/Y32/Z32 — worldPosFromLocal/worldPosFromComposed
  // read only the low half (see their own comments; this is a genuine RE'd narrow read, not a bug).
  int16_t  framePosX16() const     { return c->mem_r16s(base + 0x2Cu); }
  int16_t  framePosY16() const     { return c->mem_r16s(base + 0x30u); }
  int16_t  framePosZ16() const     { return c->mem_r16s(base + 0x34u); }

  // --- PARENT/CONTAINER: this node addressing its own children ---
  uint8_t  childCount() const      { return c->mem_r8(base + 0x08u); }   // loop count node[8]
  uint8_t  childCountGuard() const { return c->mem_r8(base + 0x09u); }   // continue-bound node[9]
  uint32_t childPtr(int i) const   { return c->mem_r32(base + 0xC0u + 4u * (uint32_t)i); }
};

// Scratchpad work areas — one fixed set, reused (with the same meaning) by build/buildWithOffset/
// propagate/buildFromChild. Named per role, not per call site.
constexpr uint32_t kScrSrcMatrix = 0x1F800000u;   // 8-word source (diagonal-seeded) matrix
constexpr uint32_t kScrRot       = 0x1F800020u;   // rotmat/rotX-Y-Z output
constexpr uint32_t kScrCompose   = 0x1F800040u;   // matMul(rot, srcMatrix) intermediate

// Seed an 8-word scratch block { x,0, y,0, z,0, 0,0 } — the diagonal-matrix shape build()/
// buildWithOffset()/propagate() all use as their rotmat "scale" input. `x`/`y`/`z` already sign-
// extended to s32 by the caller (matches the guest's sx16 reads at each field).
void seedDiagScratch(Core* c, uint32_t scr, int32_t x, int32_t y, int32_t z) {
  c->mem_w32(scr +  0, (uint32_t)x); c->mem_w32(scr +  4, 0);
  c->mem_w32(scr +  8, (uint32_t)y); c->mem_w32(scr + 12, 0);
  c->mem_w32(scr + 16, (uint32_t)z); c->mem_w32(scr + 20, 0);
  c->mem_w32(scr + 24, 0);           c->mem_w32(scr + 28, 0);
}

// Guest-stack frame mirrors — RE'd from the generated prologues (gen_func_<addr>), contracts
// confirmed against `tools/abi_extract.py <addr> --contract` (2026-07-14). None of these functions
// read/write r29 in their own C++ body (all locals are named C++ variables, not register-mapped
// stack slots), but the RECOMP side descends a real frame and spills whatever the CALLER currently
// has in r16../ra at the RE'd offsets, then restores those exact values on return — a net-zero r29
// move that nonetheless writes real, comparable bytes into guest RAM for the frame's lifetime.
// Omitting this leaves that stack region untouched on the native side while the recomp side's
// spills/restores run, so whatever OTHER call last wrote there shows through instead — a real,
// reproducible SBS residual (see docs/findings/render.md, the f117-class NodeXform residual).
// Mirrored per "MIRROR THE GUEST STACK" (docs/faithful-execution.md; same pattern as
// Cull::wrapFrame/performBaseCullFramed, Render::perObjRenderDispatch's GuestFrame): spill the LIVE
// c->r[..] values (whatever they happen to hold — that's what the substrate's own callee-save spill
// captures too, since it has no idea what those registers "mean" to the caller) into the RE'd
// offsets, run the body, then restore.
constexpr GuestFrameSpill kBuildSpills[] = {{16, 16}, {17, 20}, {18, 24}, {31, 28}};      // -32
constexpr GuestFrameSpill kBuildAxisSpills[] = {{16, 16}, {17, 20}, {31, 24}};            // -32
constexpr GuestFrameSpill kPropagateRotmatSpills[] =                                      // -40
    {{16, 16}, {17, 20}, {18, 24}, {19, 28}, {20, 32}, {31, 36}};
constexpr GuestFrameSpill kPropagateAxisSpills[] =                                        // -48
    {{16, 16}, {17, 20}, {18, 24}, {19, 28}, {20, 32}, {21, 36}, {22, 40}, {31, 44}};
constexpr GuestFrameSpill kPropagateSpills[] =                                            // -56
    {{16, 16}, {17, 20}, {18, 24}, {19, 28}, {20, 32}, {21, 36}, {22, 40}, {23, 44}, {31, 48}};
constexpr GuestFrameSpill kBuildFromChildSpills[] =                                       // -48
    {{16, 16}, {17, 20}, {18, 24}, {19, 28}, {20, 32}, {21, 36}, {31, 40}};
constexpr GuestFrameSpill kWorldPosSpills[] = {{16, 16}, {17, 20}, {31, 24}};              // -32

}  // namespace

// REGISTER FAITHFULNESS (2026-07-08, the f117 residual root cause): frame-descent alone is NOT
// enough. These functions load `node` (and scratch-base pointers) into CALLEE-SAVED registers
// (`r16=r4`, `r17=r16+152`, …) and then TAIL-CALL a nested NodeXform function (build/buildWithOffset
// -> propagate, buildAxis -> propagateAxis) whose OWN prologue spills the caller's live r16..r23 into
// ITS frame. For those spilled bytes to byte-match the substrate, the caller's c->r[16..] must hold
// the SAME values the recomp computed — the native C++ body would otherwise use local variables and
// never touch c->r[16..], leaving them stale (a real reproducible SBS diff at 0x801FE8E8: the recomp
// spilled r16=node=0x800FD010 while the native spilled a stale 0x1000). So each OUTER function that
// makes a nested NodeXform call sets its callee-saved node/scratch registers to the RE'd recomp
// values (via GuestReg<N>, below) after the frame descent — the nested callee then spills the right
// bytes; the frame RAII still restores the caller's own incoming values on exit. The Math leaves
// (rotmat/matMul/rotX/…, game/math/gte_math.cpp) are FRAMELESS and touch only r2..r15/r24/r25, so
// they never spill a callee-saved register — the ONLY nested spills that matter are the
// NodeXform->NodeXform tail calls.
void NodeXform::build(uint32_t nodeAddr) {
  Core* c = core;
  GuestFrame<32, 4> frame(c, kBuildSpills);
  Node node{c, nodeAddr};
  // register faithfulness for the nested propagate() spill (gen_func_80051844: r16=scratch source
  // matrix, r17=node, r18=scratch rot output, at the func_80051128 tail call).
  GuestReg<16> r16(c); GuestReg<17> r17(c); GuestReg<18> r18(c);
  r16 = kScrSrcMatrix; r17 = nodeAddr; r18 = kScrRot;

  seedDiagScratch(c, kScrSrcMatrix, node.localScaleX(), node.localScaleY(), node.localScaleZ());
  mathOf(c).rotmat(node.localEulerPtr(), kScrRot);                  // libgte RotMatrix at 0x80085480
  mathOf(c).matMul(kScrRot, kScrSrcMatrix, node.worldMatrixPtr());  // world matrix = rot × scale (0x80084110)
  node.setWorldPosX(node.localPosX16());                          // world-space position copy
  node.setWorldPosY(node.localPosY16());
  node.setWorldPosZ(node.localPosZ16());
  propagate(nodeAddr);
}

// NodeXform::buildWithOffset — PC-native reimpl of guest FUN_800518FC.
//
// RE'd from disas 0x800518FC. Sibling of NodeXform::build (FUN_80051844): identical scratchpad
// seeding + rotmat + matMul (world matrix = rot(local euler) × scale(local scale diag)). The two
// diverge at the world-position step:
//   - build():           worldPos = sx16(localPos16)                                    [direct]
//   - buildWithOffset(): worldPos = ApplyMatrixLV(worldMatrix, anchorLocal) + sx16(localPos16)
//                                                                                       [rotated]
// i.e. the local anchor svec is rotated by the composed matrix and the node's own local position is
// then added on top. Ends with propagate(node) — same as build(). All callees already native
// (Math::rotmat/matMul/applyMatrixLV, NodeXform::propagate).
//
// Callsites (5, all AI behaviour handlers): beh_cull_substate_orchestrator, beh_area_event_dispatch,
// beh_id_compare_motion_dispatch (×3).
void NodeXform::buildWithOffset(uint32_t nodeAddr) {
  Core* c = core;
  GuestFrame<32, 4> frame(c, kBuildSpills);
  Node node{c, nodeAddr};
  // register faithfulness for the nested propagate() spill — same r16/r17/r18 shape as build().
  GuestReg<16> r16(c); GuestReg<17> r17(c); GuestReg<18> r18(c);
  r16 = kScrSrcMatrix; r17 = nodeAddr; r18 = kScrRot;

  seedDiagScratch(c, kScrSrcMatrix, node.localScaleX(), node.localScaleY(), node.localScaleZ());
  mathOf(c).rotmat(node.localEulerPtr(), kScrRot);
  mathOf(c).matMul(kScrRot, kScrSrcMatrix, node.worldMatrixPtr());
  mathOf(c).applyMatrixLV(node.worldMatrixPtr(), node.anchorLocalPtr(), node.worldPosPtr());
  // pos += own local position (int16 sign-extended). Order matches disas (X, then Y+Z paired).
  node.setWorldPosX(node.worldPosX() + node.localPosX16());
  node.setWorldPosY(node.worldPosY() + node.localPosY16());
  node.setWorldPosZ(node.worldPosZ() + node.localPosZ16());
  propagate(nodeAddr);
}

// FUN_80051128 — per-object CHILD-NODE TRANSFORM loop. RE'd from disas:
//   guard: if node.childCountGuard()==0 -> return
//   loop i in [0, node.childCount()) with continue-bound childCountGuard() (dual-bound idiom):
//     child = node.childPtr(i)
//     seed scratchpad @kScrSrcMatrix: diagonal { child.childScale, 0,0,0 }
//     rotmat(child.childEuler → kScrRot) ; matMul(rot, srcMatrix → kScrCompose)
//     ROOT (child.sentinel()==-1): matMul(node.worldMatrix, kScrCompose → child.frameMatrix);
//       applyMatlv(child → child.framePos); child.framePos += node.worldPos
//     SIBLING: p = node.childPtr(sentinel); matMul(p.frameMatrix, kScrCompose → child.frameMatrix);
//       applyMatlv(...); child.framePos += p.framePos
// (GOTCHAS: childScale fields are sign-extended lhu+sll16+sra16; sentinel is sll'd by 2 before the
// branch so in the sibling path it's already a byte offset — childPtr(i) does that shift itself, so
// passing the raw sentinel value works unchanged. NO render packets, NO GTE ops.)
void NodeXform::propagate(uint32_t nodeAddr) {
  Core* c = core;
  GuestFrame<56, 9> frame(c, kPropagateSpills);
  Node node{c, nodeAddr};
  if (node.childCountGuard() == 0) return;
  int i = 0;
  while (i < (int)node.childCount()) {
    Node child{c, node.childPtr(i)};
    seedDiagScratch(c, kScrSrcMatrix, child.childScaleX(), child.childScaleY(), child.childScaleZ());
    int16_t sentinel = child.sentinel();
    mathOf(c).rotmat(child.childEulerPtr(), kScrRot);
    mathOf(c).matMul(kScrRot, kScrSrcMatrix, kScrCompose);
    if (sentinel == -1) {
      mathOf(c).matMul(node.worldMatrixPtr(), kScrCompose, child.frameMatrixPtr());
      mathOf(c).applyMatlv(child.base, child.framePosPtr());
      child.addFramePos32(node.worldPosX(), node.worldPosY(), node.worldPosZ());
    } else {
      Node p{c, node.childPtr((int)sentinel)};
      mathOf(c).matMul(p.frameMatrixPtr(), kScrCompose, child.frameMatrixPtr());
      mathOf(c).applyMatlv(child.base, child.framePosPtr());
      child.addFramePos32(p.framePosX32(), p.framePosY32(), p.framePosZ32());
    }
    i++;
    if (!(i < (int)node.childCountGuard())) break;
  }
}

// FUN_800517BC — trivial 8-word block seeder: {x,0,y,0,z,0,0,0}. RE'd + cross-checked verbatim
// against generated/shard_5.c gen_func_800517BC (sign-extends a1-a3 from int16 first).
void NodeXform::seedBlock(uint32_t ptr, int16_t x, int16_t y, int16_t z) {
  Core* c = core;
  seedDiagScratch(c, ptr, x, y, z);
}

// FUN_80051300 — per-object CHILD-NODE TRANSFORM loop, rotmat-single-call variant. RE'd +
// cross-checked verbatim against generated/shard_1.c gen_func_80051300 (ground truth for the
// register-level shape; this port keeps the same control flow, using the already-native
// Math::rotmat/matMul/applyMatlv leaves in place of func_80085480/80084110/80084220):
//   guard: if node.childCountGuard()==0 -> return
//   loop i in [0, node.childCount()) with continue-bound childCountGuard() (dual-bound idiom, same
//   as propagate()):
//     child = node.childPtr(i)
//     Math::rotmat(child.childEuler -> scratch kScrSrcMatrix)
//     ROOT (child.sentinel()==-1): Math::matMul(node.worldMatrix, scratch, child.frameMatrix)
//     SIBLING:                     p = node.childPtr(sentinel); Math::matMul(p.frameMatrix, scratch,
//                                  child.frameMatrix)
//     Math::applyMatlv(child, child.framePos)      // reads the matrix matMul just loaded into GTE
//                                                    // CR via its CTC2 side effect
//     child.framePos += (ROOT: node.worldPos) or (SIBLING: p.framePos)
// Called directly (native C++ call, no rec_dispatch) by GraphicsBind::renderUpdateBody
// (FUN_800517F8) — its "downstream render setup" step.
void NodeXform::propagateRotmat(uint32_t nodeAddr) {
  Core* c = core;
  GuestFrame<40, 6> frame(c, kPropagateRotmatSpills);
  Node node{c, nodeAddr};
  if (node.childCountGuard() == 0) return;
  int i = 0;
  while (i < (int)node.childCount()) {
    Node child{c, node.childPtr(i)};
    int16_t sentinel = child.sentinel();
    mathOf(c).rotmat(child.childEulerPtr(), kScrSrcMatrix);
    if (sentinel == -1) {
      mathOf(c).matMul(node.worldMatrixPtr(), kScrSrcMatrix, child.frameMatrixPtr());
      mathOf(c).applyMatlv(child.base, child.framePosPtr());
      child.addFramePos32(node.worldPosX(), node.worldPosY(), node.worldPosZ());
    } else {
      Node p{c, node.childPtr((int)sentinel)};
      mathOf(c).matMul(p.frameMatrixPtr(), kScrSrcMatrix, child.frameMatrixPtr());
      mathOf(c).applyMatlv(child.base, child.framePosPtr());
      child.addFramePos32(p.framePosX32(), p.framePosY32(), p.framePosZ32());
    }
    i++;
    if (!(i < (int)node.childCountGuard())) break;
  }
}

// FUN_80051464 — sibling of propagateRotmat(): identical control flow, but the child's rotation is
// built by an EXPLICIT identity + rotX(child.childEulerX)/rotY(child.childEulerY)/
// rotZ(child.childEulerZ) composition instead of a single Math::rotmat() call. RE'd + cross-checked
// verbatim against generated/shard_2.c gen_func_80051464. Tail call of buildAxis(); also reached
// directly from AI behaviour handlers (beh_anim_trigger_gates.cpp, via rec_dispatch — that caller is
// an overlay, so rec_dispatch is the only way to reach MAIN from it).
void NodeXform::propagateAxis(uint32_t nodeAddr) {
  Core* c = core;
  GuestFrame<48, 8> frame(c, kPropagateAxisSpills);
  Node node{c, nodeAddr};
  if (node.childCountGuard() == 0) return;
  int i = 0;
  while (i < (int)node.childCount()) {
    Node child{c, node.childPtr(i)};
    seedDiagScratch(c, kScrSrcMatrix, 0x1000, 0x1000, 0x1000);   // identity (1.0 in GTE 4.12 fixed)
    mathOf(c).rotX(child.childEulerX(), kScrSrcMatrix);
    mathOf(c).rotY(child.childEulerY(), kScrSrcMatrix);
    mathOf(c).rotZ(child.childEulerZ(), kScrSrcMatrix);
    int16_t sentinel = child.sentinel();
    if (sentinel == -1) {
      mathOf(c).matMul(node.worldMatrixPtr(), kScrSrcMatrix, child.frameMatrixPtr());
      mathOf(c).applyMatlv(child.base, child.framePosPtr());
      child.addFramePos32(node.worldPosX(), node.worldPosY(), node.worldPosZ());
    } else {
      Node p{c, node.childPtr((int)sentinel)};
      mathOf(c).matMul(p.frameMatrixPtr(), kScrSrcMatrix, child.frameMatrixPtr());
      mathOf(c).applyMatlv(child.base, child.framePosPtr());
      child.addFramePos32(p.framePosX32(), p.framePosY32(), p.framePosZ32());
    }
    i++;
    if (!(i < (int)node.childCountGuard())) break;
  }
}

// FUN_80051C8C — node-level sibling of build(): composes THIS node's own world matrix via
// identity + rotX(localEulerX)/rotY(localEulerY)/rotZ(localEulerZ) (explicit per-axis, matching
// propagateAxis's convention — NOT a single rotmat() call), copies the raw local position straight
// into the world-pos triple (NO rotation applied, unlike buildWithOffset), then tail-calls
// propagateAxis(node). RE'd + cross-checked verbatim against generated/shard_5.c gen_func_80051C8C.
void NodeXform::buildAxis(uint32_t nodeAddr) {
  Core* c = core;
  GuestFrame<32, 3> frame(c, kBuildAxisSpills);
  Node node{c, nodeAddr};
  // register faithfulness for the nested propagateAxis() spill (gen_func_80051C8C: r16=node,
  // r17=node.worldMatrix at the func_80051464 tail call — the exact f117 residual writer).
  GuestReg<16> r16(c); GuestReg<17> r17(c);
  r16 = nodeAddr; r17 = node.worldMatrixPtr();

  seedDiagScratch(c, node.worldMatrixPtr(), 0x1000, 0x1000, 0x1000);   // identity
  mathOf(c).rotX(node.localEulerX(), node.worldMatrixPtr());
  mathOf(c).rotY(node.localEulerY(), node.worldMatrixPtr());
  mathOf(c).rotZ(node.localEulerZ(), node.worldMatrixPtr());
  node.setWorldPosX(node.localPosX16());
  node.setWorldPosY(node.localPosY16());
  node.setWorldPosZ(node.localPosZ16());
  propagateAxis(nodeAddr);
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

// FUN_80051614 — RE'd from generated/shard_3.c gen_func_80051614 (ground truth; Ghidra's decompile
// mislabeled the parent-table read as "(&DAT_800e7f40)[tableIdx]" — the generated C computes the
// base as literal 0x800E7E80, which IS ActorTomba::G_ADDR, so this reads *(G_ADDR + tableIdx*4 +
// 0xC0): one of Tomba's own child-record slots, not a separate global table):
//   parent = *(u32*)(G_ADDR + tableIdx*4 + 0xC0)
//   mode==0:  M = rotmat(node.localEuler)                              [scratch kScrSrcMatrix]
//   mode!=0:  scale = diag(node.localScale); rot = rotmat(node.localEuler);
//             M = rot × scale                             [kScrRot, kScrCompose -> kScrSrcMatrix]
//   node.worldMatrix = parent.frameMatrix × M
//   node.worldPos = ApplyMatlv(inVec) via the CR loaded by the matMul above (reads parent.frameMatrix
//     as the effective rotation — same CR-coupling idiom as NodeXform::propagate), THEN +=
//     parent.framePos (32-bit add) — same accumulate-parent-translation idiom as
//     NodeXform::propagate/propagateRotmat's root-child case. (2026-07-08 fix: this draft was
//     missing the += entirely — a real bug, not just an unwired leaf.)
//   node.localPos16 = (int16)node.worldPos                              (mirror down, AFTER the add)
//   mode==0: propagateRotmat(node)     mode!=0: propagate(node)
// REGISTER FAITHFULNESS (traced against generated/shard_3.c lines 13334-13425 — the callee-saved
// registers propagate()/propagateRotmat()'s OWN frame will spill at the tail-call site): r16 is
// ALWAYS kScrSrcMatrix on both paths; r17 is kScrRot ONLY on the mode!=0 path — on mode==0 the
// recomp NEVER touches r17, so it carries whatever buildFromChild's OWN caller left there (this port
// matches that by simply not writing c->r[17] on the mode==0 path); r18=node, r19=parent, r20=mode,
// r21=inVec on BOTH paths (propagateRotmat's own frame only spills r16-r20; propagate's spills
// r16-r23, so r22/r23 are never touched here either way — same "leave alone" argument applies to
// them). The tail-call ra (0x80051760 for propagate / 0x80051770 for propagateRotmat) IS mirrored
// into c->r[31] here — every `jal` overwrites $ra unconditionally, so (unlike the OUTER-caller case
// where a still-substrate caller's own compiled body already sets c->r[31] before reaching an
// overrides::dispatch-intercepted rec_dispatch) an internal tail-call inside an already-native function
// must set it itself, or the nested frame's ra spill diverges.
void NodeXform::buildFromChild(uint32_t nodeAddr, uint32_t inVec, uint32_t tableIdx, uint32_t mode) {
  Core* c = core;
  GuestFrame<48, 7> frame(c, kBuildFromChildSpills);
  Node node{c, nodeAddr};
  Node parent{c, c->mem_r32(ActorTomba::G_ADDR + tableIdx * 4u + 0xC0u)};
  GuestReg<16> r16(c); GuestReg<17> r17(c); GuestReg<18> r18(c);
  GuestReg<19> r19(c); GuestReg<20> r20(c); GuestReg<21> r21(c); GuestReg<31> ra(c);
  r18 = nodeAddr; r19 = parent.base; r20 = mode; r21 = inVec;
  if (mode == 0) {
    r16 = kScrSrcMatrix;
    mathOf(c).rotmat(node.localEulerPtr(), kScrSrcMatrix);
  } else {
    r16 = kScrSrcMatrix;
    r17 = kScrRot;
    seedDiagScratch(c, kScrCompose, node.localScaleX(), node.localScaleY(), node.localScaleZ());
    mathOf(c).rotmat(node.localEulerPtr(), kScrRot);
    mathOf(c).matMul(kScrRot, kScrCompose, kScrSrcMatrix);
  }
  mathOf(c).matMul(parent.frameMatrixPtr(), kScrSrcMatrix, node.worldMatrixPtr());
  mathOf(c).applyMatlv(inVec, node.worldPosPtr());
  node.setWorldPosX(node.worldPosX() + parent.framePosX32());
  node.setWorldPosY(node.worldPosY() + parent.framePosY32());
  node.setWorldPosZ(node.worldPosZ() + parent.framePosZ32());
  node.setLocalPosX16((int16_t)node.worldPosX());
  node.setLocalPosY16((int16_t)node.worldPosY());
  node.setLocalPosZ16((int16_t)node.worldPosZ());
  if (mode == 0) { ra = 0x80051770u; propagateRotmat(nodeAddr); }
  else           { ra = 0x80051760u; propagate(nodeAddr); }
}

// FUN_80051D90 — RE'd from generated/shard_7.c gen_func_80051D90 (frame: addiu sp,-0x20; spill
// r17(node)=sp+20, r16(outVec)=sp+16, ra=sp+24). The recomp calls FUN_800844C0 with ONLY r4 (=node's
// frameMatrix, 0x18) explicitly loaded — r5/r6 pass through UNCHANGED from this function's OWN
// incoming r5 (inVec)/r6 (outVec), i.e. FUN_800844C0(matrix, in=inVec, out=outVec) is a 3-arg libgte
// "ApplyMatrixLV, packed-SVECTOR-out" leaf distinct from the already-native Math::applyMatrixLV
// (which writes unclamped 32-bit MACs, not a packed int16 triple — see gen_func_800844C0 vs
// gen_func_80084470). FUN_800844C0 is OUTSIDE this region (0x800844C0) and UNOWNED (frameless,
// confirmed via generated/shard_3.c — no register-faithfulness consequence, but ra=0x80051DB0u
// mirrored anyway per ground truth, via guest_fn's ra_const argument). After that call, outVec holds
// the transformed local-space vector; this function adds node's LOCAL-frame position (framePos16,
// the low-16 view of the same bytes propagate's family accumulates as a full 32-bit world position)
// on top.
void NodeXform::worldPosFromLocal(uint32_t nodeAddr, uint32_t inVec, uint32_t outVec) {
  Core* c = core;
  GuestFrame<32, 3> frame(c, kWorldPosSpills);
  Node node{c, nodeAddr};
  GuestReg<17> r17(c); GuestReg<16> r16(c);
  r17 = nodeAddr; r16 = outVec;
  guest_fn(c, 0x800844C0u, 0x80051DB0u, node.frameMatrixPtr(), inVec, outVec);
  c->mem_w16(outVec + 0, (uint16_t)(c->mem_r16(outVec + 0) + (uint16_t)node.framePosX16()));
  c->mem_w16(outVec + 2, (uint16_t)(c->mem_r16(outVec + 2) + (uint16_t)node.framePosY16()));
  c->mem_w16(outVec + 4, (uint16_t)(c->mem_r16(outVec + 4) + (uint16_t)node.framePosZ16()));
}

// FUN_80051D20 — sibling of worldPosFromLocal() using node's COMPOSED world matrix and world-space
// position instead of the local ones. RE'd from generated/shard_6.c gen_func_80051D20 (same frame
// shape as worldPosFromLocal, ra=0x80051D40u before the FUN_800844C0 call).
void NodeXform::worldPosFromComposed(uint32_t nodeAddr, uint32_t inVec, uint32_t outVec) {
  Core* c = core;
  GuestFrame<32, 3> frame(c, kWorldPosSpills);
  Node node{c, nodeAddr};
  GuestReg<17> r17(c); GuestReg<16> r16(c);
  r17 = nodeAddr; r16 = outVec;
  guest_fn(c, 0x800844C0u, 0x80051D40u, node.worldMatrixPtr(), inVec, outVec);
  c->mem_w16(outVec + 0, (uint16_t)(c->mem_r16(outVec + 0) + (uint16_t)node.worldPosX()));
  c->mem_w16(outVec + 2, (uint16_t)(c->mem_r16(outVec + 2) + (uint16_t)node.worldPosY()));
  c->mem_w16(outVec + 4, (uint16_t)(c->mem_r16(outVec + 4) + (uint16_t)node.worldPosZ()));
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
  rend(c)->mNodeXform.seedBlock(c->r[4], (int16_t)c->r[5], (int16_t)c->r[6], (int16_t)c->r[7]);
}
static void eov_propagateRotmat(Core* c) {
  rend(c)->mNodeXform.propagateRotmat(c->r[4]);
  c->r[2] = 0;
}
static void eov_propagateAxis(Core* c) {
  rend(c)->mNodeXform.propagateAxis(c->r[4]);
  c->r[2] = 0;
}
// buildAxis (0x80051C8C): the native NodeXform::buildAxis body already mirrors the substrate's
// 32-byte frame internally (kBuildAxisSpills above, verified matching the abi_extract contract).
// Adding a trampoline-level GuestFrame here would create a DOUBLE frame (pushing sp 32 bytes too
// deep), which shifts all downstream callee spills to wrong addresses — confirmed: it moved the
// SBS diverge from f389 to f117. The MIRROR_VERIFY failure on 0x80051C8C is from the internal
// frame's register setup, NOT a missing trampoline frame. Left bare.
static void eov_buildAxis(Core* c) {
  rend(c)->mNodeXform.buildAxis(c->r[4]);
}

// --- WIDE-RE DRAFT wiring (2026-07-08 frontier pass) --- copyMatrixBlock / buildFromChild /
// worldPosFromLocal / worldPosFromComposed. copyMatrixBlock (0x80051B34) and buildFromChild
// (0x80051614) have direct same-module callers (confirmed via generated/shard_6.c, shard_5.c,
// shard_1.c) -> dual-wired. worldPosFromLocal (0x80051D90) / worldPosFromComposed (0x80051D20)
// have NO direct same-module caller (every reference is rec_dispatch from an AI overlay) ->
// registered rec_dispatch-only (nullptr setter), same as buildAxis above.
extern void gen_func_80051B34(Core*);
extern void gen_func_80051614(Core*);
static void eov_copyMatrixBlock(Core* c) {
  rend(c)->mNodeXform.copyMatrixBlock(c->r[4], c->r[5]);
}
static void eov_buildFromChild(Core* c) {
  rend(c)->mNodeXform.buildFromChild(c->r[4], c->r[5], c->r[6], c->r[7]);
  c->r[2] = 0;   // v0 always 0 at return -- tail-dispatches into propagate()/propagateRotmat(),
                 // both of which structurally always leave v0==0 (see the note above this block).
}
static void eov_worldPosFromLocal(Core* c) {
  rend(c)->mNodeXform.worldPosFromLocal(c->r[4], c->r[5], c->r[6]);
}
static void eov_worldPosFromComposed(Core* c) {
  rend(c)->mNodeXform.worldPosFromComposed(c->r[4], c->r[5], c->r[6]);
}
// buildWithOffset (0x800518FC) — the object matrix-compose-with-offset (svec scale + rotmat + matMul +
// applyMatrixLV + world-pos accumulate + propagate). Dual-wired: 8 direct substrate func_800518FC(c)
// call sites across the shards + many rec_dispatch/guest_leaf AI callers were ALL falling through to the
// substrate (no override registered); wiring here makes them native and lets MIRROR_VERIFY gate it. The
// native body mirrors its own 32-byte frame internally (kBuildSpills) — bare trampoline, like buildAxis
// (a GuestFrame here would double the frame). v0 unset (void guest leaf; matches build()/propagate()).
// This also RETIRES engine.cpp's Engine::objMatrixCompose — a duplicate of the SAME guest fn via
// substrate leaves (found by codemap --conflicts); its lone caller now routes through here.
extern void gen_func_800518FC(Core*);
static void eov_buildWithOffset(Core* c) {
  rend(c)->mNodeXform.buildWithOffset(c->r[4]);
}

extern void gen_func_80051C8C(Core*);   // buildAxis — rec_dispatch-only (no direct caller)
extern void gen_func_80051D90(Core*);   // worldPosFromLocal — rec_dispatch-only
extern void gen_func_80051D20(Core*);   // worldPosFromComposed — rec_dispatch-only

void NodeXform::registerOverrides(Game* /*game*/) {
  using overrides::install;
  // Dual-wired (direct same-module func_<addr>(c) callers exist) -> shard_set_override installs the thunk.
  install(0x800517BCu, "NodeXform::seedBlock",       eov_seedBlock,       gen_func_800517BC, shard_set_override);
  install(0x80051300u, "NodeXform::propagateRotmat", eov_propagateRotmat, gen_func_80051300, shard_set_override);
  install(0x80051464u, "NodeXform::propagateAxis",   eov_propagateAxis,   gen_func_80051464, shard_set_override);
  install(0x80051B34u, "NodeXform::copyMatrixBlock", eov_copyMatrixBlock, gen_func_80051B34, shard_set_override);
  install(0x80051614u, "NodeXform::buildFromChild",  eov_buildFromChild,  gen_func_80051614, shard_set_override);
  install(0x800518FCu, "NodeXform::buildWithOffset", eov_buildWithOffset, gen_func_800518FC, shard_set_override);
  // 0x80051C8C / 0x80051D90 / 0x80051D20 have no direct same-module caller — every reference is
  // rec_dispatch(c, addr) from an overlay, so no thunk (setter omitted); rec_dispatch covers them.
  install(0x80051C8Cu, "NodeXform::buildAxis",            eov_buildAxis,            gen_func_80051C8C);
  install(0x80051D90u, "NodeXform::worldPosFromLocal",    eov_worldPosFromLocal,    gen_func_80051D90);
  install(0x80051D20u, "NodeXform::worldPosFromComposed", eov_worldPosFromComposed, gen_func_80051D20);
}
