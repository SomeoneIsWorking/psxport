#include "emulated_time.h"

namespace {

constexpr unsigned kFractionBits = 32;

unsigned __int128 display_field_duration_q32(uint32_t fields, uint32_t parts, uint32_t fieldRateMilliHz) {
  if (fields == 0 || parts == 0 || fieldRateMilliHz == 0) {
    return 0;
  }
  const unsigned __int128 numerator = (static_cast<unsigned __int128>(kNominalPsxCpuHz) * 1000u * fields)
                                      << kFractionBits;
  const unsigned __int128 denominator = static_cast<unsigned __int128>(fieldRateMilliHz) * parts;
  return numerator / denominator;
}

} // namespace

uint64_t display_field_cpu_ticks(uint32_t fields, uint32_t parts, uint32_t fieldRateMilliHz) {
  return static_cast<uint64_t>(display_field_duration_q32(fields, parts, fieldRateMilliHz) >> kFractionBits);
}

void EmulatedTime::advanceInstructions(uint32_t ticks) {
  mNowQ32 += static_cast<unsigned __int128>(ticks) << kFractionBits;
}

bool EmulatedTime::advanceDisplayFields(uint32_t fields, uint32_t parts, uint32_t fieldRateMilliHz) {
  const unsigned __int128 duration = display_field_duration_q32(fields, parts, fieldRateMilliHz);
  if (duration == 0) {
    return false;
  }

  unsigned __int128 boundary = mDisplayBoundaryQ32 + duration;
  if (boundary < mNowQ32) {
    // The CPU passed the scheduled field while it was executing. This delivery is immediate; anchor
    // the following field at the observed guest time instead of accumulating catch-up debt.
    boundary = mNowQ32;
  }
  mDisplayBoundaryQ32 = boundary;
  mNowQ32 = boundary;
  return true;
}

uint64_t EmulatedTime::nowTicks() const {
  return static_cast<uint64_t>(mNowQ32 >> kFractionBits);
}
