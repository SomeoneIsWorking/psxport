// emulated_time.h — authoritative guest CPU time across executed instructions and display waits.
#pragma once

#include <cstdint>

constexpr uint32_t kNominalPsxCpuHz = 33'868'800u;

// Return the integral CPU ticks represented by a display-field interval. Invalid cadence inputs
// return zero. EmulatedTime retains the fractional part internally; this helper is for evidence and
// diagnostics, not a second clock implementation.
uint64_t display_field_cpu_ticks(uint32_t fields, uint32_t parts, uint32_t fieldRateMilliHz);

class EmulatedTime {
public:
  void advanceInstructions(uint32_t ticks);
  bool advanceDisplayFields(uint32_t fields, uint32_t parts, uint32_t fieldRateMilliHz);
  [[nodiscard]] uint64_t nowTicks() const;
  [[nodiscard]] uint64_t hSyncCount(uint32_t fieldRateMilliHz, uint32_t linesPerField) const;

private:
  // Q32 CPU ticks preserve the fractional NTSC field duration without a floating-point or host-time
  // dependency. The display boundary is a phase anchor: instructions can consume a field interval,
  // and a display wait advances TO the boundary rather than adding a second interval on top.
  unsigned __int128 mNowQ32 = 0;
  unsigned __int128 mDisplayBoundaryQ32 = 0;
};
