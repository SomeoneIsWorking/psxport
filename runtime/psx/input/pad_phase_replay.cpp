// pad_phase_replay.cpp — the phase matcher (see pad_phase_replay.h for the rules).

#include "pad_phase_replay.h"

#include <lucent/log.h>

#include <utility>

namespace psx::input {

PhaseReplay::PhaseReplay(PadRecording recording, std::uint32_t phaseWaitLimit)
    : recording_(std::move(recording)), phaseWaitLimit_(phaseWaitLimit) {
  progress_.segmentsTotal = recording_.segments().size();
  progress_.framesRecorded = recording_.totalFrames();
  if (!progress_.segmentsTotal) {
    progress_.status = ReplayStatus::Complete;
  }
}

std::optional<std::uint16_t> PhaseReplay::next(std::uint64_t observedPhase) {
  if (!playing()) {
    return std::nullopt;
  }
  const PadSegment &current = recording_.segments()[segment_];
  const bool matches = current.phase == kUnkeyedPhase || current.phase == observedPhase;
  if (entered_ && matches) {
    return offset_ < current.frames() ? std::optional<std::uint16_t>(emit()) : wait(observedPhase);
  }
  if (entered_) {
    // The game has left the phase this segment was recorded in. Close it; if it was the last one,
    // the recording is spent.
    closeCurrent();
    if (!playing()) {
      return std::nullopt;
    }
  }
  if (const std::optional<std::size_t> target = findNeutralPath(segment_, observedPhase)) {
    enter(*target, *target - segment_);
    return emit();
  }
  return wait(observedPhase);
}

std::uint16_t PhaseReplay::emit() {
  const std::uint16_t mask = recording_.segments()[segment_].maskAt(offset_);
  offset_++;
  progress_.framesEmitted++;
  if (atLastFrame()) {
    progress_.status = ReplayStatus::Complete;
    lucent::info("padphase",
                 "replay COMPLETE: all {} segment(s) consumed, {} of {} recorded frame(s) delivered",
                 progress_.segmentsTotal,
                 progress_.framesEmitted,
                 progress_.framesRecorded);
  }
  return mask;
}

bool PhaseReplay::atLastFrame() const {
  return segment_ + 1 == recording_.segments().size() && offset_ == recording_.segments()[segment_].frames();
}

std::optional<std::uint16_t> PhaseReplay::wait(std::uint64_t observedPhase) {
  if (waitRun_ >= phaseWaitLimit_) {
    const PadSegment &awaited = recording_.segments()[segment_];
    progress_.status = ReplayStatus::Stalled;
    progress_.stallSegment = segment_;
    progress_.stallExpectedPhase = awaited.phase;
    progress_.stallObservedPhase = observedPhase;
    lucent::error("padphase",
                  "replay STALLED at segment {} of {}: the recording {} phase {} ({} recorded frame(s), {} "
                  "delivered) but the game has been in phase {} for {} frame(s). The replay stops driving "
                  "the pad here, so nothing after this point follows the recording",
                  segment_ + 1,
                  progress_.segmentsTotal,
                  entered_ ? "had finished" : "expects",
                  describePhase(awaited.phase),
                  awaited.frames(),
                  offset_,
                  describePhase(observedPhase),
                  waitRun_);
    return std::nullopt;
  }
  waitRun_++;
  progress_.framesWaited++;
  return kNeutralMask;
}

void PhaseReplay::enter(std::size_t segment, std::size_t passed) {
  segment_ = segment;
  offset_ = 0;
  entered_ = true;
  progress_.segmentsEntered++;
  progress_.segmentsPassed += passed;
  const std::uint64_t phase = recording_.segments()[segment_].phase;
  if (passed) {
    lucent::info("padphase",
                 "{} neutral-only segment(s) before segment {} were never observed; they held no "
                 "input, so nothing recorded was lost",
                 passed,
                 segment_ + 1);
  }
  if (waitRun_) {
    progress_.phasesWaitedFor++;
    if (waitRun_ > progress_.longestWait) {
      progress_.longestWait = waitRun_;
      progress_.longestWaitSegment = segment_;
    }
    lucent::info("padphase",
                 "entered phase {} (segment {} of {}) after holding neutral input for {} frame(s)",
                 describePhase(phase),
                 segment_ + 1,
                 progress_.segmentsTotal,
                 waitRun_);
  }
  lucent::debug("padphase",
                "segment {} of {}: phase {}, {} recorded frame(s)",
                segment_ + 1,
                progress_.segmentsTotal,
                describePhase(phase),
                recording_.segments()[segment_].frames());
  waitRun_ = 0;
}

void PhaseReplay::closeCurrent() {
  const PadSegment &left = recording_.segments()[segment_];
  const std::uint32_t dropped = left.inputFramesFrom(offset_);
  if (dropped) {
    progress_.inputFramesDropped += dropped;
    progress_.segmentsWithDroppedInput++;
    lucent::warn("padphase",
                 "phase {} (segment {} of {}) ended after {} of {} recorded frame(s); {} recorded frame(s) "
                 "holding a button were never delivered",
                 describePhase(left.phase),
                 segment_ + 1,
                 progress_.segmentsTotal,
                 offset_,
                 left.frames(),
                 dropped);
  }
  if (segment_ + 1 == recording_.segments().size()) {
    progress_.status = ReplayStatus::Complete;
    lucent::info("padphase",
                 "replay COMPLETE: the last phase ended; {} of {} recorded frame(s) delivered",
                 progress_.framesEmitted,
                 progress_.framesRecorded);
    return;
  }
  segment_++;
  offset_ = 0;
  entered_ = false;
}

// The segment to enter for `phase`, starting the search at `from` (not yet entered). Only segments
// that hold no input may be passed over to reach it.
std::optional<std::size_t> PhaseReplay::findNeutralPath(std::size_t from, std::uint64_t phase) const {
  const std::vector<PadSegment> &segments = recording_.segments();
  for (std::size_t index = from; index < segments.size(); index++) {
    if (segments[index].phase == kUnkeyedPhase || segments[index].phase == phase) {
      return index;
    }
    if (segments[index].inputFramesFrom(0)) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

void PhaseReplay::reportRunEnd() const {
  const ReplayProgress &p = progress_;
  const char *status = p.status == ReplayStatus::Complete  ? "COMPLETE"
                       : p.status == ReplayStatus::Stalled ? "STALLED"
                                                           : "TRUNCATED (the run ended first)";
  lucent::log(p.status == ReplayStatus::Complete ? lucent::Level::Info : lucent::Level::Warn,
              "padphase",
              lucent::format("run-end: replay {} — segments entered {} of {} (+{} neutral passed unobserved), "
                             "frames delivered {} of {}, neutral frames waited {} over {} phase(s) (longest {} "
                             "at segment {}), button frames dropped {} in {} segment(s)",
                             status,
                             p.segmentsEntered,
                             p.segmentsTotal,
                             p.segmentsPassed,
                             p.framesEmitted,
                             p.framesRecorded,
                             p.framesWaited,
                             p.phasesWaitedFor,
                             p.longestWait,
                             p.longestWaitSegment + 1,
                             p.inputFramesDropped,
                             p.segmentsWithDroppedInput));
}

} // namespace psx::input
