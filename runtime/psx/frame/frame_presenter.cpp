#include "frame_presenter.h"

#include "cfg.h"
#include "core.h"
#include "frame_composer.h"
#include "frame_dump_window.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_native_internal.h"
#include "gpu_vk.h"
#include "in_between_present.h"
#include "keyed_blend.h"
#include "producer_census.h"
#include "texture_feedback.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <variant>
#include <vector>

int gpu_vk_enabled();

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

void dump_present(Core *core, uint64_t fence, int &sequence, bool interpolated) {
  // Keep the established channel name compatible while moving its lifecycle out of Fps60. It now
  // captures any presented frame; an already-60fps title does not need the interpolation subsystem.
  static const lucent::Channel channel{"fps60dump"};
  if (!channel) {
    sequence = 0;
    return;
  }
  if (!frameDumpWindowContains(fence, cfg_int("PSXPORT_FPS60_DUMP_FROM", 0))) {
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
  if (gpu_vk_enabled()) {
    gpu_vk_shot(core, path);
  } else {
    gpu_native_shot(core, path);
  }
  ++sequence;
}

class CoreFramePresentationBackend final : public FramePresentationBackend {
public:
  CoreFramePresentationBackend(Core &core, int &dumpSequence) : core_(core), dumpSequence_(dumpSequence) {}

  void emit(std::span<const RqItem> items) override {
    std::vector<const RqItem *> stream;
    stream.reserve(items.size());
    for (const RqItem &item : items) {
      stream.push_back(&item);
    }
    core_.game->rq.mLedger.inRealPresent = true;
    core_.game->rq.emitItemStream(&core_, stream);
    core_.game->rq.mLedger.inRealPresent = false;
  }

  void presentReal() override {
    gpu_present_ex(&core_, 1);
  }

  void captureDiagnostic(uint64_t fence, bool interpolated) override {
    dump_present(&core_, fence, dumpSequence_, interpolated);
  }

  void pace(int guestFields, int parts) override {
    if (core_.game->runtime) {
      core_.game->runtime->pacePresentation(core_, guestFields, parts);
    } else {
      // A neutral framework instance has no title scheduler and retains combined field pacing.
      core_.game->framePacer.paceSubframeFields(core_, guestFields, parts);
    }
  }

  void reconcile(uint64_t fence) override {
    core_.game->rq.mLedger.reconcile(static_cast<long>(fence), cfg_on("PSXPORT_GATE_PRESENTATION"));
  }

  void beginLedgerFrame() override {
    core_.game->rq.mLedger.beginFrame();
  }

  bool interpolatesRecords() const override {
    return core_.rsub.mode.path() == RenderPath::Record && core_.game->mods.fps60 != 0;
  }

  bool sealedFrameIsCut() override {
    return core_.game->runtime != nullptr && core_.game->runtime->sealedFrameIsCut(core_);
  }

  gpu::RecordRect displayedBuffer() override {
    const gpu::DisplayArea area = core_.gpuDevice.displayArea();
    return gpu::clampedVramRect(area.x, area.y, area.width, GpuState::presentedHeight(&core_, area.height));
  }

  void presentInBetween() override {
    gpu_present_in_between(&core_);
  }

  const present::StateProducers *stateProducers() override {
    return core_.rsub.mode.path() == RenderPath::Record ? &core_.stateProducers : nullptr;
  }

private:
  Core &core_;
  int &dumpSequence_;
};

} // namespace

void FramePresenter::capture(const RqItem *items, int count) {
  if (count <= 0) {
    return;
  }
  if (!items_) {
    items_ = std::make_unique<RqItem[]>(RQ_MAX);
  }
  if (count_ + count > RQ_MAX) {
    lucent::error("presentation",
                  "FramePresenter::capture OVERFLOW: {} captured + {} this flush > RQ_MAX {}. "
                  "Raise the cap; do not drop prims.",
                  count_,
                  count,
                  RQ_MAX);
    std::abort();
  }

  std::memcpy(items_.get() + count_, items, static_cast<size_t>(count) * sizeof(RqItem));
  for (int i = 0; i < count; ++i) {
    RqItem &captured = items_[count_ + i];
    captured.seq += sequenceBase_;
    captured.flush_ordinal = flushOrdinal_;
  }
  count_ += count;
  sequenceBase_ += static_cast<uint32_t>(count);
  ++flushOrdinal_;
}

CapturedFrameView FramePresenter::capturedFrame() const {
  return {{items_.get(), static_cast<size_t>(count_)}, fence_};
}

void FramePresenter::commit(Core *core, int guestFields, TemporalFramePresentation *temporal) {
  if (!core || !core->game) {
    lucent::error("presentation", "FramePresenter::commit requires a bound Core/Game");
    std::abort();
  }
  present::FrameRecord sealed = core->gpuDevice.sealRecord();
  psx::debug::censusRecord(*core, sealed);
  present::FrameState state = core->frameStates.collect(sealed);
  core->frameStates.endFrame();
  CoreFramePresentationBackend backend(*core, dumpSequence_);
  commit(backend, core, std::move(sealed), std::move(state), guestFields, temporal);
}

void FramePresenter::commitUnpresented(Core *core) {
  if (!core || !core->game) {
    lucent::error("presentation", "FramePresenter::commitUnpresented requires a bound Core/Game");
    std::abort();
  }
  CoreFramePresentationBackend backend(*core, dumpSequence_);
  commitUnpresented(backend);
}

void FramePresenter::commitUnpresented(FramePresentationBackend &backend) {
  ++fence_;
  backend.reconcile(fence_);
  backend.beginLedgerFrame();
  resetCapture();
}

void FramePresenter::commit(FramePresentationBackend &backend,
                            int guestFields,
                            present::FrameRecord sealed,
                            present::FrameState state) {
  commit(backend, nullptr, std::move(sealed), std::move(state), guestFields, nullptr);
}

void FramePresenter::commit(FramePresentationBackend &backend,
                            Core *core,
                            present::FrameRecord sealed,
                            present::FrameState state,
                            int guestFields,
                            TemporalFramePresentation *temporal) {
  const bool interpolates = backend.interpolatesRecords();
  const bool cut = interpolates && backend.sealedFrameIsCut();
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
  ++fence_;
  const CapturedFrameView frame = capturedFrame();
  if (interpolates) {
    presentRecords(backend, frame, guestFields);
  } else {
    shownSequence_.reset();
    if (temporal) {
      temporal->present(backend, *core, frame, guestFields);
    } else {
      const present::StateProducers *producers = backend.stateProducers();
      const bool composes = producers != nullptr && !producers->empty();
      presentCurrent(backend, frame, guestFields, 1, composes ? shownRecord(backend.displayedBuffer()) : nullptr);
    }
  }
  backend.reconcile(fence_);
  backend.beginLedgerFrame();
  resetCapture();
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

void FramePresenter::presentRecords(FramePresentationBackend &backend, CapturedFrameView frame, int guestFields) {
  const SealedRecord *shown = shownRecord(backend.displayedBuffer());
  const SealedRecord *from = shownSequence_ ? sealed(*shownSequence_) : nullptr;
  shownSequence_.reset();
  if (shown != nullptr) {
    shownSequence_ = shown->record->sequence();
  }
  if (shown == nullptr || from == nullptr || !continuesTo(from->record->sequence(), shown->record->sequence())) {
    presentCurrent(backend, frame, guestFields, 1, shown);
    return;
  }
  const bool cut = cutBetween(from->record->sequence(), shown->record->sequence());
  present::FrameRecord blended =
      cut ? present::FrameRecord() : present::keyedBlend(*from->record, *shown->record, 0.5f);
  const present::FrameRecord &base = cut ? *shown->record : blended;
  if (const present::StateProducers *producers = backend.stateProducers()) {
    composedStore_ = present::composeFrame(base, cut ? nullptr : from->state.get(), *shown->state, 0.5f, *producers);
  }
  composed_ = composedStore_ ? &*composedStore_ : &base;
  composedAdvances_ = false;
  backend.presentInBetween();
  composed_ = nullptr;
  composedStore_.reset();
  backend.captureDiagnostic(fence_, true);
  backend.pace(guestFields, 2);
  presentCurrent(backend, frame, guestFields, 2, shown);
}

void FramePresenter::presentCurrent(
    FramePresentationBackend &backend, CapturedFrameView frame, int guestFields, int parts, const SealedRecord *shown) {
  const present::StateProducers *producers = backend.stateProducers();
  if (shown != nullptr && producers != nullptr) {
    composedStore_ = present::composeFrame(*shown->record, nullptr, *shown->state, 1.0f, *producers);
  }
  composed_ = composedStore_ ? &*composedStore_ : nullptr;
  composedAdvances_ = composed_ != nullptr;
  backend.emit(frame.items);
  backend.presentReal();
  composed_ = nullptr;
  composedAdvances_ = false;
  composedStore_.reset();
  backend.captureDiagnostic(fence_, false);
  backend.pace(guestFields, parts);
}

void FramePresenter::resetCapture() {
  count_ = 0;
  sequenceBase_ = 0;
  flushOrdinal_ = 0;
}

} // namespace psx::frame
