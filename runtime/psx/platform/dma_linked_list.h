// What a DMA channel's CHCR sync mode MEANS, and what a mode-2 (linked-list) chain says.
//
// This is its own owner because it is a separate question from "which register was written": the sync
// mode decides whether MADR and BCR are addresses-and-sizes at all. It is CHCR bits 9-10; BCR is only
// the block size and count, so a block size whose low bits read 2 (98 words, a partial last sector)
// must never select a chain.
#pragma once

#include <cstdint>

class Core;

namespace psx::dma {

// CHCR bits 9-10. Mode 2 IGNORES BCR entirely: MADR points at a chain of `{count:8, next:24}` headers and
// the payload of each node FOLLOWS that node's header.
enum SyncMode : unsigned { kManual = 0, kRequest = 1, kLinkedList = 2 };

[[nodiscard]] unsigned syncMode(std::uint32_t chcr);

// A hard ceiling on a chain walk, because the chain is guest RAM and a cycle or a bad `next` would
// otherwise spin here forever. 0x10000 words is 256 KiB, larger than any single chain a PSX title uses,
// and the same ceiling mem.cpp's block transfers already apply.
inline constexpr int kChainWordCap = 0x10000;

// Walk a mode-2 chain from `madr`.
//
// Returns the number of words the chain covers, or 0 with `*refused` set when the walk hit the ceiling —
// and a refusal is a REFUSAL, not a truncation, because transferring the counted part and then announcing
// completion is the exact failure this owner exists to remove. `*endMadr` receives one past the last word
// the chain would write, which is what hardware leaves in MADR and what a guest resuming from MADR needs.
[[nodiscard]] int chainWords(Core &core, std::uint32_t madr, std::uint32_t *endMadr, bool *refused);

} // namespace psx::dma
