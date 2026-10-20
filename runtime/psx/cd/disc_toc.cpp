#include "disc_toc.h"

#include "cd_position.h"
#include "disc.h"

namespace psx::disc_toc {
namespace {

bool valid_toc(const cd::Disc &disc) {
  if (disc.track_count == 0 || disc.track_count > psx::cd::kDiscMaxTracks) {
    return false;
  }
  int64_t previous_end = -1;
  uint8_t previous_number = 0;
  for (uint8_t index = 0; index < disc.track_count; ++index) {
    const psx::cd::DiscTrackInfo &track = disc.tracks[index];
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
  if (lba + cd::kLeadInSectors >= 100LL * cd::kSecondsPerMinute * cd::kSectorsPerSecond) {
    return false;
  }
  const std::array<uint8_t, 3> msf = cd::lbaToMsf(static_cast<int>(lba));
  out.first = msf[0];
  out.second = msf[1];
  return true;
}

bool loaded(cd::Disc &disc) {
  if (disc.track_count == 0 && !disc.open()) {
    return false;
  }
  return valid_toc(disc);
}

} // namespace

bool track_range(cd::Disc &disc, BcdPair &out) {
  if (!loaded(disc)) {
    return false;
  }
  out.first = cd::toBcd(disc.tracks[0].number);
  out.second = cd::toBcd(disc.tracks[disc.track_count - 1].number);
  return true;
}

bool track_start(cd::Disc &disc, uint8_t track_bcd, BcdPair &out) {
  if (!loaded(disc) || !cd::isBcd(track_bcd)) {
    return false;
  }
  const int track_number = cd::fromBcd(track_bcd);
  int64_t lba = -1;
  if (track_number == 0) {
    const psx::cd::DiscTrackInfo &last = disc.tracks[disc.track_count - 1];
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
