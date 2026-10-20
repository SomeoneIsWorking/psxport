#include "wav_writer.h"

#include "audio_queue_policy.h"

#include <lucent/log.h>

namespace psx::audio {
namespace {

constexpr long kRiffSizeOffset = 4;
constexpr long kDataSizeOffset = 40;
constexpr std::uint32_t kHeaderBytesAfterRiffSize = 36;

void putLe(std::FILE *file, std::uint32_t value, int bytes) {
  for (int i = 0; i < bytes; i++) {
    std::fputc(static_cast<int>((value >> (8 * i)) & 0xFFu), file);
  }
}

} // namespace

WavWriter::WavWriter(const std::string &path, std::uint32_t sampleRateHz)
    : file_(std::fopen(path.c_str(), "wb")), path_(path), sampleRateHz_(sampleRateHz) {
  if (file_ == nullptr) {
    lucent::error("wav", "cannot open {}", path);
    return;
  }
  constexpr auto kBytesPerFrame = static_cast<std::uint32_t>(AudioQueuePolicy::kBytesPerFrame);
  std::fwrite("RIFF", 1, 4, file_);
  putLe(file_, 0, 4);
  std::fwrite("WAVEfmt ", 1, 8, file_);
  putLe(file_, 16, 4);
  putLe(file_, 1, 2); // PCM
  putLe(file_, AudioQueuePolicy::kChannels, 2);
  putLe(file_, sampleRateHz_, 4);
  putLe(file_, sampleRateHz_ * kBytesPerFrame, 4);
  putLe(file_, kBytesPerFrame, 2);
  putLe(file_, AudioQueuePolicy::kBytesPerSample * 8, 2);
  std::fwrite("data", 1, 4, file_);
  putLe(file_, 0, 4);
}

WavWriter::~WavWriter() {
  if (file_ == nullptr) {
    return;
  }
  syncSizes();
  std::fclose(file_);
  lucent::info("wav",
               "{}: {} PCM bytes ({:.2f} s @ {} Hz)",
               path_,
               dataBytes_,
               dataBytes_ / (static_cast<double>(sampleRateHz_) * AudioQueuePolicy::kBytesPerFrame),
               sampleRateHz_);
}

void WavWriter::append(const std::int16_t *stereo, std::uint32_t frames) {
  if (file_ == nullptr) {
    return;
  }
  const std::size_t bytes = std::size_t{frames} * AudioQueuePolicy::kBytesPerFrame;
  std::fwrite(stereo, 1, bytes, file_);
  dataBytes_ += static_cast<std::uint32_t>(bytes);
}

void WavWriter::syncSizes() {
  if (file_ == nullptr) {
    return;
  }
  const long end = std::ftell(file_);
  std::fseek(file_, kRiffSizeOffset, SEEK_SET);
  putLe(file_, kHeaderBytesAfterRiffSize + dataBytes_, 4);
  std::fseek(file_, kDataSizeOffset, SEEK_SET);
  putLe(file_, dataBytes_, 4);
  std::fseek(file_, end, SEEK_SET);
  std::fflush(file_);
}

} // namespace psx::audio
