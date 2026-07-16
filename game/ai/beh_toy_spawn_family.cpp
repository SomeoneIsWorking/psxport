// game/ai/beh_toy_spawn_family.cpp — PC-native drafts for a cluster of small "toy/child spawner"
// leaves in the A00 gameplay overlay (0x80126000-0x80127798 band, the frontier just below the
// already-owned `beh_area_transition_machine` at 0x80127798).
//
// VERIFIED + WIRED (this pass): line-by-line diffed against the generated substrate C
// (`generated/ov_a00_shard_{0,1}.c`, symbols `ov_a00_gen_<addr>`) — the ground truth for this
// overlay. 3 real bugs found and fixed (see the inline "BUG FIX" comments at each function):
// beh_spawn_toy_child_type5_80127720 called the wrong callee arg AND returned the wrong value
// (child pointer instead of the 0/1 success flag), beh_spawn_toy_child_type4_8012763c called the
// wrong function address with the wrong argument (a residual-register case — ground truth reuses
// whatever the prior FUN_80072DDC dispatch left in a0, not `child`), and
// beh_spawn_toy_child_type2_80127510 had a wrong "boost gate" constant (should read GBASE+0x183,
// the SAME byte the OR below writes — not a separate field). Wired via `overrides::install` passing
// `ov_a00_set_override` as the setter (the only real callers are DIRECT `ov_a00_func_<addr>(c)`
// sites inside ov_a00_shard_1.c), which also makes it reachable via rec_dispatch (native-caller
// tracing) — same shape as game/ai/actor_melee_engage.cpp. Field-role names beyond what's RE'd
// here (e.g. exact semantics of GBASE's individual bytes) still await a live RAM dump — see
// docs/engine_re.md.
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
#include "game.h"
#include "override_registry.h"   // overrides::install — the one native-override registry
#include <cstdint>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {

// GBASE — the shared global blob at 0x800BF870 (== 32780<<16 - 1936). Same struct
// beh_lift_platform.cpp names by absolute address (0x800BF854 etc); role of individual byte
// offsets used here (0x82=130 dec, 0x183=387 dec) is NOT yet confirmed against a RAM dump.
constexpr uint32_t GBASE = 0x800BF870u;
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
//   if (!child) return 0
//   FUN_8004D604(84, 1)   -- ground truth passes the LITERAL constant 84 as a0, NOT child; that
//                            function (gen_func_8004D604) indexes GBASE+a0 as a flag-byte array,
//                            it is not an object-pointer callee. (draft originally guessed `child`)
//   child[+0x1C]=CHILD_TABLE_PTR; child[+3]=5; child[+0x5C]=0; owner[+0x24]=30
//   return 1     -- ground truth returns the success flag, NOT the child pointer (draft originally
//                   returned `child`)
uint32_t beh_spawn_toy_child_type5_80127720(Core* c, uint32_t owner) {
  const uint32_t child = leaf4Ret(c, owner, 0u, 2u, 20u, 0x80072DDCu);
  if (child == 0) return 0;
  leaf2(c, 84u, 1u, 0x8004D604u);
  c->mem_w32(child + 0x1Cu, CHILD_TABLE_PTR);
  c->mem_w8(child + 0x03u, 5u);
  c->mem_w16(child + 0x5Cu, 0u);
  c->mem_w32(owner + 0x24u, 30u);
  return 1u;
}

// FUN_8012763C(owner) — spawn a type-4 companion child, then feed GBASE's mode byte to pick which
// SFX/anim variant (77 vs 78) plays via FUN_8004ED94(id, 65), and bump owner[+0x24]=30.
//   child = FUN_80072DDC(owner, 0, 2, 20)
//   if (!child) return 0
//   FUN_8004D650(<residual a0 left by the FUN_80072DDC dispatch above>, 1)   -- ground truth never
//     reloads r4/a0 between the two calls, so a0 for this call is whatever gen_func_80072DDC left
//     in r4 at return (real MIPS a0 is caller-saved / not respilled here). NOT `child` and NOT
//     FUN_8004D604 (draft originally guessed both wrong: wrong callee address AND wrong arg).
//   child[+0x1C]=CHILD_TABLE_PTR; child[+3]=4; child[+0x5C]=0
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
  // Faithful residual-register reproduction: do NOT set r4 here. r4 must retain whatever the
  // FUN_80072DDC dispatch above left it as (matches ground truth exactly since we route through
  // the same rec_dispatch, which mutates the same shared Core register file).
  c->r[5] = 1u;
  rec_dispatch(c, 0x8004D650u);
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
//   if (GBASE[+0x183] != 0):     -- NOT a separate "gate" byte: same field the OR below writes,
//                                   read BEFORE the OR (self-referential: once any bit is set by a
//                                   prior call, every later call takes the boosted path)
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
  if (c->mem_r8(GBASE + 0x183u) != 0u) {
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

// ---------------------------------------------------------------------------------------------
// Wiring: the only real callers found for all 5 addresses are DIRECT `ov_a00_func_<addr>(c)` sites
// inside ov_a00_shard_1.c (the surrounding cluster's top-level state dispatcher, not yet drafted —
// see the file banner). That call shape goes through the recompiler's OWN per-overlay
// g_ov_a00_override[] table, never rec_dispatch — so installing without a setter would be blind to
// it. `overrides::install` is passed ov_a00_set_override as the setter, oracle-gated same as
// gen/native everywhere else, same pattern as game/ai/actor_melee_engage.cpp.
// ---------------------------------------------------------------------------------------------
extern void ov_a00_set_override(uint32_t, void (*)(Core*));
extern void ov_a00_gen_80127420(Core*);
extern void ov_a00_gen_801274BC(Core*);
extern void ov_a00_gen_80127720(Core*);
extern void ov_a00_gen_8012763C(Core*);
extern void ov_a00_gen_80127510(Core*);

namespace {
void ov_behArmCountdown80127420(Core* c)  { c->r[2] = beh_arm_countdown_if_linked_ready_80127420(c, c->r[4]); }
void ov_behDistanceBand801274bc(Core* c)  { c->r[2] = (uint32_t)beh_distance_band_predicate_801274bc(c, c->r[4]); }
void ov_behSpawnToyType5_80127720(Core* c){ c->r[2] = beh_spawn_toy_child_type5_80127720(c, c->r[4]); }
void ov_behSpawnToyType4_8012763c(Core* c){ c->r[2] = beh_spawn_toy_child_type4_8012763c(c, c->r[4]); }
void ov_behSpawnToyType2_80127510(Core* c){ c->r[2] = beh_spawn_toy_child_type2_80127510(c, c->r[4], c->r[5]); }
}  // namespace

void RegisterBehToySpawnFamilyOverrides(Game* /*game*/) {
  using overrides::install;
  install(0x80127420u, "beh_arm_countdown_if_linked_ready", ov_behArmCountdown80127420,  ov_a00_gen_80127420, ov_a00_set_override);
  install(0x801274BCu, "beh_distance_band_predicate",       ov_behDistanceBand801274bc,  ov_a00_gen_801274BC, ov_a00_set_override);
  install(0x80127720u, "beh_spawn_toy_child_type5",         ov_behSpawnToyType5_80127720,ov_a00_gen_80127720, ov_a00_set_override);
  install(0x8012763Cu, "beh_spawn_toy_child_type4",         ov_behSpawnToyType4_8012763c,ov_a00_gen_8012763C, ov_a00_set_override);
  install(0x80127510u, "beh_spawn_toy_child_type2",         ov_behSpawnToyType2_80127510,ov_a00_gen_80127510, ov_a00_set_override);
}
