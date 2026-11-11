// owner_ledger.h — the writer of every main-RAM word: the guest code (block-entry PC, call target, dispatched entry,
// code image) or GP0 port that stored it last.
#pragma once

#include "guest_address_range.h"
#include "image_identity.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class Core;

namespace psx::present {

// A word's writer: 0 for none, else an interned writer.
using OwnerHandle = std::uint32_t;
inline constexpr OwnerHandle kNoOwner = 0;

// Guest code stored the words, or a draw reached GP0 through a port that carries no packet writer.
enum class WriterKind : std::uint8_t { Guest, BlockDma, MmioGp0, LoadImage };

// The code that ran a store, or that drove a port.
struct GuestWriter {
  WriterKind kind = WriterKind::Guest;
  std::uint32_t blockPc = 0;    // block-entry PC
  std::uint32_t function = 0;   // innermost call target the shadow call stack holds; 0 unknown
  std::uint32_t dispatched = 0; // innermost host-dispatched entry (an override or a guest call from the host)
  cpu::ImageIdentity image;     // the code image holding blockPc, when one does
  std::uint64_t revision = 0;   // catalog revision `image` was resolved at
};

const char *writerKindName(WriterKind kind);

// One per Core, fed by Core::bookStore: every store binds its words to the writer running it.
class OwnerLedger {
public:
  explicit OwnerLedger(Core &core);

  // Binds every main-RAM word `store` touches to the running writer.
  void noteStore(GuestSpan store);
  // The writer of the word at `address`; kNoOwner outside main RAM or before any store.
  OwnerHandle ownerOf(std::uint32_t address) const;
  // The writer a GP0 port's words come from: the running code, through `kind`.
  OwnerHandle port(WriterKind kind);
  // The writer behind a handle, or null.
  const GuestWriter *writer(OwnerHandle handle) const;

  // Main RAM was replaced wholesale (a savestate load): nothing has a writer.
  void clear();

private:
  // Distinct writers a run interns; past it every new writer shares the first entry.
  static constexpr std::size_t kMaxWriters = 1u << 16;
  static constexpr std::size_t kWriterSlots = kMaxWriters * 2u;

  OwnerHandle intern(const GuestWriter &writer);

  Core &core_;
  std::vector<OwnerHandle> words_; // per main-RAM word
  std::vector<GuestWriter> writers_;
  std::vector<std::uint32_t> writerSlots_; // open addressing: writer index + 1, 0 empty
  // The last writer interned, what named it and the catalog revision it was resolved at, so a run of stores from one
  // block costs no lookup until an image changes.
  GuestWriter last_{};
  std::uint64_t lastRevision_ = 0;
  OwnerHandle lastHandle_ = kNoOwner;
  bool overflowReported_ = false;
};

} // namespace psx::present
