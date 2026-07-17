// PC-native INVENTORY / ITEM-COLLECTION subsystem for Tomba!2.
//
// Per the CLAUDE.md boundary update (2026-06-21) game CONTENT — including the item/inventory state and
// pickup logic — is a valid native-ownership target. This module owns the engine's shared inventory core:
// the central item-add routine and the two give-wrappers every per-item pickup handler funnels through.
// Structured like a PC game's item code: an inventory_add(item, amount) primitive, a has()/count() query,
// and the wrappers; the recomp body remains the behavioral reference + the live fallback for the per-item
// give-handlers and the event sink (those stay PSX, dispatched).
//
// ---- Reverse-engineering (disasm: tools/disas.py 0x<addr>) ----
// SAVE/STATE BLOCK base B = 0x800BF870  (built everywhere as `lui 0x800c; addiu -1936`). The inventory
// fields all live in this contiguous block (the memcard serializes it). Verified fields:
//   B+0x13  (0x800BF883)  recently-acquired ring LENGTH (u8)
//   B+0x14  (0x800BF884)  recently-acquired ring DATA   (u8[], types 23..28 appended)
//   B+0x31  (0x800BF8A1)  event counter (variant-B quest pass)            (u8)
//   B+0x32  (0x800BF8A2)  event counter (variant-A quest pass)            (u8)
//   B+0x244 (0x800BFAB4)  ITEM COUNT array, 1 byte/type, 99-capped        (u8[])
//   B+0x344 (0x800BFBB4)  quest-want cross-reference array, 256 bytes     (u8[])
// quest table  T = 0x800A2BE8 (12-byte stride, the want/recipe driver).
//
// FUN_8004D338  inventory_add(a0 = itemType, a1 = amount)  — PURE LEAF (no jal). Three parts:
//   (1) RING: if (unsigned)(type-23) < 6  [types 23..28]:  ring[len]=type; len++   (B+0x13 len, B+0x14 data)
//   (2) QUEST CROSS-REF: only when count[type]==0 (B+0x244+type). Two variants picked by the FIRST byte of
//       the type's quest-table row (T+type*12):
//         row0==0 -> variant A: for i in [0,256): if count[i]!=0 && T[i*12]==0 -> questref[i]++ ;
//                    then questref[type]=0; B[0x32]++
//         row0!=0 -> variant B: for i in [0,256): if count[i]!=0 && T[i*12]!=0 -> questref[i]++ ;
//                    then questref[type]=0; B[0x31]++
//   (3) ADD+CLAMP: count[type] += amount; if ((count[type] & 0xff) >= 100) count[type] = 99
//   void return (the recomp epilogue sets nothing explicit; the two wrappers below ignore v0).
//
// FUN_8004D4C4  inventory_give_and_flag(a0=type, a1=amount): jal 0x8004D338(type,amount); jal 0x8004ED0C
//   (type, 2)  — the give + the "item acquired" event/flag emit. set_item_flag (0x8004ED0C) has a jal to
//   the event sink 0x8004FA38, so it stays PSX (dispatched in-context); we own only the call sequencing.
// FUN_8004D4F4  inventory_give(a0=type, a1=amount): jal 0x8004D338(type,amount)  — give only.
//
// VERIFY: `invverify` — full RAM (0x200000) + scratchpad (0x400) A/B gate vs rec_super_call (same template
// as playerverify/scriptvm): run native, snapshot, roll back, run the recomp body, diff. The pure-leaf core
// touches no stack below entry sp; the wrappers dispatch a PSX callee that leaves transient below-sp residue
// in BOTH passes (same A/B artifact as the player/grid families), so the wrapper gate excludes the
// top-of-RAM stack window [sp-0x800, sp) — far above all game data.
#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "inventory.h"
#include "game.h"      // c->game->verify — the shared A/B verify scaffold
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)
void rec_dispatch(Core*, uint32_t);     // hybrid call: recomp body if emitted, else interpret

// ---- inventory state block addresses ----
#define INV_BASE        0x800BF870u
#define INV_RING_LEN    (INV_BASE + 0x13u)   // 0x800BF883
#define INV_RING_DATA   (INV_BASE + 0x14u)   // 0x800BF884
#define INV_EVT_B       (INV_BASE + 0x31u)   // 0x800BF8A1
#define INV_EVT_A       (INV_BASE + 0x32u)   // 0x800BF8A2
#define INV_COUNT       (INV_BASE + 0x244u)  // 0x800BFAB4 (count[type])
#define INV_QUESTREF    (INV_BASE + 0x344u)  // 0x800BFBB4 (questref[type])
#define INV_QUEST_TABLE 0x800A2BE8u          // 12-byte stride

// ---- class Inventory: query API (PC-game-shaped) --------------------------------------------------
int Inventory::count(int item) const {
  if (item < 0 || item > 0xff) return 0;
  return core->mem_r8(INV_COUNT + (uint32_t)item);
}
int Inventory::has(int item) const { return count(item) > 0; }

// PC-native reimplementation of FUN_8004D338 (inventory_add). Writes are byte-identical to the recomp body.
void Inventory::addNative(Core* c, uint32_t type, uint32_t amount) {
  // (1) recently-acquired ring (types 23..28)
  if ((type - 23u) < 6u) {
    uint32_t len = c->mem_r8(INV_RING_LEN);
    c->mem_w8(INV_RING_DATA + len, (uint8_t)type);
    c->mem_w8(INV_RING_LEN, (uint8_t)(len + 1));
  }
  // (2) quest cross-reference — only when this item was not already owned
  if (c->mem_r8(INV_COUNT + type) == 0) {
    uint32_t row0 = c->mem_r8(INV_QUEST_TABLE + type * 12u);
    if (row0 == 0) {
      // variant A
      for (uint32_t i = 0; i < 256u; i++) {
        if (c->mem_r8(INV_COUNT + i) != 0 && c->mem_r8(INV_QUEST_TABLE + i * 12u) == 0)
          c->mem_w8(INV_QUESTREF + i, (uint8_t)(c->mem_r8(INV_QUESTREF + i) + 1));
      }
      c->mem_w8(INV_QUESTREF + type, 0);
      c->mem_w8(INV_EVT_A, (uint8_t)(c->mem_r8(INV_EVT_A) + 1));
    } else {
      // variant B
      for (uint32_t i = 0; i < 256u; i++) {
        if (c->mem_r8(INV_COUNT + i) != 0 && c->mem_r8(INV_QUEST_TABLE + i * 12u) != 0)
          c->mem_w8(INV_QUESTREF + i, (uint8_t)(c->mem_r8(INV_QUESTREF + i) + 1));
      }
      c->mem_w8(INV_QUESTREF + type, 0);
      c->mem_w8(INV_EVT_B, (uint8_t)(c->mem_r8(INV_EVT_B) + 1));
    }
  }
  // (3) add + 99-clamp (Tomba's classic item cap)
  uint32_t n = (c->mem_r8(INV_COUNT + type) + amount) & 0xffu;
  c->mem_w8(INV_COUNT + type, (uint8_t)n);
  if (n >= 100u) c->mem_w8(INV_COUNT + type, 99);
}

// ---- the FUN_8004D338 override + invverify gate ----------------------------------------------------
void Inventory::addBody(Core* c) {
  addNative(c, c->r[4], c->r[5]);
}

// REENTRANCY GUARD: when a gate's rec_super_call interprets a WRAPPER body (give_and_flag = 0x8004D4C4),
// that body's `jal 0x8004D338` re-enters ov_inventory_add. We must NOT start a nested gate there (it would
// snapshot/roll-back with the same buffers and corrupt the outer A/B). While inside a gate, every nested
// inventory override runs its plain native body directly. (Same constraint the scriptvm/grid gates face;
// here the wrapper→core call makes it explicit.)

// Full RAM+scratchpad A/B vs rec_super_call. The pure-leaf core touches no guest stack; the wrappers
// dispatch the PSX event sink (0x8004ED0C->0x8004FA38), whose own below-sp frame differs harmlessly across
// the twice-run passes, so the wrapper gate excludes the top-of-RAM stack window [sp-0x800, sp).
void Inventory::abGate(Core* c, uint32_t addr, void (*native)(Core*), int exclude_stack, const char* nm) {
  uint8_t* ram0 = c->game->verify.ram0();
  uint8_t* ramN = c->game->verify.ramN();
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t a0 = c->r[4], a1 = c->r[5];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  inv(c).inGate++; native(c); inv(c).inGate--;
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  inv(c).inGate++; rec_super_call(c, addr); inv(c).inGate--;    // inner jal 0x8004D338 re-enters native-only (guard)
  uint32_t v0_o = c->r[2];
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1;
  for (uint32_t a = 0; a < 0x200000; a++)
    if (c->ram[a] != ramN[a] && !(exclude_stack && a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  VerifyHarness::Check& chk = c->game->verify.check("invverify");
  long &ng = chk.nMatch, &nb = chk.nMismatch;
  if (ro >= 0 || so >= 0) {  // these fns return void; v0 is dead, don't gate on it
    if (nb++ < 40)
      fprintf(stderr, "[%s] MISMATCH a0=%08x a1=%x v0 n=%x o=%x ram@%x(n=%02x o=%02x) spad@%x sp=%x\n",
              nm, a0, a1, v0_n, v0_o, ro, ro>=0?ramN[ro]:0, ro>=0?c->ram[ro]:0, so, sp);
  } else if (++ng % 200 == 0) fprintf(stderr, "[%s] %ld matches (last a0=%08x a1=%x)\n", nm, ng, a0, a1);
}


void Inventory::addEntry(Core* c) {       // FUN_8004D338
  if (c->game->verify.on("invverify") && !inv(c).inGate) { abGate(c, 0x8004D338u, &Inventory::addBody, 0, "invverify"); return; }
  addBody(c);
}

// FUN_8004D4C4  give_and_flag(type, amount): native add, then dispatch the PSX flag/event emit (0x8004ED0C
// has a jal to the event sink 0x8004FA38 -> stays content/dispatched). We own the call sequencing.
void Inventory::giveAndFlagBody(Core* c) {
  uint32_t type = c->r[4], amount = c->r[5];
  addNative(c, type, amount);
  c->r[4] = type; c->r[5] = 2;                 // set_item_flag(type, 2)
  rec_dispatch(c, 0x8004ED0Cu);
}
void Inventory::giveAndFlagEntry(Core* c) {    // FUN_8004D4C4
  if (c->game->verify.on("invverify") && !inv(c).inGate) { abGate(c, 0x8004D4C4u, &Inventory::giveAndFlagBody, 1, "invverify"); return; }
  giveAndFlagBody(c);
}

// FUN_8004D4F4  give_only(type, amount): native add only.
void Inventory::giveBody(Core* c) {
  addNative(c, c->r[4], c->r[5]);
}
void Inventory::giveEntry(Core* c) {           // FUN_8004D4F4
  if (c->game->verify.on("invverify") && !inv(c).inGate) { abGate(c, 0x8004D4F4u, &Inventory::giveBody, 1, "invverify"); return; }
  giveBody(c);
}

// (registration of the class is by value in Core; no init function needed.)

// ---- PC-shape mutators: set the guest ABI regs and route through the static entry (invverify gate).
void Inventory::add(uint32_t type, uint32_t amount) {
  core->r[4] = type; core->r[5] = amount; Inventory::addEntry(core);
}
void Inventory::give(uint32_t type, uint32_t amount) {
  core->r[4] = type; core->r[5] = amount; Inventory::giveEntry(core);
}
void Inventory::giveAndFlag(uint32_t type, uint32_t amount) {
  core->r[4] = type; core->r[5] = amount; Inventory::giveAndFlagEntry(core);
}
