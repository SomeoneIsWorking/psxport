#include "frame_presenter.h"
#include "record_raster_setup.h"

#include "core.h"
#include "diagnostic_config.h"
#include "display_presenter.h"
#include "frame_composer.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_device.h"
#include "producer_census.h"
#include "render_mode.h"
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

// A producer render reads saved state only; the armed display pass aborts any guest store it makes.
const present::FrameRecord *composeGuarded(Core &core,
                                           present::FrameComposer &composer,
                                           const present::FrameRecord &shown,
                                           const present::FrameState *from,
                                           const present::FrameState &to,
                                           float t,
                                           const present::StateProducers &producers) {
  const present::DisplayPassGuard guard(core.rsub.mode);
  return composer.compose(shown, from, to, t, producers, core.gpuDevice().vram());
}

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

const present::StateProducer *RuntimeStateProducers::find(std::uint32_t producer) const {
  return core_.runtime.stateProducer(core_, producer);
}

void FramePresenter::commit(Core *core, int guestFields) {
  if (!core || !core->game) {
    lucent::error("presentation", "FramePresenter::commit requires a bound Core/Game");
    std::abort();
  }
  // The last frame's present lands on its deadline before this frame's composition starts.
  core->game->display.finishShow();
  RecordSlot &slot = freeSlot();
  core->gpuDevice().sealRecord(slot.record);
  core->game->bugReport.frameSealed(*core);
  core->frameStates.collect(slot.record, slot.state);
  psx::debug::censusRecord(*core, slot.record, slot.state);
  core->frameStates.endFrame();
  commitSealed(*core, slot, guestFields);
}

FramePresenter::RecordSlot &FramePresenter::freeSlot() {
  for (RecordSlot &slot : slots_) {
    const bool held =
        &slot.record == current_ || std::any_of(history_.begin(), history_.end(), [&slot](const SealedRecord &entry) {
          return entry.record == &slot.record;
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

void FramePresenter::commitSealed(Core &core, const RecordSlot &sealed, int guestFields) {
  const bool recordPath = core.rsub.mode.path() == present::RenderPath::Record;
  const bool interpolates = recordPath && core.game->mods.fps60 != 0;
  const bool cut = interpolates && core.runtime.sealedFrameIsCut(core);
  const RuntimeStateProducers runtimeProducers(core);
  const present::StateProducers *producers = recordPath ? &runtimeProducers : nullptr;
  // Only a title with state producers saves state, so an empty state has nothing to compose.
  const bool savedState = !sealed.state.empty();
  const bool gap = !lastSequence_ || sealed.record.sequence() != *lastSequence_ + 1u;
  lastSequence_ = sealed.record.sequence();
  current_ = &sealed.record;
  pendingCut_ = pendingCut_ || cut;
  pendingBreak_ = pendingBreak_ || gap || !current_->complete();
  if (drawsAnything(*current_)) {
    std::move(history_.begin() + 1, history_.end(), history_.begin());
    history_.back() = {current_, &sealed.state, pendingCut_, !pendingBreak_, core.runtime.debugMenu().screenKey(core)};
    pendingCut_ = false;
    pendingBreak_ = false;
  }
  holdUndrawnWrites(*current_);
  ++fence_;
  if (interpolates) {
    presentRecords(core, producers, guestFields);
  } else {
    shownSequence_.reset();
    shownMoment_.reset();
    const SealedRecord *shown = savedState ? shownRecord(displayedBuffer(core)) : nullptr;
    const present::FrameRecord *composedFrame =
        shown != nullptr ? composeShown(core, endComposer_, producers, *shown, nullptr, 1.0f) : nullptr;
    presentCurrent(core, {guestFields, 1}, shown, composedFrame);
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

void FramePresenter::presentRecords(Core &core, const present::StateProducers *producers, int guestFields) {
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
    presentCurrent(core,
                   {guestFields, 1},
                   shown,
                   shown != nullptr ? composeShown(core, endComposer_, producers, *shown, nullptr, 1.0f) : nullptr);
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
  // The interval S'→S is drawn one way at every t: composed whole, or S held as the guest drew it.
  bool held = producers == nullptr;
  if (cut) {
    journal.record(core, {.reason = present::OwnerReason::IntervalHeld, .cause = present::HeldCause::Cut}, false);
  } else {
    const bool still = verdict_.judge(*from->record, base, core.owners);
    held = held || !still;
    for (const present::OwnerFinding &finding : verdict_.findings()) {
      journal.record(core, finding, !still);
    }
  }
  // Present j shows the guest moment L fields before its own field: t = (j + d + n - L) / n, n the fields between
  // S' and S's draw entries and d those from S's draw entry to this commit.
  const int fields = intervalFields(*from->record, base, guestFields);
  // A cut or a held interval shows no motion, so the latency returns to its least there.
  latency_ = held || cut ? lead + guestFields - 1
                         : intervalLatency(entry, {.lead = lead, .presents = guestFields, .fields = fields});
  const auto tAt = [&](int present) {
    return std::clamp(static_cast<float>(present + lead + fields - latency_) / static_cast<float>(fields), 0.0f, 1.0f);
  };
  // The last present is composed first: a refusal at any t holds the whole interval, so every t is tried before any
  // present is shown.
  const auto composeAt = [&](present::FrameComposer &composer, int present) -> const present::FrameRecord * {
    if (held) {
      return nullptr;
    }
    const float t = tAt(present);
    const present::FrameRecord *composedFrame =
        composeShown(core, composer, producers, *shown, cut || t >= 1.0f ? nullptr : from->state, t);
    held = composedFrame == nullptr && (composer.refusal().refused());
    return composedFrame;
  };
  const present::FrameRecord *last = composeAt(endComposer_, guestFields - 1);
  for (int present = guestFields - 2; present > 0 && !held; --present) {
    composeAt(composer_, present);
  }
  // One present per field, so each vblank of a display at the field rate shows the frame a field further on.
  const FieldCadence cadence{guestFields, guestFields};
  float shownT = -1.0f;
  for (int present = 0; present + 1 < guestFields; ++present) {
    const present::FrameRecord *composedFrame = composeAt(composer_, present);
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

const present::FrameRecord *FramePresenter::composeShown(Core &core,
                                                         present::FrameComposer &composer,
                                                         const present::StateProducers *producers,
                                                         const SealedRecord &shown,
                                                         const present::FrameState *from,
                                                         float t) {
  if (producers == nullptr) {
    return nullptr;
  }
  const present::FrameRecord *composedFrame =
      composeGuarded(core, composer, *shown.record, from, *shown.state, from != nullptr ? t : 1.0f, *producers);
  const present::FrameComposer::Refusal &refusal = composer.refusal();
  if (composedFrame == nullptr && refusal.refused()) {
    present::OwnerFinding finding{.reason =
                                      refusal.baked ? present::OwnerReason::Baked : present::OwnerReason::IntervalHeld,
                                  .cause = refusal.cause,
                                  .producer = refusal.producer};
    const std::span<const present::RecordEntry> entries = shown.record->entries();
    if (refusal.entry < entries.size()) {
      if (const auto *primitive = std::get_if<present::DrawPrimitive>(&entries[refusal.entry])) {
        finding.table = primitive->slot ? primitive->slot->table : present::kNoTable;
        finding.bucketLo = primitive->slot ? primitive->slot->index : 0u;
        finding.bucketHi = finding.bucketLo;
        finding.primitives = 1;
        finding.sampleAddress = primitive->sourceAddress;
        finding.sampleCommand = primitive->command;
      }
    }
    core.game->ownerJournal.record(core, finding, true);
  }
  return composedFrame;
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
  if (composedFrame != nullptr && !between) {
    ledger_.reconcile(*shown->record, *composedFrame, fence_, psx::config::gate_presentation());
  }
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
  ledger_.report();
}

} // namespace psx::frame
