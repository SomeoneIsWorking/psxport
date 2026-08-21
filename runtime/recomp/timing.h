// timing.h — class Timing — native VBlank/VSync frame clock subsystem, owned by Game
// (`c->game->timing`, back-pointer wired in Game()). Owns the libetc VSync counter mirror +
// the vsync/frame-tick behavior (timing.cpp).
#pragma once
#include <cstdint>
class Game;
struct CdcState;

class Timing {
public:
  Game *game = nullptr;
  uint32_t vblank = 0;     // libetc VSync counter mirror (was g_vblank)
  uint32_t logicFrame = 0; // logic-frame counter, advanced by native_step_frame each iteration.
                           // Read by Cd::audioTrace / [bgmreq]-style diags. Was global g_bgm_frame.
  // Deterministic instruction-time estimate: one tick per instruction. This is not a cycle-accurate
  // R3000 model; issue 0007 tracks opcode/cache/GTE stall timing.
  uint64_t guestInstructionTicks = 0;

  void bindCdcClock(CdcState *cdc);
  void advanceGuestInstructionTicks(uint32_t ticks);

  // vsyncCallback(): 0x80085BB0 FUN_80085bb0 VSyncCallback(func) — no-op. Native frame loop
  //   owns pacing; the libapi per-vblank IRQ vector isn't modeled. Was ov_vsync_callback.
  void vsyncCallback();

  // vsync(): 0x80085900 FUN_80085900 = libetc VSync(mode). Currently unreachable — sync_overrides
  //   traps VSync (all pacing is PC-native). Kept for RE reference / future re-enable.
  void vsync();

  // frameTick(): advance the canonical libetc VSync counter once per native frame. Called from
  //   the PC-native frame loop (native_step_frame) so recomp code reading DAT_800abde0 for
  //   pacing/idle-timers keeps advancing.
  void frameTick();

private:
  static uint64_t readGuestInstructionTicks(void *context);
};
