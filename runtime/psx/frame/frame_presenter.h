// frame_presenter.h — the non-temporal current-frame capture, present, ledger and cadence fence.
#pragma once

#include "frame_record.h"
#include "frame_state.h"
#include "record_raster_setup.h"
#include "render_queue.h"
#include "state_producer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

class Core;

namespace psx::frame {

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
  // True when presents rasterize frame records and 60 fps in-betweens are on.
  virtual bool interpolatesRecords() const = 0;
  // GameRuntime::sealedFrameIsCut for the frame just sealed.
  virtual bool sealedFrameIsCut() = 0;
  // The VRAM rect the next present scans out.
  virtual gpu::RecordRect displayedBuffer() = 0;
  // Presents FramePresenter::composedRecord() without advancing the logic frame.
  virtual void presentInBetween() = 0;
  // The renders of the title's state producers on the record path; null draws every record as sealed.
  virtual const present::StateProducers *stateProducers() {
    return nullptr;
  }
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

  // Same state machine with an injected host backend, the device record it sealed and the producer states
  // that record draws. This is a real seam, not a test reimplementation.
  void commit(FramePresentationBackend &backend,
              int guestFields = 0,
              present::FrameRecord sealed = {},
              present::FrameState state = {});

  // Rotate one delivered-but-deliberately-unpresented field: the fence advances, the ledger rotates,
  // and the capture resets exactly as commit() does, but nothing is emitted, presented, paced, or
  // captured diagnostically. This is also the existing SBS/diff-mode behavior, exposed so a
  // single-core title can suppress presentation without skipping required frame bookkeeping.
  void commitUnpresented(Core *core);
  void commitUnpresented(FramePresentationBackend &backend);

  // The device's GP0 work of the last committed logic frame, N.
  const present::FrameRecord &currentRecord() const {
    return *current_;
  }
  // The frame at t the present draws instead of N's picture, non-null only during a present that has one.
  const present::FrameRecord *composedRecord() const {
    return composed_;
  }
  // Whether the present showing composedRecord() also brings the image to N (the t = 1 present).
  bool composedAdvances() const {
    return composedAdvances_;
  }

  CapturedFrameView capturedFrame() const;
  int capturedCount() const {
    return count_;
  }
  uint64_t fence() const {
    return fence_;
  }

private:
  void commit(FramePresentationBackend &backend,
              Core *core,
              present::FrameRecord sealed,
              present::FrameState state,
              int guestFields,
              TemporalFramePresentation *temporal);
  // The last three records that drew: a double-buffered display shows the one before the newest, whose
  // in-between blends from the one before that. Records that drew nothing are not pictures.
  static constexpr std::size_t kRecordHistory = 3;
  struct SealedRecord {
    std::shared_ptr<const present::FrameRecord> record;
    std::shared_ptr<const present::FrameState> state; // the producer states its packets were drawn from
    bool cut = false;                                 // a cut was sealed since the drawing record before it
    bool follows = false; // every record since the drawing record before it was committed and complete
  };

  // The latest record that drew `buffer`, or null.
  const SealedRecord *latestDrawing(const gpu::RecordRect &buffer) const;
  const SealedRecord *sealed(std::uint64_t sequence) const;
  // Every drawing record after `from` up to `to` is held, follows its predecessor and is no cut.
  bool continuesTo(std::uint64_t from, std::uint64_t to) const;
  bool cutBetween(std::uint64_t from, std::uint64_t to) const;
  // The record path at 60 fps: the shown record at t = 0.5 from the one shown before, then at t = 1.
  void presentRecords(FramePresentationBackend &backend, CapturedFrameView frame, int guestFields);
  // N's present; with `shown`, the shown record composed at t = 1.
  void presentCurrent(FramePresentationBackend &backend,
                      CapturedFrameView frame,
                      int guestFields,
                      int parts,
                      const SealedRecord *shown);
  void resetCapture();

  std::unique_ptr<RqItem[]> items_;
  int count_ = 0;
  uint32_t sequenceBase_ = 0;
  uint32_t flushOrdinal_ = 0;
  uint64_t fence_ = 0;
  int dumpSequence_ = 0;
  std::shared_ptr<const present::FrameRecord> current_ = std::make_shared<const present::FrameRecord>();
  std::array<SealedRecord, kRecordHistory> history_; // drawing records, oldest first
  std::optional<std::uint64_t> lastSequence_;
  bool pendingCut_ = false;
  bool pendingBreak_ = false;
  // The record whose picture the last present showed.
  std::optional<std::uint64_t> shownSequence_;
  std::optional<present::FrameRecord> composedStore_;
  const present::FrameRecord *composed_ = nullptr;
  bool composedAdvances_ = false;
};
} // namespace psx::frame
