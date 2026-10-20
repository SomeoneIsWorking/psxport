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
std::optional<present::FrameRecord> composeGuarded(Core &core,
                                                   const present::FrameRecord &shown,
                                                   const present::FrameState *from,
                                                   const present::FrameState &to,
                                                   float t,
                                                   const present::StateProducers &producers) {
  const present::DisplayPassGuard guard(core.rsub.mode);
  return present::composeFrame(shown, from, to, t, producers);
}

gpu::RecordRect displayedBuffer(Core &core) {
  return gpu::clampedVramRect(present::DisplayPresenter::presentedRect(core));
}

void buildReal(Core &core) {
  core.game->display.build(present::PresentCompletion::MainFrame);
}

void pace(Core &core, FieldCadence cadence) {
  core.runtime.pacePresentation(core, cadence);
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
  present::FrameRecord sealed = core->gpuDevice().sealRecord();
  present::FrameState state = core->frameStates.collect(sealed);
  psx::debug::censusRecord(*core, sealed, state);
  core->frameStates.endFrame();
  commitSealed(*core, std::move(sealed), std::move(state), guestFields);
}

void FramePresenter::commitUnpresented() {
  ++fence_;
}

void FramePresenter::commitSealed(Core &core, present::FrameRecord sealed, present::FrameState state, int guestFields) {
  const bool recordPath = core.rsub.mode.path() == present::RenderPath::Record;
  const bool interpolates = recordPath && core.game->mods.fps60 != 0;
  const bool cut = interpolates && core.runtime.sealedFrameIsCut(core);
  const RuntimeStateProducers runtimeProducers(core);
  const present::StateProducers *producers = recordPath ? &runtimeProducers : nullptr;
  // Only a title with state producers saves state, so an empty state has nothing to compose.
  const bool savedState = !state.empty();
  const bool gap = !lastSequence_ || sealed.sequence() != *lastSequence_ + 1u;
  lastSequence_ = sealed.sequence();
  current_ = std::make_shared<const present::FrameRecord>(std::move(sealed));
  pendingCut_ = pendingCut_ || cut;
  pendingBreak_ = pendingBreak_ || gap || !current_->complete();
  if (drawsAnything(*current_)) {
    std::move(history_.begin() + 1, history_.end(), history_.begin());
    history_.back() = {
        current_, std::make_shared<const present::FrameState>(std::move(state)), pendingCut_, !pendingBreak_};
    pendingCut_ = false;
    pendingBreak_ = false;
  }
  holdUndrawnWrites(*current_);
  ++fence_;
  if (interpolates) {
    presentRecords(core, producers, guestFields);
  } else {
    shownSequence_.reset();
    const bool composes = savedState && producers != nullptr;
    presentCurrent(core, producers, {guestFields, 1}, composes ? shownRecord(displayedBuffer(core)) : nullptr);
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
                  "records so far: presents={} in_betweens={} without_in_between={} repeats={} backwards={}",
                  stability_.recordPresents,
                  stability_.inBetweens,
                  stability_.withoutInBetween,
                  stability_.repeats,
                  stability_.backwards);
  }
  if (shown != nullptr) {
    shownSequence_ = shown->record->sequence();
    if (previous && *shownSequence_ == *previous) {
      ++stability_.repeats;
    } else if (previous && *shownSequence_ < *previous) {
      ++stability_.backwards;
    }
  }
  if (shown == nullptr || from == nullptr || !continuesTo(from->record->sequence(), shown->record->sequence())) {
    ++stability_.withoutInBetween;
    presentCurrent(core, producers, {guestFields, 1}, shown);
    return;
  }
  ++stability_.inBetweens;
  const bool cut = cutBetween(from->record->sequence(), shown->record->sequence());
  const present::FrameRecord &base = *shown->record;
  if (producers != nullptr) {
    composedStore_ = composeGuarded(core, base, cut ? nullptr : from->state.get(), *shown->state, 0.5f, *producers);
  }
  composed_ = composedStore_ ? &*composedStore_ : &base;
  composedAdvances_ = false;
  core.game->display.buildInBetween();
  composed_ = nullptr;
  composedStore_.reset();
  pace(core, {guestFields, 2});
  core.game->display.show();
  dumpPresent(core, true);
  presentCurrent(core, producers, {guestFields, 2}, shown);
}

// A present is build, pace, show: only the show waits, so it lands on its deadline whatever the build cost.
void FramePresenter::presentCurrent(Core &core,
                                    const present::StateProducers *producers,
                                    FieldCadence cadence,
                                    const SealedRecord *shown) {
  if (shown != nullptr && producers != nullptr) {
    composedStore_ = composeGuarded(core, *shown->record, nullptr, *shown->state, 1.0f, *producers);
    if (composedStore_) {
      ledger_.reconcile(*shown->record, *composedStore_, fence_, psx::config::gate_presentation());
    }
  }
  composed_ = composedStore_ ? &*composedStore_ : nullptr;
  composedAdvances_ = composed_ != nullptr;
  buildReal(core);
  composed_ = nullptr;
  composedAdvances_ = false;
  composedStore_.reset();
  pace(core, cadence);
  core.game->display.show();
  dumpPresent(core, false);
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
               "records: presents={} in_betweens={} without_in_between={} repeats={} backwards={}",
               stability_.recordPresents,
               stability_.inBetweens,
               stability_.withoutInBetween,
               stability_.repeats,
               stability_.backwards);
  ledger_.report();
}

} // namespace psx::frame
