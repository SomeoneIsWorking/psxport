// psx::audio::WavWriter: one 16-bit stereo PCM WAV file, its RIFF sizes patched by syncSizes() and on destruction.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace psx::audio {

class WavWriter {
public:
  WavWriter(const std::string &path, std::uint32_t sampleRateHz);
  ~WavWriter();
  WavWriter(const WavWriter &) = delete;
  WavWriter &operator=(const WavWriter &) = delete;
  WavWriter(WavWriter &&) = delete;
  WavWriter &operator=(WavWriter &&) = delete;

  bool isOpen() const {
    return file_ != nullptr;
  }
  void append(const std::int16_t *stereo, std::uint32_t frames);
  // Patch the sizes and flush, so a killed process still leaves a valid file.
  void syncSizes();
  std::uint32_t dataBytes() const {
    return dataBytes_;
  }

private:
  std::FILE *file_;
  std::string path_;
  std::uint32_t sampleRateHz_;
  std::uint32_t dataBytes_ = 0;
};

} // namespace psx::audio
