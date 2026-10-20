#include "frame_presenter.h"
#include "record_raster_setup.h"

#include "core.h"
#include "diagnostic_config.h"
#include "display_presenter.h"
#include "frame_composer.h"
#include "frame_dump_window.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_device.h"
#include "gpu_vk.h"
#include "gpu_vk_internal.h"
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

void dump_present(Core *core, uint64_t fence, int &sequence, bool interpolated) {
  // Any presented frame, real or in-between.
  if (!presentDumpChannel) {
    sequence = 0;
    return;
  }
  if (!frameDumpWindowContains(fence, psx::config::present_dump_first_fence())) {
    return;
  }
  if (sequence >= kDumpMax) {
    if (sequence == kDumpMax) {
      lucent::info("fps60dump", "cap ({} files) reached — stop capturing", kDumpMax);
      ++sequence;
    }
    return;
  }

  char path[192];
  std::snprintf(path,
                sizeof(path),
                "scratch/framedump/f%06llu_%04d_%s.png",
                static_cast<unsigned long long>(fence),
                sequence,
                interpolated ? "interp" : "real");
  if (!Fs::ensureParentDirs(path)) {
    return;
  }
  core->game->gpuVk().shot(path);
  ++sequence;
}

// The title's renders, asked through its runtime.
class CoreFramePresentationBackend final : public FramePresentationBackend {
public:
  CoreFramePresentationBackend(Core &core, int &dumpSequence)
      : core_(core), dumpSequence_(dumpSequence), producers_(core) {}

  void buildReal() override {
    core_.game->display.build(present::PresentCompletion::MainFrame);
  }

  void buildInBetween() override {
    core_.game->display.buildInBetween();
  }

  void show() override {
    core_.game->display.show();
  }

  void captureDiagnostic(uint64_t fence, bool interpolated) override {
    dump_present(&core_, fence, dumpSequence_, interpolated);
  }

  void pace(FieldCadence cadence) override {
    core_.runtime.pacePresentation(core_, cadence);
  }

  bool ledgerIsFatal() const override {
    return psx::config::gate_presentation();
  }

  bool interpolatesRecords() const override {
    return core_.rsub.mode.path() == RenderPath::Record && core_.game->mods.fps60 != 0;
  }

  bool sealedFrameIsCut() override {
    return core_.runtime.sealedFrameIsCut(core_);
  }

  gpu::RecordRect displayedBuffer() override {
    const gpu::DisplayArea area = core_.gpuDevice().displayArea();
    return gpu::clampedVramRect(
        {area.x, area.y, area.width, present::DisplayPresenter::presentedHeight(core_, area.height)});
  }

  const present::StateProducers *stateProducers() override {
    return core_.rsub.mode.path() == RenderPath::Record ? &producers_ : nullptr;
  }

  RenderMode *displayPassMode() override {
    return &core_.rsub.mode;
  }

private:
  Core &core_;
  int &dumpSequence_;
  RuntimeStateProducers producers_;
};

// A producer render reads saved state only; the armed display pass aborts any guest store it makes.
std::optional<present::FrameRecord> composeGuarded(FramePresentationBackend &backend,
                                                   const present::FrameRecord &shown,
                                                   const present::FrameState *from,
                                                   const present::FrameState &to,
                                                   float t,
                                                   const present::StateProducers &producers) {
  std::optional<DisplayPassGuard> guard;
  if (RenderMode *mode = backend.displayPassMode()) {
    guard.emplace(*mode);
  }
  return present::composeFrame(shown, from, to, t, producers);
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
  CoreFramePresentationBackend backend(*core, dumpSequence_);
  commitSealed(backend, std::move(sealed), std::move(state), guestFields);
}

void FramePresenter::commitUnpresented() {
  ++fence_;
}

void FramePresenter::commit(FramePresentationBackend &backend,
                            int guestFields,
                            present::FrameRecord sealed,
                            present::FrameState state) {
  commitSealed(backend, std::move(sealed), std::move(state), guestFields);
}

void FramePresenter::commitSealed(FramePresentationBackend &backend,
                                  present::FrameRecord sealed,
                                  present::FrameState state,
                                  int guestFields) {
  const bool interpolates = backend.interpolatesRecords();
  const bool cut = interpolates && backend.sealedFrameIsCut();
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
    presentRecords(backend, guestFields);
  } else {
    shownSequence_.reset();
    const bool composes = savedState && backend.stateProducers() != nullptr;
    presentCurrent(backend, {guestFields, 1}, composes ? shownRecord(backend.displayedBuffer()) : nullptr);
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
    const gpu::VramRect area{rect->x0, rect->y0, rect->x1 - rect->x0, rect->y1 - rect->y0};
    const auto same = std::find_if(undrawnWrites_.begin(), undrawnWrites_.end(), [&rect](const UndrawnWrite &write) {
      return gpu::clampedVramRect(write.area) == *rect;
    });
    if (same != undrawnWrites_.end()) {
      same->sequence = record.sequence();
    } else {
      undrawnWrites_.push_back({area, record.sequence()});
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
    if (write.sequence > latest->record->sequence() && gpu::rectsOverlap(gpu::clampedVramRect(write.area), buffer)) {
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

void FramePresenter::presentRecords(FramePresentationBackend &backend, int guestFields) {
  const SealedRecord *shown = shownRecord(backend.displayedBuffer());
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
    presentCurrent(backend, {guestFields, 1}, shown);
    return;
  }
  ++stability_.inBetweens;
  const bool cut = cutBetween(from->record->sequence(), shown->record->sequence());
  const present::FrameRecord &base = *shown->record;
  if (const present::StateProducers *producers = backend.stateProducers()) {
    composedStore_ = composeGuarded(backend, base, cut ? nullptr : from->state.get(), *shown->state, 0.5f, *producers);
  }
  composed_ = composedStore_ ? &*composedStore_ : &base;
  composedAdvances_ = false;
  backend.buildInBetween();
  composed_ = nullptr;
  composedStore_.reset();
  backend.pace({guestFields, 2});
  backend.show();
  backend.captureDiagnostic(fence_, true);
  presentCurrent(backend, {guestFields, 2}, shown);
}

void FramePresenter::presentCurrent(FramePresentationBackend &backend,
                                    FieldCadence cadence,
                                    const SealedRecord *shown) {
  const present::StateProducers *producers = backend.stateProducers();
  if (shown != nullptr && producers != nullptr) {
    composedStore_ = composeGuarded(backend, *shown->record, nullptr, *shown->state, 1.0f, *producers);
    if (composedStore_) {
      ledger_.reconcile(*shown->record, *composedStore_, fence_, backend.ledgerIsFatal());
    }
  }
  composed_ = composedStore_ ? &*composedStore_ : nullptr;
  composedAdvances_ = composed_ != nullptr;
  backend.buildReal();
  composed_ = nullptr;
  composedAdvances_ = false;
  composedStore_.reset();
  // Only the show waits, so it lands on its deadline whatever the guest step and the build cost.
  backend.pace(cadence);
  backend.show();
  backend.captureDiagnostic(fence_, false);
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
