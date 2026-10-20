// CD-XA sector subheaders and the ADPCM sector decode (Beetle cdc.c DecodeXAADPCM).
#pragma once

#include <cstdint>

namespace psx::audio {

inline constexpr int kXaDefaultRateHz = 37800;
// 4-bit mono: 18 sound groups of 8 units of 28 samples, all on one channel.
inline constexpr int kXaMaxSectorFrames = 4032;

// The Mode2 subheader of a raw 2352-byte sector (sync 12, header 4, subheader 8).
struct SectorSubheader {
  std::uint8_t file = 0;
  std::uint8_t channel = 0;
  std::uint8_t submode = 0;
  std::uint8_t coding = 0;

  bool audio() const {
    return (submode & 0x04u) != 0;
  }
  bool endOfFile() const {
    return (submode & 0x80u) != 0;
  }
};

constexpr bool isMode2(const std::uint8_t *raw) {
  return raw[15] == 2;
}

constexpr SectorSubheader sectorSubheader(const std::uint8_t *raw) {
  return {raw[16], raw[17], raw[18], raw[19]};
}

// The rate and PCM frame count an audio sector's coding byte selects.
struct XaFormat {
  int rate = 0;
  int frames = 0;
};
XaFormat xaSectorFormat(const std::uint8_t *raw);

// One raw XA audio sector to interleaved S16 stereo (mono duplicated); `hist` persists across a stream's sectors.
XaFormat decodeXaSector(const std::uint8_t *raw, std::int16_t *out, std::int16_t hist[2][2]);

} // namespace psx::audio
