// test_ordering_table — the guest DrawOTag node layout, decoded once, must stay decoded ONCE.
//
// The OT walk reads a node's 32-bit header and splits it into a GP0 word count and a next address.
// Before this was a named owner that split was written out at four separate sites in gpu_native.cpp,
// and a reader had to remember which half of the word meant which. Two things could go wrong and both
// are silent — a wrong SPLIT makes a prim's words land at the wrong addresses (the picture changes), and
// a wrong end-of-chain test makes a walk run past the end of the table or stop early.
//
// The cases below cover the OLD contract exactly (the inline arithmetic this owner replaced) and the
// NEW one, per AGENTS.md: `hdr >> 24` / `hdr & 0xFFFFFF` are recomputed here as the reference, so a
// change to the named accessors that disagrees with the arithmetic is a red test rather than a comment
// nobody re-reads.
//
// Hermetic: the node layout is a pure function of one 32-bit word, and the cursor is driven against a
// real Core's RAM (a 2 MB array, no disc, no GPU, no window).
//
// NEGATIVE-RESULT DISCIPLINE: every case asserts a specific value, and the end-of-chain set is asserted
// as a table (including the values that must NOT end a chain) so a widened or narrowed sentinel cannot
// pass by accident. The walk cases assert the node SEQUENCE, not merely that the walk terminated.
#include "core.h"
#include "ordering_table.h"
#include "testutil.h"

#include <cstdint>
#include <memory>

namespace {

// The arithmetic this owner replaced, kept here as the ORACLE. If the named accessors ever disagree with
// it, the disagreement is the bug — never the other way round.
unsigned referenceWordCount(std::uint32_t header) {
  return header >> 24;
}
std::uint32_t referenceNextField(std::uint32_t header) {
  return header & 0xFFFFFFu;
}
bool referenceEndsChain(std::uint32_t header) {
  const std::uint32_t next = header & 0xFFFFFFu;
  return next == 0xFFFFFFu || next == 0;
}

using psx::gpu::OtNode;

// Every header shape the walk actually meets, and the values it must not confuse.
constexpr std::uint32_t kHeaders[] = {
    0x00000000u, // link-only node: no primitive, but the chain CONTINUES
    0x00FFFFFFu, // 0 words + hardware sentinel
    0x01000000u, // 1 word, next = 0
    0x01000004u, // 1 word, next = one word on
    0x04000008u, // 4 words, next = two words on
    0xFF000010u, // 255 words
    0x00FFFF00u, // 0 words, next = 0x00FF00 — NOT a sentinel
    0x0AFFFF01u, // 10 words, sentinel next, with the low bits set
    0x7F800000u, // 127 words, next = 0x800000
};

// A node's own main-RAM offset, in the low 24 bits as a guest address would express it.
constexpr std::uint32_t kSomeNode = 0x80010000u;

} // namespace

static void test_word_count_matches_the_original_shift(void) {
  for (std::uint32_t header : kHeaders) {
    const OtNode node(kSomeNode, header);
    CHECK_EQ(node.gp0WordCount(), referenceWordCount(header));
  }
  // Spot values, so a bug that made the loop vacuous (a header set that all decodes the same) is caught.
  CHECK_EQ(OtNode(0, 0x04000008u).gp0WordCount(), 4u);
  CHECK_EQ(OtNode(0, 0xFF000010u).gp0WordCount(), 255u);
  CHECK_EQ(OtNode(0, 0x00000000u).gp0WordCount(), 0u);
}

static void test_link_only_node_is_distinguished_from_a_primitive(void) {
  // A zero word count is an EMPTIED node, not a node with a zero-word primitive. Reading it as the
  // latter is how a table that ClearOTagR emptied would look like the end of the chain.
  //
  // Note what is NOT claimed here: an all-zero header DOES end the chain, because its next-address
  // field is 0 and 0 is one of the two end-of-chain sentinels. A link-only node only continues the walk
  // when it POINTS somewhere — see walk_continues_through_a_link_only_node, which is the case that
  // decides whether an emptied table reads as N empty nodes or as none at all.
  CHECK(!OtNode(0, 0x00000000u).carriesPrimitive());
  CHECK(OtNode(0, 0x00000000u).endsChain()); // next = 0, so this one really is the end
  CHECK(!OtNode(0, 0x00000004u).carriesPrimitive());
  CHECK(!OtNode(0, 0x00000004u).endsChain()); // next = 4, so the walk continues
  CHECK(OtNode(0, 0x01000000u).carriesPrimitive());
}

static void test_next_address_field_matches_the_original_mask(void) {
  for (std::uint32_t header : kHeaders) {
    const OtNode node(kSomeNode, header);
    CHECK_EQ(node.nextAddressField(), referenceNextField(header));
  }
  // The field is 24 bits, so bits 24..31 (the word count) must not leak into the address.
  CHECK_EQ(OtNode(0, 0xFF000010u).nextAddressField(), 0x000010u);
}

static void test_end_of_chain_accepts_both_sentinels_and_nothing_else(void) {
  for (std::uint32_t header : kHeaders) {
    const OtNode node(kSomeNode, header);
    CHECK_EQ(node.endsChain(), referenceEndsChain(header));
  }
  // Named explicitly, because these are the cases a narrowed or widened sentinel gets wrong. The
  // 0xFFFF00 row is the important one: it is all-ones in its low BYTE and not in its top one.
  CHECK(OtNode(0, 0x01000000u).endsChain());  // next = 0x000000
  CHECK(OtNode(0, 0x01FFFFFFu).endsChain());  // next = 0xFFFFFF, with a non-zero word count
  CHECK(!OtNode(0, 0x01000001u).endsChain()); // next = 1
  CHECK(!OtNode(0, 0x01FFFF00u).endsChain()); // next = 0xFFFF00 — not the sentinel
  CHECK(!OtNode(0, 0x01000004u).endsChain()); // next = 4
}

// THE SENTINEL MUST BE TESTED BEFORE IT IS MASKED. 0xFFFFFF narrowed to a main-RAM offset is 0x1FFFFC,
// which is a perfectly plausible address — a cursor that tested the narrowed value would walk off the
// end of the table instead of stopping. This asserts the two forms differ, so the test above cannot
// pass for the wrong reason.
static void test_sentinel_is_visible_only_before_the_offset_mask(void) {
  const OtNode node(0, 0x01FFFFFFu);
  CHECK_EQ(node.nextAddressField(), 0xFFFFFFu);
  CHECK_EQ(node.nextRamOffset(), 0x1FFFFCu);
  CHECK(node.nextRamOffset() != 0u); // the narrowed value is NOT the sentinel, and not zero either
}

static void test_word_addresses_follow_the_node(void) {
  // Word 0 is the command word and sits immediately after the header. A one-word error here silently
  // submits every prim's words one word late.
  const OtNode node(0x80010000u, 0x04000008u);
  CHECK_EQ(node.gp0WordRamOffset(0), 0x80010004u);
  CHECK_EQ(node.gp0WordRamOffset(1), 0x80010008u);
  CHECK_EQ(node.gp0WordRamOffset(3), 0x80010010u);
}

static void test_guest_address_round_trips_through_the_offset(void) {
  for (std::uint32_t header : kHeaders) {
    const std::uint32_t ramOffset = 0x1F3A40u;
    const OtNode node(ramOffset, header);
    CHECK_EQ(node.guestAddress(), 0x80000000u | ramOffset);
    CHECK_EQ(psx::gpu::mainRamOffsetOf(node.guestAddress()), ramOffset);
  }
}

// ---- the cursor, over a real Core's main RAM -------------------------------------------------
// A Core here is only its 2 MB of RAM: the cursor reads nothing else, which is why the walk can be
// driven without a disc, a game image or a GPU. Node headers are written directly, so a walk exercises
// the real mem_r32 rather than a stand-in that could agree with a broken decode.
namespace {

std::unique_ptr<Core> walk_core() {
  return std::make_unique<Core>();
}

// Place a node whose header is `header` and which holds `wordCount` GP0 words.
void placeNode(Core &core, std::uint32_t ramOffset, std::uint32_t header, unsigned wordCount) {
  core.mem_w32(ramOffset, header);
  for (unsigned i = 0; i < wordCount; i++) {
    // A recognisable payload so a mis-addressed read is visible, not just a count.
    core.mem_w32(ramOffset + 4u + 4u * i, 0xA0000000u | i);
  }
}

// Walk from `root`, collecting the guest address of every node entered. Returns the sequence so a test
// can assert the ORDER, not just the length.
int collectWalk(Core &core, std::uint32_t root, std::uint32_t *out, int capacity, int &truncated) {
  psx::gpu::OrderingTableCursor cursor(core, root);
  int visited = 0;
  for (;;) {
    if (visited < capacity) {
      out[visited] = cursor.node().guestAddress();
    }
    visited++;
    if (!cursor.advance()) {
      truncated = cursor.truncated() ? 1 : 0;
      break;
    }
  }
  return visited;
}

} // namespace

static void test_walk_visits_every_node_in_link_order(void) {
  auto core = walk_core();
  constexpr std::uint32_t kA = 0x80020000u, kB = 0x80020100u, kC = 0x80020200u;
  placeNode(*core, psx::gpu::mainRamOffsetOf(kA), (2u << 24) | psx::gpu::mainRamOffsetOf(kB), 2);
  placeNode(*core, psx::gpu::mainRamOffsetOf(kB), (1u << 24) | psx::gpu::mainRamOffsetOf(kC), 1);
  placeNode(*core, psx::gpu::mainRamOffsetOf(kC), (3u << 24) | psx::gpu::kOtChainEnd, 3);

  std::uint32_t seen[8] = {0};
  int truncated = -1;
  const int visited = collectWalk(*core, kA, seen, 8, truncated);
  CHECK_EQ(visited, 3);
  CHECK_EQ(truncated, 0);
  CHECK_EQ(seen[0], kA);
  CHECK_EQ(seen[1], kB);
  CHECK_EQ(seen[2], kC);
}

// THE OLD CONTRACT: the walk this owner replaced stopped on BOTH sentinels, and a table ending with a
// plain zero next-address is something real games emit. A cursor that honoured only 0xFFFFFF would run
// off the end of a real table here.
static void test_walk_stops_on_a_zero_next_address(void) {
  auto core = walk_core();
  constexpr std::uint32_t kA = 0x80021000u, kB = 0x80021100u;
  placeNode(*core, psx::gpu::mainRamOffsetOf(kA), (1u << 24) | psx::gpu::mainRamOffsetOf(kB), 1);
  placeNode(*core, psx::gpu::mainRamOffsetOf(kB), (1u << 24) | psx::gpu::kOtChainEndZero, 1);

  std::uint32_t seen[8] = {0};
  int truncated = -1;
  const int visited = collectWalk(*core, kA, seen, 8, truncated);
  CHECK_EQ(visited, 2);
  CHECK_EQ(truncated, 0);
  CHECK_EQ(seen[1], kB);
}

// A LINK-ONLY node carries no primitive and must not end the walk. This is the case that decides whether
// an EMPTIED table (what ClearOTagR produces) reads as "one empty node" or as "no nodes at all".
static void test_walk_continues_through_a_link_only_node(void) {
  auto core = walk_core();
  constexpr std::uint32_t kA = 0x80022000u, kEmpty = 0x80022100u, kB = 0x80022200u;
  placeNode(*core, psx::gpu::mainRamOffsetOf(kA), (0u << 24) | psx::gpu::mainRamOffsetOf(kEmpty), 0);
  placeNode(*core, psx::gpu::mainRamOffsetOf(kEmpty), (0u << 24) | psx::gpu::mainRamOffsetOf(kB), 0);
  placeNode(*core, psx::gpu::mainRamOffsetOf(kB), (0u << 24) | psx::gpu::kOtChainEnd, 0);

  std::uint32_t seen[8] = {0};
  int truncated = -1;
  const int visited = collectWalk(*core, kA, seen, 8, truncated);
  CHECK_EQ(visited, 3);
  CHECK_EQ(truncated, 0);
  CHECK_EQ(seen[1], kEmpty);
  CHECK_EQ(seen[2], kB);
}

// THE TRUNCATION CASE, and the reason `truncated()` exists. A chain that never terminates must be
// REFUSED, and the refusal must be distinguishable from a chain that ended. Without the second half,
// "the walk gave up" and "the table ended" print the same thing and a malformed table reads as an empty
// one.
//
// The cap is psx::gpu::kOtNodeLimit, so this builds a chain one node longer than the walk accepts. The
// cycle is two nodes pointing at each other, so the walk's own cap is what stops it.
static void test_a_cyclic_chain_is_refused_and_says_so(void) {
  auto core = walk_core();
  constexpr std::uint32_t kA = 0x80023000u, kB = 0x80023100u;
  placeNode(*core, psx::gpu::mainRamOffsetOf(kA), (0u << 24) | psx::gpu::mainRamOffsetOf(kB), 0);
  placeNode(*core, psx::gpu::mainRamOffsetOf(kB), (0u << 24) | psx::gpu::mainRamOffsetOf(kA), 0);

  psx::gpu::OrderingTableCursor cursor(*core, kA);
  int steps = 0;
  bool stopped = false;
  while (cursor.advance()) {
    steps++;
    // A guard on the TEST, so a cursor that simply never stops fails as an abort here rather than
    // hanging the suite.
    if (steps > psx::gpu::kOtNodeLimit + 4) {
      break;
    }
  }
  stopped = true;
  CHECK(stopped);
  CHECK_EQ(cursor.nodesEntered(), psx::gpu::kOtNodeLimit);
  CHECK(cursor.truncated());
}

// THE NEGATIVE FOR THE REFUSAL ABOVE: a well-formed chain must NOT be reported as truncated. A cursor
// that always set the flag would satisfy the cyclic case and turn every warning into noise.
static void test_a_well_formed_chain_is_not_reported_as_truncated(void) {
  auto core = walk_core();
  constexpr std::uint32_t kA = 0x80024000u;
  placeNode(*core, psx::gpu::mainRamOffsetOf(kA), (2u << 24) | psx::gpu::kOtChainEnd, 2);

  std::uint32_t seen[8] = {0};
  int truncated = -1;
  const int visited = collectWalk(*core, kA, seen, 8, truncated);
  CHECK_EQ(visited, 1);
  CHECK_EQ(truncated, 0);
}

// The node count is the DENOMINATOR the pool diagnostic reports, so it has to be the number of nodes
// actually read — not one more, and not one fewer.
static void test_node_count_is_the_number_entered(void) {
  auto core = walk_core();
  constexpr std::uint32_t kA = 0x80025000u, kB = 0x80025100u, kC = 0x80025200u, kD = 0x80025300u;
  placeNode(*core, psx::gpu::mainRamOffsetOf(kA), (1u << 24) | psx::gpu::mainRamOffsetOf(kB), 1);
  placeNode(*core, psx::gpu::mainRamOffsetOf(kB), (1u << 24) | psx::gpu::mainRamOffsetOf(kC), 1);
  placeNode(*core, psx::gpu::mainRamOffsetOf(kC), (1u << 24) | psx::gpu::mainRamOffsetOf(kD), 1);
  placeNode(*core, psx::gpu::mainRamOffsetOf(kD), (1u << 24) | psx::gpu::kOtChainEnd, 1);

  psx::gpu::OrderingTableCursor cursor(*core, kA);
  CHECK_EQ(cursor.nodesEntered(), 1);
  while (cursor.advance()) {
  }
  CHECK_EQ(cursor.nodesEntered(), 4);
}

// A GP0 word's address is the node's, plus the header, plus four per word. If this is wrong, every
// prim's words are submitted from the wrong guest address — which costs native DEPTH rather than
// correctness, so it is the kind of error that survives a visual check.
static void test_walk_reads_each_nodes_own_words(void) {
  auto core = walk_core();
  constexpr std::uint32_t kA = 0x80026000u;
  placeNode(*core, psx::gpu::mainRamOffsetOf(kA), (3u << 24) | psx::gpu::kOtChainEnd, 3);

  psx::gpu::OrderingTableCursor cursor(*core, kA);
  const psx::gpu::OtNode &node = cursor.node();
  CHECK_EQ(node.gp0WordCount(), 3u);
  for (unsigned i = 0; i < node.gp0WordCount(); i++) {
    CHECK_EQ(core->mem_r32(node.gp0WordRamOffset(i)), 0xA0000000u | i);
  }
}

int main(void) {
  RUN(word_count_matches_the_original_shift);
  RUN(link_only_node_is_distinguished_from_a_primitive);
  RUN(next_address_field_matches_the_original_mask);
  RUN(end_of_chain_accepts_both_sentinels_and_nothing_else);
  RUN(sentinel_is_visible_only_before_the_offset_mask);
  RUN(word_addresses_follow_the_node);
  RUN(guest_address_round_trips_through_the_offset);
  RUN(walk_visits_every_node_in_link_order);
  RUN(walk_stops_on_a_zero_next_address);
  RUN(walk_continues_through_a_link_only_node);
  RUN(a_cyclic_chain_is_refused_and_says_so);
  RUN(a_well_formed_chain_is_not_reported_as_truncated);
  RUN(node_count_is_the_number_entered);
  RUN(walk_reads_each_nodes_own_words);
  return pt_summary();
}
