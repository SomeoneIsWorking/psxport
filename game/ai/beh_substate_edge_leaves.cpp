// game/ai/beh_substate_edge_leaves.cpp — WIDE-RE DRAFT case-handler leaves of
// beh_substate_edge_orchestrator (game/ai/beh_substate_edge_orchestrator.cpp, guest 0x8012EB54).
//
// STATUS: UNWIRED / UNVERIFIED (wide-RE tier, docs/fleet-workflow.md §6). These are hand-
// transliterated 1:1 from generated/ov_a00_shard_{0,1}.c ground truth (ov_a00_gen_<addr>) — NOT
// mechanically diffed against it yet. Per §9, a wiring pass MUST re-diff every line against the
// generated C before registering + SBS-gating. Nothing here is called from anywhere (not installed
// in the override registry, no shard_set_override) — dead code that only needs to COMPILE.
//
// Drafted 2026-07-08: 0x8012E8A8 (162 gen-C ln), 0x8012F494 (64 ln), 0x80130524 (133 ln).
// Drafted 2026-07-10 (dedicated wide-RE pass, near-mechanical goto-preserving transliteration —
// same style as game/ai/beh_cull_substate_leaves.cpp's 0x80132A88/0x80132EDC, chosen specifically
// to minimize branch-polarity/operand-order risk on this wide-frame cluster per
// docs/fleet-workflow.md §9): 0x8012ED84 (401 gen-C ln, edge-orchestrator STATE 0 init).
//
// STILL MAPPED-ONLY, NOT drafted (0x8012F5B4 428 ln, 0x8012FD88 406 ln — see docs/engine_re.md for
// the call-graph/field-shape mapping notes; a prior 2026-07-10 pass judged these too large/uncertain
// for one confident pass and this session's budget went to a careful single draft of 0x8012ED84
// instead, per "correctness over coverage — draft ONE well rather than three badly").
//
// Field notes common to all three (from the orchestrator's own header + this session's RE):
//   obj[4]=state, obj[5]=substate, obj[8]=childCount(u8), obj[0xC0+4*i]=child-pointer table (SAME
//   table NodeXform::propagate walks) — obj[0x60] here is 0x1F800137 (fixed CD/controller-state
//   scratchpad byte guarded by case 1 of the orchestrator, unrelated to this file's obj fields).
#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "math/gte_math.h"   // Math::rotmat/matMul/applyMatlv/applyMatrixLV/rotY/rotZ (mathOf(c))
#include "math/mtx.h"        // Mtx::identity (mtxOf(c))
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
      mathOf(c).rotmat(child + 8, SCR_A);
      mathOf(c).matMul(obj + 0x98u, SCR_A, child + 0x18u);
      mathOf(c).applyMatlv(child, child + 0x2Cu);
      c->mem_w32(child + 0x2Cu, c->mem_r32(child + 0x2Cu) + (uint32_t)(int32_t)c->mem_r16s(obj + 0x2Eu));
      c->mem_w32(child + 0x30u, c->mem_r32(child + 0x30u) + (uint32_t)(int32_t)c->mem_r16s(obj + 0x32u));
      c->mem_w32(child + 0x34u, c->mem_r32(child + 0x34u) + (uint32_t)(int32_t)c->mem_r16s(obj + 0x36u));
      continue;
    }

    uint32_t rPtr;
    const bool attachSlot = (i == 2) || (i == 3 && (c->mem_r16(obj + 0x60u) & 2) != 0);
    if (attachSlot) {
      mathOf(c).rotmat(child + 8, SCR_A);
      mtxOf(c).identity(SCR_B);
      const uint32_t base = (c->mem_r16(obj + 0x60u) & 1) ? c->mem_r32(obj + 0xC4u) : c->mem_r32(obj + 0xC0u);
      mathOf(c).rotZ(c->mem_r16s(base + 12), SCR_B);
      mathOf(c).rotY(c->mem_r16s(obj + 0x56u), SCR_B);
      mathOf(c).matMul(SCR_B, SCR_A, child + 0x18u);
      rPtr = SCR_A;  // unused past this point; matMul above already produced child+0x18
    } else {
      if (c->mem_r16(obj + 0x60u) & 1) {
        mtxOf(c).identity(SCR_A);
        c->r[4] = child + 8; c->r[5] = SCR_A;
        rec_dispatch(c, 0x80084A80u);   // UNOWNED leaf — no native owner found (tools/codemap.py)
      } else {
        mathOf(c).rotmat(child + 8, SCR_A);
      }
      rPtr = SCR_A;
    }
    (void)rPtr;

    const uint32_t p = c->mem_r32(obj + 0xC0u + 4u * (uint32_t)sentinelRaw);
    if (!attachSlot) {
      mathOf(c).matMul(p + 0x18u, SCR_A, child + 0x18u);
    }
    mathOf(c).applyMatlv(child, child + 0x2Cu);
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

// func_8012ED84 — DRAFT (2026-07-10 dedicated wide-RE pass). RE'd from generated/ov_a00_shard_1.c
// ov_a00_gen_8012ED84 (0x8012ED84..0x8012F464, 401 gen-C lines). Called by
// beh_substate_edge_orchestrator's STATE 0 (per-type init) case
// (game/ai/beh_substate_edge_orchestrator.cpp, `rec_dispatch(c, 0x8012ED84u)`).
//
// Frame -56, widest in this cluster: spills r16..r23+r30+ra at +16/20/24/28/32/36/40/44/48/52.
//
// TRANSLITERATION STYLE: near-mechanical, register-preserving (same as
// game/ai/beh_cull_substate_leaves.cpp's 0x80132A88/0x80132EDC) — `c->r[N]` is used AS THE GEN CODE
// USES IT (not renamed into semantic locals), including places where a register's value is left
// over from an EARLIER, semantically-unrelated computation and then reused as a call argument or
// branch condition several lines later. This is deliberate, not sloppy: register-reuse-across-
// unrelated-uses bugs are a confirmed recurring bug class in this codebase (see the perobj_billboard
// findings — C2D4/C464/C8F4 register-faithfulness). Renaming into "clean" locals at draft time would
// silently paper over exactly this class of bug, so field/semantic naming is deferred to the wiring
// pass once a live-RAM dump can confirm roles.
//
// CONFIRMED STRUCTURE (RE'd this session, cross-checked line-by-line against ground truth twice):
//   1. 5-entry lookup loop filling obj[96/98/100/102/104] (u16 each) from the two-level GBASE table
//      (class = byte_table[0x8014A334+obj[3]]; obj[96+2i] = u16_table[0x8014A340+(class*5+i)*2]) —
//      matches docs/engine_re.md's corrected (5-entry, not 6) description.
//   2. obj[112] = (obj[3]==0) ? (u16)-170 : 0.
//   3. obj[0]=1, obj[4]++, obj[13]=4, obj[11]=0, obj[9]=0. r21 = *(u32*)0x801ECFAC (a fixed
//      main-RAM global — LOW CONFIDENCE on role, inferred as a record-pool base pointer from how
//      r21 is added to every table-lookup result below and stored into a `+64` field).
//   4. TWO near-identical record-allocation loops (up to 3 entries, walking obj[192/196/200/204] —
//      the SAME per-slot child-record array `0x8012ED84`'s callers/siblings call "linked child"),
//      differing only in which GBASE table/constant-base they read from and which of two flag-bit
//      tests (obj[96]&1 vs obj[96]&2/&4) gates entry — selected by a leftover register (r4, still
//      holding `obj[96]&1` from step 3's test) that is ALSO the a0 ARGUMENT to the record-alloc
//      call `rec_dispatch(0x8007AAE8)` in BOTH loops (confirmed: no intervening write to r4 in
//      ground truth between the `obj[96]&1` test and either loop's call site). A null result from
//      0x8007AAE8 in EITHER loop sets obj[4]=3 and jumps STRAIGHT to the epilogue
//      (L_8012F414->L_8012F464 in ground truth), BYPASSING the entire common tail below — this is
//      the cluster's known epilogue-bypass trap shape (docs/engine_re.md, 0x80132EDC precedent);
//      confirmed twice by re-reading ground truth's goto targets, not assumed.
//   5. Common tail (only reached if both record-alloc attempts, if taken, succeeded): default
//      obj[128/130/132/134] position-table values (100/200/125/250); conditional obj[196 or
//      192]->+12 = 170 gated by obj[96] bits 8 and 1; a 4-way switch on obj[3] (==3 / <4 / ==0 /
//      ==11 / else) writing obj[196]->+8 and, for the ==0 and ==11 sub-cases, extra fields on
//      obj[200]/[204]/[106]/[196]->+10; then unconditional calls into 0x80131600, 0x801314B4,
//      0x8012E8A8 (the leaf drafted earlier in this file — confirms it's a shared sub-routine, not
//      just an orchestrator-tail call, per docs/engine_re.md), zeroing obj[110/114/118/122], then
//      0x80133444; a nested obj[3]<3 / obj[3]<2 gate calling 0x8013892C / 0x80125F50 (the latter's
//      RETURN VALUE r2, masked &3, becomes a NEW child-slot index into obj[0xC0+4*idx] whose
//      record[62] gets `|3`'d); unconditional 0x801312CC; then an obj[3]==0/==1/else 3-way call
//      into 0x8004CBD8(obj, 0 or 6) (skipped entirely for obj[3]>=2); a final fixed main-RAM byte
//      read at 0x800BF89C (`0x800C0000-1892`, LOW CONFIDENCE role — some kind of global game-mode/
//      difficulty flag, gates obj[5]=5 when ==2 AND obj[3]!=0); obj[41]=0, obj[43]=0 always.
//   Every sub-call routed via rec_dispatch per this file's convention (see header note); the ra
//   spills before overlay-local calls (`c->r[31] = 0x8012Fxxxu`) are kept literal from ground truth
//   per CLAUDE.md's guest-stack-mirroring directive — they are genuinely part of the machine state
//   the callee's own frame may read back, not decorative comments.
//
// LOW CONFIDENCE items (flagged, not resolved — a live-RAM-dump wiring pass should confirm):
//   - r21's role (0x801ECFAC global pointer) and the "+64 field = r21+lookup" meaning.
//   - The 0x800BF89C fixed-address byte read's semantic role.
//   - obj[0xC0+4*idx] reuse as a child-slot index at the very end (idx = 80125F50's result & 3) —
//     same child-pointer table 0x8012E8A8/NodeXform::propagate walk, but this is the first site in
//     the cluster that WRITES into a slot's record[62] rather than just reading the table.
//
// UNVERIFIED: UNWIRED (not installed in the override registry, no shard_set_override, no SBS run) per
// docs/fleet-workflow.md §6/§9. A wiring pass MUST re-diff line-by-line against
// generated/ov_a00_shard_1.c:18777-19125 before registering + SBS-gating (per §9, drafts in this
// cluster have historically needed such a re-diff to catch bugs even after careful hand-tracing).
void func_8012ED84(Core* c) {
  constexpr uint32_t TBL_U16   = 0x8014A340u;  // 0x80150000 - 23744
  constexpr uint32_t TBL_CLASS = 0x8014A334u;  // 0x80150000 - 23756

  const uint32_t s16 = c->r[16], s17 = c->r[17], s18 = c->r[18], s19 = c->r[19];
  const uint32_t s20 = c->r[20], s21 = c->r[21], s22 = c->r[22], s23 = c->r[23];
  const uint32_t s30 = c->r[30], sra = c->r[31];
  c->r[29] -= 56;

  c->r[19] = c->r[4];   // obj
  c->r[18] = 0;

  // Step 1: 5-entry two-level table lookup filling obj[96..104].
  for (;;) {
    uint32_t r2 = (uint32_t)c->mem_r8(c->r[19] + 3u);
    r2 = r2 + TBL_CLASS;
    uint32_t r3 = (uint32_t)c->mem_r8(r2);          // class byte
    r2 = (r3 << 2) + r3;                             // class * 5
    r2 = r2 + c->r[18];
    r2 = r2 << 1;
    r2 = r2 + TBL_U16;
    r2 = (uint32_t)c->mem_r16(r2);
    c->r[18] = c->r[18] + 1u;
    c->mem_w16(c->r[4] + 96u, (uint16_t)r2);          // via r4 (== obj at loop entry, walks +2/iter)
    const bool cont = (int32_t)c->r[18] < 5;
    c->r[4] = c->r[4] + 2u;
    if (cont) continue;
    break;
  }

  // Step 2: obj[112].
  if (c->mem_r8(c->r[19] + 3u) == 0) {
    c->mem_w16(c->r[19] + 112u, (uint16_t)(uint32_t)-170);
  } else {
    c->mem_w16(c->r[19] + 112u, 0);
  }

  // Step 3: header init.
  {
    uint32_t r2 = (uint32_t)c->mem_r8(c->r[19] + 4u);
    c->mem_w8(c->r[19] + 0u, 1u);
    uint32_t r3 = (uint32_t)c->mem_r16(c->r[19] + 96u);   // obj[96], kept live across both loops
    r2 = r2 + 1u;
    c->mem_w8(c->r[19] + 4u, (uint8_t)r2);
    c->r[21] = c->mem_r32(0x800ECFACu);   // *(u32*)0x800ECFAC (32783<<16 - 12372) — fixed main-RAM
                                            // global, LOW CONFIDENCE role (inferred record-pool base)
    r2 = 4u;
    c->r[4] = r3 & 1u;                     // LIVE past this point: also the arg to 0x8007AAE8 below
    c->mem_w8(c->r[19] + 13u, (uint8_t)r2);
    c->mem_w8(c->r[19] + 11u, 0u);
    c->mem_w8(c->r[19] + 9u, 0u);
    c->r[22] = c->r[21] + r2;   // r22 = r21 + 4  (side effect always runs, matches ground truth)

    if (c->r[4] == 0u) {
      goto L_8012F06C;
    }

    // (obj[96]&1)!=0 path.
    {
      uint32_t r2b = r3 & 2u;   // side effect (always assigned), the compare against 1 below is
      (void)r2b;                // structurally always-false here (r4 is 0 or 1, we're in r4==1) —
      // kept for structural fidelity; ground truth never actually takes goto L_8012F238 from this
      // specific comparison in this branch.
    }
    uint32_t r2c = (c->mem_r16(c->r[19] + 96u) & 2u) != 0u ? 12u : 7u;
    c->mem_w8(c->r[19] + 8u, (uint8_t)r2c);
    c->r[20] = 0u;
    if (c->mem_r8(c->r[19] + 8u) == 0u) {
      c->r[18] = c->r[20];
      goto L_8012F238;
    }
    c->r[23] = 3u;
    c->r[30] = 0x8014A274u;   // 0x80150000 - 23948
    c->r[16] = c->r[19];
    c->r[17] = c->r[30];

    // Loop A (obj[96]&1 != 0): up to 3 record-alloc entries walking obj[192/196/200/204].
    for (;;) {
      c->r[31] = 0x8012EEACu;
      rec_dispatch(c, 0x8007AAE8u);   // UNOWNED record-alloc; arg = leftover r4 (obj[96]&1)
      const uint32_t r3v = c->r[2];
      if (r3v == 0u) { c->mem_w8(c->r[19] + 4u, 3u); goto L_8012F464; }  // epilogue-bypass trap

      bool skip1017 = (c->mem_r16(c->r[19] + 96u) & 2u) != 0u;
      if (!skip1017 && c->r[18] == c->r[23]) {
        c->r[17] = c->r[17] + 10u;
        c->r[20] = c->r[20] + 1u;
      }

      c->mem_w32(c->r[16] + 192u, r3v);
      c->mem_w16(r3v + 6u, c->mem_r16(c->r[17] + 0u));
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 0u, c->mem_r16(c->r[17] + 2u));
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 2u, c->mem_r16(c->r[17] + 4u));
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 4u, c->mem_r16(c->r[17] + 6u));
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 8u, 0u);
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 10u, 0u);
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 12u, 0u);
      c->mem_w8(c->mem_r32(c->r[16] + 192u) + 62u, 0u);
      c->mem_w8(c->mem_r32(c->r[16] + 192u) + 63u, 0u);

      // Lookup switch keyed on r20 (a DIFFERENT counter from the r18 loop index — r20 can advance
      // by 2 in one iteration via the "!skip1017 && r18==r23" bump above, so r20 and r18 diverge;
      // ground truth's switch really is on r20, not r18). Goto-preserving per this file's house
      // style for this cluster (mechanical translation, not restructured, to avoid mis-nesting the
      // r20==2 vs r20==r23(3) vs r20>3 3-way split — an earlier draft pass of this exact block
      // nested r20==2 as an unconditional alias of r20==r23 and silently dropped the r20>3 default,
      // caught and fixed by re-reading ground truth line-by-line a second time).
      {
        uint32_t lookupAddr = 0, lookupVal = 0;
        if (c->r[20] == 1u) goto lA_EFBC;
        if (!((int32_t)c->r[20] < 2)) goto lA_EF80;
        if (c->r[20] == 0u) goto lA_EF9C;
        goto lA_F01C;

      lA_EF80:
        if (c->r[20] == 2u) goto lA_EFEC;
        if (c->r[20] == c->r[23]) {
          c->mem_w16(c->mem_r32(c->r[16] + 192u) + 10u, 2048u);
          goto lA_EFEC;
        }
        goto lA_F01C;

      lA_EF9C: {
        uint32_t rr2 = (uint32_t)c->mem_r8(c->r[19] + 96u);
        int32_t rr3 = c->mem_r16s(c->r[30] + 8u);
        rr2 = rr2 >> 4; rr2 = rr2 << 2; rr3 = rr3 << 2;
        lookupAddr = (uint32_t)rr3 + c->r[22] + rr2;
        goto lA_F02C;
      }
      lA_EFBC: {
        // r7 = 0x80150000 - 23938 = 0x8014A27E
        uint32_t rr2 = (uint32_t)c->mem_r16(c->r[19] + 96u);
        int32_t rr3 = c->mem_r16s(0x8014A27Eu + 8u);
        rr2 = rr2 & 3840u; rr2 = rr2 >> 6; rr3 = rr3 << 2;
        lookupAddr = (uint32_t)rr3 + c->r[22] + rr2;
        goto lA_F02C;
      }
      lA_EFEC: {
        c->mem_w8(c->mem_r32(c->r[16] + 192u) + 63u, (uint8_t)c->r[23]);
        uint32_t rr3 = c->mem_r16(c->r[19] + 96u) & 4u;
        int32_t rr2 = c->mem_r16s(c->r[17] + 8u);
        rr2 = rr2 << 2;
        lookupVal = c->mem_r32(rr3 + (uint32_t)rr2 + c->r[22]);
        goto lA_F030;
      }
      lA_F01C: {
        int32_t rr2 = c->mem_r16s(c->r[17] + 8u);
        rr2 = rr2 << 2;
        lookupAddr = (uint32_t)rr2 + c->r[22];
        goto lA_F02C;
      }
      lA_F02C:
        lookupVal = c->mem_r32(lookupAddr);
      lA_F030:
        c->mem_w32(c->mem_r32(c->r[16] + 192u) + 64u, c->r[21] + lookupVal);
      }

      c->r[16] = c->r[16] + 4u;
      c->r[18] = c->r[18] + 1u;
      c->r[17] = c->r[17] + 10u;
      c->r[20] = c->r[20] + 1u;
      uint32_t r9 = (uint32_t)c->mem_r8(c->r[19] + 9u);
      const bool cont = (int32_t)c->r[18] < (int32_t)c->mem_r8(c->r[19] + 8u);
      c->mem_w8(c->r[19] + 9u, (uint8_t)(r9 + 1u));
      if (cont) continue;
      break;
    }
    goto L_8012F238;
  }

L_8012F06C: {
    // (obj[96]&1)==0 path.
    uint32_t r3 = (uint32_t)c->mem_r16(c->r[19] + 96u);
    uint32_t r2 = (r3 & 4u) != 0u ? 7u : 3u;
    c->mem_w8(c->r[19] + 8u, (uint8_t)r2);
    if (c->mem_r8(c->r[19] + 8u) == 0u) {
      c->r[18] = 0u;
      goto L_8012F238;
    }
    c->r[20] = 0x8014A2ECu;   // 0x80150000 - 23828
    c->r[23] = c->r[20] + 20u;
    c->r[17] = c->r[20];
    c->r[16] = c->r[19];

    // Loop B (obj[96]&1 == 0): up to 3 record-alloc entries, mirrors Loop A with a different table.
    for (;;) {
      c->r[31] = 0x8012F0ACu;
      rec_dispatch(c, 0x8007AAE8u);   // UNOWNED record-alloc; arg = leftover r4 (== 0 in this path)
      const uint32_t r3v = c->r[2];
      if (r3v == 0u) { c->mem_w8(c->r[19] + 4u, 3u); goto L_8012F464; }  // epilogue-bypass trap

      c->mem_w32(c->r[16] + 192u, r3v);
      c->mem_w16(r3v + 6u, c->mem_r16(c->r[17] + 0u));
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 0u, c->mem_r16(c->r[17] + 2u));
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 2u, c->mem_r16(c->r[17] + 4u));
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 4u, c->mem_r16(c->r[17] + 6u));
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 8u, 0u);
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 10u, 0u);
      c->mem_w16(c->mem_r32(c->r[16] + 192u) + 12u, 0u);
      c->mem_w8(c->mem_r32(c->r[16] + 192u) + 62u, 0u);
      c->mem_w8(c->mem_r32(c->r[16] + 192u) + 63u, 0u);

      // Lookup switch keyed on r18 (the loop index itself, UNLIKE loop A which keys on the
      // diverging r20 — loop B never does the extra "+10/+1" bump, so its own counter stays the
      // switch key). Goto-preserving for the same reason as loop A's switch above: an earlier draft
      // pass nested r18==2 as a sub-case of r18==1 and silently dropped the r18>2 generic-default
      // fallthrough (L_8012F1F4) — re-read ground truth's goto targets line-by-line to fix.
      {
        uint32_t rDst = 0, rVal = 0;
        if (c->r[18] == 1u) goto lB_F194;
        if (!((int32_t)c->r[18] < 2)) goto lB_F158;
        goto lB_F168;   // r18==0 always here (r18==1 already excluded above)

      lB_F158:
        if (c->r[18] == 2u) goto lB_F1C0;
        goto lB_F1F4;

      lB_F168: {
        uint32_t rr2 = (uint32_t)c->mem_r8(c->r[19] + 96u);
        int32_t rr3 = c->mem_r16s(c->r[20] + 8u);
        rr2 = rr2 >> 4; rr2 = rr2 << 2; rr3 = rr3 << 2;
        rVal = c->r[21] + c->mem_r32((uint32_t)rr3 + c->r[22] + rr2);
        rDst = c->mem_r32(c->r[19] + 192u);
        goto lB_F210;
      }
      lB_F194: {
        uint32_t rr2 = (uint32_t)c->mem_r16(c->r[19] + 96u);
        int32_t rr3 = c->mem_r16s(c->r[20] + 18u);
        rr2 = rr2 & 3840u; rr2 = rr2 >> 6; rr3 = rr3 << 2;
        rVal = c->r[21] + c->mem_r32((uint32_t)rr3 + c->r[22] + rr2);
        rDst = c->mem_r32(c->r[19] + 196u);
        goto lB_F210;
      }
      lB_F1C0: {
        const uint32_t base = c->r[19] + 8u;   // FIXED slot (obj+8), not r16 — matches ground truth
        c->mem_w8(c->mem_r32(base + 192u) + 63u, 3u);
        uint32_t rr3 = c->mem_r16(c->r[19] + 96u) & 4u;
        int32_t rr2 = c->mem_r16s(c->r[23] + 8u);
        rr2 = rr2 << 2;
        rVal = c->r[21] + c->mem_r32(rr3 + (uint32_t)rr2 + c->r[22]);
        rDst = c->mem_r32(base + 192u);
        goto lB_F210;
      }
      lB_F1F4: {
        int32_t rr2 = c->mem_r16s(c->r[17] + 8u);
        rr2 = rr2 << 2;
        rVal = c->r[21] + c->mem_r32((uint32_t)rr2 + c->r[22]);
        rDst = c->mem_r32(c->r[16] + 192u);
        goto lB_F210;
      }
      lB_F210:
        c->mem_w32(rDst + 64u, rVal);
      }

      c->r[17] = c->r[17] + 10u;
      c->r[16] = c->r[16] + 4u;
      c->r[18] = c->r[18] + 1u;
      uint32_t r9 = (uint32_t)c->mem_r8(c->r[19] + 9u);
      const bool cont = (int32_t)c->r[18] < (int32_t)c->mem_r8(c->r[19] + 8u);
      c->mem_w8(c->r[19] + 9u, (uint8_t)(r9 + 1u));
      if (cont) continue;
      break;
    }
  }

L_8012F238: {
    // Common tail (only reached if a loop wasn't entered, or completed without a null-record abort).
    const uint32_t flags96 = (uint32_t)c->mem_r16(c->r[19] + 96u);
    c->mem_w16(c->r[19] + 128u, 100u);
    c->mem_w16(c->r[19] + 130u, 200u);
    c->mem_w16(c->r[19] + 132u, 125u);
    c->mem_w16(c->r[19] + 134u, 250u);

    if ((flags96 & 8u) != 0u) {
      const uint16_t v170 = 170u;
      if ((flags96 & 1u) != 0u) {
        c->mem_w16(c->mem_r32(c->r[19] + 196u) + 12u, v170);
      } else {
        c->mem_w16(c->mem_r32(c->r[19] + 192u) + 12u, v170);
      }
    }

    const uint32_t type3 = (uint32_t)c->mem_r8(c->r[19] + 3u);
    c->mem_w16(c->r[19] + 106u, 735u);
    uint32_t linkPtr, linkVal;
    if (type3 == 3u) {
      linkPtr = c->mem_r32(c->r[19] + 196u);
      linkVal = (uint32_t)-192;
    } else if (type3 < 4u) {
      if (type3 == 0u) {
        c->mem_w16(c->r[19] + 106u, 800u);
        c->mem_w16(c->mem_r32(c->r[19] + 200u) + 4u, (uint16_t)(uint32_t)-800);
        c->mem_w16(c->mem_r32(c->r[19] + 204u) + 4u, c->mem_r16(c->r[19] + 106u));
        c->mem_w16(c->mem_r32(c->r[19] + 196u) + 10u, c->mem_r16(c->r[19] + 112u));
        linkPtr = c->mem_r32(c->r[19] + 196u);
        linkVal = 240u;
      } else {
        linkPtr = c->mem_r32(c->r[19] + 196u);
        linkVal = (uint32_t)c->mem_r16(c->r[19] + 100u);
      }
    } else if (type3 == 11u) {
      c->mem_w16(c->r[19] + 106u, 734u);
      c->mem_w16(c->mem_r32(c->r[19] + 200u) + 4u, (uint16_t)(uint32_t)-734);
      c->mem_w16(c->mem_r32(c->r[19] + 196u) + 8u, 0u);
      linkPtr = 0u;  // no obj[+8] write below for this case (matches ground truth's goto L_8012F340)
      linkVal = 0u;
      goto after_link8;
    } else {
      linkPtr = c->mem_r32(c->r[19] + 196u);
      linkVal = (uint32_t)c->mem_r16(c->r[19] + 100u);
    }
    c->mem_w16(linkPtr + 8u, (uint16_t)linkVal);
  after_link8:;

    // Sub-leaf calls (record init).
    {
      const uint32_t link = c->mem_r32(c->r[19] + 196u);
      const uint32_t masked = c->mem_r16(link + 8u) & 4095u;
      c->r[4] = c->r[19];
      c->r[31] = 0x8012F35Cu;
      c->mem_w16(link + 8u, (uint16_t)masked);
      rec_dispatch(c, 0x80131600u);   // UNOWNED

      c->r[31] = 0x8012F364u;
      c->r[4] = c->r[19];
      rec_dispatch(c, 0x801314B4u);   // UNOWNED counter-advance leaf

      c->r[31] = 0x8012F36Cu;
      c->r[4] = c->r[19];
      rec_dispatch(c, 0x8012E8A8u);   // == func_8012E8A8, drafted above in this file

      c->r[4] = c->r[19];
      c->mem_w16(c->r[19] + 110u, 0u);
      c->mem_w16(c->r[19] + 114u, 0u);
      c->mem_w16(c->r[19] + 118u, 0u);
      c->r[31] = 0x8012F384u;
      c->mem_w16(c->r[19] + 122u, 0u);
      rec_dispatch(c, 0x80133444u);   // UNOWNED
    }

    if ((uint32_t)c->mem_r8(c->r[19] + 3u) < 3u) {
      c->r[31] = 0x8012F3A0u;
      c->r[4] = c->r[19];
      rec_dispatch(c, 0x8013892Cu);   // UNOWNED

      if ((uint32_t)c->mem_r8(c->r[19] + 3u) < 2u) {
        c->r[31] = 0x8012F3BCu;
        c->r[4] = c->r[19];
        rec_dispatch(c, 0x80125F50u);   // UNOWNED
        c->r[18] = c->r[2] & 3u;
        if (c->r[18] != 0u) {
          c->mem_w16(c->r[19] + 122u, (uint16_t)(c->r[2] << 4));
          const uint32_t slot = c->r[19] + (c->r[18] << 2);
          const uint32_t child = c->mem_r32(slot + 192u);
          c->mem_w8(child + 62u, (uint8_t)(c->mem_r8(child + 62u) | 3u));
        } else {
          c->mem_w16(c->r[19] + 122u, (uint16_t)c->r[2]);
        }
      }

      c->r[31] = 0x8012F3F8u;
      c->r[4] = c->r[19];
      rec_dispatch(c, 0x801312CCu);   // UNOWNED
    }

    const uint32_t type3b = (uint32_t)c->mem_r8(c->r[19] + 3u);
    if (type3b == 0u) {
      c->r[4] = c->r[19]; c->r[5] = 0u;
      c->r[31] = 0x8012F434u;
      rec_dispatch(c, 0x8004CBD8u);   // GraphicsBind-shaped, dual-owned per codemap — unconfirmed here
    } else if (type3b == 1u) {
      c->r[4] = c->r[19]; c->r[5] = 6u;
      c->r[31] = 0x8012F434u;
      rec_dispatch(c, 0x8004CBD8u);
    }

    if (c->mem_r8(0x800BF89Cu) == 2u && type3b != 0u) {
      c->mem_w8(c->r[19] + 5u, 5u);
    }
    c->mem_w8(c->r[19] + 41u, 0u);
    c->mem_w8(c->r[19] + 43u, 0u);
  }

L_8012F464:
  c->r[31] = sra;
  c->r[30] = s30;
  c->r[23] = s23;
  c->r[22] = s22;
  c->r[21] = s21;
  c->r[20] = s20;
  c->r[19] = s19;
  c->r[18] = s18;
  c->r[17] = s17;
  c->r[16] = s16;
  c->r[29] += 56;
}
