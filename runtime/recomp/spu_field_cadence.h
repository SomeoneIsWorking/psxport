#pragma once
// SPU field cadence — converts the exact display-field rate to integer SPU work without drift.
//
// The PSX SPU consumes 33,868,800 system clocks per second and emits one 44.1 kHz stereo frame for
// every 768 clocks. NTSC delivers 60,000 fields per 1,001 seconds, so neither quantity is integral
// per field. This owner carries the two division remainders across calls; rounding each call would
// permanently lose 44.1 stereo frames per second.

#include "field_rate.h"

#include <cstdint>

struct SpuFieldAdvance {
  uint32_t clocks;
  uint32_t samples;
};

class SpuFieldCadence {
public:
  static constexpr uint32_t kClockRateHz = 33868800u;
  static constexpr uint32_t kSampleRateHz = 44100u;
  static constexpr uint32_t kMaximumStandardSamplesPerField = kSampleRateHz / 50u;

  SpuFieldAdvance advance(DisplayFieldRate rate) {
    if (rate != mRate) {
      // GP1(0x08) may change standards at runtime. Remainders have the old rate's denominator and
      // cannot be carried into the new rational domain, so the new standard starts a new phase.
      mRate = rate;
      mClockRemainder = 0;
      mSampleRemainder = 0;
    }

    const uint32_t clocks = divideStep(kClockRateHz, rate, mClockRemainder);
    const uint32_t samples = divideStep(kSampleRateHz, rate, mSampleRemainder);
    return {clocks, samples};
  }

private:
  static uint32_t divideStep(uint32_t unitsPerSecond, DisplayFieldRate rate, uint64_t &remainder) {
    const uint64_t numerator = remainder + static_cast<uint64_t>(unitsPerSecond) * rate.frequencyDenominator;
    const uint32_t units = static_cast<uint32_t>(numerator / rate.frequencyNumerator);
    remainder = numerator % rate.frequencyNumerator;
    return units;
  }

  DisplayFieldRate mRate{};
  uint64_t mClockRemainder = 0;
  uint64_t mSampleRemainder = 0;
};
