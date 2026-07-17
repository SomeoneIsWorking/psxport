// class ParallaxBg — the SOP field-mode's PARALLAX BACKGROUND state machine.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::parallaxBg`. Back-pointer wired
// once by Core's constructor. Callers reach it via the object graph:
//
//     eng(c).parallaxBg.step();     // was FUN_8010BFFC (called from Sop::fieldUpdate)
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as
// BgSceneTransitionSm, ObjectList, Sop.
//
// Owns the guest-side SM struct at 0x800ED018 (a 60-byte record embedded in engine data). Two
// states:
//   * SM[0] == 0 : INIT. Copies 9 u16s + 2 u8s from *(u16*)0x800ECF84 (per-area BG params ptr —
//     grid dims / tile sizes / palette / two data-buffer sizes) into SM+4..+0x11, seeds the three
//     data-buffer pointers at SM+0x14/+0x18/+0x1C (parameter-block base + variable-sized runs),
//     stamps the two scroll-modulus fields (SM+0x30 = grid_w×16, SM+0x32 = (grid_h×0x8E8)/0x90),
//     sets SM[0]=1, SM[3]=0, SM+0x38=1, and clears the 3 SOP-overlay animation counters at
//     0x8010D390/391/392.
//   * SM[0] == 1 : RUNNING. Computes wrapped scroll offsets (SM+0x28/+0x2A) from the camera yaw
//     @0x1F8000F2 × SM+0x2C and pitch @0x1F8000F0 × SM+0x2E, modulo SM+0x30 / SM+0x32. Decrements
//     the SM+0x38 counter; on signed-byte wrap sets SM[3]=1 (a "frame settled" latch used by the
//     BG tile scroller / draw pass).
//
// Does NOT touch GP0 or the packet pool — that lives in FUN_8010C26C (BG TILE SCROLLER), which is
// intentionally left as substrate: it emits raw GP0 packet bytes and belongs to the PC-native BG
// renderer rewrite, not a mechanical port (per "REBUILD, don't transcribe" in CLAUDE.md).
#pragma once
#include <cstdint>
class Core;

class ParallaxBg {
public:
  Core* core = nullptr;

  // Guest-RAM address of the parallax BG state-machine struct (60 bytes at 0x800ED018).
  static constexpr uint32_t SM_ADDR = 0x800ED018u;

  // Per-field-frame tick. Guarded by the SOP scene-beat != 5 check at the call site
  // (Sop::fieldUpdate) — for beat 5 (narration void) BG isn't visible so the caller skips this.
  void step();
};
