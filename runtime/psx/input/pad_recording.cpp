#include "pad_recording.h"

#include "fs_util.h"

#include <lucent/content.h>
#include <lucent/log.h>

#include <algorithm>
#include <cstring>

namespace psx::input {


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
  return kNoButtons;
}

std::uint32_t PadSegment::inputFramesFrom(std::uint32_t offset) const {
  std::uint32_t pressed = 0;
  std::uint32_t start = 0;
  for (const PadRun &run : runs) {
    const std::uint32_t end = start + run.frames;
    if (run.mask != kNoButtons && end > offset) {
      pressed += end - std::max(start, offset);
    }
    start = end;
  }
  return pressed;
}

PadRecording::PadRecording(CardIdentity card) : card_(card) {}

PadAppend PadRecording::append(PadFrame frame) {
  PadAppend what = PadAppend::Extended;
  if (segments_.empty() || segments_.back().phase != frame.phase) {
    segments_.push_back(PadSegment{.phase = frame.phase, .runs = {}});
    what = PadAppend::NewSegment;
  }
  std::vector<PadRun> &runs = segments_.back().runs;
  if (runs.empty() || runs.back().mask != frame.mask) {
    runs.push_back(PadRun{.mask = frame.mask, .frames = 0});
    what = what == PadAppend::NewSegment ? what : PadAppend::NewRun;
  }
  runs.back().frames++;
  totalFrames_++;
  return what;
}

bool PadRecording::keyed() const {
  return std::ranges::any_of(segments_, [](const PadSegment &segment) {
    return segment.phase != kUnkeyedPhase;
  });
}

void PadRecording::putHeader(psx::platform::BlobWriter &out, const CardIdentity &card) {
  for (const char c : kMagic) {
    out.u8(static_cast<std::uint8_t>(c));
  }
  out.u32(kVersion);
  out.u32(static_cast<std::uint32_t>(card.kind));
  const bool image = card.kind == CardIdentity::Kind::Image;
  for (const std::uint8_t byte : card.sha256) {
    out.u8(image ? byte : 0u);
  }
}

void PadRecording::putPhase(psx::platform::BlobWriter &out, std::uint64_t phase) {
  out.u8(kPhaseTag);
  out.u64(phase);
}

void PadRecording::putRun(psx::platform::BlobWriter &out, PadRun run) {
  out.u8(kRunTag);
  out.u16(run.mask);
  out.u32(run.frames);
}

std::vector<std::uint8_t> PadRecording::encode(std::size_t frameLimit) const {
  psx::platform::BlobWriter out;
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
      putRun(out, PadRun{.mask = run.mask, .frames = frames});
      remaining -= frames;
    }
  }
  return out.take();
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
  // Every field read below has had its length checked first.
  const auto field = [&bytes](std::size_t at) {
    return psx::platform::BlobReader(bytes.subspan(at));
  };
  const auto version = field(8).u32();
  if (version != kVersion) {
    return refuse(lucent::format(
        "pad recording format version {} is not supported; this build reads version {}", version, kVersion));
  }
  const auto cardKind = field(12).u32();
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
      recording.segments_.push_back(PadSegment{.phase = field(at + 1).u64(), .runs = {}});
      at += kPhaseRecordBytes;
    } else if (tag == kRunTag) {
      if (bytes.size() - at < kRunRecordBytes) {
        return refuse(lucent::format("truncated run record at byte {}", at));
      }
      if (recording.segments_.empty()) {
        return refuse(lucent::format("run record at byte {} precedes any phase record", at));
      }
      const auto mask = field(at + 1).u16();
      const auto frames = field(at + kRunFramesOffset).u32();
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
  psx::platform::BlobWriter header;
  PadRecording::putHeader(header, card);
  if (!writer->write(header.bytesOut())) {
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

bool PadRecordingWriter::write(std::span<const std::uint8_t> bytes) {
  if (failed_) {
    return false;
  }
  file_.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  file_.flush();
  return file_ ? true : fail("write");
}

bool PadRecordingWriter::append(PadFrame frame, PadAppend what) {
  if (failed_) {
    return false;
  }
  psx::platform::BlobWriter bytes;
  if (what == PadAppend::Extended) {
    // Rewrite the open run's frame count in place, then return to the end of the file.
    runFrames_++;
    bytes.u32(runFrames_);
    if (!file_.seekp(runFramesPos_)) {
      return fail("seek");
    }
    const bool wrote = write(bytes.bytesOut());
    if (!file_.seekp(0, std::ios::end)) {
      return fail("seek");
    }
    return wrote;
  }
  if (what == PadAppend::NewSegment) {
    PadRecording::putPhase(bytes, frame.phase);
  }
  const std::streamoff end = file_.tellp();
  if (end < 0) {
    return fail("position query");
  }
  runFramesPos_ = end + static_cast<std::streamoff>(bytes.bytesOut().size() + PadRecording::kRunFramesOffset);
  PadRecording::putRun(bytes, PadRun{.mask = frame.mask, .frames = 1});
  runFrames_ = 1;
  return write(bytes.bytesOut());
}

} // namespace psx::input
