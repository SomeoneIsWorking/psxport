#include "wav_writer.h"

#include "audio_queue_policy.h"
#include "blob_codec.h"

#include <lucent/log.h>

#include <string_view>

namespace psx::audio {
namespace {

constexpr long kRiffSizeOffset = 4;
constexpr long kDataSizeOffset = 40;
constexpr std::uint32_t kHeaderBytesAfterRiffSize = 36;

void fourcc(psx::platform::BlobWriter &out, std::string_view tag) {
  out.bytes({reinterpret_cast<const std::uint8_t *>(tag.data()), tag.size()});
}

void put(std::FILE *file, const psx::platform::BlobWriter &blob) {
  std::fwrite(blob.bytesOut().data(), 1, blob.bytesOut().size(), file);
}

void putU32(std::FILE *file, std::uint32_t value) {
  psx::platform::BlobWriter blob;
  blob.u32(value);
  put(file, blob);
}

} // namespace

WavWriter::WavWriter(const std::string &path, std::uint32_t sampleRateHz)
    : file_(std::fopen(path.c_str(), "wb")), path_(path), sampleRateHz_(sampleRateHz) {
  if (file_ == nullptr) {
    lucent::error("wav", "cannot open {}", path);
    return;
  }
  constexpr auto kBytesPerFrame = static_cast<std::uint32_t>(AudioQueuePolicy::kBytesPerFrame);
  psx::platform::BlobWriter header;
  fourcc(header, "RIFF");
  header.u32(0);
  fourcc(header, "WAVEfmt ");
  header.u32(16);
  header.u16(1); // PCM
  header.u16(AudioQueuePolicy::kChannels);
  header.u32(sampleRateHz_);
  header.u32(sampleRateHz_ * kBytesPerFrame);
  header.u16(kBytesPerFrame);
  header.u16(AudioQueuePolicy::kBytesPerSample * 8);
  fourcc(header, "data");
  header.u32(0);
  put(file_, header);
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
  putU32(file_, kHeaderBytesAfterRiffSize + dataBytes_);
  std::fseek(file_, kDataSizeOffset, SEEK_SET);
  putU32(file_, dataBytes_);
  std::fseek(file_, end, SEEK_SET);
  std::fflush(file_);
}

} // namespace psx::audio
