#include "movie_clock.h"

#include <cmath>

namespace psx::movie {

void MovieClock::learn(const SectorFacts &sector) {
  const std::uint32_t at = next_++;
  if (sector.audio.rate > 0) {
    if (!firstAudio_) {
      firstAudio_ = at;
      audio_ = sector.audio;
    } else if (!sectorMs_) {
      const auto stride = static_cast<double>(at - *firstAudio_);
      sectorMs_ = 1000.0 * audio_.frames / (static_cast<double>(audio_.rate) * stride);
    }
  }
  const bool video = sector.videoFrame != 0;
  if (video && previousFrame_ != 0 && sector.videoFrame == previousFrame_ + 1 && slotSectors_ == 0) {
    if (endedFrame_ == 0) {
      endedFrame_ = previousFrame_;
      endedAt_ = at;
    } else {
      const std::int64_t sectors = static_cast<std::int64_t>(at) - endedAt_;
      const std::int64_t frames = previousFrame_ - endedFrame_;
      if (frames > 0 && sectors > 0 && sectors % frames == 0) {
        slotSectors_ = sectors / frames;
        slotOrigin_ = static_cast<std::int64_t>(endedAt_) - endedFrame_ * slotSectors_;
      }
    }
  }
  previousFrame_ = sector.videoFrame;
}

double MovieClock::sectorMs() const {
  return sectorMs_.value_or(1000.0 / kSilentSectorsPerSecond);
}

double MovieClock::sectorDeliveredMs(std::uint32_t sector) const {
  return (static_cast<double>(sector) + 1.0) * sectorMs();
}

double MovieClock::frameDueMs(ReadFrame frame) const {
  if (slotSectors_ == 0) {
    return sectorDeliveredMs(frame.lastChunk);
  }
  return static_cast<double>(slotOrigin_ + frame.number * slotSectors_) * sectorMs();
}

MovieFieldGrid::Show MovieFieldGrid::show(double dueMs) {
  // The epsilon keeps a due time that lands exactly on a field boundary on that field.
  auto field = static_cast<std::int64_t>(std::ceil(dueMs / fieldMs_ - 1e-9));
  if (field <= shownField_) {
    field = shownField_ + 1;
  }
  const auto held = static_cast<int>(field - shownField_);
  shownField_ = field;
  return {static_cast<double>(field) * fieldMs_, held};
}

} // namespace psx::movie
