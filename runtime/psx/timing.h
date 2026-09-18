// timing.h — native display-time state, owned by Game.
#pragma once
#include "emulated_time.h"

#include <cstdint>
class Game;
struct CdcState;

class Timing {
public:
  Game *game = nullptr;
  uint32_t vblank = 0;     // Host field count; titles own any guest-memory mirror.
  uint32_t logicFrame = 0; // logic-frame counter, advanced by the title's native FrameDriver.
                           // Read by Cd::audioTrace / [bgmreq]-style diags. Was global g_bgm_frame.
  // Diagnostic raw instruction count. CDC deadlines use mEmulatedTime, which also crosses display
  // waits; neither counter is yet a cycle-accurate R3000 model (issue 0007).
  uint64_t guestInstructionTicks = 0;

  // The CDC drive clock is the emulated CPU clock: executed instructions plus the display fields
  // the native frame loop delivers. The drive, the display field clock that owes host turns, and
  // the per-field SPU pull therefore share one time base, so a guest that busy-polls the drive
  // sees fields, sectors, and audio advance in the console's ratio at any host speed. A previous
  // wall-locked drive clock (Vagrant Story issue #25) fixed an A/V drift that came from mixing an
  // instruction-cost drive with a host-paced SPU pull; it also made every synchronous CD wait cost
  // the guest a host-speed-dependent number of fields (Spyro 1's loader hand-off spun through
  // ~60 fields at JIT speed) and made loads non-reproducible between runs and against the console.

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
  // Service CDC deadlines against the emulated drive clock without advancing anything else
  // (test + REPL entry point; the run loop reaches the same path via the two advance methods).
  void serviceCdcTickSource() {
    serviceCdc();
  }
  [[nodiscard]] uint64_t emulatedCpuTicks() const;
  [[nodiscard]] uint16_t hSyncCounter() const;

  // vsyncCallback(): 0x80085BB0 FUN_80085bb0 VSyncCallback(func) — no-op. Native frame loop
  //   owns pacing; the libapi per-vblank IRQ vector isn't modeled. Was ov_vsync_callback.
  void vsyncCallback();

  // Advance the host field count once at the title's native frame boundary.
  void frameTick();

private:
  EmulatedTime mEmulatedTime;
  // Exact rational phase for display pacing subdivisions. fps60 delivers two 1/2-field pacing
  // calls for one physical field; only the completed whole field raises VBlank.
  unsigned __int128 mDisplayFieldPhaseNumerator = 0;
  unsigned __int128 mDisplayFieldPhaseDenominator = 1;

  static uint64_t readEmulatedCpuTicks(void *context);
  uint32_t consumeCompletedDisplayFields(uint32_t fields, uint32_t parts);
  void raiseVBlank(uint32_t fields);
  void serviceCdc();
};
