// Executor state transfer must not replay side-effectful MTC2/CTC2 register-port writes.
#include "game.h"
#include "gte_state.h"
#include "hw_bind.h"
#include "image_identity.h"
#include "lightrec_executor.h"
#include "testutil.h"

#include <algorithm>
#include <memory>

namespace {

constexpr uint32_t kEntry = 0x00010000u;
constexpr uint32_t kReturn = 0x00010100u;
constexpr uint32_t kGpf = 0x4a08003du; // GPF, sf=1
constexpr uint32_t kSqr = 0x4a000028u; // SQR, sf=0

void seedFifos(GteRegs &state) {
  state.REG[12] = 0x00110022u;
  state.REG[13] = 0x00330044u;
  state.REG[14] = 0x00550066u;
  state.REG[15] = 0x00770088u; // Raw SXYP storage is not a request to advance SXY0..2.
  state.REG[20] = 0x01112233u;
  state.REG[21] = 0x04445566u;
  state.REG[22] = 0x07778899u;
}

void test_guest_gpf_sqr_preserves_ir_inputs_against_isolated_gte() {
  auto game = std::make_unique<Game>();
  auto &core = game->core;
  core.imageCatalog().activate("gte-transfer-fixture", {kEntry, kReturn + 4u}, 1u);
  seedFifos(game->gte);
  GteRegs expected{};
  std::copy_n(game->gte.REG, 64, expected.REG);
  // IRGB storage remains zero while MTC2 independently updates the IR inputs.
  expected.REG[15] = expected.REG[14]; // Architectural SXYP is an alias of SXY2.
  expected.REG[8] = 4096u;
  expected.REG[9] = 33u;
  expected.REG[10] = static_cast<uint32_t>(-65);
  expected.REG[11] = 127u;
  CHECK(GTE_ExecuteIsolated(&expected, kGpf) >= 0);
  CHECK(GTE_ExecuteIsolated(&expected, kSqr) >= 0);
  CHECK(expected.REG[25] != 0);
  CHECK(expected.REG[26] != 0);
  CHECK(expected.REG[27] != 0);

  core.r[4] = 4096u;
  core.r[5] = 33u;
  core.r[6] = static_cast<uint32_t>(-65);
  core.r[7] = 127u;
  const uint32_t instructions[] = {
      0x48844000u, // mtc2 a0, IR0
      0x48854800u, // mtc2 a1, IR1
      0x48865000u, // mtc2 a2, IR2
      0x48875800u, // mtc2 a3, IR3
      kGpf,
      kSqr,
      0x03e00008u, // jr ra
      0u,
  };
  for (uint32_t index = 0; index < std::size(instructions); ++index) {
    core.mem_w32(kEntry + index * 4, instructions[index]);
  }
  core.r[31] = kReturn;
  const auto result =
      core.lightrecExecutor().executeFunction(kEntry, kReturn, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK(result.returned());
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
  for (uint32_t index : {8u, 9u, 10u, 11u, 25u, 26u, 27u, 63u}) {
    CHECK_EQ(game->gte.REG[index], expected.REG[index]);
  }
  for (uint32_t index = 12; index <= 22; ++index) {
    CHECK_EQ(game->gte.REG[index], expected.REG[index]);
  }
  CHECK_EQ(game->gte.FLAGS, expected.FLAGS);
}

void test_non_gte_guest_exit_preserves_raw_state_without_fifo_or_irgb_side_effects() {
  auto game = std::make_unique<Game>();
  auto &core = game->core;
  core.imageCatalog().activate("gte-transfer-fixture", {kEntry, kReturn + 4u}, 1u);
  seedFifos(game->gte);
  game->gte.REG[9] = 33u;
  game->gte.REG[10] = static_cast<uint32_t>(-65);
  game->gte.REG[11] = 127u;
  game->gte.REG[28] = 0;
  game->gte.REG[29] = 0x12345678u;
  game->gte.REG[30] = 0x00ffffffu;
  game->gte.REG[31] = 8u;
  game->gte.REG[63] = 0x80001234u; // Bulk transfer preserves raw FLAG, unlike CTC2's write mask.
  game->gte.FLAGS = game->gte.REG[63];
  uint32_t expected[64];
  std::copy_n(game->gte.REG, 64, expected);
  expected[15] = expected[14]; // Ignore unused backend slot15; the native alias must remain coherent.
  core.mem_w32(kEntry, 0x03e00008u);
  core.mem_w32(kEntry + 4, 0u);
  core.r[31] = kReturn;

  const auto result =
      core.lightrecExecutor().executeFunction(kEntry, kReturn, psx::cpu::ExecutionBudget::fromCycles(100));

  CHECK(result.returned());
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
  for (uint32_t index = 0; index < 64; ++index) {
    CHECK_EQ(game->gte.REG[index], expected[index]);
  }
  CHECK_EQ(game->gte.FLAGS, expected[63]);
}

void test_guest_sxyp_write_advances_fifo_once_and_reads_alias() {
  auto game = std::make_unique<Game>();
  auto &core = game->core;
  core.imageCatalog().activate("gte-transfer-fixture", {kEntry, kReturn + 4u}, 1u);
  seedFifos(game->gte);
  GteRegs expected{};
  std::copy_n(game->gte.REG, 64, expected.REG);
  const uint32_t pushed = 0x11223344u;
  GTE_BindState(&expected);
  gte_write_data(15, pushed); // Independent production register-port reference.
  uint32_t expectedReads[4];
  for (uint32_t index = 0; index < 4; ++index) {
    expectedReads[index] = gte_read_data(12 + index);
  }
  gte_bind(&core);
  core.r[4] = pushed;
  const uint32_t instructions[] = {
      0x48847800u, // mtc2 a0, SXYP
      0x48056000u, // mfc2 a1, SXY0
      0u,
      0x48066800u, // mfc2 a2, SXY1
      0u,
      0x48077000u, // mfc2 a3, SXY2
      0u,
      0x48087800u, // mfc2 t0, SXYP
      0u,
      0x03e00008u,
      0u,
  };
  for (uint32_t index = 0; index < std::size(instructions); ++index) {
    core.mem_w32(kEntry + index * 4, instructions[index]);
  }
  core.r[31] = kReturn;

  const auto result =
      core.lightrecExecutor().executeFunction(kEntry, kReturn, psx::cpu::ExecutionBudget::fromCycles(100));

  CHECK(result.returned());
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
  for (uint32_t index = 0; index < 4; ++index) {
    CHECK_EQ(core.r[5 + index], expectedReads[index]);
    CHECK_EQ(game->gte.REG[12 + index], expected.REG[12 + index]);
    CHECK_EQ(gte_read_data(12 + index), expectedReads[index]);
  }
}

} // namespace

int main() {
  RUN(guest_gpf_sqr_preserves_ir_inputs_against_isolated_gte);
  RUN(guest_sxyp_write_advances_fifo_once_and_reads_alias);
  RUN(non_gte_guest_exit_preserves_raw_state_without_fifo_or_irgb_side_effects);
  return pt_summary();
}
