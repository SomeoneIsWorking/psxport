#include "frame_presenter.h"
#include "record_raster_setup.h"

#include "core.h"
#include "diagnostic_config.h"
#include "display_presenter.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_device.h"
#include "guest_widescreen_projection.h"
#include "render_mode.h"
#include "replay_frame.h"
#include "texture_feedback.h"
#include "title_debug_menu.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <variant>
#include <vector>

namespace psx::frame {
namespace {

bool drawsAnything(const present::FrameRecord &record) {
  for (const present::RecordEntry &entry : record.entries()) {
    if (std::holds_alternative<present::DrawPrimitive>(entry)) {
      return true;
    }
  }
  return false;
}

constexpr int kDumpMax = 600;
constexpr lucent::Channel presentDumpChannel{"fps60dump"};

gpu::RecordRect displayedBuffer(Core &core) {
  return gpu::clampedVramRect(present::DisplayPresenter::presentedRect(core));
}

void buildReal(Core &core) {
  core.game->display.build(present::PresentCompletion::MainFrame);
}

// The built present is shown at its paced deadline; the guest runs on until then.
void schedule(Core &core, FieldCadence cadence, present::PresentNote note) {
  note.fields = cadence.fields;
  note.parts = cadence.parts;
  note.logic = core.game->timing.logicFrame;
  core.game->display.showAt(core.runtime.presentDeadline(core, cadence), note);
}

} // namespace

void FramePresenter::commit(Core *core, int guestFields) {
  if (!core || !core->game) {
    lucent::error("presentation", "FramePresenter::commit requires a bound Core/Game");
    std::abort();
  }
  // The last frame's present lands on its deadline before this frame's composition starts.
  core->game->display.finishShow();
  present::FrameRecord &slot = freeSlot();
  core->gpuDevice().sealRecord(slot);
  slot.setWindows(present::frameWindows(*core));
  present::classifyCropLayers(*core, slot.entries());
  core->game->bugReport.frameSealed(*core, slot.sequence());
  commitSealed(*core, slot, guestFields);
}

present::FrameRecord &FramePresenter::freeSlot() {
  for (present::FrameRecord &slot : slots_) {
    const bool held =
        &slot == current_ || std::any_of(history_.begin(), history_.end(), [&slot](const SealedRecord &entry) {
          return entry.record == &slot;
        });
    if (!held) {
      return slot;
    }
  }
  lucent::error("presentation", "every record slot is held");
  std::abort();
}

void FramePresenter::commitUnpresented() {
  ++fence_;
}

void FramePresenter::commitSealed(Core &core, const present::FrameRecord &sealed, int guestFields) {
  const bool recordPath = core.rsub.mode.path() == present::RenderPath::Record;
  const bool interpolates = recordPath && core.game->mods.fps60 != 0;
  const bool cut = interpolates && core.runtime.sealedFrameIsCut(core);
  const bool gap = !lastSequence_ || sealed.sequence() != *lastSequence_ + 1u;
  lastSequence_ = sealed.sequence();
  current_ = &sealed;
  pendingCut_ = pendingCut_ || cut;
  pendingBreak_ = pendingBreak_ || gap || !current_->complete();
  if (drawsAnything(*current_)) {
    std::move(history_.begin() + 1, history_.end(), history_.begin());
    history_.back() = {current_, pendingCut_, !pendingBreak_, core.runtime.debugMenu().screenKey(core)};
    pendingCut_ = false;
    pendingBreak_ = false;
  }
  if (present::ReplayFrame *replay = core.runtime.replayFrame(core)) {
    replay->sealed(current_->sequence(), drawsAnything(*current_));
  }
  holdUndrawnWrites(*current_);
  ++fence_;
  if (interpolates) {
    presentRecords(core, guestFields);
  } else {
    shownSequence_.reset();
    shownMoment_.reset();
    presentCurrent(core, {guestFields, 1}, nullptr, nullptr);
  }
}

const FramePresenter::SealedRecord *FramePresenter::latestDrawing(const gpu::RecordRect &buffer) const {
  for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
    if (!it->record) {
      continue;
    }
    for (const present::RecordEntry &entry : it->record->entries()) {
      const auto *primitive = std::get_if<present::DrawPrimitive>(&entry);
      if (primitive != nullptr && gpu::drawAreaSpansBuffer(primitive->state, buffer)) {
        return &*it;
      }
    }
  }
  return nullptr;
}

void FramePresenter::holdUndrawnWrites(const present::FrameRecord &record) {
  const auto oldest = std::find_if(history_.begin(), history_.end(), [](const SealedRecord &entry) {
    return entry.record != nullptr;
  });
  if (oldest == history_.end()) {
    undrawnWrites_.clear();
    return;
  }
  const std::uint64_t oldestSequence = oldest->record->sequence();
  std::erase_if(undrawnWrites_, [oldestSequence](const UndrawnWrite &write) {
    return write.sequence < oldestSequence;
  });
  if (drawsAnything(record)) {
    return;
  }
  for (const present::RecordEntry &entry : record.entries()) {
    const std::optional<gpu::RecordRect> rect = gpu::entryWrittenRect(entry);
    if (!rect) {
      continue;
    }
    const auto same = std::find_if(undrawnWrites_.begin(), undrawnWrites_.end(), [&rect](const UndrawnWrite &write) {
      return write.area == *rect;
    });
    if (same != undrawnWrites_.end()) {
      same->sequence = record.sequence();
    } else {
      undrawnWrites_.push_back({*rect, record.sequence()});
    }
  }
}

const FramePresenter::SealedRecord *FramePresenter::shownRecord(const gpu::RecordRect &buffer) const {
  const SealedRecord *latest = latestDrawing(buffer);
  if (latest == nullptr) {
    return nullptr;
  }
  const auto writesBuffer = [&buffer](const present::FrameRecord &record) {
    return std::any_of(record.entries().begin(), record.entries().end(), [&buffer](const present::RecordEntry &entry) {
      return gpu::entryWritesRect(entry, buffer);
    });
  };
  for (const SealedRecord &entry : history_) {
    if (entry.record && entry.record->sequence() > latest->record->sequence() && writesBuffer(*entry.record)) {
      return nullptr;
    }
  }
  for (const UndrawnWrite &write : undrawnWrites_) {
    if (write.sequence > latest->record->sequence() && gpu::rectsOverlap(write.area, buffer)) {
      return nullptr;
    }
  }
  return current_->sequence() > latest->record->sequence() && writesBuffer(*current_) ? nullptr : latest;
}

const FramePresenter::SealedRecord *FramePresenter::sealed(std::uint64_t sequence) const {
  for (const SealedRecord &entry : history_) {
    if (entry.record && entry.record->sequence() == sequence) {
      return &entry;
    }
  }
  return nullptr;
}

bool FramePresenter::continuesTo(std::uint64_t from, std::uint64_t to) const {
  if (from >= to) {
    return false;
  }
  for (const SealedRecord &entry : history_) {
    if (entry.record && entry.record->sequence() > from && entry.record->sequence() <= to && !entry.follows) {
      return false;
    }
  }
  return true;
}

bool FramePresenter::cutBetween(std::uint64_t from, std::uint64_t to) const {
  for (const SealedRecord &entry : history_) {
    if (entry.record && entry.record->sequence() > from && entry.record->sequence() <= to && entry.cut) {
      return true;
    }
  }
  return false;
}

void FramePresenter::presentRecords(Core &core, int guestFields) {
  const SealedRecord *shown = shownRecord(displayedBuffer(core));
  const SealedRecord *from = shownSequence_ ? sealed(*shownSequence_) : nullptr;
  const std::optional<std::uint64_t> previous = shownSequence_;
  shownSequence_.reset();
  ++stability_.recordPresents;
  if (stability_.recordPresents % 600 == 0) {
    lucent::debug("playcheck",
                  "records so far: presents={} in_betweens={} without_in_between={} repeats={} backwards={} stalled={}",
                  stability_.recordPresents,
                  stability_.inBetweens,
                  stability_.withoutInBetween,
                  stability_.repeats,
                  stability_.backwards,
                  stability_.stalled);
  }
  if (shown != nullptr) {
    shownSequence_ = shown->record->sequence();
    if (previous && *shownSequence_ == *previous) {
      ++stability_.repeats;
    } else if (previous && *shownSequence_ < *previous) {
      ++stability_.backwards;
    }
  }
  present::OwnerJournal &journal = core.game->ownerJournal;
  // The interval shows S, so its rows belong to the screen S was sealed on.
  journal.observeScreen(core, shown != nullptr ? shown->screen : core.runtime.debugMenu().screenKey(core));
  const int lead = shown != nullptr ? fieldsSinceDrawEntry(core, *shown->record) : 0;
  // S's draw entry, in display fields.
  const std::int64_t entry = static_cast<std::int64_t>(core.game->timing.displayField()) - lead;
  // A logic frame held for one field is one present: the next field already shows the next frame.
  if (guestFields < 2) {
    presentCurrent(core, {guestFields, 1}, shown, nullptr);
    shownMoment_ = entry;
    return;
  }
  const present::HeldCause chain = shown == nullptr  ? present::HeldCause::StaleShown
                                   : from == nullptr ? present::HeldCause::NoPrevious
                                   : !continuesTo(from->record->sequence(), shown->record->sequence())
                                       ? present::HeldCause::ChainBreak
                                       : present::HeldCause::None;
  if (chain != present::HeldCause::None) {
    ++stability_.withoutInBetween;
    journal.record(core, {.reason = present::OwnerReason::IntervalHeld, .cause = chain}, true);
    presentCurrent(core, {guestFields, 1}, shown, nullptr);
    shownMoment_ = entry;
    return;
  }
  ++stability_.inBetweens;
  const bool cut = cutBetween(from->record->sequence(), shown->record->sequence());
  const present::FrameRecord &base = *shown->record;
  // Present j shows the guest moment L fields before its own field: t = (j + d + n - L) / n, n the fields between
  // S' and S's draw entries and d those from S's draw entry to this commit.
  const int fields = intervalFields(*from->record, base, guestFields);
  // A title that replays its draw section brings whole in-betweens. An S whose draw ran no section (a menu, a load)
  // shows S as the guest drew it at every in-between.
  present::ReplayFrame *replay = core.runtime.replayFrame(core);
  if (replay != nullptr && !replay->replays(base.sequence())) {
    replay = nullptr;
  }
  // The interval S'→S is drawn one way at every t: replayed whole, or S held as the guest drew it.
  bool held = false;
  if (cut) {
    // A replay blends its tracks whatever the title declared afterwards, so across a cut S is shown as drawn.
    held = replay != nullptr;
    journal.record(core, {.reason = present::OwnerReason::IntervalHeld, .cause = present::HeldCause::Cut}, false);
  } else if (replay != nullptr) {
    if (!replay->covers({.shown = base.sequence(), .from = from->record->sequence(), .fields = fields})) {
      held = true;
      journal.record(
          core, {.reason = present::OwnerReason::IntervalHeld, .cause = present::HeldCause::NotReplayed}, true);
    }
  } else {
    const bool still = verdict_.judge(*from->record, base, core.owners);
    held = !still;
    for (const present::OwnerFinding &finding : verdict_.findings()) {
      journal.record(core, finding, !still);
    }
  }
  // A cut or a held interval shows no motion, so the latency returns to its least there.
  latency_ = held || cut ? lead + guestFields - 1
                         : intervalLatency(entry, {.lead = lead, .presents = guestFields, .fields = fields});
  const auto tAt = [&](int present) {
    return std::clamp(static_cast<float>(present + lead + fields - latency_) / static_cast<float>(fields), 0.0f, 1.0f);
  };
  // The last present is replayed first: a refusal at any t holds the whole interval, so every t is tried before any
  // present is shown.
  const auto replayAt = [&](int present) -> const present::FrameRecord * {
    if (held || replay == nullptr) {
      return nullptr;
    }
    const int k = present + lead + fields - latency_;
    if (k < 1 || k >= fields) {
      return nullptr;
    }
    const present::FrameRecord *replayed = replay->at(base, *from->record, k, core.gpuDevice().vram());
    held = replayed == nullptr;
    return replayed;
  };
  const present::FrameRecord *last = replayAt(guestFields - 1);
  for (int present = guestFields - 2; present > 0 && !held; --present) {
    replayAt(present);
  }
  // One present per field, so each vblank of a display at the field rate shows the frame a field further on.
  const FieldCadence cadence{guestFields, guestFields};
  float shownT = -1.0f;
  for (int present = 0; present + 1 < guestFields; ++present) {
    const present::FrameRecord *composedFrame = replayAt(present);
    const bool composed = composedFrame != nullptr;
    const float t = tAt(present);
    stability_.stalled += composed && t <= shownT ? 1u : 0u;
    shownT = t;
    // Uncomposed, the in-between is N's own present: S as the guest drew it, rastered once for the interval.
    composed_ = composedFrame;
    composedAdvances_ = false;
    core.game->display.buildInBetween();
    composed_ = nullptr;
    dumpPresent(core, true);
    schedule(core,
             cadence,
             {.fence = fence_,
              .shown = base.sequence(),
              .from = from->record->sequence(),
              .t = t,
              .interval = fields,
              .composed = composed,
              .cut = cut});
  }
  const float t = tAt(guestFields - 1);
  stability_.stalled += !held && t <= shownT ? 1u : 0u;
  presentCurrent(core, cadence, shown, held ? nullptr : last, cut ? nullptr : from, t, fields);
  shownMoment_ = held || cut ? entry : std::min(entry, entry + lead + guestFields - 1 - latency_);
}

int FramePresenter::intervalLatency(std::int64_t entry, IntervalShape interval) const {
  // The last present lands on S or before it; the first shows neither a moment already shown nor one before S'.
  const int lowest = interval.lead + interval.presents - 1;
  std::int64_t highest = interval.lead + interval.fields;
  if (shownMoment_) {
    highest = std::min(highest, entry + interval.lead - *shownMoment_ - 1);
  }
  return static_cast<int>(std::min<std::int64_t>(std::max(latency_, lowest), highest));
}

int FramePresenter::fieldsSinceDrawEntry(const Core &core, const present::FrameRecord &record) {
  const std::optional<std::uint32_t> entry = record.drawEntryField();
  const auto now = static_cast<std::uint32_t>(core.game->timing.displayField());
  if (!entry || now < *entry || now - *entry > kMaxIntervalFields) {
    return 0;
  }
  return static_cast<int>(now - *entry);
}

int FramePresenter::intervalFields(const present::FrameRecord &from, const present::FrameRecord &to, int fallback) {
  const std::optional<std::uint32_t> begin = from.drawEntryField();
  const std::optional<std::uint32_t> end = to.drawEntryField();
  if (!begin || !end || *end <= *begin || *end - *begin > kMaxIntervalFields) {
    return fallback;
  }
  return static_cast<int>(*end - *begin);
}

// A present is build, then a show scheduled for its deadline: only the show waits, so it lands on time whatever
// the build cost, and the next logic frame's guest work runs while it waits.
void FramePresenter::presentCurrent(Core &core,
                                    FieldCadence cadence,
                                    const SealedRecord *shown,
                                    const present::FrameRecord *composedFrame,
                                    const SealedRecord *from,
                                    float t,
                                    int interval) {
  const bool between = composedFrame != nullptr && from != nullptr && t < 1.0f;
  composed_ = composedFrame;
  composedAdvances_ = composed_ != nullptr;
  buildReal(core);
  composed_ = nullptr;
  composedAdvances_ = false;
  dumpPresent(core, false);
  schedule(core,
           cadence,
           {.fence = fence_,
            .shown = shown != nullptr ? std::optional(shown->record->sequence()) : std::nullopt,
            .from = between ? std::optional(from->record->sequence()) : std::nullopt,
            .t = between ? t : 1.0f,
            .interval = interval,
            .composed = composedFrame != nullptr});
}

void FramePresenter::dumpPresent(Core &core, bool interpolated) {
  if (!presentDumpChannel) {
    dumpSequence_ = 0;
    return;
  }
  const long firstFence = psx::config::present_dump_first_fence();
  if (firstFence > 0 && fence_ < static_cast<std::uint64_t>(firstFence)) {
    return;
  }
  if (dumpSequence_ >= kDumpMax) {
    lucent::info("fps60dump", "cap ({} files) reached, capture stopped", kDumpMax);
    dumpSequence_ = -1;
    return;
  }
  if (dumpSequence_ < 0) {
    return;
  }
  char path[192];
  std::snprintf(path,
                sizeof(path),
                "scratch/framedump/f%06llu_%04d_%s.png",
                static_cast<unsigned long long>(fence_),
                dumpSequence_,
                interpolated ? "interp" : "real");
  if (!Fs::ensureParentDirs(path)) {
    return;
  }
  core.game->display.shot(path);
  ++dumpSequence_;
}

void FramePresenter::reportStability() const {
  lucent::info("playcheck",
               "records: presents={} in_betweens={} without_in_between={} repeats={} backwards={} stalled={}",
               stability_.recordPresents,
               stability_.inBetweens,
               stability_.withoutInBetween,
               stability_.repeats,
               stability_.backwards,
               stability_.stalled);
}

void FramePresenter::save(psx::platform::BlobWriter &out) const {
  std::vector<const SealedRecord *> carried;
  for (auto it = history_.rbegin(); it != history_.rend() && carried.size() < kCarriedDrawing; ++it) {
    if (it->record != nullptr) {
      carried.insert(carried.begin(), &*it);
    }
  }
  out.u8(static_cast<std::uint8_t>(carried.size()));
  for (const SealedRecord *entry : carried) {
    out.boolean(entry->cut);
    out.boolean(entry->follows);
    out.u64(entry->screen);
    entry->record->save(out);
  }
  const bool currentHeld = !carried.empty() && carried.back()->record == current_;
  out.boolean(currentHeld);
  if (!currentHeld) {
    current_->save(out);
  }
  const auto optional = [&out](const std::optional<std::uint64_t> &value) {
    out.boolean(value.has_value());
    out.u64(value.value_or(0));
  };
  optional(lastSequence_);
  optional(shownSequence_);
  out.boolean(shownMoment_.has_value());
  out.i64(shownMoment_.value_or(0));
  out.i32(latency_);
  out.boolean(pendingCut_);
  out.boolean(pendingBreak_);
  out.u32(static_cast<std::uint32_t>(undrawnWrites_.size()));
  for (const UndrawnWrite &write : undrawnWrites_) {
    for (const int value : {write.area.x0, write.area.y0, write.area.x1, write.area.y1}) {
      out.i32(value);
    }
    out.u64(write.sequence);
  }
}

bool FramePresenter::load(psx::platform::BlobReader &in, std::string &error) {
  const std::size_t count = in.u8();
  if (!in.ok() || count > kCarriedDrawing) {
    error = "presenter: its drawing record count does not decode";
    return false;
  }
  history_ = {};
  undrawnWrites_.clear();
  std::size_t slot = 0;
  for (std::size_t i = 0; i < count; i++) {
    SealedRecord &entry = history_[kRecordHistory - count + i];
    entry.cut = in.boolean();
    entry.follows = in.boolean();
    entry.screen = in.u64();
    if (!slots_[slot].load(in, error)) {
      return false;
    }
    entry.record = &slots_[slot++];
  }
  const bool currentHeld = in.boolean();
  if (currentHeld && count == 0) {
    error = "presenter: the current record is held with no drawing record";
    return false;
  }
  if (currentHeld) {
    current_ = history_.back().record;
  } else {
    if (!slots_[slot].load(in, error)) {
      return false;
    }
    current_ = &slots_[slot];
  }
  const auto optional = [&in]() -> std::optional<std::uint64_t> {
    const bool present = in.boolean();
    const std::uint64_t value = in.u64();
    return present ? std::optional(value) : std::nullopt;
  };
  lastSequence_ = optional();
  shownSequence_ = optional();
  const bool moment = in.boolean();
  const std::int64_t shown = in.i64();
  shownMoment_ = moment ? std::optional(shown) : std::nullopt;
  latency_ = in.i32();
  pendingCut_ = in.boolean();
  pendingBreak_ = in.boolean();
  const std::uint32_t writes = in.u32();
  if (!in.ok() || writes > in.size() - in.offset()) {
    error = "presenter: its undrawn writes do not decode";
    return false;
  }
  for (std::uint32_t i = 0; i < writes; i++) {
    UndrawnWrite write;
    write.area.x0 = in.i32();
    write.area.y0 = in.i32();
    write.area.x1 = in.i32();
    write.area.y1 = in.i32();
    write.sequence = in.u64();
    undrawnWrites_.push_back(write);
  }
  composed_ = nullptr;
  composedAdvances_ = false;
  if (!in.ok()) {
    error = "presenter: its state is truncated";
    return false;
  }
  return true;
}

std::vector<const present::FrameRecord *> FramePresenter::heldRecords() const {
  std::vector<const present::FrameRecord *> held;
  for (const SealedRecord &entry : history_) {
    if (entry.record != nullptr) {
      held.push_back(entry.record);
    }
  }
  if (std::find(held.begin(), held.end(), current_) == held.end()) {
    held.push_back(current_);
  }
  std::sort(held.begin(), held.end(), [](const present::FrameRecord *a, const present::FrameRecord *b) {
    return a->sequence() < b->sequence();
  });
  return held;
}

} // namespace psx::frame
