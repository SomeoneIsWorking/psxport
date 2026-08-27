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

  // frameTick(): mirror one title-owned native frame into the compatibility counter so finite recomp
  //   leaves reading DAT_800abde0 for pacing/idle-timers keep advancing.
  void frameTick();

private:
  EmulatedTime mEmulatedTime;

  static uint64_t readEmulatedCpuTicks(void *context);
  static uint64_t readWallLockedCdcTicks(void *context);
  void serviceCdc();
};
