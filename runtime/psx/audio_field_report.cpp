// audio_field_report.cpp — exact per-field SPU report comparison for SBS.
#include "audio_field_report.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace {

bool sameDouble(double a, double b) {
  return std::abs(a - b) <= 0.000001;
}

AudioFieldCompareResult mismatch(size_t index, std::string reason) {
  return AudioFieldCompareResult{false, index, std::move(reason)};
}

AudioFieldCompareResult compareOne(size_t index, const AudioFieldReport &a, const AudioFieldReport &b) {
#define AUDIO_FIELD_COMPARE(field)                                                                                     \
  if (a.field != b.field) {                                                                                            \
    return mismatch(index, std::format("field {}: {} differs (A={} B={})", index, #field, a.field, b.field));          \
  }
  AUDIO_FIELD_COMPARE(ordinal)
  AUDIO_FIELD_COMPARE(expectedSamples)
  AUDIO_FIELD_COMPARE(expectedClocks)
  AUDIO_FIELD_COMPARE(renderedSamples)
  AUDIO_FIELD_COMPARE(queuedSamples)
  AUDIO_FIELD_COMPARE(pcmHash)
  AUDIO_FIELD_COMPARE(pcmNonzero)
  AUDIO_FIELD_COMPARE(pcmPeak)
  AUDIO_FIELD_COMPARE(xaWr)
  if (!sameDouble(a.xaRd, b.xaRd)) {
    return mismatch(index, std::format("field {}: xaRd differs (A={:.6f} B={:.6f})", index, a.xaRd, b.xaRd));
  }
  AUDIO_FIELD_COMPARE(xaPulls)
  AUDIO_FIELD_COMPARE(xaSectors)
  AUDIO_FIELD_COMPARE(deltaWr)
  if (!sameDouble(a.deltaRd, b.deltaRd)) {
    return mismatch(index, std::format("field {}: deltaRd differs (A={:.6f} B={:.6f})", index, a.deltaRd, b.deltaRd));
  }
  AUDIO_FIELD_COMPARE(deltaPulls)
  AUDIO_FIELD_COMPARE(deltaSectors)
  AUDIO_FIELD_COMPARE(pal)
  if (a.pcmSamples.size() != b.pcmSamples.size()) {
    return mismatch(
        index,
        std::format("field {}: PCM sample count differs (A={} B={})", index, a.pcmSamples.size(), b.pcmSamples.size()));
  }
  for (size_t sample = 0; sample < a.pcmSamples.size(); ++sample) {
    if (a.pcmSamples[sample] != b.pcmSamples[sample]) {
      return mismatch(index,
                      std::format("field {}: PCM sample {} differs (A={} B={})",
                                  index,
                                  sample,
                                  a.pcmSamples[sample],
                                  b.pcmSamples[sample]));
    }
  }
#undef AUDIO_FIELD_COMPARE
  return AudioFieldCompareResult{true, index, {}};
}

} // namespace

AudioFieldCompareResult
compareAudioFieldReports(const AudioFieldReport *a, size_t aCount, const AudioFieldReport *b, size_t bCount) {
  if (aCount == 0 && bCount == 0) {
    return mismatch(0, "no audio field reports");
  }
  if (aCount != bCount) {
    return mismatch(std::min(aCount, bCount), std::format("report count differs (A={} B={})", aCount, bCount));
  }
  for (size_t i = 0; i < aCount; ++i) {
    const AudioFieldCompareResult result = compareOne(i, a[i], b[i]);
    if (!result.equal) {
      return result;
    }
  }
  return AudioFieldCompareResult{true, aCount, {}};
}
