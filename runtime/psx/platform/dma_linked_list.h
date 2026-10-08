// What a DMA channel's BCR sync mode MEANS, and what a mode-2 (linked-list) chain says.
//
// This is its own owner because it is a separate question from "which register was written": the sync
// mode decides whether MADR and BCR are addresses-and-sizes at all, and getting that wrong is invisible.
// Reading BCR as if bits 0-1 were not there made a chained transfer run a block count taken from a word
// that is not a size, then clear busy and announce completion — so the guest was told its chain had run
// when no chain had been walked.
//
// MEASURED consequence, and the reason this lives in the framework rather than in a title: Mega Man X4's
// post-movie task programs a chain and polls a guest flag for completion, so with no chain walk the flag
// never arrives and every post-movie frame is one flat clear colour. Any title whose media path chains
// its transfers hits the same wall.
#pragma once

#include <cstdint>

class Core;

namespace psx::dma {

// BCR bits 0-1. Mode 2 IGNORES BCR entirely: MADR points at a chain of `{count:8, next:24}` headers and
// the payload of each node FOLLOWS that node's header.
enum SyncMode : unsigned { kManual = 0, kRequest = 1, kLinkedList = 2 };

[[nodiscard]] unsigned syncMode(std::uint32_t bcr);

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
