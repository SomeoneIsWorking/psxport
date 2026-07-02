// class Pool — PC-native object-pool / per-area control-block INIT subsystem.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::pool`. Back-pointer wired once by
// Core's constructor. Callers reach the init cluster through the object graph:
//
//     c->engine.pool.init();          // guest FUN_8007B18C — top-level object-pool init
//     c->engine.pool.resetControlBlock();       // guest FUN_800796DC
//     c->engine.pool.seedAreaObjects();          // guest FUN_800263E8
//     c->engine.pool.reset75240();              // guest FUN_80075240
//     c->engine.pool.setupViewScroll();          // guest FUN_800783DC (per-area view/scroll)
//     c->engine.pool.finalViewInit();           // guest FUN_80078610 (final per-area view)
//     c->engine.pool.selectStateIndex(area);    // guest FUN_80074F24 (per-area state-index; a0=area)
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
};
#endif
