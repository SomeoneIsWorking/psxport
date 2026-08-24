// test_spu_field_cadence.cpp — the SPU sink must follow the delivered field rate rather than
// assuming every field is exactly 1/60 second.
//
// The shipping cadence owns both system-clock and sample-frame remainders. These long runs exercise
// that implementation directly and prove that its fractional steps neither drift nor round every
// field independently.
#include "testutil.h"

#include <cstdint>
#include <limits>

#include "field_rate.h"
#include "spu_field_cadence.h"

namespace {

struct Totals {
  uint64_t clocks = 0;
  uint64_t samples = 0;
  uint32_t minClocks = std::numeric_limits<uint32_t>::max();
  uint32_t maxClocks = 0;
  uint32_t minSamples = std::numeric_limits<uint32_t>::max();
  uint32_t maxSamples = 0;
};

Totals runFields(DisplayFieldRate rate, uint32_t fieldCount) {
  SpuFieldCadence cadence;
  Totals totals;
  for (uint32_t field = 0; field < fieldCount; ++field) {
    const SpuFieldAdvance step = cadence.advance(rate);
    totals.clocks += step.clocks;
    totals.samples += step.samples;
    if (step.clocks < totals.minClocks) {
      totals.minClocks = step.clocks;
    }
    if (step.clocks > totals.maxClocks) {
      totals.maxClocks = step.clocks;
    }
    if (step.samples < totals.minSamples) {
      totals.minSamples = step.samples;
    }
    if (step.samples > totals.maxSamples) {
      totals.maxSamples = step.samples;
    }
  }
  return totals;
}

} // namespace

static void test_exact_sixty_hz_is_the_legacy_exact_step(void) {
  const Totals totals = runFields(DisplayFieldRate{60, 1}, 60);

  CHECK_EQ(totals.clocks, SpuFieldCadence::kClockRateHz);
  CHECK_EQ(totals.samples, SpuFieldCadence::kSampleRateHz);
  CHECK_EQ(totals.minClocks, 564480);
  CHECK_EQ(totals.maxClocks, 564480);
  CHECK_EQ(totals.minSamples, 735);
  CHECK_EQ(totals.maxSamples, 735);
}

static void test_ntsc_fractional_steps_have_exact_long_run_totals(void) {
  const Totals totals = runFields(DISPLAY_FIELD_RATE_NTSC, DISPLAY_FIELD_RATE_NTSC.frequencyNumerator);

  CHECK_EQ(totals.clocks, static_cast<uint64_t>(SpuFieldCadence::kClockRateHz) * 1001u);
  CHECK_EQ(totals.samples, static_cast<uint64_t>(SpuFieldCadence::kSampleRateHz) * 1001u);
  CHECK_EQ(totals.minClocks, 565044);
  CHECK_EQ(totals.maxClocks, 565045);
  CHECK_EQ(totals.minSamples, 735);
  CHECK_EQ(totals.maxSamples, 736);
}

static void test_rate_switch_starts_the_new_rational_phase(void) {
  SpuFieldCadence cadence;
  const SpuFieldAdvance ntsc = cadence.advance(DISPLAY_FIELD_RATE_NTSC);
  CHECK_EQ(ntsc.samples, 735);

  const SpuFieldAdvance firstPal = cadence.advance(DISPLAY_FIELD_RATE_PAL);
  const SpuFieldAdvance secondPal = cadence.advance(DISPLAY_FIELD_RATE_PAL);
  CHECK_EQ(firstPal.clocks, 677376);
  CHECK_EQ(firstPal.samples, 882);
  CHECK_EQ(secondPal.clocks, 677376);
  CHECK_EQ(secondPal.samples, 882);
}

int main(void) {
  RUN(exact_sixty_hz_is_the_legacy_exact_step);
  RUN(ntsc_fractional_steps_have_exact_long_run_totals);
  RUN(rate_switch_starts_the_new_rational_phase);
  return pt_summary();
}
