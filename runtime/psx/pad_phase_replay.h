// pad_phase_replay.h — replay a phase-keyed pad recording against the phases the game enters NOW.
//
// THE RULE. Each recorded frame belongs to a phase segment and sits at an offset from that phase's
// entry. On replay, the mask for a pad frame is the current segment's mask at the current offset,
// and only while the game is in that segment's phase. So a boot, load or CD read that takes more or
// fewer frames than when the session was recorded moves the moment a phase is entered, and the
// presses inside the phase move with it.
//
//   the game is in the segment's phase     emit the next recorded frame; once the segment's recorded
//                                          frames are spent, hold neutral until the game moves on
//   the game leaves the segment's phase    the segment is closed. Recorded frames it never reached
//                                          are dropped, and any that held a button are COUNTED
//   the game is not yet in the next phase  hold neutral input and count the wait
//
// A segment the game passes through without the replay observing it (a phase that lasted less than a
// frame now) may be passed over ONLY when it holds no input at all, so nothing recorded is lost;
// each one is counted. A segment that holds input is never passed over: the replay waits for it, and
// a wait longer than the stall limit ends the replay as STALLED with the phase it expected and the
// phase the game was in. A phase that never arrives is therefore reported, not skipped.
//
// Unkeyed segments (kUnkeyedPhase) match any phase, which is how an unkeyed recording replays
// frame-for-frame from boot.
#pragma once

#include "pad_recording.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace psx::input {

// One minute of 60 Hz pad frames. A phase that runs longer than its recording plus this, or a
// recorded phase the game has not entered within it, is a replay that has left its route.
inline constexpr std::uint32_t kDefaultPhaseWaitLimit = 3600;

enum class ReplayStatus { Playing, Complete, Stalled };

// The replay's own denominators. `framesEmitted` counts recorded frames delivered to the guest,
// `framesWaited` the neutral frames held while waiting for a phase.
struct ReplayProgress {
  ReplayStatus status = ReplayStatus::Playing;
  std::size_t segmentsTotal = 0;
  std::size_t segmentsEntered = 0; // segments whose phase the game was observed to enter
  std::size_t segmentsPassed = 0;  // neutral-only segments passed over unobserved
  std::size_t framesRecorded = 0;
  std::size_t framesEmitted = 0;
  std::size_t framesWaited = 0;
  std::size_t phasesWaitedFor = 0; // wait episodes that ended with the awaited phase entered
  std::uint32_t longestWait = 0;
  std::size_t longestWaitSegment = 0;
  std::size_t inputFramesDropped = 0; // recorded frames holding a button that were never delivered
  std::size_t segmentsWithDroppedInput = 0;
  std::size_t stallSegment = 0;         // valid when status == Stalled
  std::uint64_t stallExpectedPhase = 0; // valid when status == Stalled
  std::uint64_t stallObservedPhase = 0; // valid when status == Stalled
};

class PhaseReplay {
public:
  explicit PhaseReplay(PadRecording recording, std::uint32_t phaseWaitLimit = kDefaultPhaseWaitLimit);

  // One pad frame. `observedPhase` is the title's phase this frame. Returns the mask to deliver, or
  // nothing once the replay no longer drives the pad (complete or stalled).
  std::optional<std::uint16_t> next(std::uint64_t observedPhase);

  bool playing() const {
    return progress_.status == ReplayStatus::Playing;
  }
  const ReplayProgress &progress() const {
    return progress_;
  }
  const PadRecording &recording() const {
    return recording_;
  }

  // One line with every denominator, at a level that matches the outcome. A run that ends while the
  // replay is still playing is reported TRUNCATED.
  void reportRunEnd() const;

private:
  std::uint16_t emit();
  std::optional<std::uint16_t> wait(std::uint64_t observedPhase);
  void enter(std::size_t segment, std::size_t passed);
  void closeCurrent();
  std::optional<std::size_t> findNeutralPath(std::size_t from, std::uint64_t phase) const;
  bool atLastFrame() const;

  PadRecording recording_;
  std::uint32_t phaseWaitLimit_;
  ReplayProgress progress_;
  std::size_t segment_ = 0;
  std::uint32_t offset_ = 0;
  bool entered_ = false;
  std::uint32_t waitRun_ = 0;
};

} // namespace psx::input
