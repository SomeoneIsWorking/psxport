// owner_journal.h — the rows an agent turns into work: draws with no owner, draws through a port, baked or
// amended draws, refused redraws and held intervals, each with the writer, the OT span, the screen and its warp.
#pragma once

#include "owner_ledger.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

class Core;

namespace psx::present {

enum class OwnerReason : std::uint8_t {
  UnownedMoving, // guest-written draws with no owner that S' did not draw the same
  Port,          // draws that reached GP0 through block DMA, MMIO or LoadImage and moved
  Baked,         // an owned object whose pixels a texture-feedback upload baked
  Overwrote,     // a writer stored over a word a declared owner wrote
  ReplayRefused, // a guest redraw that reached a device or a syscall
  IntervalHeld,  // the interval was shown at the logic rate for `HeldCause`
};

enum class HeldCause : std::uint8_t {
  None,
  Cut,             // the title declared a cut
  StaleShown,      // no drawing record holds the displayed buffer, or a later write replaced it
  NoPrevious,      // the record shown before is gone from the history or was never shown
  ChainBreak,      // a sequence gap or an incomplete record between S' and S
  NoState,         // an owned object with no render or no saved state
  ProducerRefused, // a producer's composition refused t
  Unplaceable,     // a render named a slot S never walked, or the CLUT pool was full
  Unpaired,        // an owned object S has and S' did not draw (no from state, no rest)
};

const char *ownerReasonName(OwnerReason reason);
const char *heldCauseName(HeldCause cause);

inline constexpr std::uint16_t kNoTable = 0xFFFFu;

// One interval's finding before it lands in a row.
struct OwnerFinding {
  OwnerReason reason = OwnerReason::UnownedMoving;
  HeldCause cause = HeldCause::None;
  OwnerHandle writer = kNoOwner; // the unowned writer or port, a sample of its group
  std::uint32_t producer = 0;    // the declared owner involved, 0 when none
  std::uint16_t table = kNoTable;
  std::uint32_t bucketLo = 0;
  std::uint32_t bucketHi = 0;
  std::uint32_t primitives = 0;
  std::uint32_t sampleAddress = 0;
  std::uint8_t sampleCommand = 0;
};

// A finding's aggregate on one screen.
struct OwnerRow {
  OwnerFinding key;
  GuestWriter writer; // copied, so the row outlives a ledger clear
  std::size_t screen = 0;
  std::uint64_t intervals = 0; // intervals the finding was seen in
  std::uint64_t intervalsLost = 0;
  std::uint64_t primitives = 0;
  std::uint32_t maxPrimitives = 0;
  std::uint32_t firstField = 0;
  std::uint32_t lastField = 0;
  std::uint64_t firstPresent = 0;
  std::uint64_t lastPresent = 0;
};

class OwnerJournal {
public:
  OwnerJournal();

  // The screen the machine shows now; a change writes the journal file first.
  void observeScreen(Core &core);
  // One finding of the interval being presented; `lost` when that interval was held.
  void record(Core &core, const OwnerFinding &finding, bool lost);
  // The rows, most intervals lost first, at most `limit`, one line each, for the `owners` verb.
  std::string describe(std::size_t limit) const;
  // Every row as JSONL to `path`; false when it cannot be written.
  bool writeJsonl(Core &core, const std::string &path) const;
  // Writes the journal file and logs the row count; at run end.
  void flush(Core &core) const;
  std::span<const OwnerRow> rows() const {
    return rows_;
  }

private:
  struct Screen {
    std::uint64_t key = 0;
    std::string route; // the verb line that reaches it, empty when none does
  };
  static constexpr std::size_t kMaxRows = 4096;
  static constexpr std::size_t kMaxScreens = 256;

  std::string rowJson(Core &core, const OwnerRow &row) const;
  std::string path(Core &core) const;

  std::vector<OwnerRow> rows_;
  std::vector<Screen> screens_;
  std::size_t screen_ = 0;
  bool screenKnown_ = false;
  bool fullReported_ = false;
  std::string stamp_; // run start, names the file
};

} // namespace psx::present
