#include "xa_adpcm.h"

#include <cstddef>
#include <cstring>

namespace psx::audio {
namespace {

constexpr std::int32_t kFilterWeights[16][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
constexpr int kSoundGroups = 18;
constexpr int kSamplesPerUnit = 28;

struct Coding {
  bool stereo;
  bool eightBit;
  int units;

  explicit Coding(std::uint8_t coding)
      : stereo((coding & 0x01u) != 0), eightBit((coding & 0x10u) != 0), units(eightBit ? 4 : 8) {}
};

// `param` is the unit header: shift in the low nibble, filter in the high.
void decodeUnit(const std::uint8_t *in, std::int16_t *out, int param) {
  const int shift = param & 0x0F;
  const int filter = param >> 4;
  for (int i = 0; i < kSamplesPerUnit; i++) {
    std::int32_t s = static_cast<std::int16_t>(in[i] << 8);
    s >>= shift;
    s += (out[i - 1] * kFilterWeights[filter][0] + out[i - 2] * kFilterWeights[filter][1]) >> 6;
    if (s < -32768) {
      s = -32768;
    } else if (s > 32767) {
      s = 32767;
    }
    out[i] = static_cast<std::int16_t>(s);
  }
}

} // namespace

XaFormat xaSectorFormat(const std::uint8_t *raw) {
  const std::uint8_t coding = sectorSubheader(raw).coding;
  const Coding parsed(coding);
  return {(coding & 0x04u) != 0 ? kXaDefaultRateHz / 2 : kXaDefaultRateHz,
          kSoundGroups * parsed.units * kSamplesPerUnit / (parsed.stereo ? 2 : 1)};
}

XaFormat decodeXaSector(const std::uint8_t *raw, std::int16_t *out, std::int16_t hist[2][2]) {
  const Coding coding(sectorSubheader(raw).coding);
  const int nibbleShift = coding.eightBit ? 0 : 1;
  std::int16_t channel[2][kXaMaxSectorFrames];
  int written[2] = {0, 0};
  for (int group = 0; group < kSoundGroups; group++) {
    // 16 parameter bytes, then 112 sample bytes.
    const std::uint8_t *sg = raw + 24 + std::size_t{128} * group;
    for (int unit = 0; unit < coding.units; unit++) {
      const int param = sg[(unit & 3) | ((unit & 4) << 1)];
      const int paramCopy = sg[4 | (unit & 3) | ((unit & 4) << 1)];
      std::uint8_t in[kSamplesPerUnit];
      for (int i = 0; i < kSamplesPerUnit; i++) {
        std::uint8_t t = sg[16 + i * 4 + (unit >> nibbleShift)];
        if (nibbleShift != 0) {
          t = static_cast<std::uint8_t>((t << ((unit & 1) != 0 ? 0 : 4)) & 0xF0);
        }
        in[i] = t;
      }
      const int ch = ((unit & 1) != 0 && coding.stereo) ? 1 : 0;
      std::int16_t decoded[2 + kSamplesPerUnit];
      decoded[0] = hist[ch][0];
      decoded[1] = hist[ch][1];
      decodeUnit(in, decoded + 2, param);
      hist[ch][0] = decoded[kSamplesPerUnit];
      hist[ch][1] = decoded[kSamplesPerUnit + 1];
      // A unit whose two header copies disagree is corrupt and plays as silence (Beetle).
      if (param != paramCopy) {
        std::memset(decoded, 0, sizeof decoded);
      }
      for (int s = 0; s < kSamplesPerUnit; s++) {
        channel[ch][written[ch]++] = decoded[2 + s];
        if (!coding.stereo) {
          channel[1][written[1]++] = decoded[2 + s];
        }
      }
    }
  }
  const int frames = written[0];
  for (int i = 0; i < frames; i++) {
    out[std::size_t{2} * i] = channel[0][i];
    out[std::size_t{2} * i + 1] = channel[1][i];
  }
  return {xaSectorFormat(raw).rate, frames};
}

} // namespace psx::audio
