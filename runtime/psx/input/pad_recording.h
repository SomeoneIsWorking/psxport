// pad_recording.h — the phase-keyed .pad recording: its in-memory form, its file format, and the
// streaming sink that writes it while a session is being played.
//
// WHY THE FORMAT CHANGED. The original .pad was one uint16 mask per pad-service frame from boot.
// Replaying it assumed every boot, load and menu took exactly as many frames as when it was
// recorded, and assumed the memory card held what it held then. Spyro's only gameplay route
// (spyro issue 0116) broke on the second assumption: recorded against a card that held a save, it
// was replayed against one that did not, the menu took a different branch, and the recorded presses
// answered a dialog they were never meant for, silently. This format stores both facts the old one
// could not: which PHASE each frame belongs to (input_phase.h), and the identity of the card image
// the recording started from.
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
// A run before the first phase record, a phase record with no runs, a zero-length run, a truncated
// record, an unknown tag, an unknown version and a file without the magic are all REFUSED by name.
// In particular the pre-v1 raw format is refused, never read as absolute input: it carries neither
// phase nor card identity, which is the defect this format exists to end. `tools/psx_pad.py migrate`
// converts one into a v1 file with a single explicitly UNKEYED segment and an UNKNOWN card, which
// preserves its from-boot meaning and says so in the header.
#pragma once

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

inline constexpr std::uint16_t kNeutralMask = 0xFFFFu; // active-low: no button held

// A phase as a log reads it: hex, or "unkeyed" for kUnkeyedPhase.
std::string describePhase(std::uint64_t phase);

// Which memory-card image a recording started from. A route through a title's front end depends on
// what the card holds (Spyro asks to create a save file only when it finds none), so a replay against
// a different card is a replay of a different route even when every frame lands where it did.
struct CardIdentity {
  enum class Kind : std::uint32_t { Unknown = 0, Absent = 1, Image = 2 };
  Kind kind = Kind::Unknown;
  std::array<std::uint8_t, 32> sha256{};

  bool operator==(const CardIdentity &) const = default;
  std::string describe() const; // "unknown", "no card image", or "sha256 <hex>"
};

struct PadRun {
  std::uint16_t mask = kNeutralMask;
  std::uint32_t frames = 0;
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
};

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

  // One pad frame. A phase different from the previous frame's opens a new segment.
  void append(std::uint64_t phase, std::uint16_t mask);

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

  // Record encoders, shared with the streaming writer so there is one spelling of each record.
  static void putHeader(std::vector<std::uint8_t> &out, const CardIdentity &card);
  static void putPhase(std::vector<std::uint8_t> &out, std::uint64_t phase);
  static void putRun(std::vector<std::uint8_t> &out, std::uint16_t mask, std::uint32_t frames);

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

// The live sink behind PSXPORT_PAD_RECORD. Every frame reaches the file before the next one is
// serviced: a new run is appended, or the current run's frame count is rewritten in place, and the
// stream is flushed. A session that crashes therefore leaves a file that decodes up to its last frame,
// which is the frame a crash repro most needs.
class PadRecordingWriter {
public:
  // Null with `error` set when the file cannot be created or the header cannot be written.
  static std::unique_ptr<PadRecordingWriter> open(const char *path, const CardIdentity &card, std::string &error);
  ~PadRecordingWriter() = default;
  PadRecordingWriter(const PadRecordingWriter &) = delete;
  PadRecordingWriter &operator=(const PadRecordingWriter &) = delete;
  PadRecordingWriter(PadRecordingWriter &&) = delete;
  PadRecordingWriter &operator=(PadRecordingWriter &&) = delete;

  // False once a write has failed; the failure is logged once and later frames are not written.
  bool append(std::uint64_t phase, std::uint16_t mask);

private:
  PadRecordingWriter() = default;
  bool write(const std::vector<std::uint8_t> &bytes);
  bool fail(const char *what);

  std::ofstream file_;
  bool failed_ = false;
  bool havePhase_ = false;
  std::uint64_t phase_ = kUnkeyedPhase;
  bool haveRun_ = false;
  std::uint16_t runMask_ = kNeutralMask;
  std::uint32_t runFrames_ = 0;
  std::streamoff runFramesPos_ = -1; // file offset of the open run's frame count
};

} // namespace psx::input
