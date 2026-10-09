// DMA sync mode (CHCR bits 9-10) decides what MADR and BCR MEAN. Reading it from BCR bits 0-1 made a
// plain block transfer of a size ending in binary 10 a linked-list walk: Tekken 3's 98-word last CD
// sector landed nowhere and the file's tail stayed zero.
//
// So these cases pin the DISPATCH (which shape each mode takes) and the three ways a guest's chain can be
// wrong, each of which must be handled by a stated rule rather than by trusting guest RAM.
#include "testutil.h"

#include <cstdint>

#include "core.h"
#include "game.h"

namespace {

constexpr uint32_t kDma3Madr = 0x1F8010B0;
constexpr uint32_t kDma3Bcr = 0x1F8010B4;
constexpr uint32_t kDma3Chcr = 0x1F8010B8;
constexpr uint32_t kStart = 0x01000100u; // bit 24 start, bit 0 RAM -> CDROM is what DMA3 wants
constexpr uint32_t kRequestMode = 1u << 9;
constexpr uint32_t kChainMode = 2u << 9;
constexpr uint32_t kChainEnd = 0x00FFFFFFu;

// A chain is `{count:24, next:24}` header words, and the payload FOLLOWS each header. Node A at
// kBaseA holds 2 payload words then points at node B; node B holds 3 then ends.
//
// `next` is 24 BITS OF PHYSICAL ADDRESS and `count` is 24 bits of word count, so a fixture that writes a
// KSEG0 guest address into `next` silently truncates it to a physical address the map does not hold — and
// the walk then lands somewhere unrelated and looks exactly like a broken walk. That is a test bug this
// file made and it is worth the comment, because a reader would otherwise trust the fixture.
constexpr uint32_t kBaseA = 0x80010000u;
constexpr uint32_t kBaseB = 0x80010100u;
constexpr uint32_t kPhysA = kBaseA & 0x00FFFFFFu;
constexpr uint32_t kPhysB = kBaseB & 0x00FFFFFFu;
constexpr uint32_t kBlockBase = 0x80010200u;
constexpr uint32_t kMarker = 0x80010300u;

void build_two_node_chain(Core &core) {
  core.mem_w32(kBaseA + 0, 0x02000000u | kPhysB); // 2 words, next -> B
  core.mem_w32(kBaseA + 4, 0x11111111u);
  core.mem_w32(kBaseA + 8, 0x22222222u);
  core.mem_w32(kBaseB + 0, 0x03000000u | kChainEnd); // 3 words, end
  core.mem_w32(kBaseB + 4, 0x33333333u);
  core.mem_w32(kBaseB + 8, 0x44444444u);
  core.mem_w32(kBaseB + 12, 0x55555555u);
  // The block-mode control: the words a mode-1 transfer must land on. Unrelated to the chain, so a
  // chain bug cannot quietly make the block case look right.
  for (int i = 0; i < 8; i++) {
    core.mem_w32(kBlockBase + (uint32_t)i * 4u, 0u);
  }
  core.mem_w32(kMarker, 0xA5A5A5A5u);
}

void program(Core &core, uint32_t madr, uint32_t bcr) {
  core.mem_w32(kDma3Madr, madr);
  core.mem_w32(kDma3Bcr, bcr);
}

// MODE 2 IS A CHAIN, NOT A BLOCK COUNT. The control is a BCR whose low word is a plausible size: if the
// handler read the BCR as a block count it would move that many words to the HEAD address and leave the
// chain nodes untouched, which is exactly the bug.
static void test_a_chained_transfer_walks_the_nodes() {
  auto *game = new Game();
  Core &core = game->core;
  build_two_node_chain(core);
  // The fixture gives mode 2 a DELIBERATELY block-shaped BCR: a handler that ignored the sync mode would
  // move 8 words from the head and never reach node B.
  program(core, kBaseA, 0x0008u | (1u << 16));
  core.mem_w32(kDma3Chcr, kStart | kChainMode);
  // The FIFO is empty in this fixture, so the sector stream reads as controller-zero. What is under
  // test is WHERE the words landed, not what they were.
  CHECK_EQ(core.mem_r32(kBaseA + 4), 0u); // node A payload was written
  CHECK_EQ(core.mem_r32(kBaseA + 8), 0u);
  CHECK_EQ(core.mem_r32(kBaseB + 4), 0u); // and node B's, which a head-only transfer never reaches
  CHECK_EQ(core.mem_r32(kBaseB + 12), 0u);
  CHECK_EQ(core.mem_r32(kBlockBase), 0u);       // the mode-1 destination is untouched by a chain
  CHECK_EQ(core.mem_r32(kMarker), 0xA5A5A5A5u); // and so is everything else
  // MADR advances to one past the chain's last written word — node B's header at 0x80010100, plus its
  // 4-byte header and 3 payload words — which is what hardware leaves and what a guest resuming from
  // MADR needs. It is stored MASKED, because MADR's upper bits are unused and a block transfer leaves
  // the guest's own value alone, so the two modes do not agree on width and this pins the chained one.
  CHECK_EQ(core.mem_r32(kDma3Madr), (kBaseB + 4u + 3u * 4u) & 0x1FFFFCu);
  CHECK_EQ(core.mem_r32(kDma3Chcr) & 0x01000000u, 0u); // busy cleared: the completion poll must pass
}

// MODE 1 IS UNCHANGED. A regression here would break every libcd title, because Sony's libcd fetches
// every sector with a block-mode DMA3 and MMX4's working STR playback is exactly that.
static void test_a_block_transfer_still_uses_the_block_count() {
  auto *game = new Game();
  Core &core = game->core;
  build_two_node_chain(core);
  program(core, kBlockBase, 0x0004u | (2u << 16)); // mode 1, 4 words x 2 blocks = 8
  core.mem_w32(kDma3Chcr, kStart | kRequestMode);
  CHECK_EQ(core.mem_r32(kBlockBase), 0u);
  CHECK_EQ(core.mem_r32(kBlockBase + 28u), 0u);
  // A block transfer's MADR is the guest's own value: the hardware does not walk a list, so advancing
  // MADR here would break a guest that reuses one MADR for a run of sector reads.
  CHECK_EQ(core.mem_r32(kDma3Madr), kBlockBase);
  CHECK_EQ(core.mem_r32(kBaseA + 4), 0x11111111u); // and the chain is untouched
}

// A SELF-LINK is the cheapest cycle a guest can write into its own RAM. It must not hang the host, and
// the words already counted must still complete so the guest is not left polling a busy channel forever.
static void test_a_self_linked_chain_terminates_and_completes() {
  auto *game = new Game();
  Core &core = game->core;
  build_two_node_chain(core);
  core.mem_w32(kBaseA + 0, 0x01000000u | kPhysA); // 1 word, next -> itself
  program(core, kBaseA, 0u);
  core.mem_w32(kDma3Chcr, kStart | kChainMode);
  CHECK_EQ(core.mem_r32(kDma3Chcr) & 0x01000000u, 0u);
  CHECK_EQ(core.mem_r32(kBaseA + 4), 0u);
}

// A COUNT over the ceiling is refused with a stated bound rather than trusted, because the chain is guest
// RAM and `count` is 24 bits of whatever the guest had there.
static void test_an_oversized_count_stops_at_the_ceiling() {
  auto *game = new Game();
  Core &core = game->core;
  build_two_node_chain(core);
  // THE COUNT FIELD IS 8 BITS. A linked-list header is `{count:8, next:24}`, so ONE node can declare at
  // most 255 words and the 0x10000-word ceiling is only reachable with 257 or more of them. A fixture
  // that writes `0xFF000000` and calls it oversized declares 255 words and transfers happily, which is
  // exactly the kind of test that makes a bound look covered when nothing exercised it. So this builds
  // the long chain the ceiling actually exists for.
  constexpr uint32_t kLongBase = 0x80011000u;
  constexpr int kNodes = 300; // 300 * 255 words = 76,500, well past the 0x10000 ceiling
  for (int i = 0; i < kNodes; i++) {
    const uint32_t at = kLongBase + (uint32_t)i * 0x200u;
    const uint32_t nxt = (i + 1 < kNodes) ? ((at + 0x200u) & 0x00FFFFFFu) : kChainEnd;
    core.mem_w32(at, 0xFF000000u | nxt);
    core.mem_w32(at + 4, 0x5A5A5A5Au); // a marker the refusal must leave alone
  }
  program(core, kLongBase, 0u);
  core.mem_w32(kDma3Chcr, kStart | kChainMode);
  // It terminates and clears busy rather than walking 76,500 words: the ceiling is a REFUSAL, and a
  // guest is never left polling a busy channel it cannot clear.
  CHECK_EQ(core.mem_r32(kDma3Chcr) & 0x01000000u, 0u);
  // The first node's payload is still its own value, so the walk stopped before transferring: a ceiling
  // that merely slowed the path down would have zeroed it.
  CHECK_EQ(core.mem_r32(kLongBase + 4), 0x5A5A5A5Au);
}

// A CHAIN ENDED BY DESIGN transfers nothing and still completes, so a guest that arms an empty chain
// does not wedge.
static void test_an_empty_chain_completes() {
  auto *game = new Game();
  Core &core = game->core;
  build_two_node_chain(core);
  core.mem_w32(kBaseA + 0, 0x00000000u | kChainEnd);
  program(core, kBaseA, 0u);
  core.mem_w32(kDma3Chcr, kStart | kChainMode);
  CHECK_EQ(core.mem_r32(kDma3Chcr) & 0x01000000u, 0u);
}

// A BLOCK SIZE ENDING IN BINARY 10 IS STILL A BLOCK. The last sector of a Tekken 3 file reads 98 words
// (BCR 0x00010062); BCR bits 0-1 are size bits, not a sync mode.
static void test_a_block_size_ending_in_binary_10_is_not_a_chain() {
  auto *game = new Game();
  Core &core = game->core;
  build_two_node_chain(core);
  for (uint32_t i = 0; i < 98u; i++) {
    core.mem_w32(kBlockBase + i * 4u, 0xCCCCCCCCu);
  }
  program(core, kBlockBase, 0x00010062u);
  core.mem_w32(kDma3Chcr, kStart);
  CHECK_EQ(core.mem_r32(kBlockBase), 0u); // the first and the 98th word were written by the block
  CHECK_EQ(core.mem_r32(kBlockBase + 97u * 4u), 0u);
  CHECK_EQ(core.mem_r32(kDma3Madr), kBlockBase); // a block leaves the guest's MADR alone
}

} // namespace

int main() {
  RUN(a_block_size_ending_in_binary_10_is_not_a_chain);
  RUN(a_chained_transfer_walks_the_nodes);
  RUN(a_block_transfer_still_uses_the_block_count);
  RUN(a_self_linked_chain_terminates_and_completes);
  RUN(an_oversized_count_stops_at_the_ceiling);
  RUN(an_empty_chain_completes);
  return pt_summary();
}
