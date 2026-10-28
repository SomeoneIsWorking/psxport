// frame_presenter.h — the logic frame's sealed record, its presents, the present ledger and the cadence fence.
#pragma once

#include "field_cadence.h"
#include "frame_composer.h"
#include "frame_record.h"
#include "frame_state.h"
#include "ownership_verdict.h"
#include "present_ledger.h"
#include "record_geometry.h"
#include "state_producer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

class Core;

namespace psx::frame {

// The renders a Core's title answers from GameRuntime::stateProducer.
class RuntimeStateProducers final : public present::StateProducers {
public:
  explicit RuntimeStateProducers(Core &core) : core_(core) {}
  const present::StateProducer *find(std::uint32_t producer) const override;

private:
  Core &core_;
};

// What the record path showed, for the play check: the shown record never repeats or goes backwards,
// and every record present at 60 fps brings its in-between.
struct PresentStability {
  std::uint64_t recordPresents = 0;
  std::uint64_t inBetweens = 0;
  std::uint64_t withoutInBetween = 0;
  std::uint64_t repeats = 0;
  std::uint64_t backwards = 0;
  // Composed presents whose guest moment did not advance: the interval had fewer fields than presents.
  std::uint64_t stalled = 0;
};

class FramePresenter {
public:
  FramePresenter() = default;
  ~FramePresenter() = default;
  FramePresenter(const FramePresenter &) = delete;
  FramePresenter &operator=(const FramePresenter &) = delete;
  FramePresenter(FramePresenter &&) = delete;
  FramePresenter &operator=(FramePresenter &&) = delete;

  // Seals the device's record at a title's measured guest frame boundary and presents it.
  void commit(Core *core, int guestFields);

  // Rotate one delivered-but-deliberately-unpresented field: the fence advances as commit() does, but
  // nothing is presented, paced, or captured diagnostically.
  void commitUnpresented();

  // The device's GP0 work of the last committed logic frame, N.
  const present::FrameRecord &currentRecord() const {
    return *current_;
  }
  // The frame at t the present draws instead of N's picture, non-null only while a present that has one builds.
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
  // The last three records that drew: a double-buffered display shows the one before the newest, whose
  // in-between blends from the one before that. Records that drew nothing are not pictures.
  static constexpr std::size_t kRecordHistory = 3;
  // A longer gap between two draw entries is a pause or a load, not a logic frame.
  static constexpr std::uint32_t kMaxIntervalFields = 8;
  // A sealed record and the producer states its packets were drawn from, in storage kept across frames.
  struct RecordSlot {
    present::FrameRecord record;
    present::FrameState state;
  };
  struct SealedRecord {
    const present::FrameRecord *record = nullptr;
    const present::FrameState *state = nullptr;
    bool cut = false;         // a cut was sealed since the drawing record before it
    bool follows = false;     // every record since the drawing record before it was committed and complete
    std::uint64_t screen = 0; // the title's screen key when the record was sealed
  };

  // A slot neither the current record nor the history holds.
  RecordSlot &freeSlot();
  void commitSealed(Core &core, const RecordSlot &sealed, int guestFields);

  // Records `record`'s VRAM writes when it drew nothing, and drops writes older than the oldest held drawing record.
  void holdUndrawnWrites(const present::FrameRecord &record);
  // The latest record that drew `buffer`, or null.
  const SealedRecord *latestDrawing(const gpu::RecordRect &buffer) const;
  // The record a present shows for `buffer`: the latest drawing one, unless a later record wrote the buffer, in
  // which case its picture is stale and the present is the device's.
  const SealedRecord *shownRecord(const gpu::RecordRect &buffer) const;
  const SealedRecord *sealed(std::uint64_t sequence) const;
  // Every drawing record after `from` up to `to` is held, follows its predecessor and is no cut.
  bool continuesTo(std::uint64_t from, std::uint64_t to) const;
  bool cutBetween(std::uint64_t from, std::uint64_t to) const;
  // The record path at 60 fps: a frame of n fields is n presents, the shown record at t = k / n from the one
  // shown before.
  void presentRecords(Core &core, const present::StateProducers *producers, int guestFields);
  // `shown` composed by `composer` at `t` from `from` (a whole frame at t = 1 when null); null when it is drawn as
  // the guest drew it. A refusal is journaled and holds the interval.
  const present::FrameRecord *composeShown(Core &core,
                                           present::FrameComposer &composer,
                                           const present::StateProducers *producers,
                                           const SealedRecord &shown,
                                           const present::FrameState *from,
                                           float t);
  // N's present: `composedFrame` when one was composed, else the shown record as the guest drew it.
  void presentCurrent(Core &core,
                      FieldCadence cadence,
                      const SealedRecord *shown,
                      const present::FrameRecord *composedFrame,
                      const SealedRecord *from = nullptr,
                      float t = 1.0f,
                      int interval = 0);
  // An interval S'->S as the presents see it: `fields` fields between the draw entries shown over `presents`
  // presents, `lead` fields after S's draw entry.
  struct IntervalShape {
    int lead = 0;
    int presents = 0;
    int fields = 0;
  };
  // The latency for `interval`, S's draw entry at display field `entry`: unchanged when the interval allows, never
  // showing a moment again or one before S'.
  int intervalLatency(std::int64_t entry, IntervalShape interval) const;
  // Fields from `record`'s draw entry to now; 0 when it has none.
  static int fieldsSinceDrawEntry(const Core &core, const present::FrameRecord &record);
  // The fields between `from`'s and `to`'s draw entries; `fallback` when either is unknown or out of range.
  static int intervalFields(const present::FrameRecord &from, const present::FrameRecord &to, int fallback);
  // fps60dump: every present, real or in-between, from PSXPORT_FPS60_DUMP_FROM up to a cap.
  void dumpPresent(Core &core, bool interpolated);

  uint64_t fence_ = 0;
  int dumpSequence_ = 0;
  int latency_ = 1;
  // The guest moment, in display fields, the last record present showed; unknown after a present off the record path.
  std::optional<std::int64_t> shownMoment_;
  // The history, the current record and the one being sealed.
  std::array<RecordSlot, kRecordHistory + 2> slots_;
  const present::FrameRecord *current_ = &slots_.front().record;
  std::array<SealedRecord, kRecordHistory> history_; // drawing records, oldest first
  // VRAM written by records that drew nothing, since the oldest held drawing record: the latest sequence per rect.
  struct UndrawnWrite {
    gpu::RecordRect area;
    std::uint64_t sequence = 0;
  };
  std::vector<UndrawnWrite> undrawnWrites_;
  std::optional<std::uint64_t> lastSequence_;
  bool pendingCut_ = false;
  bool pendingBreak_ = false;
  // The record whose picture the last present showed.
  std::optional<std::uint64_t> shownSequence_;
  present::FrameComposer composer_;
  // The interval's last present, composed before its in-betweens are shown and kept for its own present.
  present::FrameComposer endComposer_;
  present::OwnershipVerdict verdict_;
  const present::FrameRecord *composed_ = nullptr;
  bool composedAdvances_ = false;
  PresentStability stability_;
  present::PresentLedger ledger_;
};
} // namespace psx::frame
