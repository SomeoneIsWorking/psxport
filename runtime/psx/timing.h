// timing.h — native display-time and compatibility-counter state, owned by Game.
#pragma once
#include "emulated_time.h"

#include <cstdint>
class Game;
struct CdcState;

class Timing {
public:
  Game *game = nullptr;
  uint32_t vblank = 0;     // libetc VSync counter mirror (was g_vblank)
  uint32_t logicFrame = 0; // logic-frame counter, advanced by the title's native FrameDriver.
                           // Read by Cd::audioTrace / [bgmreq]-style diags. Was global g_bgm_frame.
  // Diagnostic raw instruction count. CDC deadlines use mEmulatedTime, which also crosses display
  // waits; neither counter is yet a cycle-accurate R3000 model (issue 0007).
  uint64_t guestInstructionTicks = 0;

  // Wall-locked CDC drive clock: nominal-rate CPU ticks derived from the host monotonic clock,
  // NOT from executed-instruction costs. A real drive and its crystal run in real time regardless
  // of what the CPU is doing; modelling the drive on instruction costs made it run at HOST speed
  // whenever the guest busy-polls instead of display-waiting — Vagrant Story's FMV player does
  // exactly that, which drove the CD up to ~6% hot vs the field-paced SPU pull and saturated the
  // XA ring (issue #25). Pure field-count locking was tried first and DEADLOCKED boot: libcd's
  // synchronous init waits on a command completion, i.e. on a deadline, before any field boundary.
  // Wall time advances everywhere, always, which is precisely the property a drive needs.
  uint64_t wallClockOriginNs = 0; // bind instant for the wall-locked CDC clock

  // ---- root counter 2 (0x1F801120 value / 0x1F801124 mode / 0x1F801128 target) ----------------
  // A free-running system-clock counter. Guest code uses it as a stopwatch: latch the value, spin
  // until the delta exceeds a budget. Contract and its measured limits are in timing.cpp.
  uint32_t rootCounter2Mode = 0;
  uint32_t rootCounter2Target = 0;
  uint16_t rootCounter2BaseValue = 0;
  uint64_t rootCounter2OriginTicks = 0;
  [[nodiscard]] uint16_t rootCounter2() const;
  void rootCounter2Write(uint32_t reg, uint32_t v);

  void bindCdcClock(CdcState *cdc);
  void advanceGuestInstructionTicks(uint32_t ticks);
  bool advanceDisplayFields(int fields, int parts, uint32_t fieldRateMilliHz);
  // Service CDC deadlines against the wall-locked drive clock without advancing anything else
  // (test + REPL entry point; the run loop reaches the same path via the two advance methods).
  void serviceCdcTickSource() {
    serviceCdc();
  }
  [[nodiscard]] uint64_t emulatedCpuTicks() const;
  [[nodiscard]] uint16_t hSyncCounter() const;

  // vsyncCallback(): 0x80085BB0 FUN_80085bb0 VSyncCallback(func) — no-op. Native frame loop
  //   owns pacing; the libapi per-vblank IRQ vector isn't modeled. Was ov_vsync_callback.
  void vsyncCallback();

  // frameTick(): mirror one title-owned native frame into the compatibility counter so finite guest
  //   leaves reading DAT_800abde0 for pacing/idle-timers keep advancing.
  void frameTick();

private:
  EmulatedTime mEmulatedTime;
  // Exact rational phase for display pacing subdivisions. fps60 delivers two 1/2-field pacing
  // calls for one physical field; only the completed whole field raises VBlank.
  unsigned __int128 mDisplayFieldPhaseNumerator = 0;
  unsigned __int128 mDisplayFieldPhaseDenominator = 1;

  static uint64_t readEmulatedCpuTicks(void *context);
  static uint64_t readWallLockedCdcTicks(void *context);
  uint32_t consumeCompletedDisplayFields(uint32_t fields, uint32_t parts);
  void raiseVBlank(uint32_t fields);
  void serviceCdc();
};
