// game/object/cube_text_ledger.cpp — DRAFT / UNWIRED bodies. See cube_text_ledger.h for the full
// RE derivation, layout, and the "why unwired" note. Do NOT call shard_set_override /
// EngineOverrides::* for these from any wiring file without first running the SBS-full gate.
#include "core.h"
#include "cube_text_ledger.h"
#include <cstdint>

void rec_dispatch(Core*, uint32_t); // hybrid call: override if wired (none here), else substrate.
                                     // Used only for the two still-unowned leaves this draft calls
                                     // through (freelist alloc + node init) — see the constants below.

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
constexpr uint32_t STR_TABLE_BASE    = 0x800A33C8u; // stride 12
constexpr uint32_t STR_TABLE_STRIDE  = 12u;
constexpr uint32_t COST_TABLE_BASE   = 0x800A3B38u; // stride 4, 16 entries
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

uint32_t CubeTextLedger::lookupCost(Core* c, uint32_t slot, uint32_t mode) {
  uint8_t packed = c->mem_r8(STR_TABLE_BASE + slot * STR_TABLE_STRIDE + 1);
  uint32_t nibble = (mode == 0) ? (uint32_t)(packed >> 4) : (uint32_t)(packed & 0xF);
  return c->mem_r32(COST_TABLE_BASE + nibble * 4);
}

void CubeTextLedger::activateSlot(Core* c) {
  const uint32_t slot = c->r[4];
  if (c->mem_r16s(G_LEDGER_GATE) == 0) { c->r[2] = 0xFFFFFFFFu; return; }

  if (c->mem_r8(SLOT_STATE_BASE + slot) != 0) { c->r[2] = 0; return; }

  c->mem_w8(SLOT_STATE_BASE + slot, 1);
  c->mem_w16(ACTIVE_COUNT, (uint16_t)(c->mem_r16(ACTIVE_COUNT) + 1));
  const uint32_t cost = lookupCost(c, slot, 0);
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
  const uint32_t cost = lookupCost(c, slot, 1);
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

// NOTE: no `shard_set_override`/`EngineOverrides` registration function is provided here on
// purpose — this file is a compile-only draft (cmake-listed, never called). A follow-up ownership
// wave adds the wiring (dual shard_set_override + EngineOverrides, matching actor_sm_reward.cpp's
// pattern) and the SBS-full 0-diff gate before this can be considered owned.
