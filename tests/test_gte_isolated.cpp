#include "gte_state.h"
#include "testutil.h"

#include <array>
#include <cstdint>
#include <cstring>

extern "C" {
void GTE_Init(void);
int32_t GTE_Instruction(uint32_t instruction);
}

namespace {

struct CompareResult {
  unsigned compared = 0;
  unsigned mismatched = 0;
  unsigned first = 65;
};

static CompareResult compare_regs(const GteRegs &a, const GteRegs &b) {
  CompareResult result{};
  for (unsigned i = 0; i < 64; ++i) {
    ++result.compared;
    if (a.REG[i] != b.REG[i]) {
      if (!result.mismatched)
        result.first = i;
      ++result.mismatched;
    }
  }
  ++result.compared;
  if (a.FLAGS != b.FLAGS) {
    if (!result.mismatched)
      result.first = 64;
    ++result.mismatched;
  }
  return result;
}

static uint32_t next_random(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

static GteRegs make_state(uint32_t seed) {
  GteRegs state{};
  for (auto &reg : state.REG)
    reg = next_random(seed);
  state.FLAGS = next_random(seed);
  return state;
}

static void run_differential(uint32_t instruction, uint32_t seed) {
  GteRegs expected = make_state(seed);
  GteRegs actual = expected;
  GteRegs caller = make_state(seed ^ 0xa5a55a5au);
  const GteRegs caller_before = caller;

  GTE_BindState(&expected);
  const int32_t expected_cycles = GTE_Instruction(instruction);
  GTE_BindState(&caller);
  const int32_t actual_cycles = GTE_ExecuteIsolated(&actual, instruction);

  const CompareResult output = compare_regs(expected, actual);
  const CompareResult preserved = compare_regs(caller_before, caller);
  CHECK_EQ(actual_cycles, expected_cycles);
  CHECK_EQ(output.compared, 65u);
  CHECK_EQ(output.mismatched, 0u);
  CHECK_EQ(preserved.compared, 65u);
  CHECK_EQ(preserved.mismatched, 0u);
  CHECK(GTE_CurState() == &caller);
  CHECK(actual.CR == actual.REG + 32);
  CHECK(actual.DR == actual.REG);
}

static void test_randomized_differential(void) {
  // INTPL, NCDS, and the DPCS instruction reached by Spyro's positive
  // color-blend arm.
  constexpr std::array<uint32_t, 3> instructions = {0x4a980011u, 0x4a780013u,
                                                    0x4a780010u};
  uint32_t cases = 0;
  for (const uint32_t instruction : instructions) {
    for (uint32_t i = 0; i < 128; ++i) {
      run_differential(instruction,
                       0x91e10da5u ^ instruction ^ (i * 0x9e3779b9u));
      ++cases;
    }
  }
  CHECK_EQ(cases, 384u);
}

static void test_saturation_and_other_answer(void) {
  GteRegs expected{};
  expected.REG[8] = 0x1000;       // IR0
  expected.REG[9] = 0x7fff8000u;  // IR2/IR1 extremes
  expected.REG[10] = 0x00007fffu; // IR3
  expected.REG[53] = 0x80000000u; // FC1
  expected.REG[54] = 0x7fffffffu; // FC2
  expected.REG[55] = 0x80000000u; // FC3
  GteRegs actual = expected;
  GteRegs caller = make_state(0x12345678u);

  GTE_BindState(&expected);
  GTE_Instruction(0x4a980011u);
  GTE_BindState(&caller);
  CHECK(GTE_ExecuteIsolated(&actual, 0x4a980011u) >= 0);
  CompareResult result = compare_regs(expected, actual);
  CHECK_EQ(result.compared, 65u);
  CHECK_EQ(result.mismatched, 0u);
  CHECK(actual.FLAGS != 0u);

  actual.REG[25] ^= 1u;
  result = compare_regs(expected, actual);
  CHECK_EQ(result.compared, 65u);
  CHECK_EQ(result.mismatched, 1u);
  CHECK_EQ(result.first, 25u);
  CHECK_EQ(GTE_ExecuteIsolated(nullptr, 0x4a980011u), -1);
  CHECK(GTE_CurState() == &caller);
}

static void test_sequential_interleaving(void) {
  GteRegs caller = make_state(0x11111111u);
  GteRegs a = make_state(0x22222222u);
  GteRegs b = make_state(0x33333333u);
  GteRegs expected_a = a;
  GteRegs expected_b = b;

  GTE_BindState(&expected_a);
  GTE_Instruction(0x4a980011u);
  GTE_BindState(&expected_b);
  GTE_Instruction(0x4a780013u);
  GTE_BindState(&caller);
  CHECK(GTE_ExecuteIsolated(&a, 0x4a980011u) >= 0);
  CHECK(GTE_ExecuteIsolated(&b, 0x4a780013u) >= 0);
  CHECK_EQ(compare_regs(expected_a, a).mismatched, 0u);
  CHECK_EQ(compare_regs(expected_b, b).mismatched, 0u);
  CHECK(GTE_CurState() == &caller);
}

// REPLACED 2026-08-16. This was `two_threads_keep_bindings_separate`, and its central assertion —
// that two host threads calling GTE_BindState(nullptr) get DIFFERENT default register files — encoded
// exactly the contract that broke the port. The PSX GTE is vanilla: ONE GTE, one register file, and the
// binding is guest execution state, not host-thread state (USER, 2026-08-16: "we preserve PSX GTE as
// is"). Guest code migrates across host threads (Coro spawns a thread per task) without ever running
// concurrently with itself, so a per-thread binding unbinds the GTE from the guest — which segfaulted
// PSXPORT_ORACLE=1 in every 3D scene for two days while this test stayed green. Concurrent GTE binding
// from two host threads is therefore NOT a supported contract and is not tested here;
// tests/test_gte_cross_thread.cpp pins the contract that IS real (a guest touch from an unbound thread
// must land in the bound state). What survives from the old test is its genuinely valuable half, kept
// sequential: back-to-back isolated executions each compute the right answer and restore the binding.
static void test_isolation_restores_the_binding_across_a_run(void) {
  GteRegs caller = make_state(0x40000000u);
  const GteRegs caller_before = caller;

  unsigned mismatched = 0;
  GTE_BindState(&caller);
  for (unsigned n = 0; n < 2; ++n) {
    GteRegs isolated = make_state(0x50000000u + n);
    GteRegs expected = isolated;
    const uint32_t op = n ? 0x4a780013u : 0x4a980011u;

    GTE_BindState(&expected);
    GTE_Instruction(op);            // the reference answer, computed while bound
    GTE_BindState(&caller);

    CHECK(GTE_ExecuteIsolated(&isolated, op) >= 0);
    mismatched += compare_regs(expected, isolated).mismatched;

    // The caller's binding AND its register contents must be untouched after every isolated call.
    CHECK(GTE_CurState() == &caller);
    CHECK_EQ(compare_regs(caller_before, caller).mismatched, 0u);
  }
  CHECK_EQ(mismatched, 0u);
}

} // namespace

int main(void) {
  GTE_Init();
  RUN(randomized_differential);
  RUN(saturation_and_other_answer);
  RUN(sequential_interleaving);
  RUN(isolation_restores_the_binding_across_a_run);
  return pt_summary();
}
