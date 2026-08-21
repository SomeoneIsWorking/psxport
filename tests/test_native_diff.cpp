// The per-call differential is itself nestable: an owned parent may call a separately owned child,
// and both must retain independent snapshots. Exercise the shipping ndiff_run API, including a
// deliberately different nested child so the test proves the instrument can report the other answer.
#include "game.h"
#include "native_diff.h"
#include "testutil.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kChildWord = 0x80000100u;
constexpr uint32_t kParentWord = 0x80000104u;
constexpr uint32_t kChildValue = 0x13579BDFu;
constexpr uint32_t kMutatedChildValue = 0x2468ACE0u;

void childNative(Core *c) {
  c->mem_w32(kChildWord, kChildValue);
}

void childBody(Core *c) {
  c->mem_w32(kChildWord, kChildValue);
}

void childMutatedNative(Core *c) {
  c->mem_w32(kChildWord, kMutatedChildValue);
}

void parentNative(Core *c) {
  ndiff_run(c, "nested-child-equal", childNative, childBody);
  c->mem_w32(kParentWord, c->mem_r32(kChildWord) + 1u);
}

void parentBody(Core *c) {
  ndiff_run(c, "nested-child-equal", childNative, childBody);
  c->mem_w32(kParentWord, c->mem_r32(kChildWord) + 1u);
}

void mutatedParentNative(Core *c) {
  ndiff_run(c, "nested-child-mutated", childMutatedNative, childBody);
  c->mem_w32(kParentWord, c->mem_r32(kChildWord) + 1u);
}

void mutatedParentBody(Core *c) {
  ndiff_run(c, "nested-child-mutated", childMutatedNative, childBody);
  c->mem_w32(kParentWord, c->mem_r32(kChildWord) + 1u);
}

Game &testGame() {
  static Game game;
  return game;
}

void clearGuestState(Core &c) {
  std::memset(c.ram, 0, sizeof c.ram);
  std::memset(c.scratch, 0, sizeof c.scratch);
  std::memset(static_cast<R3000 *>(&c), 0, sizeof(R3000));
  c.game->gte = {};
}

void test_equivalent_nested_calls_keep_independent_snapshots() {
  Core &c = testGame().core;
  clearGuestState(c);

  CHECK(!ndiff_run(&c, "nested-parent-equal", parentNative, parentBody));
  CHECK_EQ(ndiff_divergences(), 0);
  CHECK_EQ(c.mem_r32(kChildWord), kChildValue);
  CHECK_EQ(c.mem_r32(kParentWord), kChildValue + 1u);
}

void test_mutated_nested_child_reports_the_other_answer() {
  Core &c = testGame().core;
  clearGuestState(c);
  const int before = ndiff_divergences();

  CHECK(!ndiff_run(&c, "nested-parent-with-mutated-child", mutatedParentNative, mutatedParentBody));
  CHECK_EQ(ndiff_divergences(), before + 2);
  CHECK_EQ(c.mem_r32(kChildWord), kMutatedChildValue);
  CHECK_EQ(c.mem_r32(kParentWord), kMutatedChildValue + 1u);
}

} // namespace

int main() {
  setenv("PSXPORT_NDIFF", "8", 1);
  RUN(equivalent_nested_calls_keep_independent_snapshots);
  RUN(mutated_nested_child_reports_the_other_answer);
  return pt_summary();
}
