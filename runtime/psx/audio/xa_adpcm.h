// The CD-XA ADPCM sector decode (Beetle cdc.c DecodeXAADPCM).
#pragma once

#include <cstdint>

namespace psx::audio {

inline constexpr int kXaDefaultRateHz = 37800;
// 4-bit mono: 18 sound groups of 8 units of 28 samples, all on one channel.
inline constexpr int kXaMaxSectorFrames = 4032;

// The rate and PCM frame count an audio sector's coding byte selects.
struct XaFormat {
  int rate = 0;
  int frames = 0;
};
XaFormat xaSectorFormat(const std::uint8_t *raw);

// One raw XA audio sector to interleaved S16 stereo (mono duplicated); `hist` persists across a stream's sectors.
XaFormat decodeXaSector(const std::uint8_t *raw, std::int16_t *out, std::int16_t hist[2][2]);

} // namespace psx::audio
