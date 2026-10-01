#include "disc_toc.h"

#include "disc.h"

namespace psx::disc_toc {
namespace {

constexpr uint32_t kLeadInFrames = 150;
constexpr uint32_t kFramesPerSecond = 75;
constexpr uint32_t kSecondsPerMinute = 60;

uint8_t to_bcd(uint32_t value) {
  return static_cast<uint8_t>(((value / 10u) << 4u) | (value % 10u));
}

bool from_bcd(uint8_t value, uint8_t &decoded) {
  if ((value & 0x0Fu) > 9 || (value >> 4u) > 9) {
    return false;
  }
  decoded = static_cast<uint8_t>((value >> 4u) * 10u + (value & 0x0Fu));
  return true;
}

bool valid_toc(const DiscState &disc) {
  if (disc.track_count == 0 || disc.track_count > DISC_MAX_TRACKS) {
    return false;
  }
  int64_t previous_end = -1;
  uint8_t previous_number = 0;
  for (uint8_t index = 0; index < disc.track_count; ++index) {
    const DiscTrackInfo &track = disc.tracks[index];
    const int64_t end = static_cast<int64_t>(track.lba) + track.sectors + track.postgap;
    if (track.number == 0 || track.number > 99 || track.number != previous_number + 1 || track.lba < 0 ||
        track.sectors == 0 || track.postgap < 0 || track.lba < previous_end || end > INT32_MAX) {
      return false;
    }
    previous_number = track.number;
    previous_end = end;
  }
  return true;
}

bool lba_to_msf(int64_t lba, BcdPair &out) {
  const int64_t frames = lba + kLeadInFrames;
  if (frames < 0 || frames >= 100LL * kSecondsPerMinute * kFramesPerSecond) {
    return false;
  }
  const uint32_t total_seconds = static_cast<uint32_t>(frames / kFramesPerSecond);
  out.first = to_bcd(total_seconds / kSecondsPerMinute);
  out.second = to_bcd(total_seconds % kSecondsPerMinute);
  return true;
}

bool loaded(DiscState &disc) {
  if (disc.track_count == 0 && !disc_open(&disc)) {
    return false;
  }
  return valid_toc(disc);
}

} // namespace

bool track_range(DiscState &disc, BcdPair &out) {
  if (!loaded(disc)) {
    return false;
  }
  out.first = to_bcd(disc.tracks[0].number);
  out.second = to_bcd(disc.tracks[disc.track_count - 1].number);
  return true;
}

bool track_start(DiscState &disc, uint8_t track_bcd, BcdPair &out) {
  uint8_t track_number = 0;
  if (!loaded(disc) || !from_bcd(track_bcd, track_number)) {
    return false;
  }
  int64_t lba = -1;
  if (track_number == 0) {
    const DiscTrackInfo &last = disc.tracks[disc.track_count - 1];
    lba = static_cast<int64_t>(last.lba) + last.sectors + last.postgap;
  } else {
    for (uint8_t index = 0; index < disc.track_count; ++index) {
      if (disc.tracks[index].number == track_number) {
        lba = disc.tracks[index].lba;
        break;
      }
    }
  }
  return lba >= 0 && lba_to_msf(lba, out);
}

} // namespace psx::disc_toc
