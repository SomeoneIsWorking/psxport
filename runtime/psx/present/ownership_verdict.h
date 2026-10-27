// ownership_verdict.h — whether an interval S'→S can be composed: the draws in S with no owner, grouped by
// writer and OT table, each either drawn the same in S' (still) or moving (the interval is held).
#pragma once

#include "frame_record.h"
#include "owner_journal.h"
#include "owner_ledger.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace psx::present {

class OwnershipVerdict {
public:
  // Judges S' (`from`) to S (`to`); true when every unowned group of S is drawn word for word as in S' and no writer
  // amended an owned packet.
  bool judge(const FrameRecord &from, const FrameRecord &to, const OwnerLedger &ledger);
  // The last judgement's findings: moving unowned and port groups, and amended packets.
  std::span<const OwnerFinding> findings() const {
    return findings_;
  }

private:
  // Unowned draws of one writer in one table, as one ordered picture.
  struct Group {
    WriterKind kind = WriterKind::Guest;
    std::uint32_t code = 0; // the call target, else the block PC; 0 when no writer is known
    std::uint32_t dispatched = 0;
    std::uint16_t table = kNoTable;
    bool amended = false; // the writer stored over words a scope wrote
    std::uint32_t producer = 0;
    std::uint64_t hash = 0;
    std::uint32_t count = 0;
    std::uint32_t bucketLo = 0xFFFFFFFFu;
    std::uint32_t bucketHi = 0;
    OwnerHandle sample = kNoOwner;
    std::uint32_t sampleAddress = 0;
    std::uint8_t sampleCommand = 0;

    bool sameKey(const Group &other) const {
      return kind == other.kind && code == other.code && dispatched == other.dispatched && table == other.table &&
             amended == other.amended;
    }
  };

  static void collect(const FrameRecord &record, const OwnerLedger &ledger, std::vector<Group> &groups);

  std::vector<Group> from_;
  std::vector<Group> to_;
  std::optional<std::uint64_t> toSequence_; // the record to_ was collected from
  std::vector<OwnerFinding> findings_;
};

} // namespace psx::present
