// class SceneTransition — the PC-native SCENE / SUB-SCENE-SWAP driver.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::sceneTransition` (Engine owns the
// scene subsystem umbrella). Back-pointer `core` is wired once by Core's constructor. Callers
// reach methods via the object graph:
//
//     eng(c).sceneTransition.areaMaskTrigger(area, sub);
//     eng(c).sceneTransition.stepSwapWaiter(node);
//
// No `extern "C"` shims, no static methods, no Core-as-a-first-arg. Same shape as `NodeXform`,
// `Inventory`, `ScreenFade`, `Rng`.
//
// == The sub-scene swap handshake (docs/findings/scene.md) ==
//
//   The scene machine at 0x80026AD0 walks its state on node[5], waiting for the shared u8 at
//   0x800BF818 ("swap phase") to progress through 4 values:
//
//     bf818 == 1  : beginSwap has fired. sceneStep case 0 latches bf817 into node[3]+1.
//     bf818 == 2  : sceneStep case 1 has completed its swap-out and is WAITING for the release.
//     bf818 == 3  : swap-in released. sceneStep case 3 fires FUN_800269BC and advances.
//                   >>> This is the "2→3 handshake" — the whole reason the door freeze happens
//                       when bf818 never leaves 2. It is written by exactly one place:
//                       `completeSwap()` (guest FUN_80073300).
//     bf818 == 4  : sceneStep case 4 clears bf818 back to 0 (transition complete).
//
//   The 2→3 progression is DRIVEN by `stepSwapWaiter` (guest FUN_80073328), called every frame
//   by `beh_typed_jumptable_pair` (the resident driver for the 6 door-participating entities).
//   Case 3 SECOND-branch (gate: `node[0x29]!=0 && DAT_800e7ea9!=0 && DAT_800e7ffb==0`) is what
//   calls completeSwap → bf818:=3. If that gate never opens, bf818 stays 2 forever
//   (current pad-replay repro at scratch/bin/door_freeze.pad).
#pragma once
#include <cstdint>
class Core;

class SceneTransition {
public:
  // Back-pointer wired once by Core's constructor (same pattern as ScreenFade::core).
  Core* core = nullptr;

  // ── Guest addresses (the swap uses main-RAM registers the still-substrate scene reads) ────
  static constexpr uint32_t BF817_SWAP_KEY   = 0x800BF817u;
  static constexpr uint32_t BF818_SWAP_PHASE = 0x800BF818u;
  static constexpr uint32_t BF80F_SUSPEND    = 0x800BF80Fu;   // bf80c[3] — non-zero gates every case
  static constexpr uint32_t E7EA9_ARMED      = 0x800E7EA9u;   // case-0/3 SECOND-branch gate
  static constexpr uint32_t E7FFB_INHIBIT    = 0x800E7FFBu;   // case-0/3 SECOND-branch gate
  static constexpr uint32_t E7FC5_FLAG       = 0x800E7FC5u;   // cleared by resetSwap
  static constexpr uint32_t SCENE_BLOCK      = 0x800E7E80u;   // s0-base cleared by resetSwap
  static constexpr uint32_t IF800137_PAUSE   = 0x1F800137u;   // case-0/3 tail: latch =2 if 0

  // ── FUN_800782F0: area-mask "seen" register + area-mode bit ─────────────────────────────────
  // For area < 9, OR a per-(area,sub) bit into the 32-bit register at 0x800BFE50. Then, if the
  // CURRENT area byte 0x800BF870 is 5/6/7/8, OR one of {2,4,8,0x10} into 0x800BF9DB. Idempotent.
  // Called from beh_scene_ui_trigger state-0 (confirm-edge) and ov_800796DC (bf808 reset).
  void areaMaskTrigger(uint8_t area, uint8_t sub);

  // ── FUN_80073260: swap RESET — the shared entry beginSwap/completeSwap both call first ──
  // Writes node[0]=2, fires FUN_80074590(0x17, 0, 0xf) if node[0xBF]!=0, clears 0x800E7FC5 and
  // 0x800E7E85/86/87, dispatches FUN_80054198(0x800E7E80) (substrate).
  void resetSwap(uint32_t node);

  // ── FUN_800732C0: BEGIN the swap — resetSwap + bf818:=1 + bf817:=node[3] (the swap key) ────
  void beginSwap(uint32_t node);

  // ── FUN_80073300: COMPLETE the 2→3 handshake — resetSwap + bf818:=3 ─────────────────────────
  // >>> This is the sole writer of bf818=3 in the whole game. Invoked ONLY by stepSwapWaiter
  //     case 3 SECOND-branch. See scene_transition.cpp for the invariant.
  void completeSwap(uint32_t node);

  // ── FUN_80073328: stepSwapWaiter — per-frame WAITER driven by beh_typed_jumptable_pair ─────
  // node = 68B object base. Walks the sub-state at node[6] (0..5). Returns 0/1 exactly like the
  // guest (only case 5's "consume" returns 1); the JT1[0]==2 caller reads it to decide whether
  // to advance node[5].
  int  stepSwapWaiter(uint32_t node);

  // ── FUN_80054198: clear swap-block ephemerals ─────────────────────────────────────────────
  // Small state-clearing helper called on the SCENE_BLOCK (0x800E7E80) — resetSwap uses it as its
  // final substrate hop; beh_area_transition_machine + beh_typed_jumptable_pair fire it after
  // completing certain 2→3-handshake sub-steps. Semantics (disas 0x80054198..0x800541F0):
  //   * Early return with no writes when (node[+0x146] == 4 && node[+0] == 2)  (state 4/2 latch).
  //   * Otherwise: node[+0x44] = 0; node[+0x182] = 0;
  //     if (node[+2] != 0) { node[+0x149] = 2; return; }
  //     else               { node[+0x50] = 0; node[+0x148] = 0;
  //                          uint8_t v = node[+0x147] + 2;
  //                          node[+0x14A] = v; node[+0x149] = v; }
  void clearSwapBlock(uint32_t node);
};
