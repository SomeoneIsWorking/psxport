// block_cache.h — the translated blocks of one executor, found by their entry pc and dropped by the guest ranges
// that executable writes report.
#pragma once

#include "block_scan.h"
#include "guest_address_range.h"
#include "host_dispatch_cache.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace psx::cpu::recompiler {

struct JitContext;
// A translated block: returns a BlockEnd code and leaves its exit pc in the context.
using BlockCode = std::uint32_t (*)(JitContext *context);

// Block linking (opt-in per backend). A link site is one patchable direct exit of a translated block: unlinked it
// leaves for the dispatcher like any exit; linked it jumps to the target block's linked entry, which repeats the
// dispatcher's between-block checks and runs the executor's block entry before the body (JitContext,
// kHelperEnterLinked).
struct LinkSite {
  std::uint32_t targetPc = 0;
  void *site = nullptr;
};
// What a linking backend's translator returns: the block's entry, its linked entry, and its link sites.
struct LinkedCode {
  BlockCode code = nullptr;
  const void *linkedEntry = nullptr;
  std::vector<LinkSite> sites;
};

struct CompiledBlock;
// A link site of a block and the block it is patched to, if any.
struct OutgoingLink {
  LinkSite site;
  CompiledBlock *linkedTo = nullptr;
};

struct CompiledBlock {
  // The virtual entry pc it was translated at (KUSEG/KSEG0/KSEG1 names differ in their jump targets).
  std::uint32_t pc = 0;
  // The main-RAM bytes it was read from, [begin, end).
  std::uint32_t ramBegin = 0;
  std::uint32_t ramEnd = 0;
  // The GPRs its first instruction reads or writes or loads: a load still in flight to one of them at entry sends
  // the block to the interpreter, which runs the delay slot exactly; any other lands before the block runs.
  std::uint32_t entryConflicts = 0;
  // Null for a block the interpreter runs, for `refusal`.
  BlockCode code = nullptr;
  // The block entry's dispatch verdict, for entries made through a link (NativeDispatcher::classify).
  RememberedVerdict verdict;
  // Null unless the backend links blocks.
  const void *linkedEntry = nullptr;
  std::vector<OutgoingLink> sites;
  // The sites patched to this block, as (block, site index); unpatched when this block is dropped.
  std::vector<std::pair<CompiledBlock *, std::uint32_t>> incoming;
  Refusal refusal = Refusal::None;
  bool valid = true;
};

// Blocks indexed by the main-RAM word of their entry. Every invalidation reports a guest range after the write is
// visible (runtime/cpu/invalidation.cpp); each translated word is marked, so a store to a word no block was read
// from costs one bit test. Invalidated blocks leave the index at once and their code stays mapped until `clear`, so
// a block that overwrote itself still runs to its own exit.
class BlockCache {
public:
  BlockCache();

  // The valid block entered at virtual `pc`, or null.
  [[nodiscard]] CompiledBlock *find(std::uint32_t pc) const;
  // Takes a translated block; one already indexed at its entry word is dropped.
  CompiledBlock *insert(const CompiledBlock &block);
  // Drops every block read from any byte of main RAM [begin, end); true when one was dropped.
  bool invalidateRam(std::uint32_t begin, std::uint32_t end);
  // Drops every block.
  void clear();
  // Called for each block as it is dropped (not on clear), before its entry leaves the index.
  void onDrop(void (*hook)(void *, CompiledBlock &), void *context) {
    dropHook_ = hook;
    dropContext_ = context;
  }

  [[nodiscard]] std::size_t liveBlocks() const {
    return live_;
  }

private:
  static constexpr std::uint32_t kPageShift = 12;
  static constexpr std::size_t kPages = std::size_t{1} << (21 - kPageShift);

  void drop(CompiledBlock &block);
  void remarkPage(std::size_t page);

  std::vector<CompiledBlock *> entries_;
  // One bit per main-RAM word that some live block was read from.
  std::vector<std::uint64_t> codeWords_;
  // The blocks overlapping each 4 KiB page, live or dropped since the page was last re-marked.
  std::array<std::vector<CompiledBlock *>, kPages> pages_;
  std::vector<std::unique_ptr<CompiledBlock>> blocks_;
  void (*dropHook_)(void *, CompiledBlock &) = nullptr;
  void *dropContext_ = nullptr;
  std::size_t live_ = 0;
};

} // namespace psx::cpu::recompiler
