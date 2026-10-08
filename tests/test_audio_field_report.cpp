// test_audio_field_report.cpp — the SBS audio compare must reject missing and changed fields.
#include "audio_field_report.h"
#include "testutil.h"

#include <vector>

namespace {

AudioFieldReport report(uint64_t ordinal) {
  AudioFieldReport value;
  value.ordinal = ordinal;
  value.expectedSamples = 735;
  value.expectedClocks = 564480;
  value.renderedSamples = 735;
  value.queuedSamples = 735;
  value.pcmHash = 0x12345678;
  value.pcmNonzero = 18;
  value.pcmPeak = 240;
  value.xaWr = 4;
  value.xaRd = 1.5;
  value.xaPulls = 2;
  value.xaSectors = 3;
  value.deltaWr = 1;
  value.deltaRd = 0.5;
  value.deltaPulls = 1;
  value.deltaSectors = 1;
  value.pal = false;
  value.pcmSamples = {0, -1, 17, 0};
  return value;
}

void test_empty_trace_is_not_a_pass() {
  const AudioFieldCompareResult result = compareAudioFieldReports(nullptr, 0, nullptr, 0);
  CHECK(!result.equal);
  CHECK(result.reason.find("no audio field reports") != std::string::npos);
}

void test_identical_trace_passes() {
  const std::vector<AudioFieldReport> a = {report(1), report(2)};
  const std::vector<AudioFieldReport> b = {report(1), report(2)};
  const AudioFieldCompareResult result = compareAudioFieldReports(a.data(), a.size(), b.data(), b.size());
  CHECK(result.equal);
  CHECK_EQ(result.index, a.size());
}

void test_pcm_mismatch_is_named() {
  const std::vector<AudioFieldReport> a = {report(1)};
  std::vector<AudioFieldReport> b = a;
  b[0].pcmSamples[2] = 18;
  const AudioFieldCompareResult result = compareAudioFieldReports(a.data(), a.size(), b.data(), b.size());
  CHECK(!result.equal);
  CHECK_EQ(result.index, 0);
  CHECK(result.reason.find("PCM sample 2") != std::string::npos);
}

void test_count_mismatch_is_named() {
  const std::vector<AudioFieldReport> a = {report(1), report(2)};
  const std::vector<AudioFieldReport> b = {report(1)};
  const AudioFieldCompareResult result = compareAudioFieldReports(a.data(), a.size(), b.data(), b.size());
  CHECK(!result.equal);
  CHECK_EQ(result.index, 1);
  CHECK(result.reason.find("report count") != std::string::npos);
}

} // namespace

int main() {
  RUN(empty_trace_is_not_a_pass);
  RUN(identical_trace_passes);
  RUN(pcm_mismatch_is_named);
  RUN(count_mismatch_is_named);
  return pt_summary();
}
