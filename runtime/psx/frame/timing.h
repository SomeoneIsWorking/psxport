// Native display-time state, owned by Game: the emulated CPU clock, the field clock and root counters 1 and 2.
#pragma once
#include "emulated_time.h"
#include "field_cadence.h"
#include "field_rate.h"

#include <cstdint>
#include <optional>

class Game;
namespace psx::cd {
class CdController;
}

namespace psx::frame {

class Timing {
public:
  Game *game = nullptr;
  uint32_t vblank = 0;     // Host field count; titles own any guest-memory mirror.
  uint32_t logicFrame = 0; // advanced by the title's native FrameDriver
  // Raw executed instructions; CDC deadlines use the emulated clock, which also crosses display waits.
  uint64_t guestInstructionTicks = 0;

  // The CD drive, the field clock and the SPU pull share the emulated CPU clock, so a guest that
  // busy-polls the drive sees fields, sectors and audio advance in the console's ratio at any host speed.

  // Root counter 2 (0x1F801120 value / 0x1F801124 mode / 0x1F801128 target): a stopwatch guests latch and spin on.
  uint32_t rootCounter2Mode = 0;
  uint32_t rootCounter2Target = 0;
  uint16_t rootCounter2BaseValue = 0;
  uint64_t rootCounter2OriginTicks = 0;
  [[nodiscard]] uint16_t rootCounter2() const;
  void rootCounter2WriteValue(uint16_t value);
  void rootCounter2WriteMode(uint32_t mode);
  void rootCounter2WriteTarget(uint32_t target);

  void bindCdcClock(psx::cd::CdController *cdc);
  void advanceGuestInstructionTicks(uint32_t ticks);
  bool advanceDisplayFields(FieldCadence cadence, uint32_t fieldRateMilliHz);
  // CDC deadlines against the emulated drive clock, advancing nothing else.
  void serviceCdc();
  // The guest's display standard, from GP1(0x08).
  [[nodiscard]] DisplayFieldRate displayFieldRate() const;
  [[nodiscard]] uint64_t emulatedCpuTicks() const;
  // The guest clock at the last delivered field edge.
  [[nodiscard]] uint64_t displayBoundaryTicks() const;
  // The fields a libetc VSync(n) called now spans from the last delivered field: it waits for field n-1, then for the
  // next VBlank, so guest work that ran past field k makes it max(n, k + 1).
  [[nodiscard]] uint32_t vsyncFields(uint32_t requested) const;
  // Guest ticks left in the field the clock is in: a guest turn ends at the next VBlank.
  [[nodiscard]] uint64_t ticksToNextField() const;
  // A guest VSync(n) a frame driver replaces: n counts from the previous wait's return, so the fields earlier steps
  // already delivered since then come off it.
  void noteVSyncWait(uint32_t requested);
  // The fields a frame driver step delivers: the noted wait's, or, for a turn that ended without one, every field edge
  // its clock crossed (at least one); the clock keeps the part field it ran into.
  [[nodiscard]] uint32_t takeStepFields();
  // Guest ticks until the next scheduled device deadline (today the CD controller's drive or command
  // event); nullopt when nothing is armed, 0 when one is already due. A guest running code that touches
  // no device register never folds a deadline into the clock, so the executor ends its segment here: the
  // interrupt the deadline raises then reaches the guest while it spins, as it would on hardware.
  [[nodiscard]] std::optional<uint64_t> ticksUntilDeviceEvent() const;
  // Root counter 1: HBlank count since the guest last wrote its value (libapi ResetRCnt writes zero).
  [[nodiscard]] uint16_t hSyncCounter() const;
  void hSyncCounterWrite(uint16_t value);
  uint64_t rootCounter1Offset = 0;
  [[nodiscard]] uint64_t hSyncTotal() const;

  // Advance the host field count once at the title's native frame boundary.
  void frameTick();
  // Display fields the emulated clock has crossed: the field a draw entry or a present lands in.
  [[nodiscard]] uint64_t displayField() const;

  // The emulated clock and the display-pacing phase, for a save state.
  struct ClockSnapshot {
    EmulatedTimeQ32 time;
    uint64_t displayPhaseNumerator = 0;
    uint64_t displayPhaseDenominator = 1;
  };
  ClockSnapshot clockSnapshot() const;
  void restoreClockSnapshot(const ClockSnapshot &clock);

private:
  EmulatedTime mEmulatedTime;
  // 0 while no replaced wait is pending; vsyncFields is never 0.
  uint32_t mVSyncWaitFields = 0;
  uint32_t mFieldsSinceVSyncWait = 0;
  // fps60 delivers two half-field pacing calls per field; only a completed whole field raises VBlank.
  unsigned __int128 mDisplayFieldPhaseNumerator = 0;
  unsigned __int128 mDisplayFieldPhaseDenominator = 1;

  static uint64_t readEmulatedCpuTicks(void *context);
  uint32_t consumeCompletedDisplayFields(FieldCadence cadence);
};
} // namespace psx::frame
