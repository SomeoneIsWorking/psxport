// The phase-keyed .pad recording: in-memory form, file format and streaming sink.
//
// FILE LAYOUT, version 1, all integers little-endian:
//
//   header   char magic[8] = "PSXPADPH"
//            u32 version   = 1
//            u32 card      = 0 unknown | 1 no card image | 2 image digest follows
//            u8  cardSha256[32]            (zero unless card == 2)
//   records  'P' u64 phase                 a new phase was entered; the runs that follow belong to it
//            'R' u16 mask  u32 frames      `frames` consecutive pad frames with one active-low mask
//
// Malformed records and the pre-v1 raw format are refused by name; `tools/psx_pad.py migrate` converts raw files.
#pragma once

#include "blob_codec.h"
#include "host_input.h"
#include "input_phase.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psx::input {

// A phase as a log reads it: hex, or "unkeyed" for kUnkeyedPhase.
std::string describePhase(std::uint64_t phase);

// Which memory-card image a recording started from. A route through a title's front end depends on
// what the card holds (Spyro asks to create a save file only when it finds none), so a replay against
// a different card is a replay of a different route even when every frame lands where it did.
struct CardIdentity {
  enum class Kind : std::uint8_t { Unknown = 0, Absent = 1, Image = 2 };
  Kind kind = Kind::Unknown;
  std::array<std::uint8_t, 32> sha256{};

  bool operator==(const CardIdentity &) const = default;
  std::string describe() const; // "unknown", "no card image", or "sha256 <hex>"
};

// A card image and its identity, read at one moment. `image` is empty when the identity is Unknown.
struct CardSnapshot {
  CardIdentity identity;
  std::vector<std::uint8_t> image;
};

struct PadRun {
  std::uint16_t mask = kNoButtons;
  std::uint32_t frames = 0;
};

// One serviced pad frame: the phase the game was in and the active-low mask it received.
struct PadFrame {
  std::uint64_t phase = kUnkeyedPhase;
  std::uint16_t mask = kNoButtons;
};

// Every frame recorded while the game stayed in one phase, as run-length masks from phase entry.
struct PadSegment {
  std::uint64_t phase = kUnkeyedPhase;
  std::vector<PadRun> runs;

  std::uint32_t frames() const;
  // The mask `offset` frames after this phase was entered. `offset` must be below frames().
  std::uint16_t maskAt(std::uint32_t offset) const;
  // How many of the frames at or after `offset` hold any button. Zero means the rest is neutral.
  std::uint32_t inputFramesFrom(std::uint32_t offset) const;
  // An unkeyed segment matches every phase.
  bool matches(std::uint64_t observedPhase) const {
    return phase == kUnkeyedPhase || phase == observedPhase;
  }
};

// What appending one frame did to the recording's segments and runs.
enum class PadAppend : std::uint8_t { Extended, NewRun, NewSegment };

struct DecodedPadRecording;

class PadRecording {
public:
  static constexpr std::array<char, 8> kMagic = {'P', 'S', 'X', 'P', 'A', 'D', 'P', 'H'};
  static constexpr std::uint32_t kVersion = 1;
  static constexpr std::size_t kHeaderBytes = 8 + 4 + 4 + 32;
  static constexpr std::uint8_t kPhaseTag = 'P';
  static constexpr std::uint8_t kRunTag = 'R';
  static constexpr std::size_t kPhaseRecordBytes = 1 + 8;
  static constexpr std::size_t kRunRecordBytes = 1 + 2 + 4;
  static constexpr std::size_t kRunFramesOffset = 1 + 2; // where a run record's frame count sits

  explicit PadRecording(CardIdentity card = {});

  // A phase different from the previous frame's opens a new segment; a new mask opens a run.
  PadAppend append(PadFrame frame);

  const CardIdentity &card() const {
    return card_;
  }
  const std::vector<PadSegment> &segments() const {
    return segments_;
  }
  std::size_t totalFrames() const {
    return totalFrames_;
  }
  // True when any segment is keyed on a real phase, i.e. replaying it needs a title phase source.
  bool keyed() const;

  // The file bytes for the first `frameLimit` frames (0 = all). Only a PREFIX is ever offered: a
  // recording's first phase is the one the game boots into, and a suffix would start elsewhere.
  std::vector<std::uint8_t> encode(std::size_t frameLimit = 0) const;

  static DecodedPadRecording decode(std::span<const std::uint8_t> bytes);

  // Record encoders, shared with the streaming writer.
  static void putHeader(psx::platform::BlobWriter &out, const CardIdentity &card);
  static void putPhase(psx::platform::BlobWriter &out, std::uint64_t phase);
  static void putRun(psx::platform::BlobWriter &out, PadRun run);

private:
  CardIdentity card_;
  std::vector<PadSegment> segments_;
  std::size_t totalFrames_ = 0;
};

// A decoded file, or the reason it was refused.
struct DecodedPadRecording {
  std::optional<PadRecording> recording;
  std::string error; // set exactly when `recording` is empty
};

// PSXPORT_PAD_RECORD's sink: each frame is flushed before the next, so a crash leaves a file decodable to its last frame.
class PadRecordingWriter {
public:
  // Null with `error` set when the file cannot be created or the header cannot be written.
  static std::unique_ptr<PadRecordingWriter> open(const char *path, const CardIdentity &card, std::string &error);
  ~PadRecordingWriter() = default;
  PadRecordingWriter(const PadRecordingWriter &) = delete;
  PadRecordingWriter &operator=(const PadRecordingWriter &) = delete;
  PadRecordingWriter(PadRecordingWriter &&) = delete;
  PadRecordingWriter &operator=(PadRecordingWriter &&) = delete;

  // Writes what PadRecording::append did with `frame`; false once a write has failed.
  bool append(PadFrame frame, PadAppend what);

private:
  PadRecordingWriter() = default;
  bool write(std::span<const std::uint8_t> bytes);
  bool fail(const char *what);

  std::ofstream file_;
  bool failed_ = false;
  std::uint32_t runFrames_ = 0;
  std::streamoff runFramesPos_ = -1; // file offset of the open run's frame count
};

} // namespace psx::input
