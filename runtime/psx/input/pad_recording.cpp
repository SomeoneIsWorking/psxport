// pad_recording.cpp — encode, decode and stream the phase-keyed .pad format (see pad_recording.h).

#include "pad_recording.h"

#include "fs_util.h"

#include <lucent/content.h>
#include <lucent/log.h>

#include <algorithm>
#include <cstring>

namespace psx::input {

namespace {

void putU16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void putU32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void putU64(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

// Little-endian reads over a bounds-checked cursor. Every read is preceded by a length check in the
// decoder, so these never run past the span.
std::uint64_t getLe(std::span<const std::uint8_t> bytes, std::size_t at, std::size_t width) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; i++) {
    value |= static_cast<std::uint64_t>(bytes[at + i]) << (8 * i);
  }
  return value;
}

} // namespace

std::string describePhase(std::uint64_t phase) {
  return phase == kUnkeyedPhase ? std::string("unkeyed") : lucent::format("{:#x}", phase);
}

std::string CardIdentity::describe() const {
  switch (kind) {
  case Kind::Unknown:
    return "unknown";
  case Kind::Absent:
    return "no card image";
  case Kind::Image:
    return "sha256 " + lucent::content::sha256_hex(sha256);
  }
  return "invalid";
}

std::uint32_t PadSegment::frames() const {
  std::uint32_t total = 0;
  for (const PadRun &run : runs) {
    total += run.frames;
  }
  return total;
}

std::uint16_t PadSegment::maskAt(std::uint32_t offset) const {
  for (const PadRun &run : runs) {
    if (offset < run.frames) {
      return run.mask;
    }
    offset -= run.frames;
  }
  return kNeutralMask;
}

std::uint32_t PadSegment::inputFramesFrom(std::uint32_t offset) const {
  std::uint32_t pressed = 0;
  std::uint32_t start = 0;
  for (const PadRun &run : runs) {
    const std::uint32_t end = start + run.frames;
    if (run.mask != kNeutralMask && end > offset) {
      pressed += end - std::max(start, offset);
    }
    start = end;
  }
  return pressed;
}

PadRecording::PadRecording(CardIdentity card) : card_(card) {}

void PadRecording::append(std::uint64_t phase, std::uint16_t mask) {
  if (segments_.empty() || segments_.back().phase != phase) {
    segments_.push_back(PadSegment{.phase = phase, .runs = {}});
  }
  std::vector<PadRun> &runs = segments_.back().runs;
  if (runs.empty() || runs.back().mask != mask) {
    runs.push_back(PadRun{.mask = mask, .frames = 0});
  }
  runs.back().frames++;
  totalFrames_++;
}

bool PadRecording::keyed() const {
  return std::ranges::any_of(segments_, [](const PadSegment &segment) {
    return segment.phase != kUnkeyedPhase;
  });
}

void PadRecording::putHeader(std::vector<std::uint8_t> &out, const CardIdentity &card) {
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  putU32(out, kVersion);
  putU32(out, static_cast<std::uint32_t>(card.kind));
  const bool image = card.kind == CardIdentity::Kind::Image;
  for (const std::uint8_t byte : card.sha256) {
    out.push_back(image ? byte : 0u);
  }
}

void PadRecording::putPhase(std::vector<std::uint8_t> &out, std::uint64_t phase) {
  out.push_back(kPhaseTag);
  putU64(out, phase);
}

void PadRecording::putRun(std::vector<std::uint8_t> &out, std::uint16_t mask, std::uint32_t frames) {
  out.push_back(kRunTag);
  putU16(out, mask);
  putU32(out, frames);
}

std::vector<std::uint8_t> PadRecording::encode(std::size_t frameLimit) const {
  std::vector<std::uint8_t> out;
  putHeader(out, card_);
  std::size_t remaining = frameLimit ? frameLimit : totalFrames_;
  for (const PadSegment &segment : segments_) {
    if (!remaining) {
      break;
    }
    putPhase(out, segment.phase);
    for (const PadRun &run : segment.runs) {
      if (!remaining) {
        break;
      }
      const auto frames = static_cast<std::uint32_t>(std::min<std::size_t>(run.frames, remaining));
      putRun(out, run.mask, frames);
      remaining -= frames;
    }
  }
  return out;
}

DecodedPadRecording PadRecording::decode(std::span<const std::uint8_t> bytes) {
  const auto refuse = [](std::string why) {
    return DecodedPadRecording{.recording = std::nullopt, .error = std::move(why)};
  };
  if (bytes.size() < kMagic.size() || std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
    return refuse("not a phase-keyed pad recording: the file does not start with the PSXPADPH magic. "
                  "A pre-v1 raw recording (one uint16 per frame from boot) carries no phase keys and no "
                  "card identity, so it is refused rather than replayed blind; convert it with "
                  "`tools/psx_pad.py migrate <raw> <out>` (one explicitly unkeyed, absolute segment) "
                  "or re-record it");
  }
  if (bytes.size() < kHeaderBytes) {
    return refuse(lucent::format("truncated header: {} of {} bytes", bytes.size(), kHeaderBytes));
  }
  const auto version = static_cast<std::uint32_t>(getLe(bytes, 8, 4));
  if (version != kVersion) {
    return refuse(lucent::format(
        "pad recording format version {} is not supported; this build reads version {}", version, kVersion));
  }
  const auto cardKind = static_cast<std::uint32_t>(getLe(bytes, 12, 4));
  if (cardKind > static_cast<std::uint32_t>(CardIdentity::Kind::Image)) {
    return refuse(lucent::format("unknown card identity kind {} in the header", cardKind));
  }
  CardIdentity card{.kind = static_cast<CardIdentity::Kind>(cardKind), .sha256 = {}};
  std::copy_n(bytes.begin() + 16, card.sha256.size(), card.sha256.begin());

  PadRecording recording(card);
  std::size_t at = kHeaderBytes;
  while (at < bytes.size()) {
    const std::uint8_t tag = bytes[at];
    if (tag == kPhaseTag) {
      if (bytes.size() - at < kPhaseRecordBytes) {
        return refuse(lucent::format("truncated phase record at byte {}", at));
      }
      if (!recording.segments_.empty() && recording.segments_.back().runs.empty()) {
        return refuse(lucent::format("phase record at byte {} follows a phase that holds no frames", at));
      }
      recording.segments_.push_back(PadSegment{.phase = getLe(bytes, at + 1, 8), .runs = {}});
      at += kPhaseRecordBytes;
    } else if (tag == kRunTag) {
      if (bytes.size() - at < kRunRecordBytes) {
        return refuse(lucent::format("truncated run record at byte {}", at));
      }
      if (recording.segments_.empty()) {
        return refuse(lucent::format("run record at byte {} precedes any phase record", at));
      }
      const auto mask = static_cast<std::uint16_t>(getLe(bytes, at + 1, 2));
      const auto frames = static_cast<std::uint32_t>(getLe(bytes, at + kRunFramesOffset, 4));
      if (!frames) {
        return refuse(lucent::format("zero-length run record at byte {}", at));
      }
      recording.segments_.back().runs.push_back(PadRun{.mask = mask, .frames = frames});
      recording.totalFrames_ += frames;
      at += kRunRecordBytes;
    } else {
      return refuse(lucent::format("unknown record tag {:#04x} at byte {}", tag, at));
    }
  }
  if (recording.segments_.empty() || recording.segments_.back().runs.empty()) {
    return refuse("the recording holds no frames (or ends on a phase with none)");
  }
  return DecodedPadRecording{.recording = std::move(recording), .error = {}};
}

std::unique_ptr<PadRecordingWriter>
PadRecordingWriter::open(const char *path, const CardIdentity &card, std::string &error) {
  if (!Fs::ensureParentDirs(path)) {
    error = lucent::format("cannot create the directory for {}", path);
    return nullptr;
  }
  std::unique_ptr<PadRecordingWriter> writer(new PadRecordingWriter());
  writer->file_.open(path, std::ios::binary | std::ios::trunc);
  if (!writer->file_) {
    error = lucent::format("cannot create {}", path);
    return nullptr;
  }
  std::vector<std::uint8_t> header;
  PadRecording::putHeader(header, card);
  if (!writer->write(header)) {
    error = lucent::format("cannot write the header of {}", path);
    return nullptr;
  }
  return writer;
}

bool PadRecordingWriter::fail(const char *what) {
  failed_ = true;
  lucent::error("padrec", "recording sink {} FAILED; frames after this point are not in the file", what);
  return false;
}

bool PadRecordingWriter::write(const std::vector<std::uint8_t> &bytes) {
  if (failed_) {
    return false;
  }
  file_.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  file_.flush();
  return file_ ? true : fail("write");
}

bool PadRecordingWriter::append(std::uint64_t phase, std::uint16_t mask) {
  if (failed_) {
    return false;
  }
  std::vector<std::uint8_t> bytes;
  if (!havePhase_ || phase != phase_) {
    PadRecording::putPhase(bytes, phase);
    havePhase_ = true;
    phase_ = phase;
    haveRun_ = false;
  }
  if (haveRun_ && mask == runMask_) {
    // Extend the open run in place: rewrite its frame count, then return to the end of the file.
    runFrames_++;
    std::vector<std::uint8_t> count;
    for (int shift = 0; shift < 32; shift += 8) {
      count.push_back(static_cast<std::uint8_t>((runFrames_ >> shift) & 0xFFu));
    }
    if (!file_.seekp(runFramesPos_)) {
      return fail("seek");
    }
    const bool wrote = write(count);
    if (!file_.seekp(0, std::ios::end)) {
      return fail("seek");
    }
    return wrote;
  }
  const std::streamoff end = file_.tellp();
  if (end < 0) {
    return fail("position query");
  }
  runFramesPos_ = end + static_cast<std::streamoff>(bytes.size() + PadRecording::kRunFramesOffset);
  PadRecording::putRun(bytes, mask, 1);
  haveRun_ = true;
  runMask_ = mask;
  runFrames_ = 1;
  return write(bytes);
}

} // namespace psx::input
