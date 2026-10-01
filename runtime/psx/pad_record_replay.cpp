// pad_record_replay.cpp — class PadRecordReplay (see pad_record_replay.h).

#include "pad_record_replay.h"

#include "fs_util.h"

#include <lucent/log.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace psx::input {

namespace {

// The default sink keeps the previous five sessions (pad_session.1..5.pad): a windowed bug-repro run
// used to be destroyed by the next launch, which is how the bucket-cutscene repro was lost (#57).
void rotateDefaultSink() {
  std::error_code ignored;
  for (int k = 4; k >= 1; k--) {
    std::filesystem::rename(lucent::format("scratch/bin/pad_session.{}.pad", k),
                            lucent::format("scratch/bin/pad_session.{}.pad", k + 1),
                            ignored);
  }
  std::filesystem::rename(PadRecordReplay::kDefaultSink, "scratch/bin/pad_session.1.pad", ignored);
}

} // namespace

void PadRecordReplay::configure(const PadSessionConfig &config) {
  configured_ = true;
  const CardIdentity card = config.cardIdentity ? config.cardIdentity() : CardIdentity{};
  recording_ = PadRecording(card);
  openSink(config, card);
  // PSXPORT_PAD_RESUME is the same replay plus fast-forward until the recording is spent, then the
  // player drives. Two knobs because the two intentions want opposite pacing: a gate replays at real
  // speed, a resume as fast as the host can. Which one was meant is stated, never inferred.
  std::string path = config.replayPath;
  if (!config.resumePath.empty()) {
    if (!path.empty()) {
      lucent::warn("padrec",
                   "both PSXPORT_PAD_RESUME and PSXPORT_PAD_REPLAY are set — using RESUME ({}) and "
                   "IGNORING REPLAY ({}); they are two different intentions",
                   config.resumePath,
                   path);
    }
    path = config.resumePath;
    resume_ = true;
  }
  if (!path.empty()) {
    loadReplay(path, card);
  }
  // A resume whose file did not load is a run that silently starts a NEW GAME from boot, the exact
  // thing the player asked not to do. Say so and drop the fast-forward.
  if (resume_ && !replay_) {
    lucent::error("padrec",
                  "PSXPORT_PAD_RESUME={} produced no replay — this run starts from BOOT with no replayed "
                  "input. Not fast-forwarding",
                  path);
    resume_ = false;
  }
}

void PadRecordReplay::openSink(const PadSessionConfig &config, const CardIdentity &card) {
  std::string path = config.recordPath;
  bool defaultSink = false;
  if (path == "0") {
    return; // explicit disable
  }
  // Default-on recording is WINDOWED-ONLY and never while REPLAYING (#56/#57): headless probes have
  // no session worth keeping and were truncating the user's real captures. A RESUME keeps recording on
  // purpose: the sink captures the replayed prefix and the live play after it as one from-boot
  // recording, which is what makes a resume chainable.
  if (path.empty() && config.replayPath.empty() && config.windowed) {
    path = kDefaultSink;
    defaultSink = true;
    rotateDefaultSink();
  }
  if (path.empty()) {
    return;
  }
  std::string error;
  sink_ = PadRecordingWriter::open(path.c_str(), card, error);
  if (!sink_) {
    lucent::error("padrec", "recording sink NOT opened: {}", error);
    return;
  }
  lucent::info("padrec",
               "recording -> {} (phase-keyed v{}, card {}){}",
               path,
               PadRecording::kVersion,
               card.describe(),
               defaultSink ? " (prev rotated to pad_session.1..5.pad; use replays/<cat>/<name>.pad to keep a repro)"
                           : "");
}

void PadRecordReplay::loadReplay(const std::string &path, const CardIdentity &card) {
  const char *role = resume_ ? "resume" : "replay";
  const std::vector<std::uint8_t> bytes = Fs::readFile(path);
  if (bytes.empty()) {
    lucent::error("padrec", "{} REFUSED: {} is missing, unreadable or empty", role, path);
    return;
  }
  DecodedPadRecording decoded = PadRecording::decode(bytes);
  if (!decoded.recording) {
    lucent::error("padrec", "{} REFUSED: {}: {}", role, path, decoded.error);
    return;
  }
  const CardIdentity &recorded = decoded.recording->card();
  if (recorded.kind == CardIdentity::Kind::Unknown) {
    lucent::warn("padrec",
                 "{} {} does not record which memory card it started from (a migrated raw recording); a route "
                 "that branches on the card's contents may not follow it",
                 role,
                 path);
  } else if (recorded != card) {
    if (!resume_) {
      lucent::error("padrec",
                    "replay REFUSED: {} was recorded against card {}, this run's card is {}. The title's front "
                    "end branches on what the card holds, so the same presses would answer a different screen. "
                    "Point PSXPORT_CARD at a copy of the recorded card, or re-record",
                    path,
                    recorded.describe(),
                    card.describe());
      return;
    }
    lucent::warn("padrec",
                 "resume: {} was recorded against card {}, this run's card is {} (the session saved since). "
                 "Resuming anyway; the phase report says where the route departs if it does",
                 path,
                 recorded.describe(),
                 card.describe());
  }
  const std::size_t segments = decoded.recording->segments().size();
  const std::size_t frames = decoded.recording->totalFrames();
  const bool keyed = decoded.recording->keyed();
  replay_ = std::make_unique<PhaseReplay>(std::move(*decoded.recording));
  lucent::info("padrec",
               "{} {} frame(s) in {} {} segment(s) <- {}{}",
               resume_ ? "RESUMING from" : "replaying",
               frames,
               segments,
               keyed ? "phase-keyed" : "UNKEYED (absolute from boot)",
               path,
               resume_ ? " (fast-forward: unpaced, muted, FMVs uncapped — control is handed over when the "
                         "recording runs out)"
                       : "");
}

std::uint16_t PadRecordReplay::service(std::uint64_t phase, std::uint16_t live, std::uint16_t replMask) {
  frames_++;
  if (replay_ && !phaseSourceChecked_) {
    phaseSourceChecked_ = true;
    if (phase == kUnkeyedPhase && replay_->recording().keyed()) {
      lucent::error("padrec",
                    "replay REFUSED: the recording is phase-keyed but this title declares no input phase "
                    "(GameRuntime::inputPhase), so no segment could ever be matched");
      replay_.reset();
      resume_ = false;
    }
  }
  std::uint16_t mask = live;
  if (replay_) {
    // A replay overrides the host/forced input but MERGES the live REPL drive on top (active-low AND
    // = union of pressed bits), so an explicit press issued mid-replay is not a dead command.
    if (const std::optional<std::uint16_t> replayed = replay_->next(phase)) {
      mask = static_cast<std::uint16_t>(*replayed & replMask);
    } else if (resume_ && !resumeAnnounced_) {
      resumeAnnounced_ = true;
      lucent::info("padrec",
                   "RESUME handed over at pad frame {} — real-time pacing, sound and control are yours. This "
                   "session keeps recording, so you can resume from here too",
                   frameIndex());
    }
  }
  recording_.append(phase, mask);
  if (sink_) {
    sink_->append(phase, mask);
  }
  return mask;
}

bool PadRecordReplay::saveRecording(const char *path, std::size_t frameLimit) const {
  if (!path || !recording_.totalFrames()) {
    return false;
  }
  const std::vector<std::uint8_t> bytes = recording_.encode(frameLimit);
  return Fs::writeFile(path, bytes.data(), bytes.size());
}

} // namespace psx::input
