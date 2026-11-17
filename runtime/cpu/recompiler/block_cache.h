// block_cache.h — the translated blocks of one executor, found by their entry pc and dropped by the guest ranges
// that executable writes report.
#pragma once

#include "block_scan.h"
#include "guest_address_range.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace psx::cpu::recompiler {

struct JitContext;
// A translated block: returns a BlockEnd code and leaves its exit pc in the context.
using BlockCode = std::uint32_t (*)(JitContext *context);

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
  std::size_t live_ = 0;
};

} // namespace psx::cpu::recompiler
