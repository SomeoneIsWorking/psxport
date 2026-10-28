#include "emulated_time.h"

#include <algorithm>

namespace psx::frame {

namespace {

constexpr unsigned kFractionBits = 32;

unsigned __int128 display_field_duration_q32(uint32_t fields, uint32_t parts, uint32_t fieldRateMilliHz) {
  if (fields == 0 || parts == 0 || fieldRateMilliHz == 0) {
    return 0;
  }
  const unsigned __int128 numerator = (static_cast<unsigned __int128>(kNominalPsxCpuHz) * 1000u * fields)
                                      << kFractionBits;
  const unsigned __int128 denominator = static_cast<unsigned __int128>(fieldRateMilliHz) * parts;
  // A delivered field is a completed boundary. Round its Q32 representation upward so converting
  // that exact boundary back to HSyncs cannot report one line short solely because the fixed-point
  // duration was truncated by less than one Q32 tick.
  return (numerator + denominator - 1) / denominator;
}

} // namespace

uint64_t displayFieldCpuTicks(uint32_t fields, uint32_t parts, uint32_t fieldRateMilliHz) {
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

  if (mIntervalPart == 0 || fields != mIntervalFields || parts != mIntervalParts) {
    // The interval ends `fields` after the last boundary, moved on by the whole fields the guest's work already ran
    // past: no catch-up debt, and boundaries stay on the field grid while the clock keeps its part-field lead.
    const unsigned __int128 whole = display_field_duration_q32(fields, 1, fieldRateMilliHz);
    const unsigned __int128 field = display_field_duration_q32(1, 1, fieldRateMilliHz);
    const unsigned __int128 end = mDisplayBoundaryQ32 + whole;
    mIntervalStartQ32 = mDisplayBoundaryQ32 + (mNowQ32 > end ? (mNowQ32 - end) / field * field : 0);
    mIntervalFields = fields;
    mIntervalParts = parts;
    mIntervalPart = 0;
  }
  ++mIntervalPart;
  // A part the guest's work already ran past is delivered at once, so the last part lands on the interval's end.
  mDisplayBoundaryQ32 = mIntervalStartQ32 + display_field_duration_q32(fields * mIntervalPart, parts, fieldRateMilliHz);
  mNowQ32 = std::max(mNowQ32, mDisplayBoundaryQ32);
  if (mIntervalPart == parts) {
    mIntervalPart = 0;
  }
  return true;
}

uint64_t EmulatedTime::nowTicks() const {
  return static_cast<uint64_t>(mNowQ32 >> kFractionBits);
}

uint64_t EmulatedTime::boundaryTicks() const {
  return static_cast<uint64_t>(mDisplayBoundaryQ32 >> kFractionBits);
}

uint32_t EmulatedTime::fieldsSinceBoundary(uint32_t fieldRateMilliHz) const {
  const unsigned __int128 duration = display_field_duration_q32(1, 1, fieldRateMilliHz);
  if (duration == 0 || mNowQ32 <= mDisplayBoundaryQ32) {
    return 0;
  }
  const unsigned __int128 fields = (mNowQ32 - mDisplayBoundaryQ32) / duration;
  return fields > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(fields);
}

uint64_t EmulatedTime::ticksToNextField(uint32_t fieldRateMilliHz) const {
  const unsigned __int128 duration = display_field_duration_q32(1, 1, fieldRateMilliHz);
  if (duration == 0) {
    return 0;
  }
  const unsigned __int128 elapsed = mNowQ32 > mDisplayBoundaryQ32 ? mNowQ32 - mDisplayBoundaryQ32 : 0;
  const unsigned __int128 remaining = (elapsed / duration + 1) * duration - elapsed;
  return static_cast<uint64_t>((remaining + (static_cast<unsigned __int128>(1) << kFractionBits) - 1) >> kFractionBits);
}

uint64_t EmulatedTime::hSyncCount(uint32_t fieldRateMilliHz, uint32_t linesPerField) const {
  if (fieldRateMilliHz == 0 || linesPerField == 0) {
    return 0;
  }
  const unsigned __int128 scaled = mNowQ32 * fieldRateMilliHz * linesPerField;
  const unsigned __int128 denominator = (static_cast<unsigned __int128>(kNominalPsxCpuHz) * 1000u) << kFractionBits;
  return static_cast<uint64_t>(scaled / denominator);
}

} // namespace psx::frame
