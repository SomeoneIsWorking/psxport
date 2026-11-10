#include "replay_frame.h"

#include "core.h"
#include "emit_memory.h"
#include "game.h"
#include "guest_redraw.h"
#include "packet_chains.h"

#include <lucent/log.h>

#include <algorithm>
#include <utility>
#include <variant>

namespace psx::present {

ReplayFrame::ReplayFrame(const ReplaySection &section,
                         std::span<const MotionTrack> tracks,
                         std::span<MotionSource *const> sources)
    : section_(section), motion_(tracks, sources) {}

void ReplayFrame::beforeDraw(Core &core) {
  // Without fps60 no in-between is presented.
  if (core.game->mods.fps60 == 0) {
    forget();
    return;
  }
  ran_ = true;
  motion_.sample(core);
  pendingReady_ = false;
  if (!motion_.paired() || fields_ < 2 || fields_ - 1 > kMaxInBetweens) {
    return;
  }
  pending_.fields = fields_;
  pending_.captures.resize(static_cast<std::size_t>(fields_ - 1));
  for (int k = 1; k < fields_; ++k) {
    const float t = static_cast<float>(k) / static_cast<float>(fields_);
    if (!replay(core, t, pending_.captures[static_cast<std::size_t>(k - 1)])) {
      ++refused_;
      lucent::warn("replay", "the draw section's replay at {}/{} refused; the interval is shown as drawn", k, fields_);
      return;
    }
    ++replayed_;
  }
  pendingReady_ = true;
}

void ReplayFrame::forget() {
  motion_.forget();
  pendingReady_ = false;
}

bool ReplayFrame::replay(Core &core, float t, Capture &capture) {
  GuestRedraw redraw(core);
  motion_.write(redraw, t);
  const std::optional<std::uint32_t> head = section_.draw(core, redraw);
  // The section's flatten assigned its packets buckets for a walk that never comes.
  core.otTables.endWalk();
  if (!head) {
    return false;
  }
  capture.packets.clear();
  capture.words.clear();
  capture.places.clear();
  PacketChainWalker(EmitMemory(core), PacketBound::anywhere())
      .capture(*head, capture.packets, capture.words, capture.places);
  return true;
}

void ReplayFrame::sealed(std::uint64_t sequence, bool draws) {
  if (!draws) {
    return;
  }
  Set &set = sets_[nextSet_];
  nextSet_ = (nextSet_ + 1) % sets_.size();
  set.shown = sequence;
  set.from = pendingReady_ ? lastDrawing_ : std::nullopt;
  set.fields = pendingReady_ ? pending_.fields : 0;
  set.ran = ran_;
  std::swap(set.captures, pending_.captures);
  pendingReady_ = false;
  ran_ = false;
  lastDrawing_ = sequence;
}

const ReplayFrame::Set *ReplayFrame::setFor(std::uint64_t shown) const {
  const auto set = std::find_if(sets_.begin(), sets_.end(), [shown](const Set &candidate) {
    return candidate.shown == shown;
  });
  return set != sets_.end() ? &*set : nullptr;
}

bool ReplayFrame::replays(std::uint64_t shown) const {
  const Set *set = setFor(shown);
  return set != nullptr && set->ran;
}

bool ReplayFrame::covers(const Interval &interval) {
  fields_ = interval.fields;
  const Set *set = setFor(interval.shown);
  return set != nullptr && set->from == interval.from && set->fields == interval.fields;
}

const FrameRecord *ReplayFrame::at(const FrameRecord &shown, int k, std::span<const std::uint16_t> vram) {
  const Set *set = setFor(shown.sequence());
  if (set == nullptr || k < 1 || k >= set->fields) {
    return nullptr;
  }
  const FrameRecord *assembled = assemble(shown, set->captures[static_cast<std::size_t>(k - 1)], k, vram);
  held_ += assembled == nullptr ? 1 : 0;
  return assembled;
}

const FrameRecord *
ReplayFrame::assemble(const FrameRecord &shown, const Capture &capture, int k, std::span<const std::uint16_t> vram) {
  const std::span<const ChainRun> runs = shown.chainRuns();
  const auto drawn_by_section = [this](const ChainRun &run) {
    return section_.linked(run.head);
  };
  const auto replayed = std::find_if(runs.begin(), runs.end(), drawn_by_section);
  if (replayed == runs.end() || std::count_if(runs.begin(), runs.end(), drawn_by_section) != 1) {
    lucent::warn(
        "replay", "record {} walked {} chains, not exactly one of them the section's", shown.sequence(), runs.size());
    return nullptr;
  }
  const ChainRun &run = *replayed;
  const std::span<const RecordEntry> drawn = shown.entries().subspan(run.firstEntry, run.endEntry - run.firstEntry);
  if (std::any_of(drawn.begin(), drawn.end(), [](const RecordEntry &entry) {
        return std::holds_alternative<VramUpload>(entry);
      })) {
    lucent::warn("replay", "record {}'s chain bakes an upload the replay cannot redraw", shown.sequence());
    return nullptr;
  }
  decoded_.clear();
  if (!gpu::decodePackets(capture.words, capture.places, capture.packets, run.atStart, decoded_)) {
    lucent::warn("replay", "record {}'s in-between {} links a packet that does not draw", shown.sequence(), k);
    return nullptr;
  }
  FrameRecord &assembled = assembled_[static_cast<std::size_t>(k - 1)];
  assembled.splice(shown, run, decoded_);
  cluts_.index(shown);
  const std::span<RecordEntry> spliced = assembled.entries().subspan(run.firstEntry, decoded_.size());
  for (RecordEntry &entry : spliced) {
    auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive != nullptr && !cluts_.resolve(assembled, *primitive, vram)) {
      lucent::warn("replay", "record {}'s in-between {} filled the CLUT pool", shown.sequence(), k);
      return nullptr;
    }
  }
  return &assembled;
}

void ReplayFrame::report() const {
  lucent::info("replay",
               "run-end: {} in-between(s) replayed, {} refused by the section, {} held at assembly",
               replayed_,
               refused_,
               held_);
  motion_.report();
}

} // namespace psx::present
