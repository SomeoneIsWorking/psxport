// game/ai/beh_toy_spawn_family.cpp — PC-native drafts for a cluster of small "toy/child spawner"
// leaves in the A00 gameplay overlay (0x80126000-0x80127798 band, the frontier just below the
// already-owned `beh_area_transition_machine` at 0x80127798).
//
// WIDE-RE TIER (UNWIRED, UNVERIFIED): these are faithful register-level transliterations of the
// generated substrate C (`generated/ov_a00_shard_{0,1}.c`, symbols `ov_a00_gen_<addr>`) — the
// ground truth for this overlay. NOT wired into rec_dispatch/EngineOverrides, NOT SBS-gated. A
// future frontier pass must confirm field-role names against a live RAM dump before wiring.
//
// This file covers 5 of the ~19 functions in the surrounding cluster (a per-object "toy" behavior
// family that spawns companion/effect child objects via the LEGACY allocator FUN_80072DDC —
// distinct from the newer Spawn::dispatch(cls,type,list) primitive used by the sibling
// spawnTypedChild family in game/world/spawn.cpp). The other 14 functions in the cluster are a
// gnarlier top-level state dispatcher (0x80126264 + helpers) that reads/writes a large shared
// global blob at GBASE (0x800BF870, the SAME struct beh_lift_platform.cpp already references by
// absolute address, e.g. mem[0x800BF854]/[0x800BF89C]/[0x800BF8B9]/[0x800BFAD8]) — mapped, not
// drafted, in docs/engine_re.md (needs a RAM dump to name GBASE's fields before it can be ported
// safely). See docs/engine_re.md "0x80126040-0x80127798 toy/child spawner cluster" for the full
// cluster writeup, including a few functions on the cusp that were mapped but not drafted here.
//
// FUN_80072DDC signature (established already — beh_a08_scene_actor.cpp's sub8013DD48, and
// beh_a06_multi_actor.cpp): (a0=owner, a1=?, a2=cls, a3=type) -> child node ptr or 0. Caller then
// stamps child[+0x1C] = handler fn ptr and other per-type fields — exactly the shape below.

#include "core.h"
#include <cstdint>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {

// GBASE — the shared global blob at 0x800BF870 (== 32780<<16 - 1936). Same struct
// beh_lift_platform.cpp names by absolute address (0x800BF854 etc); role of individual byte
// offsets used here (0x82=130 dec, 0x183=387 dec) is NOT yet confirmed against a RAM dump.
constexpr uint32_t GBASE = 0x800BF870u;
// mem[0x800BFA33] == 32780<<16 - 1549 : a second byte in the same blob, gates the "boosted" path.
constexpr uint32_t GBASE_BOOST_GATE = 0x800BFA33u;
// The table pointer constant every spawned child's [+0x1C]/[+0x1C]-adjacent field gets seeded
// with (32786<<16 + 25188 == 0x80019064) — same literal beh_lift_platform.cpp and
// beh_a06_script_fades.cpp neighbors use for their own per-child table hookup.
constexpr uint32_t CHILD_TABLE_PTR = 0x80019064u;

inline void leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
inline void leaf2(Core* c, uint32_t a0, uint32_t a1, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn);
}
inline uint32_t leaf4Ret(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3; rec_dispatch(c, fn); return c->r[2];
}

}  // namespace

// FUN_80127420(obj) — arm a 20-frame countdown if the linked object (obj[+0x10]'s target,
// obj[+0x10] read as a node ptr) has its [+0x5E] "ready" byte set. Trivial guard leaf.
//   if (*(u32*)(obj+0x10))[+0x5E] != 0:  obj[+0x40]=obj[+0x4A]=20; obj[+0x05]=1
uint32_t beh_arm_countdown_if_linked_ready_80127420(Core* c, uint32_t obj) {
  const uint32_t linked = c->mem_r32(obj + 0x10u);
  if (c->mem_r8(linked + 0x5Eu) != 0) {
    c->mem_w16(obj + 0x40u, 20u);
    c->mem_w16(obj + 0x4Au, 20u);
    c->mem_w8(obj + 0x05u, 1u);
  }
  return c->r[2];
}

// FUN_801274BC(obj) — a distance-band predicate. `row` is looked up from a per-slot pointer table
// at *(obj+0x10) indexed by obj[+0x60] (row = *(u32*)(base + slot*4 + 0xC0)), then compared against
// obj[+0x2E]. Returns a 3-way band code (VERBATIM branch order from the generated C — the >=601
// arm is the FIRST comparison, not the last):
//   d = row[+0x2C] - obj[+0x2E]      (16-bit, sign-extended)
//   if (d >= 601)  return  1
//   if (d >= 251)  return -1     (251 <= d < 601)
//   return 0                     (d < 251)
int32_t beh_distance_band_predicate_801274bc(Core* c, uint32_t obj) {
  const uint32_t base = c->mem_r32(obj + 0x10u);
  const uint32_t slot = (uint32_t)(int16_t)c->mem_r16(obj + 0x60u);
  const uint32_t row = c->mem_r32(base + slot * 4u + 0xC0u);
  const uint16_t rowVal = c->mem_r16(row + 0x2Cu);
  const uint16_t objVal = c->mem_r16(obj + 0x2Eu);
  const int16_t d = (int16_t)(rowVal - objVal);
  if (d < 601) return (d < 251) ? 0 : -1;
  return 1;
}

// FUN_80127720(owner) — spawn a type-5 companion child via the legacy allocator, no ready gate.
//   child = FUN_80072DDC(owner, 0, 2, 20)
//   if (child): FUN_8004D604(child, 1); child[+0x1C]=CHILD_TABLE_PTR; child[+3]=5; child[+0x5C]=0;
//               owner[+0x24]=30
//   return child (0 on pool-exhaustion)
uint32_t beh_spawn_toy_child_type5_80127720(Core* c, uint32_t owner) {
  const uint32_t child = leaf4Ret(c, owner, 0u, 2u, 20u, 0x80072DDCu);
  if (child != 0) {
    leaf2(c, child, 1u, 0x8004D604u);
    c->mem_w32(child + 0x1Cu, CHILD_TABLE_PTR);
    c->mem_w8(child + 0x03u, 5u);
    c->mem_w16(child + 0x5Cu, 0u);
    c->mem_w32(owner + 0x24u, 30u);
  }
  return child;
}

// FUN_8012763C(owner) — spawn a type-4 companion child, then feed GBASE's mode byte to pick which
// SFX/anim variant (77 vs 78) plays via FUN_8004ED94(id, 65), and bump owner[+0x24]=30.
//   child = FUN_80072DDC(owner, 0, 2, 20)
//   if (!child) return 0
//   FUN_8004D604(child, 1); child[+0x1C]=CHILD_TABLE_PTR; child[+3]=4; child[+0x5C]=0
//   if (GBASE[+0x82] == 1):
//     child[+0x68]=0; GBASE[+0x82] |= 0x10; child[+0x5E] = GBASE[+0x183] & 4
//   else:
//     child[+0x68]=1; child[+0x5E] = (GBASE[+0x183] >> 4) & 4
//   FUN_8004ED94(child[+0x5E]==0 ? 78 : 77, 65)
//   owner[+0x24]=30
//   return 1
uint32_t beh_spawn_toy_child_type4_8012763c(Core* c, uint32_t owner) {
  const uint32_t child = leaf4Ret(c, owner, 0u, 2u, 20u, 0x80072DDCu);
  if (child == 0) return 0;
  leaf2(c, child, 1u, 0x8004D604u);
  c->mem_w32(child + 0x1Cu, CHILD_TABLE_PTR);
  c->mem_w8(child + 0x03u, 4u);
  c->mem_w16(child + 0x5Cu, 0u);

  uint8_t childFlag;
  if (c->mem_r8(GBASE + 0x82u) == 1u) {
    c->mem_w16(child + 0x68u, 0u);
    c->mem_w8(GBASE + 0x82u, (uint8_t)(c->mem_r8(GBASE + 0x82u) | 0x10u));
    childFlag = (uint8_t)(c->mem_r8(GBASE + 0x183u) & 4u);
  } else {
    c->mem_w16(child + 0x68u, 1u);
    childFlag = (uint8_t)((c->mem_r8(GBASE + 0x183u) >> 4) & 4u);
  }
  c->mem_w8(child + 0x5Eu, childFlag);
  leaf2(c, (childFlag == 0) ? 78u : 77u, 65u, 0x8004ED94u);
  c->mem_w32(owner + 0x24u, 30u);
  return 1u;
}

// FUN_80127510(owner, subtype) — spawn a child whose sub-behavior is picked by `subtype`
// (obj[+5] guest arg, the per-object script's sub-index — same role as spawnTypedChild's `sub`).
//   child = FUN_80072DDC(owner, 0, 2, 20)
//   if (!child) return 0
//   FUN_8004D650(subtype, 1); FUN_8004ED0C(subtype, 5)          -- per-subtype global setup calls
//   child[+0x1C]=CHILD_TABLE_PTR; child[+3]=2
//   if (subtype == 57): child[+0x5C]=13439; child[+0x5E]=0; GBASE[+0x1D8]++       -- 0x1D8==472
//   else:                child[+0x5C]=13375; child[+0x5E]=4
//   combined = 1 | child[+0x5E]
//   if (mem[GBASE_BOOST_GATE] != 0):
//     FUN_80040C00(61); mode = combined << 4; child[+0x68]=1; owner[+0x24]=120
//   else:
//     mode = combined; child[+0x68]=0; owner[+0x24]=30
//   GBASE[+0x183] |= mode
//   return 1
uint32_t beh_spawn_toy_child_type2_80127510(Core* c, uint32_t owner, uint32_t subtype) {
  const uint32_t child = leaf4Ret(c, owner, 0u, 2u, 20u, 0x80072DDCu);
  if (child == 0) return 0;

  leaf2(c, subtype, 1u, 0x8004D650u);
  leaf2(c, subtype, 5u, 0x8004ED0Cu);

  c->mem_w32(child + 0x1Cu, CHILD_TABLE_PTR);
  c->mem_w8(child + 0x03u, 2u);

  if (subtype == 57u) {
    c->mem_w16(child + 0x5Cu, 13439u);
    c->mem_w8(child + 0x5Eu, 0u);
    c->mem_w8(GBASE + 0x1D8u, (uint8_t)(c->mem_r8(GBASE + 0x1D8u) + 1u));
  } else {
    c->mem_w16(child + 0x5Cu, 13375u);
    c->mem_w8(child + 0x5Eu, 4u);
  }

  const uint8_t combined = (uint8_t)(1u | c->mem_r8(child + 0x5Eu));
  uint32_t mode;
  if (c->mem_r8(GBASE_BOOST_GATE) != 0u) {
    leaf1(c, 61u, 0x80040C00u);
    mode = (uint32_t)combined << 4;
    c->mem_w16(child + 0x68u, 1u);
    c->mem_w32(owner + 0x24u, 120u);
  } else {
    mode = combined;
    c->mem_w16(child + 0x68u, 0u);
    c->mem_w32(owner + 0x24u, 30u);
  }
  c->mem_w8(GBASE + 0x183u, (uint8_t)(c->mem_r8(GBASE + 0x183u) | mode));
  return 1u;
}
