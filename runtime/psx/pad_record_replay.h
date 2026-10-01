// pad_record_replay.h — class PadRecordReplay: the pad's recording and replay session, owned by Pad.
//
// One per Pad. It resolves PSXPORT_PAD_RECORD / PSXPORT_PAD_REPLAY / PSXPORT_PAD_RESUME once, on the
// first serviced frame, and from then on turns (title phase, live mask) into the mask the guest
// receives while recording every frame:
//
//   - the in-memory recording (always on) is what the debug server's `padrec save` cuts a replay from;
//   - the file sink (PSXPORT_PAD_RECORD, or the windowed default) streams the same frames to disk;
//   - a replay (PSXPORT_PAD_REPLAY) or resume (PSXPORT_PAD_RESUME) drives the pad through PhaseReplay.
//
// Every recording carries the identity of the memory-card image it started from. A REPLAY whose card
// differs is REFUSED: a title's front end branches on what the card holds, so the same presses would
// answer a different screen (spyro issue 0116). A RESUME is the player's own session, whose card
// legitimately changes as they save, so a different card is reported and the resume proceeds.
#pragma once

#include "pad_phase_replay.h"
#include "pad_recording.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace psx::input {

// Where a pad session's configuration comes from. The paths are the knob values (empty = unset);
// `cardIdentity` reads the memory-card image the run starts with, and `windowed` says whether the
// default recording sink applies.
struct PadSessionConfig {
  std::string recordPath;
  std::string replayPath;
  std::string resumePath;
  bool windowed = false;
  std::function<CardIdentity()> cardIdentity;
};

class PadRecordReplay {
public:
  // Rotated default sink used by windowed runs that name no PSXPORT_PAD_RECORD path.
  static constexpr const char *kDefaultSink = "scratch/bin/pad_session.pad";

  // Resolve the session from `config` (first serviced frame only). Separate from service() so a
  // test can drive the same code path with its own configuration.
  void configure(const PadSessionConfig &config);
  bool configured() const {
    return configured_;
  }

  // One pad frame. `live` is the mask every other input source resolved to; `replMask` is the live
  // REPL drive, which a replay merges rather than swallows. Returns the mask the guest receives.
  std::uint16_t service(std::uint64_t phase, std::uint16_t live, std::uint16_t replMask);

  // The 0-based index of the frame service() last handled (the PSXPORT_PAD_SHOT_AT/DUMP_AT axis).
  std::uint32_t frameIndex() const {
    return frames_ ? frames_ - 1u : 0u;
  }

  const PadRecording &recording() const {
    return recording_;
  }
  bool saveRecording(const char *path, std::size_t frameLimit) const;

  bool replayPending() const {
    return replay_ && replay_->playing();
  }
  bool fastForwarding() const {
    return resume_ && replayPending();
  }
  const PhaseReplay *replay() const {
    return replay_.get();
  }

private:
  void openSink(const PadSessionConfig &config, const CardIdentity &card);
  void loadReplay(const std::string &path, const CardIdentity &card);

  bool configured_ = false;
  std::uint32_t frames_ = 0;
  PadRecording recording_;
  std::unique_ptr<PadRecordingWriter> sink_;
  std::unique_ptr<PhaseReplay> replay_;
  bool resume_ = false;
  bool resumeAnnounced_ = false;
  bool phaseSourceChecked_ = false;
};

} // namespace psx::input
