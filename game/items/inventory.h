// class Inventory — the PC-native item / inventory-collection subsystem.
//
// PROPER OOP: an instance per Core (embedded as `Core::inventory`). Callers use it as
// `inv(c).give(type, amount)` — the natural PC-game shape. The FUN_xxx entry-shape
// wrappers (Core*, args in r[4]/r[5]) remain as static class entrypoints for the
// `invverify` A/B gate and any still-recomp-shaped caller (repl.cpp `give_only` etc.).
//
// State lives in guest memory in the save/state block at 0x800BF870 (memcard-serialized —
// see inventory.cpp for the field layout). No per-instance C++ state beyond the Core
// back-pointer wired in Core::Core().
#pragma once
#include <cstdint>
class Core;

class Inventory {
public:
  // Back-pointer set once by Core's constructor (same pattern as ScreenFade::core).
  Core* core = nullptr;

  // REENTRANCY GUARD for the `invverify` A/B gate: while a gate's rec_super_call interprets a
  // WRAPPER body, its inner `jal 0x8004D338` re-enters the entry — a nested gate would corrupt
  // the outer snapshot/roll-back. Nonzero while inside a gate (was file-scope s_in_gate).
  int inGate = 0;

  // Query -------------------------------------------------------------------------------
  int count(int item) const;                     // 0..99
  int has(int item) const;                       // count > 0

  // PC-shape mutators (call from native code) --------------------------------------------
  void add(uint32_t type, uint32_t amount);           // FUN_8004D338 core
  void give(uint32_t type, uint32_t amount);          // FUN_8004D4F4 — add only
  void giveAndFlag(uint32_t type, uint32_t amount);   // FUN_8004D4C4 — add + flag emit

  // FUN_xxx entry-shape statics (Core*, args in r[4]/r[5]). These carry the `invverify`
  // A/B gate; still-recomp-shaped callers (repl.cpp / any leftover rec_dispatch wire)
  // reach the native path through these.
  static void addEntry(Core* c);          // FUN_8004D338
  static void giveEntry(Core* c);         // FUN_8004D4F4
  static void giveAndFlagEntry(Core* c);  // FUN_8004D4C4

private:
  // Guest-ABI bodies + the shared add core (plain fn-pointer shape for the A/B gate).
  static void addNative(Core* c, uint32_t type, uint32_t amount);   // FUN_8004D338 core
  static void addBody(Core* c);                                     // FUN_8004D338
  static void giveAndFlagBody(Core* c);                             // FUN_8004D4C4
  static void giveBody(Core* c);                                    // FUN_8004D4F4
  static void abGate(Core* c, uint32_t addr, void (*native)(Core*), int exclude_stack,
                     const char* nm);
};
