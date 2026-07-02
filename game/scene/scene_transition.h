// class SceneTransition — the PC-native SCENE / SUB-SCENE-SWAP driver.
//
// Owns small resident functions that drive the area-mask "seen" register (FUN_800782F0) and the
// SUB-SCENE SWAP handshake — the bf818 waiter chain that gates a door/interior swap. Every entry
// is a `static` class method taking `Core*` — cross-file callers write `SceneTransition::foo(c, …)`
// directly (same shape as CutsceneCamera's static live-spine entries). No `extern "C"` shims.
//
// == The sub-scene swap handshake (docs/findings/scene.md) ==
//
//   The scene machine at 0x80026AD0 (`sceneStep`, called every frame) walks its state on node[5],
//   waiting for the shared u8 at 0x800BF818 ("swap phase") to progress. The 4 phases:
//
//     bf818 == 1  : beginSwap has fired for this transition. sceneStep case 0 latches
//                   bf817 into node[3]+1, advances the scene state.
//     bf818 == 2  : sceneStep case 1 has completed its swap-out (killed slot-1 task, reset
//                   scratchpad task+0x4c/0x4e, moved node[5] to 6) and is now WAITING for the
//                   swap-in signal.
//     bf818 == 3  : swap-in released. sceneStep case 3 fires FUN_800269BC(node) and advances.
//                   >>> This is the "2→3 handshake" — the whole reason the door freeze happens
//                       when bf818 never leaves 2. It is written by exactly one place: the
//                       `completeSwap()` method below (guest FUN_80073300).
//     bf818 == 4  : sceneStep case 4 clears bf818 back to 0 (transition complete).
//
//   The 2→3 progression is DRIVEN by the state-3 waiter machine `stepSwapWaiter` (guest
//   FUN_80073328), called every frame by `beh_typed_jumptable_pair` (the resident driver for the
//   6 entities that participate in a door). The waiter walks node[6] 0..5; case 3 with the
//   SECOND-branch gate (`node[0x29]!=0 && DAT_800e7ea9!=0 && DAT_800e7ffb==0`) is what calls
//   completeSwap → bf818:=3. If that gate never opens, bf818 stays at 2 forever and the door
//   never opens (this is the current pad-replay repro at scratch/bin/door_freeze.pad).
//
//   Making the handshake EXPLICIT — completeSwap() is named + documented rather than buried in a
//   `sb v0=3, bf818` at the tail of a case-3 branch, so a future fix for the "gate never opens"
//   bug has a clear target.
#pragma once
#include <cstdint>
class Core;

class SceneTransition {
public:
  // ── Guest addresses (the swap uses main-RAM registers the still-substrate scene machine reads) ──
  static constexpr uint32_t BF817_SWAP_KEY   = 0x800BF817u;   // set by beginSwap: node[3]
  static constexpr uint32_t BF818_SWAP_PHASE = 0x800BF818u;   // 0/1/2/3/4 — sceneStep waits here
  static constexpr uint32_t BF80F_SUSPEND    = 0x800BF80Fu;   // (bf80c[3]) non-zero gates every case
  static constexpr uint32_t E7EA9_ARMED      = 0x800E7EA9u;   // case-0/3 SECOND-branch gate
  static constexpr uint32_t E7FFB_INHIBIT    = 0x800E7FFBu;   // case-0/3 SECOND-branch gate
  static constexpr uint32_t E7FC5_FLAG       = 0x800E7FC5u;   // cleared by resetSwap
  static constexpr uint32_t SCENE_BLOCK      = 0x800E7E80u;   // s0-base cleared by resetSwap (fields +5/6/7)
  static constexpr uint32_t IF800137_PAUSE   = 0x1F800137u;   // case-0/3 tail: latch =2 if 0

  // ── FUN_800782F0: area-mask "seen" register + area-mode bit ──────────────────────────────────
  // For area < 9, OR a per-(area,sub) bit into the 32-bit register at 0x800BFE50. Then, if the
  // CURRENT area byte 0x800BF870 is 5/6/7/8, OR one of {2,4,8,0x10} into 0x800BF9DB. Idempotent.
  // Called from beh_scene_ui_trigger state-0 (confirm-edge) and ov_800796DC (bf808 reset).
  static void areaMaskTrigger(Core* c, uint8_t area, uint8_t sub);

  // ── FUN_80073260: swap RESET — the shared entry FUN_800732C0 / FUN_80073300 both call first ──
  // Writes node[0]=2, fires FUN_80074590(0x17, 0, 0xf) if node[0xBF]!=0 (a sound/rumble kick),
  // clears 0x800E7FC5 and 0x800E7E85/86/87, and dispatches FUN_80054198(0x800E7E80) (still substrate).
  static void resetSwap(Core* c, uint32_t node);

  // ── FUN_800732C0: BEGIN the swap — resetSwap + bf818:=1 + bf817:=node[3] (the swap key) ──────
  static void beginSwap(Core* c, uint32_t node);

  // ── FUN_80073300: COMPLETE the 2→3 handshake — resetSwap + bf818:=3 ──────────────────────────
  // This is the release the scene machine (FUN_80026AD0 case 3) is waiting on. Only ONE code path
  // writes bf818=3, and it is here — invoked ONLY by stepSwapWaiter case 3 SECOND-branch below.
  static void completeSwap(Core* c, uint32_t node);

  // ── FUN_80073328: stepSwapWaiter — the per-frame WAITER driven by beh_typed_jumptable_pair ──
  // node = 68B object; walks the sub-state at node[6] (0..5). Returns v0 (0 or 1) exactly like
  // the guest — beh_typed_jumptable_pair reads v0 to decide whether to advance node[5] in its
  // JT1[0]==2 branch (only case 5's "consume" returns 1; every other case returns 0).
  static int  stepSwapWaiter(Core* c, uint32_t node);
};
