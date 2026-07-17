// class Pool — PC-native object-pool / per-area control-block INIT subsystem.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::pool`. Back-pointer wired once by
// Core's constructor. Callers reach the init cluster through the object graph:
//
//     eng(c).pool.init();          // guest FUN_8007B18C — top-level object-pool init
//     eng(c).pool.resetControlBlock();       // guest FUN_800796DC
//     eng(c).pool.seedAreaObjects();          // guest FUN_800263E8
//     eng(c).pool.reset75240();              // guest FUN_80075240
//     eng(c).pool.setupViewScroll();          // guest FUN_800783DC (per-area view/scroll)
//     eng(c).pool.finalViewInit();           // guest FUN_80078610 (final per-area view)
//     eng(c).pool.selectStateIndex(area);    // guest FUN_80074F24 (per-area state-index; a0=area)
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as
// SceneTransition, Demo, Sop, ObjectList, TransitionState3.
//
// Each method carries the byte-A/B verify gate the pre-restructure ov_*_run functions had —
// channel names preserved verbatim (poolinitverify / init796dcverify / init263e8verify /
// init75240verify / init783dcverify / init78610verify / init74f24verify) so live
// diagnostics are unaffected.
#ifndef GAME_WORLD_POOL_CLASS_H
#define GAME_WORLD_POOL_CLASS_H
#include <cstdint>
class Core;

class Pool {
public:
  Core* core = nullptr;

  void init();                          // FUN_8007B18C
  void resetControlBlock();             // FUN_800796DC
  void seedAreaObjects();               // FUN_800263E8
  void reset75240();                    // FUN_80075240
  void setupViewScroll();               // FUN_800783DC
  void finalViewInit();                 // FUN_80078610
  void selectStateIndex(uint8_t area);  // FUN_80074F24 (a0=area byte)

  // -- Internal init helpers (formerly rec_dispatched from Pool::init) ---------------------
  // Both are Pool::init's own callees; kept separate for clarity + potential re-use. Neither has
  // a non-trivial API surface — they zero a fixed region / build a fixed pool structure.

  // clearBf548Region — guest FUN_8004FB20. Zeroes the 700-byte region at 0x800BF548 (a per-area
  //   scratch scene control block). Trivial memset wrapper; kept as a method so Pool::init's flow
  //   stays a clean sequence of named steps instead of a `call_fn`.
  void clearBf548Region();

  // initTypedPools — guest FUN_800798F8. The heart of the object subsystem: builds the 5 typed
  //   free-list pools (records + 208-byte nodes) chained via +0x24 next-ptr with +0x28=pool-class,
  //   clears the 3 active list heads (T2_OBJLIST_HEAD_1, T2_OBJLIST_HEAD_2, AUX_LIST_HEAD), and
  //   seeds the 3 aux-render list heads/tails in scratchpad (0x1F80013C/148/154). Pool bases +
  //   strides + counts summary:
  //     pool 0 : base 0x800ED8D8  stride 0x88   count 0x34 (52)  class 0
  //     pool 1 : base 0x800EF478  stride 0xC4   count 0x3A (58)  class 1
  //     pool 2 : base 0x800FE198  stride 0xD0   count 0x2A (42)  class 2    ← the 208-byte NODES
  //     pool 3 : base 0x800FB858  stride 0x108  count 0x28 (40)  class 3
  //     pool 4 : base 0x800FB218  stride 0x140  count 0x05       class 4
  //   Free-list head words (u32 ptr each) live at 0x800E8098 / 0x800E80A0 / 0x800F2398 /
  //   0x800ED8D4 / 0x800ED8D0; per-pool free counts at 0x800E7E7C / 0x800E7E7D / 0x800ED8CC /
  //   0x800ED8C5 / 0x800ED8C4. Ghidra decomp scratch/decomp/pool_init_leads.c.
  void initTypedPools();
};
#endif
