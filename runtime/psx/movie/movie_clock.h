// psx::movie::MovieClock: when each sector and frame of an STR movie is due.
//
// An STR stream is read at a constant sector rate. Its XA audio sectors recur at a fixed stride, each
// holding the audio for that many sectors, so the sector period is one XA sector's audio duration over
// the stride: the audio clock. Each video frame owns a fixed slot of sectors and is due when its slot
// has been read. Both are learned from the stream's leading sectors.
#pragma once

#include "cd_drive_timing.h"
#include "xa_adpcm.h"

#include <cstdint>
#include <optional>

namespace psx::movie {

// What the clock reads from one STR sector.
struct SectorFacts {
  psx::audio::XaFormat audio; // rate 0 unless an XA audio sector
  int videoFrame = 0; // STR frame number of a video chunk, 0 otherwise
};

class MovieClock {
public:
  // A movie without audio streams at the double-speed drive rate.
  static constexpr double kSilentSectorsPerSecond = psx::cd::driveSectorsPerSecond(psx::cd::kModeDoubleSpeed);
  // Sectors read to learn the layout; past this a silent movie keeps the drive rate.
  static constexpr std::uint32_t kLearnSectors = 512;

  // Describe sectors 0, 1, 2, ... of the stream, in order, until learned().
  void learn(const SectorFacts &sector);
  bool learned() const {
    return sectorMs_.has_value() && slotSectors_ != 0;
  }

  double sectorMs() const;
  // When the drive has delivered `sector`.
  double sectorDeliveredMs(std::uint32_t sector) const;
  // A frame whose chunks have all been read.
  struct ReadFrame {
    int number = 0;
    std::uint32_t lastChunk = 0; // its last video sector, the due point when the slot is unknown
  };
  // When `frame` is due: its slot has been read.
  double frameDueMs(ReadFrame frame) const;
  // Sectors per frame slot; 0 when it could not be learned.
  std::int64_t slotSectors() const {
    return slotSectors_;
  }
  // The first XA audio sector, whose delivery starts the audio clock.
  std::optional<std::uint32_t> firstAudioSector() const {
    return firstAudio_;
  }

private:
  std::uint32_t next_ = 0;
  // Sector period, from the first two XA sectors.
  std::optional<std::uint32_t> firstAudio_;
  psx::audio::XaFormat audio_;
  std::optional<double> sectorMs_;
  // Frame slot, from two frames whose slot end is exact: the next frame's first chunk follows directly.
  int previousFrame_ = 0; // frame of the previous sector, 0 when it was not a video chunk
  int endedFrame_ = 0;
  std::uint32_t endedAt_ = 0;
  std::int64_t slotOrigin_ = 0;
  std::int64_t slotSectors_ = 0;
};

// One movie picture per display field, held for whole fields: each frame shows from the first field
// at or after it is due, counted from the movie's start.
class MovieFieldGrid {
public:
  explicit MovieFieldGrid(double fieldMs) : fieldMs_(fieldMs) {}

  struct Show {
    double atMs = 0.0;  // when the picture is presented
    int heldFields = 0; // the fields the previous picture was held for
  };
  Show show(double dueMs);

private:
  double fieldMs_;
  std::int64_t shownField_ = 0;
};

} // namespace psx::movie
