// frame_presenter.h — the non-temporal current-frame capture, present, ledger and cadence fence.
#pragma once

#include "render_queue.h"

#include <cstdint>
#include <memory>
#include <span>

class Core;

struct CapturedFrameView {
  std::span<const RqItem> items;
  uint64_t fence = 0;
};

// The host operations behind one frame fence. The production backend targets one Core; the narrow
// interface also lets the complete shipping state machine be falsified without a GPU or a disc.
class FramePresentationBackend {
public:
  virtual ~FramePresentationBackend() = default;

  virtual void emit(std::span<const RqItem> items) = 0;
  virtual void presentReal() = 0;
  virtual void captureDiagnostic(uint64_t fence, bool interpolated) = 0;
  virtual void pace(int guestFields, int parts) = 0;
  virtual void reconcile(uint64_t fence) = 0;
  virtual void beginLedgerFrame() = 0;
};

// Optional decorator for titles whose logic cadence needs synthesized presentation frames. A direct
// GameRuntime gets no decorator by default; the neutral presenter therefore owns no temporal history.
class TemporalFramePresentation {
public:
  virtual ~TemporalFramePresentation() = default;

  virtual void present(FramePresentationBackend &backend, Core &core, CapturedFrameView frame, int guestFields) = 0;
};

class FramePresenter {
public:
  FramePresenter() = default;
  ~FramePresenter() = default;
  FramePresenter(const FramePresenter &) = delete;
  FramePresenter &operator=(const FramePresenter &) = delete;
  FramePresenter(FramePresenter &&) = delete;
  FramePresenter &operator=(FramePresenter &&) = delete;

  // Accumulates every sorted DrawOTag/queue flush belonging to the current guest frame. Sequence
  // values are rebased across physical flushes so authored ordering remains deterministic.
  void capture(const RqItem *items, int count);

  // Production entry point used by a title's measured guest frame boundary.
  void commit(Core *core, int guestFields = 0, TemporalFramePresentation *temporal = nullptr);

  // Same state machine with an injected host backend. This is a real seam, not a test reimplementation.
  void commit(FramePresentationBackend &backend, int guestFields = 0);

  // Rotate one delivered-but-deliberately-unpresented field: the fence advances, the ledger rotates,
  // and the capture resets exactly as commit() does, but nothing is emitted, presented, paced, or
  // captured diagnostically. This is also the existing SBS/diff-mode behavior, exposed so a
  // single-core title can suppress presentation without skipping required frame bookkeeping.
  void commitUnpresented(Core *core);
  void commitUnpresented(FramePresentationBackend &backend);

  CapturedFrameView capturedFrame() const;
  int capturedCount() const {
    return count_;
  }
  uint64_t fence() const {
    return fence_;
  }

private:
  void commit(FramePresentationBackend &backend, Core *core, int guestFields, TemporalFramePresentation *temporal);
  void resetCapture();

  std::unique_ptr<RqItem[]> items_;
  int count_ = 0;
  uint32_t sequenceBase_ = 0;
  uint32_t flushOrdinal_ = 0;
  uint64_t fence_ = 0;
  int dumpSequence_ = 0;
};
