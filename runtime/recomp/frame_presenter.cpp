#include "frame_presenter.h"

#include "cfg.h"
#include "core.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_native_internal.h"
#include "gpu_vk.h"

#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int gpu_vk_enabled();

namespace {

constexpr int kDumpMax = 600;

void dump_present(Core *core, uint64_t fence, int &sequence, bool interpolated) {
  // Keep the established channel name compatible while moving its lifecycle out of Fps60. It now
  // captures any presented frame; an already-60fps title does not need the interpolation subsystem.
  static const lucent::Channel channel{"fps60dump"};
  if (!channel) {
    sequence = 0;
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

  void presentIntermediate() override {
    gpu_fps60_present_pass(&core_);
  }

  void captureDiagnostic(uint64_t fence, bool interpolated) override {
    dump_present(&core_, fence, dumpSequence_, interpolated);
  }

  void pace(int guestFields, int parts) override {
    gpu_pace_subframe_fields(&core_, guestFields, parts);
  }

  void reconcile(uint64_t fence) override {
    core_.game->rq.mLedger.reconcile(static_cast<long>(fence), cfg_on("PSXPORT_GATE_PRESENTATION"));
  }

  void beginLedgerFrame() override {
    core_.game->rq.mLedger.beginFrame();
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
  CoreFramePresentationBackend backend(*core, dumpSequence_);
  if (core->game->diff_mode) {
    ++fence_;
    backend.reconcile(fence_);
    backend.beginLedgerFrame();
    resetCapture();
    return;
  }
  commit(backend, core, guestFields, temporal);
}

void FramePresenter::commit(FramePresentationBackend &backend, int guestFields) {
  commit(backend, nullptr, guestFields, nullptr);
}

void FramePresenter::commit(FramePresentationBackend &backend,
                            Core *core,
                            int guestFields,
                            TemporalFramePresentation *temporal) {
  ++fence_;
  const CapturedFrameView frame = capturedFrame();
  if (temporal) {
    temporal->present(backend, *core, frame, guestFields);
  } else {
    backend.emit(frame.items);
    backend.presentReal();
    backend.captureDiagnostic(fence_, false);
    backend.pace(guestFields, 1);
  }
  backend.reconcile(fence_);
  backend.beginLedgerFrame();
  resetCapture();
}

void FramePresenter::resetCapture() {
  count_ = 0;
  sequenceBase_ = 0;
  flushOrdinal_ = 0;
}
