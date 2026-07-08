// game/ai/beh_substate_edge_leaves.cpp — WIDE-RE DRAFT case-handler leaves of
// beh_substate_edge_orchestrator (game/ai/beh_substate_edge_orchestrator.cpp, guest 0x8012EB54).
//
// STATUS: UNWIRED / UNVERIFIED (wide-RE tier, docs/fleet-workflow.md §6). These are hand-
// transliterated 1:1 from generated/ov_a00_shard_{0,1}.c ground truth (ov_a00_gen_<addr>) — NOT
// mechanically diffed against it yet. Per §9, a wiring pass MUST re-diff every line against the
// generated C before registering + SBS-gating. Nothing here is called from anywhere (no
// EngineOverrides registration, no shard_set_override) — dead code that only needs to COMPILE.
//
// Drafted this session: 0x8012E8A8 (162 gen-C ln), 0x8012F494 (64 ln), 0x80130524 (133 ln).
// The other 3 leaves the orchestrator calls (0x8012ED84 401 ln, 0x8012F5B4 428 ln, 0x8012FD88
// 406 ln) were RE'd for call-graph/field shape but NOT drafted this session (too large to transcribe
// with confidence in one pass) — see docs/engine_re.md for the mapping notes.
//
// Field notes common to all three (from the orchestrator's own header + this session's RE):
//   obj[4]=state, obj[5]=substate, obj[8]=childCount(u8), obj[0xC0+4*i]=child-pointer table (SAME
//   table NodeXform::propagate walks) — obj[0x60] here is 0x1F800137 (fixed CD/controller-state
//   scratchpad byte guarded by case 1 of the orchestrator, unrelated to this file's obj fields).
#include "core.h"
#include "cfg.h"
#include "math/gte_math.h"   // Math::rotmat/matMul/applyMatlv/applyMatrixLV/rotY/rotZ (c->math)
#include "math/mtx.h"        // Mtx::identity (c->mtx)
#include <stdint.h>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {
constexpr uint32_t SCR_A = 0x1F800000u;  // scratch matrix A (rotmat/identity dest)
constexpr uint32_t SCR_B = 0x1F800020u;  // scratch matrix B (identity/rotZ/rotY compose dest)
}  // namespace

// func_8012E8A8 — DRAFT. RE'd from generated/ov_a00_shard_1.c gen_8012E8A8 (0x8012E8A8..0x8012EB2C).
// Called by beh_substate_edge_orchestrator's per-node[1] tail case
// (game/ai/beh_substate_edge_orchestrator.cpp:134, `rec_dispatch(c, 0x8012E8A8u)`).
//
// Per-child transform-propagate variant of the NodeXform::propagate family (game/render/
// node_xform.cpp): iterates i in [0, obj[8]) over the child-pointer table at obj[0xC0 + 4*i] (the
// SAME table NodeXform walks). Frame -48: spills r16..r22+ra at +16/20/24/28/32/36/40/44 (mirrored
// below as plain save/restore of the incoming register values — none of the callees this leaf
// invokes are themselves NodeXform-family nested-frame functions, so no register-faithfulness
// forwarding is needed per the node_xform.cpp precedent).
//
// Per child (ptr `child` = obj[0xC0+4*i]), branch on `sentinel = (s16)child[6]`:
//   sentinel == -1  ("root" slot):
//     rotmat(child+8 euler -> SCR_A); matMul(obj+0x98(152)=rPtr, SCR_A=mPtr, child+0x18(24)=outPtr);
//     applyMatlv(child, child+0x2C(44));
//     child[0x2C/0x30/0x34] += sx16(obj[0x2E]/[0x32]/[0x36])   <- obj's OWN raw offset fields, NOT
//       the accumulated 0xAC/B0/B4 world-pos fields NodeXform::propagate's ROOT case uses. This is
//       the one confirmed divergence from that sibling function.
//   sentinel != -1, AND (i==2 OR (i==3 && obj[0x60]&2)) ("attach" slots — LOW CONFIDENCE, obj+0x60/
//   +0x56/+0x36(reused as euler Y here, distinct from the ROOT case's translate use of the same
//   offset) roles are inferred from operand shape only, never confirmed against a live dump):
//     rotmat(child+8 -> SCR_A); identity(SCR_B);
//     base = (obj[0x60]&1) ? obj[0xC4] : obj[0xC0];
//     rotZ(sx16(base+12), SCR_B); rotY(sx16(obj[0x56]), SCR_B);
//     matMul(SCR_B=rPtr, SCR_A=mPtr, child+0x18=outPtr);
//     [[shared tail below]]
//   sentinel != -1, all other i (plain "parent lookup" slots):
//     if (obj[0x60]&1) { identity(SCR_A); UNOWNED FUN_80084A80(child+8, SCR_A) [rec_dispatch — no
//       native owner found for 0x80084A80 per tools/codemap.py] }
//     else               { rotmat(child+8, SCR_A); }
//     [[shared tail below, using SCR_A directly as rPtr instead of SCR_B]]
//
//   [[shared tail]] (both non-root branches): p = obj[0xC0 + 4*sentinel] (same array — this
//     child's OWN child[6] value re-indexes obj's table, exactly like NodeXform::propagate's
//     SIBLING case): matMul(p+0x18=rPtr, SCR=mPtr, child+0x18=outPtr); applyMatlv(child,
//     child+0x2C); child[0x2C/0x30/0x34] += p[0x2C/0x30/0x34].
//
// UNVERIFIED: this leaf sits behind the idle/active field path per the orchestrator's own header
// note — likely NOT exercised by intro-area SBS autonav. A future session with wider scene coverage
// should re-gate after wiring.
void func_8012E8A8(Core* c) {
  const uint32_t obj = c->r[4];
  const uint32_t s16 = c->r[16], s17 = c->r[17], s18 = c->r[18];
  const uint32_t s19 = c->r[19], s20 = c->r[20], s21 = c->r[21], s22 = c->r[22];
  c->r[29] -= 48;

  const uint8_t count = c->mem_r8(obj + 8);
  for (int i = 0; i < (int)count; i++) {
    const uint32_t child = c->mem_r32(obj + 0xC0u + 4u * (uint32_t)i);
    const int16_t sentinel = c->mem_r16s(child + 6);
    const uint16_t sentinelRaw = c->mem_r16(child + 6);   // raw (unsigned) index for the table lookup

    if (sentinel == -1) {
      c->math.rotmat(child + 8, SCR_A);
      c->math.matMul(obj + 0x98u, SCR_A, child + 0x18u);
      c->math.applyMatlv(child, child + 0x2Cu);
      c->mem_w32(child + 0x2Cu, c->mem_r32(child + 0x2Cu) + (uint32_t)(int32_t)c->mem_r16s(obj + 0x2Eu));
      c->mem_w32(child + 0x30u, c->mem_r32(child + 0x30u) + (uint32_t)(int32_t)c->mem_r16s(obj + 0x32u));
      c->mem_w32(child + 0x34u, c->mem_r32(child + 0x34u) + (uint32_t)(int32_t)c->mem_r16s(obj + 0x36u));
      continue;
    }

    uint32_t rPtr;
    const bool attachSlot = (i == 2) || (i == 3 && (c->mem_r16(obj + 0x60u) & 2) != 0);
    if (attachSlot) {
      c->math.rotmat(child + 8, SCR_A);
      c->mtx.identity(SCR_B);
      const uint32_t base = (c->mem_r16(obj + 0x60u) & 1) ? c->mem_r32(obj + 0xC4u) : c->mem_r32(obj + 0xC0u);
      c->math.rotZ(c->mem_r16s(base + 12), SCR_B);
      c->math.rotY(c->mem_r16s(obj + 0x56u), SCR_B);
      c->math.matMul(SCR_B, SCR_A, child + 0x18u);
      rPtr = SCR_A;  // unused past this point; matMul above already produced child+0x18
    } else {
      if (c->mem_r16(obj + 0x60u) & 1) {
        c->mtx.identity(SCR_A);
        c->r[4] = child + 8; c->r[5] = SCR_A;
        rec_dispatch(c, 0x80084A80u);   // UNOWNED leaf — no native owner found (tools/codemap.py)
      } else {
        c->math.rotmat(child + 8, SCR_A);
      }
      rPtr = SCR_A;
    }
    (void)rPtr;

    const uint32_t p = c->mem_r32(obj + 0xC0u + 4u * (uint32_t)sentinelRaw);
    if (!attachSlot) {
      c->math.matMul(p + 0x18u, SCR_A, child + 0x18u);
    }
    c->math.applyMatlv(child, child + 0x2Cu);
    c->mem_w32(child + 0x2Cu, c->mem_r32(child + 0x2Cu) + c->mem_r32(p + 0x2Cu));
    c->mem_w32(child + 0x30u, c->mem_r32(child + 0x30u) + c->mem_r32(p + 0x30u));
    c->mem_w32(child + 0x34u, c->mem_r32(child + 0x34u) + c->mem_r32(p + 0x34u));
  }

  c->r[31] = 0; // restored by caller's own frame convention below
  c->r[22] = s22; c->r[21] = s21; c->r[20] = s20; c->r[19] = s19;
  c->r[18] = s18; c->r[17] = s17; c->r[16] = s16;
  c->r[29] += 48;
}

// func_8012F494 — DRAFT. RE'd from generated/ov_a00_shard_0.c gen_8012F494 (0x8012F494..0x8012F5A4).
// Called by beh_substate_edge_orchestrator's node[5]==0 sub-state case
// (game/ai/beh_substate_edge_orchestrator.cpp:71, `rec_dispatch(c, 0x8012F494u)`). Frame -24:
// spills r16+ra at +16/+20.
//
// Structure: if obj[6]==0, call the (still-unowned) counter-advance leaf 0x801314B4 and post-
// increment obj[6]. Then a flag/type gate on obj[122]&2 (bit1) and obj[96]&0xF0 selects one of
// three sub-paths:
//   - obj[96]&2 clear, obj[96]&0xF0==0x40: call 0x80130788(obj,1); on nonzero result set obj[5]=v0,
//     obj[6]=0 and skip the tail call; on ==0 result with obj[3]==2 AND
//     (obj[100] != sx16(obj[84])&0xFFF) something: recompute a "delta-clamped" value into
//     child(obj[196])[8] — LOW CONFIDENCE past the `r6=mem_r32(obj+196)` load (a fixed-point angle
//     clamp against a 2049/0xF000 threshold, never confirmed against a live sample).
//   - obj[96]&2 set (r3==64 path skipped): clear obj[120] bit1 if set.
//   - (fallthrough / obj[96]&0xF0!=0x40, obj[96]&2 clear): call the tail leaf 0x801308E0(obj)
//     unconditionally.
// The tail leaf 0x801308E0(obj) is invoked in the "not already returned" paths (root branch when
// r2==0 AND the two extra conditions fail, and the obj[96]&2-clear/!=0x40 fallthrough — see the
// `goto L_8012F59C` labels in ground truth). Transliterated 1:1 below preserving that exact branch
// shape (kept close to the generated C's own control flow rather than re-derived semantics, since
// the field roles past obj[96]/[122]/[100]/[84] are NOT independently confirmed).
void func_8012F494(Core* c) {
  const uint32_t obj = c->r[4];
  const uint32_t s16 = c->r[16];
  c->r[29] -= 24;

  if (c->mem_r8(obj + 6) == 0) {
    c->r[4] = obj; rec_dispatch(c, 0x801314B4u);   // UNOWNED — counter-advance leaf
    c->mem_w8(obj + 6, (uint8_t)(c->mem_r8(obj + 6) + 1));
  }

  bool callTail = true;
  if ((c->mem_r16(obj + 122) & 2) == 0) {
    if ((c->mem_r16(obj + 96) & 240) == 64) {
      callTail = false;
      c->r[4] = obj; c->r[5] = 1;
      rec_dispatch(c, 0x80130788u);   // UNOWNED
      uint32_t v0 = c->r[2];
      if (v0 != 0) {
        c->mem_w8(obj + 5, (uint8_t)v0);
        c->mem_w8(obj + 6, 0);
      } else if (c->mem_r8(obj + 3) == 2 &&
                 (uint16_t)c->mem_r16(obj + 100) != (uint16_t)(c->mem_r16s(obj + 84) & 4095)) {
        const uint32_t childPtr = c->mem_r32(obj + 196);
        int32_t band = (int32_t)(int16_t)(c->mem_r16(childPtr + 8) - 4);
        uint32_t clampedHi = ((uint32_t)band < 2049) ? (uint32_t)band : (uint32_t)(band | 0xF000);
        int32_t target = (int16_t)c->mem_r16(obj + 100);
        uint32_t result = ((int32_t)((uint32_t)target << 16) < (int32_t)(clampedHi << 16))
                               ? (clampedHi & 4095)
                               : ((uint32_t)target & 4095);
        c->mem_w16(childPtr + 8, (uint16_t)result);
        callTail = true;
      } else {
        callTail = true;
      }
    } else {
      callTail = true;
    }
  } else {
    if ((c->mem_r16(obj + 120) & 2) != 0) c->mem_w16(obj + 120, 0);
    callTail = true;
  }

  if (callTail) {
    c->r[4] = obj; rec_dispatch(c, 0x801308E0u);   // UNOWNED tail leaf
  }

  c->r[16] = s16;
  c->r[29] += 24;
}

// func_80130524 — DRAFT. RE'd from generated/ov_a00_shard_1.c gen_80130524 (0x80130524..0x80130778).
// Called by beh_substate_edge_orchestrator's node[5]==3 sub-state case
// (game/ai/beh_substate_edge_orchestrator.cpp:82, `rec_dispatch(c, 0x80130524u)`). Frame -24:
// spills r16+ra at +16/+20.
//
// Structure (LOW-MEDIUM CONFIDENCE on field roles — transliterated close to ground truth's own
// control flow; obj[64]/[70]/[71]/[94]/[108]/[112]/[118]/[120] are counters/flags whose exact game
// meaning is not independently confirmed):
//   switch on obj[6] (0/1/2, else falls to the common tail at L_80130758):
//     0: pick a "target angle" from obj[71] bits 0x40/0x80 (32/96/64), stash it to
//        obj[196][+0x12] (a linked child's angle field) and to obj[64] (own copy); look up a
//        per-type child transform pointer at obj[0xC0 + 4*sx16(obj[108])], call the RNG-ish leaf
//        0x80077768(obj[70]<<4, child[10]) and if nonzero, NEGATE obj[196][+0x12]; then bump
//        obj[6] and set obj[94]=4 (a state-machine sub-code).
//     1: accumulate obj[196][+0xA] += obj[196][+0x12] (a heading integrator against a child node),
//        then a symmetric clamp of obj[196][+0xA] against sx16(obj[112])±sx16(obj[64]) (only runs
//        if within band), flip obj[196][+0xA]'s sign based on obj[196][+0x12]>0, negate
//        obj[196][+0x12] again, obj[64]-=16 and if obj[64]<=0 clamp to 10 + reset obj[94]=0 and
//        bump obj[6].
//     2: clamp obj[196][+0xA] toward sx16(obj[112]) by ±8, decrement obj[64]; at obj[64]==0, snap
//        obj[196][+0xA]=obj[112] and (if obj[120]!=0) borrow-and-clear a "pending flag" byte at
//        childOf(obj[192])[+62] into obj[5]/obj[6], else zero obj[5]/[6]/[118]/[72]/[78].
//   common tail (ALWAYS runs): call the (still-unowned) leaf 0x801308E0(obj); if its result (r2) is
//     nonzero, obj[196][+0xA] = obj[112] (final snap).
void func_80130524(Core* c) {
  const uint32_t obj = c->r[4];
  const uint32_t s16 = c->r[16];
  c->r[29] -= 24;

  const uint8_t sub = c->mem_r8(obj + 6);
  if (sub == 0) {
    uint16_t angle = (c->mem_r8(obj + 71) & 64) ? 32u : ((c->mem_r8(obj + 71) & 128) ? 96u : 64u);
    const uint32_t link = c->mem_r32(obj + 196);
    c->mem_w16(link + 18, angle);
    c->mem_w16(obj + 64, angle);
    const uint32_t child = c->mem_r32(obj + 0xC0u + 4u * (uint32_t)(uint32_t)(int32_t)c->mem_r16s(obj + 108));
    c->r[4] = (uint32_t)c->mem_r8(obj + 70) << 4; c->r[5] = c->mem_r16s(child + 10);
    rec_dispatch(c, 0x80077768u);   // UNOWNED
    if (c->r[2] != 0) c->mem_w16(link + 18, (uint16_t)(0 - c->mem_r16(link + 18)));
    c->mem_w8(obj + 94, 4);
    c->mem_w8(obj + 6, (uint8_t)(sub + 1));
  } else if (sub == 1) {
    const uint32_t link = c->mem_r32(obj + 196);
    c->mem_w16(link + 10, (uint16_t)(c->mem_r16(link + 10) + c->mem_r16(link + 18)));
    const int32_t hdg = c->mem_r16s(obj + 112), band = c->mem_r16s(obj + 64);
    const int32_t linkHdg = c->mem_r16s(link + 10);
    if (!((hdg + band < linkHdg) && !(linkHdg < hdg - band))) {
      c->mem_w16(link + 10, (uint16_t)((c->mem_r16s(link + 18) > 0) ? (uint32_t)(hdg + band) : (uint32_t)(hdg - band)));
      c->mem_w16(link + 18, (uint16_t)(0 - c->mem_r16(link + 18)));
      int32_t cnt = (int32_t)(int16_t)(c->mem_r16(obj + 64) - 16);
      c->mem_w16(obj + 64, (uint16_t)cnt);
      if (cnt <= 0) {
        c->mem_w16(obj + 64, 10);
        c->mem_w8(obj + 94, 0);
        c->mem_w8(obj + 6, (uint8_t)(sub + 1));
      }
    }
  } else if (sub == 2) {
    const uint32_t link = c->mem_r32(obj + 196);
    int16_t hdg = c->mem_r16s(obj + 112);
    int16_t linkHdg = c->mem_r16s(link + 10);
    int16_t nv = (hdg < linkHdg) ? (int16_t)(linkHdg - 8) : (int16_t)(linkHdg + 8);
    c->mem_w16(link + 10, (uint16_t)nv);
    int16_t cnt = (int16_t)(c->mem_r16(obj + 64) - 1);
    c->mem_w16(obj + 64, (uint16_t)cnt);
    if (cnt == 0) {
      c->mem_w16(link + 10, (uint16_t)hdg);
      if (c->mem_r16s(obj + 120) != 0) {
        const uint32_t child = c->mem_r32(obj + 192);
        c->mem_w8(obj + 5, 1);
        c->mem_w8(obj + 6, c->mem_r8(child + 62));
        c->mem_w8(child + 62, 0);
      } else {
        c->mem_w8(obj + 5, 0);
        c->mem_w8(obj + 6, 0);
        c->mem_w16(obj + 118, 0);
        c->mem_w16(obj + 72, 0);
        c->mem_w16(obj + 78, 0);
      }
    }
  }

  c->r[4] = obj; rec_dispatch(c, 0x801308E0u);   // UNOWNED tail leaf
  if (c->r[2] != 0) {
    const uint32_t link = c->mem_r32(obj + 196);
    c->mem_w16(link + 10, c->mem_r16(obj + 112));
  }

  c->r[16] = s16;
  c->r[29] += 24;
}
