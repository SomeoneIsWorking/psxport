#include "block_cache.h"

#include "guest_memory_map.h"

#include <algorithm>

namespace psx::cpu::recompiler {

BlockCache::BlockCache() : entries_(kMainRamBytes / 4u, nullptr), codeWords_(kMainRamBytes / 4u / 64u, 0) {}

CompiledBlock *BlockCache::find(std::uint32_t pc) const {
  const std::uint32_t phys = physical(pc);
  if (phys >= kRamMirrorSpan) {
    return nullptr;
  }
  CompiledBlock *const block = entries_[(phys & (kMainRamBytes - 1u)) >> 2];
  return block != nullptr && block->pc == pc ? block : nullptr;
}

CompiledBlock *BlockCache::insert(const CompiledBlock &block) {
  CompiledBlock *&slot = entries_[block.ramBegin >> 2];
  if (slot != nullptr) {
    drop(*slot);
  }
  blocks_.push_back(std::make_unique<CompiledBlock>(block));
  CompiledBlock *const owned = blocks_.back().get();
  slot = owned;
  ++live_;
  for (std::size_t page = owned->ramBegin >> kPageShift; page <= (owned->ramEnd - 1u) >> kPageShift; ++page) {
    pages_[page].push_back(owned);
  }
  for (std::uint32_t word = owned->ramBegin >> 2; word < owned->ramEnd >> 2; ++word) {
    codeWords_[word >> 6] |= std::uint64_t{1} << (word & 63u);
  }
  return owned;
}

void BlockCache::drop(CompiledBlock &block) {
  if (!block.valid) {
    return;
  }
  block.valid = false;
  --live_;
  CompiledBlock *&slot = entries_[block.ramBegin >> 2];
  if (slot == &block) {
    slot = nullptr;
  }
}

// Clears the page's dropped blocks from its list and its words' marks, then marks the words its live blocks hold.
void BlockCache::remarkPage(std::size_t page) {
  std::vector<CompiledBlock *> &list = pages_[page];
  list.erase(std::remove_if(list.begin(),
                            list.end(),
                            [](const CompiledBlock *b) {
                              return !b->valid;
                            }),
             list.end());
  const std::uint32_t first = static_cast<std::uint32_t>(page << kPageShift) >> 2;
  const std::uint32_t last = first + ((1u << kPageShift) >> 2);
  for (std::uint32_t word = first; word < last; word += 64u) {
    codeWords_[word >> 6] = 0;
  }
  for (const CompiledBlock *block : list) {
    const std::uint32_t begin = std::max(block->ramBegin >> 2, first);
    const std::uint32_t end = std::min(block->ramEnd >> 2, last);
    for (std::uint32_t word = begin; word < end; ++word) {
      codeWords_[word >> 6] |= std::uint64_t{1} << (word & 63u);
    }
  }
}

bool BlockCache::invalidateRam(std::uint32_t begin, std::uint32_t end) {
  if (begin >= end) {
    return false;
  }
  bool marked = false;
  for (std::uint32_t word = begin >> 2; word < (end + 3u) >> 2 && !marked; ++word) {
    marked = ((codeWords_[word >> 6] >> (word & 63u)) & 1u) != 0;
    if (!marked && (word & 63u) == 0 && codeWords_[word >> 6] == 0) {
      word += 63u; // a whole clear 64-word group
    }
  }
  if (!marked) {
    return false;
  }
  bool dropped = false;
  for (std::size_t page = begin >> kPageShift; page <= (end - 1u) >> kPageShift; ++page) {
    for (CompiledBlock *block : pages_[page]) {
      if (block->valid && block->ramBegin < end && begin < block->ramEnd) {
        drop(*block);
        dropped = true;
      }
    }
    remarkPage(page);
  }
  return dropped;
}

void BlockCache::clear() {
  std::fill(entries_.begin(), entries_.end(), nullptr);
  std::fill(codeWords_.begin(), codeWords_.end(), 0);
  for (std::vector<CompiledBlock *> &list : pages_) {
    list.clear();
  }
  blocks_.clear();
  live_ = 0;
}

} // namespace psx::cpu::recompiler
