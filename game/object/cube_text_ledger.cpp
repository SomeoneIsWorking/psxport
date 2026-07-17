// game/object/cube_text_ledger.cpp — see cube_text_ledger.h for the full RE derivation, layout,
// and the WIRING note (2026-07-08: activateSlot/deactivateSlot/spawnPopup dual-wired, lookupCost
// deliberately left unwired).
#include "core.h"
#include "game_ctx.h"
#include "cube_text_ledger.h"
#include "override_registry.h"   // overrides::install — the one native-override registry
#include "core/engine.h"
#include "game.h"
#include <cstdint>

void rec_dispatch(Core*, uint32_t); // hybrid call: override if wired, else substrate.
                                     // Used only for the two still-unowned leaves this draft calls
                                     // through (freelist alloc + node init) — see the constants below.
extern void shard_set_override(uint32_t, void (*)(Core*)); // wire the recompiler's OWN g_override[] table

namespace {

// Fixed cells (addresses already folded from the recompiler's `<hi>16<<16 + <lo>` constants —
// see the .h file header for the derivation of each).
constexpr uint32_t RUNNING_COST      = 0x800BF874u; // u32
constexpr uint32_t ACTIVE_COUNT      = 0x800BF8A8u; // u16
constexpr uint32_t DEACTIVATE_COUNT  = 0x800BF8AAu; // u16
constexpr uint32_t SLOT_STATE_BASE   = 0x800BF8B4u; // u8[], indexed by slot
constexpr uint32_t G_LEDGER_GATE     = 0x800E7FEEu; // s16 (alias of actor_sm_reward.cpp's G_TALLY_CUR)
constexpr uint32_t LOG_INDEX         = 0x800ED06Du; // u8
constexpr uint32_t LOG_SLOT_BASE     = 0x800ED06Eu; // u8[]
constexpr uint32_t LOG_EVENT_BASE    = 0x800ED074u; // u8[]
constexpr uint32_t POPUP_ACTIVE_COUNT= 0x800BF849u; // u8 (beh_cube_text_spawn.cpp counterpart)
constexpr uint32_t CUBE_TEXT_VTABLE  = 0x8003AD48u; // beh_cube_text_spawn (LIVE, game/ai/)

// Opaque still-unowned leaves this cluster calls through, per the "own it" directive these are
// NOT reimplemented here (out of this agent's RE'd scope) — invoked via rec_dispatch so a future
// ownership wave on THEIR addresses transparently intercepts these call sites too.
constexpr uint32_t FN_FREELIST_ALLOC = 0x8007A980u; // (class,size,list) -> node ptr or 0
constexpr uint32_t FN_NODE_INIT      = 0x800727D4u; // (node, value, variant)

void appendLog(Core* c, uint32_t slot, uint8_t event) {
  uint8_t idx = c->mem_r8(LOG_INDEX);
  c->mem_w8(LOG_SLOT_BASE + idx, (uint8_t)slot);
  c->mem_w8(LOG_EVENT_BASE + idx, event);
  c->mem_w8(LOG_INDEX, (uint8_t)(idx + 1));
}

} // namespace

// FUN_80040B48 (activate / scene-event ARM) is NOT owned here — it is SceneEvents::armBody
// (game/scene/scene_events.cpp), wired by SceneEvents::registerOverrides. This file used to carry
// an independent second copy (CubeTextLedger::activateSlot), found via `codemap.py --conflicts` and
// deduped onto the single canonical body (mirrors how FUN_80040A58 was deduped onto classSize).

void CubeTextLedger::deactivateSlot(Core* c) {
  const uint32_t slot = c->r[4];
  if (c->mem_r16s(G_LEDGER_GATE) == 0) { c->r[2] = 0xFFFFFFFFu; return; }

  uint8_t state = c->mem_r8(SLOT_STATE_BASE + slot);
  if (state == 0) {
    // Ground-truth quirk (FUN_80040C00): a deactivate on a never-activated slot still bumps
    // ACTIVE_COUNT before re-reading the (still-zero) state byte. Reproduced verbatim.
    c->mem_w16(ACTIVE_COUNT, (uint16_t)(c->mem_r16(ACTIVE_COUNT) + 1));
    state = c->mem_r8(SLOT_STATE_BASE + slot);
  }
  if (state == 0xFF) { c->r[2] = 0; return; }

  c->mem_w8(SLOT_STATE_BASE + slot, 0xFF);
  c->mem_w16(DEACTIVATE_COUNT, (uint16_t)(c->mem_r16(DEACTIVATE_COUNT) + 1));
  const uint32_t cost = eng(c).sceneEvents.classSize((uint8_t)slot, /*nibbleLo=*/true);
  c->mem_w32(RUNNING_COST, c->mem_r32(RUNNING_COST) + cost);
  appendLog(c, slot, 1);
  c->r[2] = 1;
}

void CubeTextLedger::spawnPopup(Core* c) {
  const uint32_t value   = c->r[4];
  const uint32_t variant = c->r[5];

  c->r[4] = 4; c->r[5] = 3; c->r[6] = 1;
  rec_dispatch(c, FN_FREELIST_ALLOC);
  const uint32_t node = c->r[2];
  if (node == 0) { c->r[2] = 0; return; }

  c->mem_w32(node + 0x1C, CUBE_TEXT_VTABLE);
  c->mem_w8 (node + 0x02, 0x0B);
  c->mem_w8 (node + 0x03, (uint8_t)variant);
  c->mem_w16(node + 0x60, (uint16_t)value);
  c->mem_w8 (node + 0x28, (uint8_t)(c->mem_r8(node + 0x28) | 0x80));
  c->mem_w8 (POPUP_ACTIVE_COUNT, (uint8_t)(c->mem_r8(POPUP_ACTIVE_COUNT) + 1));

  c->r[4] = node; c->r[5] = value; c->r[6] = variant;
  rec_dispatch(c, FN_NODE_INIT);
  c->r[2] = node;
}

// CubeTextLedger::registerOverrides() — install activateSlot/deactivateSlot/spawnPopup into the
// override registry, same shape as ActorReward::registerOverrides (see actor_sm_reward.cpp): one
// overrides::install() per address, with a shard_set_override setter so the shared thunk lands in
// the recompiler's own g_override[] table (what the substrate's direct `func_<addr>(c)` call sites
// consult — confirmed via generated/shard_0.c, shard_1.c, shard_2.c, shard_4.c, shard_5.c for
// FN_40B48/FN_40C00, and generated/shard_3.c:8421 for FN_40AA4), while any native caller reaching
// these via rec_dispatch(c, addr) (ActorReward::smEventDispatch does, for FN_40B48/FN_40C00) hits
// the same registry entry. The oracle-leg gate (core B / psx_fallback keeps running the exact
// recompiled body) lives once inside the registry, not per-trampoline.
extern void gen_func_80040C00(Core*);
extern void gen_func_80040AA4(Core*);

void CubeTextLedger::registerOverrides(Game* /*game*/) {
  using overrides::install;
  install(0x80040C00u, "CubeTextLedger::deactivateSlot", CubeTextLedger::deactivateSlot, gen_func_80040C00, shard_set_override);
  install(0x80040AA4u, "CubeTextLedger::spawnPopup",     CubeTextLedger::spawnPopup,     gen_func_80040AA4, shard_set_override);
}
