// audio_field_report.h — one deterministic SPU field result and its oracle comparison.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AudioFieldReport {
  uint64_t ordinal = 0;
  uint32_t expectedSamples = 0;
  uint32_t expectedClocks = 0;
  uint32_t renderedSamples = 0;
  uint32_t queuedSamples = 0;
  uint32_t pcmHash = 0;
  uint32_t pcmNonzero = 0;
  uint32_t pcmPeak = 0;
  uint32_t xaWr = 0;
  double xaRd = 0.0;
  uint32_t xaPulls = 0;
  uint32_t xaSectors = 0;
  uint32_t deltaWr = 0;
  double deltaRd = 0.0;
  uint32_t deltaPulls = 0;
  uint32_t deltaSectors = 0;
  bool pal = false;
  std::vector<int16_t> pcmSamples;
};

struct AudioFieldCompareResult {
  bool equal = false;
  size_t index = 0;
  std::string reason;
};

AudioFieldCompareResult
compareAudioFieldReports(const AudioFieldReport *a, size_t aCount, const AudioFieldReport *b, size_t bCount);
