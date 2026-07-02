// class SceneTransition — the PC-native SCENE/AREA-TRANSITION helpers.
//
// Owns small resident functions that drive the area-mask "seen" register and the sub-scene swap
// handshake (bf818 waiter). Every entry is a `static` class method taking `Core*` — cross-file
// callers write `SceneTransition::method(c, ...)` directly (same shape as CutsceneCamera's static
// live-spine entries). No `extern "C"` shims.
//
// Currently owned:
//   * areaMaskTrigger  — FUN_800782F0. Marks the per-area BIT in the visited/"seen" mask
//                        0x800BFE50 keyed by (area, sub) and, for areas A05..A08 (5..8), sets
//                        one of four mode-select bits in 0x800BF9DB. Called from
//                        `beh_scene_ui_trigger` state-0 on the confirm edge (the moment the
//                        player triggers an area/sub transition) and from `ov_800796DC`
//                        (the area control-block reset at 0x800BF808).
//
// Follow-ons planned (docs/findings/scene.md — the hut-door sub-scene swap DoD):
//   * SubSceneSwap {beginSwap / completeSwap / resetSwap / stepSwapWaiter} — the FUN_80073260 /
//     FUN_800732C0 / FUN_80073300 / FUN_80073328 chain that drives the bf818 waiter (case-3
//     SECOND branch = bf818:=3 = release FUN_80026ad0 case 3).
#pragma once
#include <cstdint>
class Core;

class SceneTransition {
public:
  // FUN_800782F0 — area-mask "seen" register + area-mode bit.
  //
  // For area < 9, index the resident per-area PTR table at 0x800A54A8 with `area`, walk that
  // table by `sub * 8` to grab the u16 at +6, and OR bit
  //   1 << ((byte at 0x800A55B0[area]) + ((hword>>9) & 3)) & 0x1F
  // into the 32-bit register at 0x800BFE50. Then, if the CURRENT area byte 0x800BF870 is 5/6/7/8,
  // OR the corresponding bit 2/4/8/0x10 into 0x800BF9DB (the running-area sub-mode flags read by
  // the scene machine + the AI scripts).
  //
  // Idempotent (bit-OR); called at both scene-reset and area-transition confirm.
  static void areaMaskTrigger(Core* c, uint8_t area, uint8_t sub);
};
