// frame_presenter.h — the logic frame's sealed record, its presents, the present ledger and the cadence fence.
#pragma once

#include "field_cadence.h"
#include "frame_record.h"
#include "frame_state.h"
#include "present_ledger.h"
#include "state_producer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

class Core;
class RenderMode;

namespace psx::gpu {
struct RecordRect;
} // namespace psx::gpu

namespace psx::frame {

// The host operations behind one frame fence. The production backend targets one Core; the narrow
// interface also lets the complete shipping state machine be falsified without a GPU or a disc.
// The renders a Core's title answers from GameRuntime::stateProducer.
class RuntimeStateProducers final : public present::StateProducers {
public:
  explicit RuntimeStateProducers(Core &core) : core_(core) {}
  const present::StateProducer *find(std::uint32_t producer) const override;

private:
  Core &core_;
};

class FramePresentationBackend {
public:
  virtual ~FramePresentationBackend() = default;

  virtual void presentReal() = 0;
  virtual void captureDiagnostic(uint64_t fence, bool interpolated) = 0;
  virtual void pace(FieldCadence cadence) = 0;
  // Whether a dropped producer aborts the run (PSXPORT_GATE_PRESENTATION).
  virtual bool ledgerIsFatal() const {
    return false;
  }
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
  // The mode whose display pass is armed while state producers render; a guest store then aborts.
  virtual RenderMode *displayPassMode() {
    return nullptr;
  }
};

// What the record path showed, for the play check: the shown record never repeats or goes backwards,
// and every record present at 60 fps brings its in-between.
struct PresentStability {
  std::uint64_t recordPresents = 0;
  std::uint64_t inBetweens = 0;
  std::uint64_t withoutInBetween = 0;
  std::uint64_t repeats = 0;
  std::uint64_t backwards = 0;
};

class FramePresenter {
public:
  FramePresenter() = default;
  ~FramePresenter() = default;
  FramePresenter(const FramePresenter &) = delete;
  FramePresenter &operator=(const FramePresenter &) = delete;
  FramePresenter(FramePresenter &&) = delete;
  FramePresenter &operator=(FramePresenter &&) = delete;

  // Production entry point used by a title's measured guest frame boundary.
  void commit(Core *core, int guestFields);

  // Same state machine with an injected host backend, the device record it sealed and the producer states
  // that record draws. This is a real seam, not a test reimplementation.
  void commit(FramePresentationBackend &backend,
              int guestFields = 0,
              present::FrameRecord sealed = {},
              present::FrameState state = {});

  // Rotate one delivered-but-deliberately-unpresented field: the fence advances as commit() does, but
  // nothing is presented, paced, or captured diagnostically.
  void commitUnpresented();

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

  uint64_t fence() const {
    return fence_;
  }
  const PresentStability &stability() const {
    return stability_;
  }
  const present::PresentLedger &ledger() const {
    return ledger_;
  }
  // The stability counts and the ledger, once, at run end.
  void reportStability() const;

private:
  void commitSealed(FramePresentationBackend &backend,
                    present::FrameRecord sealed,
                    present::FrameState state,
                    int guestFields);
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
  // The record a present shows for `buffer`: the latest drawing one, unless a later record wrote the buffer, in
  // which case its picture is stale and the present is the device's.
  const SealedRecord *shownRecord(const gpu::RecordRect &buffer) const;
  const SealedRecord *sealed(std::uint64_t sequence) const;
  // Every drawing record after `from` up to `to` is held, follows its predecessor and is no cut.
  bool continuesTo(std::uint64_t from, std::uint64_t to) const;
  bool cutBetween(std::uint64_t from, std::uint64_t to) const;
  // The record path at 60 fps: the shown record at t = 0.5 from the one shown before, then at t = 1.
  void presentRecords(FramePresentationBackend &backend, int guestFields);
  // N's present; with `shown`, the shown record composed at t = 1.
  void presentCurrent(FramePresentationBackend &backend, FieldCadence cadence, const SealedRecord *shown);

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
  PresentStability stability_;
  present::PresentLedger ledger_;
};
} // namespace psx::frame
