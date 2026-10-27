// owner_ledger.h — the owner of every main-RAM word: a declared producer scope, or the unowned guest writer
// (block-entry PC, call target, dispatched entry, code image) or GP0 port that stored it last.
#pragma once

#include "frame_record.h"
#include "guest_address_range.h"
#include "image_identity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

class Core;

namespace psx::present {

// A word's owner: 0 for none, a scope instance, or an interned writer.
using OwnerHandle = std::uint32_t;
inline constexpr OwnerHandle kNoOwner = 0;

// Guest code stored the words, or a draw reached GP0 through a port that carries no packet owner.
enum class WriterKind : std::uint8_t { Guest, BlockDma, MmioGp0, LoadImage };

// An unowned writer: the code that ran the store, or that drove a port.
struct GuestWriter {
  WriterKind kind = WriterKind::Guest;
  std::uint32_t blockPc = 0;    // block-entry PC
  std::uint32_t function = 0;   // innermost call target the shadow call stack holds; 0 unknown
  std::uint32_t dispatched = 0; // innermost host-dispatched entry (an override or a guest call from the host)
  cpu::ImageIdentity image;     // the code image holding blockPc, when one does
  std::uint64_t revision = 0;   // catalog revision `image` was resolved at
};

const char *writerKindName(WriterKind kind);

// One per Core, fed by Core::bookStore. A store under an open scope binds its words to that scope instance;
// any other store binds them to the writer running it, and a writer storing over a scope's word keeps the pair. Scope
// handles name a ring slot and its epoch, so a word bound by a scope long since reused reads as unowned rather than as
// the slot's new scope.
class OwnerLedger {
public:
  explicit OwnerLedger(Core &core);

  // A handle for a scope instance opened with `key`.
  OwnerHandle openScope(const RecordKey &key);
  // Binds every main-RAM word `store` touches to `scope`, or to the running writer when `scope` is kNoOwner.
  void noteStore(GuestSpan store, OwnerHandle scope);
  // The owner of the word at `address`; kNoOwner outside main RAM.
  OwnerHandle ownerOf(std::uint32_t address) const;
  // A draw list went to the GPU: a writer storing over a word a scope wrote before it is reusing memory, not amending.
  void drawListSubmitted() {
    listStart_ = opened_;
  }
  // The scope whose word the word's writer stored over, until a scope stores it again; kNoOwner when none did.
  OwnerHandle overwrote(std::uint32_t address) const;
  // The writer a GP0 port's words come from: the running code, through `kind`.
  OwnerHandle port(WriterKind kind);

  static bool isScope(OwnerHandle handle) {
    return handle != kNoOwner && (handle & kWriterBit) == 0;
  }
  static bool isWriter(OwnerHandle handle) {
    return (handle & kWriterBit) != 0;
  }
  // The key of a live scope handle; nullopt for a writer, none, or a scope its slot has since been reused by.
  std::optional<RecordKey> scopeKey(OwnerHandle handle) const;
  // The writer behind a writer handle, or null.
  const GuestWriter *writer(OwnerHandle handle) const;

  // Main RAM was replaced wholesale (a savestate load): nothing is owned.
  void clear();

private:
  static constexpr OwnerHandle kWriterBit = 0x80000000u;
  static constexpr std::size_t kScopeSlots = 1u << 16;
  static constexpr std::uint32_t kEpochMask = 0x7FFFu;
  // Distinct writers a run interns; past it every new writer shares the first entry.
  static constexpr std::size_t kMaxWriters = 1u << 16;
  static constexpr std::size_t kWriterSlots = kMaxWriters * 2u;

  struct ScopeSlot {
    RecordKey key;
    std::uint32_t epoch = 0;
    std::uint64_t opened = 0; // scopes opened before it
  };

  OwnerHandle intern(const GuestWriter &writer);
  // A live scope opened since the last draw list was submitted.
  bool inDrawList(OwnerHandle handle) const;
  OwnerHandle runningWriter();

  Core &core_;
  std::vector<OwnerHandle> words_;     // per main-RAM word
  std::vector<OwnerHandle> overwrote_; // per main-RAM word: the scope its writer replaced
  std::vector<ScopeSlot> scopes_;
  std::uint32_t nextScope_ = 0;
  std::uint32_t epoch_ = 1;
  std::uint64_t opened_ = 0;
  std::uint64_t listStart_ = 0; // scopes opened before the last draw list was submitted
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
