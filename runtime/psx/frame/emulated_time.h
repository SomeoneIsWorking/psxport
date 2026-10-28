// emulated_time.h — authoritative guest CPU time across executed instructions and display waits.
#pragma once

#include <cstdint>

namespace psx::frame {

constexpr uint32_t kNominalPsxCpuHz = 33'868'800u;

// A reading of the emulated CPU clock.
enum class CpuTicks : uint64_t {};

// Return the integral CPU ticks represented by a display-field interval. Invalid cadence inputs
// return zero. EmulatedTime retains the fractional part internally; this helper is for evidence and
// diagnostics, not a second clock implementation.
uint64_t displayFieldCpuTicks(uint32_t fields, uint32_t parts, uint32_t fieldRateMilliHz);

// Save-state form of the 128-bit Q32 clock; values are truncated to 64 bits.
struct EmulatedTimeQ32 {
  uint64_t now = 0;
  uint64_t displayBoundary = 0;
};

class EmulatedTime {
public:
  void advanceInstructions(uint32_t ticks);
  // One of `parts` deliveries of a `fields`-field interval; the interval's parts arrive back to back.
  bool advanceDisplayFields(uint32_t fields, uint32_t parts, uint32_t fieldRateMilliHz);
  [[nodiscard]] uint64_t nowTicks() const;
  // The last delivered display field edge.
  [[nodiscard]] uint64_t boundaryTicks() const;
  // Whole fields of guest time since the last delivered display boundary.
  [[nodiscard]] uint32_t fieldsSinceBoundary(uint32_t fieldRateMilliHz) const;
  // Whole ticks from now to the next field edge after the last delivered boundary; never 0.
  [[nodiscard]] uint64_t ticksToNextField(uint32_t fieldRateMilliHz) const;
  [[nodiscard]] uint64_t hSyncCount(uint32_t fieldRateMilliHz, uint32_t linesPerField) const;
  [[nodiscard]] uint64_t hSyncsSinceBoundary(uint32_t fieldRateMilliHz, uint32_t linesPerField) const;

  [[nodiscard]] EmulatedTimeQ32 q32() const {
    return {static_cast<uint64_t>(mNowQ32), static_cast<uint64_t>(mDisplayBoundaryQ32)};
  }
  void restore(EmulatedTimeQ32 state) {
    mNowQ32 = static_cast<unsigned __int128>(state.now);
    mDisplayBoundaryQ32 = static_cast<unsigned __int128>(state.displayBoundary);
    mIntervalPart = 0;
  }

private:
  static uint64_t hSyncCountAt(unsigned __int128 timeQ32, uint32_t fieldRateMilliHz, uint32_t linesPerField);
  // Q32 CPU ticks preserve the fractional NTSC field duration without a floating-point or host-time
  // dependency. The display boundary is a phase anchor on the field grid: instructions can consume a field
  // interval, and a display wait advances TO the boundary rather than adding a second interval on top.
  unsigned __int128 mNowQ32 = 0;
  unsigned __int128 mDisplayBoundaryQ32 = 0;
  // The interval being delivered: where its first part starts and how many of its parts have landed.
  unsigned __int128 mIntervalStartQ32 = 0;
  uint32_t mIntervalFields = 0;
  uint32_t mIntervalParts = 0;
  uint32_t mIntervalPart = 0;
};
} // namespace psx::frame
