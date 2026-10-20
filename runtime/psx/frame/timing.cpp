#include "timing.h"

#include "cdc_state.h"
#include "core.h"
#include "game.h"
#include "gpu_device.h"

namespace psx::frame {

uint64_t Timing::readEmulatedCpuTicks(void *context) {
  return static_cast<Timing *>(context)->mEmulatedTime.nowTicks();
}

void Timing::bindCdcClock(psx::cd::CdController *cdc) {
  cdc->bindTickSource(this, readEmulatedCpuTicks);
}

void Timing::advanceGuestInstructionTicks(uint32_t ticks) {
  guestInstructionTicks += ticks;
  mEmulatedTime.advanceInstructions(ticks);
  game->sio.service();
  serviceCdc();
}

bool Timing::advanceDisplayFields(FieldCadence cadence, uint32_t fieldRateMilliHz) {
  if (cadence.fields <= 0 || cadence.parts <= 0 || fieldRateMilliHz == 0) {
    return false;
  }
  if (!mEmulatedTime.advanceDisplayFields(
          static_cast<uint32_t>(cadence.fields), static_cast<uint32_t>(cadence.parts), fieldRateMilliHz)) {
    return false;
  }
  // Re-anchored before the field's callbacks run, or one would request the field it is inside.
  game->core.hostTurn.onDisplayField(game->core);
  game->sio.service();
  // A latch, not a count, as the hardware bit is; a guest that leaves VBlank masked sees nothing.
  if (consumeCompletedDisplayFields(cadence) != 0) {
    game->core.raiseInterrupt(IRQ_BIT_VBLANK);
  }
  serviceCdc();
  game->fieldDigest.recordField(*game);
  return true;
}

namespace {

unsigned __int128 gcd128(unsigned __int128 a, unsigned __int128 b) {
  while (b != 0) {
    const unsigned __int128 remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

} // namespace

uint32_t Timing::consumeCompletedDisplayFields(FieldCadence cadence) {
  const auto fields = static_cast<uint32_t>(cadence.fields);
  const auto parts = static_cast<uint32_t>(cadence.parts);
  const unsigned __int128 common = gcd128(mDisplayFieldPhaseDenominator, parts);
  const unsigned __int128 leftScale = parts / common;
  const unsigned __int128 rightScale = mDisplayFieldPhaseDenominator / common;
  const unsigned __int128 denominator = mDisplayFieldPhaseDenominator * leftScale;
  const unsigned __int128 numerator = mDisplayFieldPhaseNumerator * leftScale + fields * rightScale;
  const unsigned __int128 completed = numerator / denominator;
  mDisplayFieldPhaseNumerator = numerator % denominator;
  mDisplayFieldPhaseDenominator = denominator;
  if (mDisplayFieldPhaseNumerator == 0) {
    mDisplayFieldPhaseDenominator = 1;
  } else {
    const unsigned __int128 reduction = gcd128(mDisplayFieldPhaseNumerator, mDisplayFieldPhaseDenominator);
    mDisplayFieldPhaseNumerator /= reduction;
    mDisplayFieldPhaseDenominator /= reduction;
  }
  return static_cast<uint32_t>(completed);
}

std::optional<uint64_t> Timing::ticksUntilDeviceEvent() const {
  uint64_t deadline = 0;
  if (!game->cdc.nextDeadlineTicks(&deadline)) {
    return std::nullopt;
  }
  const uint64_t now = mEmulatedTime.nowTicks();
  return deadline > now ? deadline - now : 0;
}

uint64_t Timing::emulatedCpuTicks() const {
  return mEmulatedTime.nowTicks();
}

// Root counter 2 on the emulated clock, wrapping at 16 bits; mode bit 9 is system clock / 8, and sync
// modes 0 and 3 stop it. The target/wrap IRQ and the reached-target status bits are not modelled.
uint16_t Timing::rootCounter2() const {
  const uint32_t syncMode = (rootCounter2Mode >> 1u) & 0x3u;
  const bool stopped = (rootCounter2Mode & 0x1u) != 0 && (syncMode == 0u || syncMode == 3u);
  if (stopped) {
    return rootCounter2BaseValue;
  }
  const unsigned shift = (rootCounter2Mode & 0x200u) ? 3u : 0u; // bit 9: system clock / 8
  const uint64_t count = rootCounter2BaseValue + ((mEmulatedTime.nowTicks() - rootCounter2OriginTicks) >> shift);
  if (rootCounter2Mode & 0x008u) { // reset on target: the programmed target is the wrap period
    const uint32_t period = rootCounter2Target > 1u ? rootCounter2Target : 1u;
    return static_cast<uint16_t>(count % period);
  }
  return static_cast<uint16_t>(count);
}

// Writing the value restarts counting from it.
void Timing::rootCounter2WriteValue(uint16_t value) {
  rootCounter2BaseValue = value;
  rootCounter2OriginTicks = mEmulatedTime.nowTicks();
}

// A mode write resets the counter to zero, as on hardware.
void Timing::rootCounter2WriteMode(uint32_t mode) {
  rootCounter2Mode = mode & 0x3FFu;
  rootCounter2BaseValue = 0;
  rootCounter2OriginTicks = mEmulatedTime.nowTicks();
}

void Timing::rootCounter2WriteTarget(uint32_t target) {
  rootCounter2Target = target & 0xFFFFu;
}

DisplayFieldRate Timing::displayFieldRate() const {
  return psx::frame::displayFieldRate(game != nullptr && game->core.gpuDevice().displayArea().pal);
}

uint64_t Timing::hSyncTotal() const {
  const DisplayFieldRate rate = displayFieldRate();
  return mEmulatedTime.hSyncCount(fieldRateMilliHz(rate), displayLinesPerField(rate == DISPLAY_FIELD_RATE_PAL));
}

uint16_t Timing::hSyncCounter() const {
  return static_cast<uint16_t>(hSyncTotal() - rootCounter1Offset);
}

void Timing::hSyncCounterWrite(uint16_t value) {
  rootCounter1Offset = hSyncTotal() - value;
}

void Timing::serviceCdc() {
  if (game->cdc.service()) {
    game->core.irqStatLatch();
  }
}

void Timing::frameTick() {
  vblank += 1u;
}

Timing::ClockSnapshot Timing::clockSnapshot() const {
  return {mEmulatedTime.q32(),
          static_cast<uint64_t>(mDisplayFieldPhaseNumerator),
          static_cast<uint64_t>(mDisplayFieldPhaseDenominator)};
}

void Timing::restoreClockSnapshot(const ClockSnapshot &clock) {
  mEmulatedTime.restore(clock.time);
  mDisplayFieldPhaseNumerator = clock.displayPhaseNumerator;
  mDisplayFieldPhaseDenominator = clock.displayPhaseDenominator;
}

} // namespace psx::frame
