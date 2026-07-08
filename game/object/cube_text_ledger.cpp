// game/object/cube_text_ledger.cpp — see cube_text_ledger.h for the full RE derivation, layout,
// and the WIRING note (2026-07-08: activateSlot/deactivateSlot/spawnPopup dual-wired, lookupCost
// deliberately left unwired).
#include "core.h"
#include "cube_text_ledger.h"
#include "engine_overrides.h"
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

void CubeTextLedger::activateSlot(Core* c) {
  const uint32_t slot = c->r[4];
  if (c->mem_r16s(G_LEDGER_GATE) == 0) { c->r[2] = 0xFFFFFFFFu; return; }

  if (c->mem_r8(SLOT_STATE_BASE + slot) != 0) { c->r[2] = 0; return; }

  c->mem_w8(SLOT_STATE_BASE + slot, 1);
  c->mem_w16(ACTIVE_COUNT, (uint16_t)(c->mem_r16(ACTIVE_COUNT) + 1));
  const uint32_t cost = c->engine.sceneEvents.classSize((uint8_t)slot, /*nibbleLo=*/false);
  c->mem_w32(RUNNING_COST, c->mem_r32(RUNNING_COST) + cost);
  appendLog(c, slot, 0);
  c->r[2] = 1;
}

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
  const uint32_t cost = c->engine.sceneEvents.classSize((uint8_t)slot, /*nibbleLo=*/true);
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

// CubeTextLedger::registerOverrides() — dual-wire activateSlot/deactivateSlot/spawnPopup, same
// shape as ActorReward::registerOverrides (see actor_sm_reward.cpp): EngineOverrides for any
// native caller reaching these via rec_dispatch(c, addr) (ActorReward::smEventDispatch does, for
// FN_40B48/FN_40C00), and shard_set_override for the recompiler's own g_override[] table (which is
// what the substrate's direct `func_<addr>(c)` call sites consult — confirmed via
// generated/shard_0.c, shard_1.c, shard_2.c, shard_4.c, shard_5.c for FN_40B48/FN_40C00, and
// generated/shard_3.c:8421 for FN_40AA4). Each shard_set_override trampoline is psx_fallback-gated
// so core B (the pure SBS reference) keeps running the exact recompiled body — g_override[] is a
// single table shared by every Core/Game, unlike EngineOverrides which is per-Game and already
// skips psx_fallback cores inside run().
extern void gen_func_80040B48(Core*);
extern void gen_func_80040C00(Core*);
extern void gen_func_80040AA4(Core*);

namespace {
void ov_activateSlot(Core* c)   { if (c->game->psx_fallback) { gen_func_80040B48(c); return; } CubeTextLedger::activateSlot(c); }
void ov_deactivateSlot(Core* c) { if (c->game->psx_fallback) { gen_func_80040C00(c); return; } CubeTextLedger::deactivateSlot(c); }
void ov_spawnPopup(Core* c)     { if (c->game->psx_fallback) { gen_func_80040AA4(c); return; } CubeTextLedger::spawnPopup(c); }
}  // namespace

void CubeTextLedger::registerOverrides(Game* game) {
  EngineOverrides& ov = game->engine_overrides;
  ov.register_(0x80040B48u, "CubeTextLedger::activateSlot",   CubeTextLedger::activateSlot);
  ov.register_(0x80040C00u, "CubeTextLedger::deactivateSlot", CubeTextLedger::deactivateSlot);
  ov.register_(0x80040AA4u, "CubeTextLedger::spawnPopup",     CubeTextLedger::spawnPopup);

  shard_set_override(0x80040B48u, ov_activateSlot);
  shard_set_override(0x80040C00u, ov_deactivateSlot);
  shard_set_override(0x80040AA4u, ov_spawnPopup);
}
