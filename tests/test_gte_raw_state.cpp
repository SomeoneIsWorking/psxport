// test_gte_raw_state.cpp — Gte::saveRaw/restoreRaw copy the register file passively, and a new
// Core's GTE projects with the divider set up.
//
// Reading DR12..DR14 pops the projection FIFO and reading a flag register clears the flag, so a
// snapshot built from register reads would already disturb the GTE it saves.

#include "game.h"
#include "gte.h"
#include "testutil.h"

#include "stub_game_runtime.h"

#include <cstdint>
#include <memory>

namespace {

constexpr uint32_t kRtps = 0x4A180001u;  // RTPS
constexpr uint32_t kDpcs = 0x4A780010u;  // DPCS: RGBC toward FC by IR0
constexpr uint32_t kMvmva = 0x4A486012u; // MVMVA sf, RT * V0

void writeAll(psx::cpu::Gte &gte, uint32_t seed) {
  for (unsigned i = 0; i < 32; ++i) {
    gte.writeData(i, (0x80000000u | (seed * 2654435761u)) ^ (i * 40503u));
    gte.writeControl(i, (0x80000000u | (seed * 2654435761u)) ^ ((i + 32) * 40503u));
  }
}

void checkSame(const GteRawState &a, const GteRawState &b) {
  for (unsigned i = 0; i < 64; ++i) {
    CHECK_EQ(a.reg[i], b.reg[i]);
  }
  CHECK_EQ(a.flags, b.flags);
}

void test_snapshot_is_the_register_file_and_restores_it() {
  psx::test::StubGameRuntime stubRuntime;
  auto game = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  psx::cpu::Gte &gte = game->core.gte();
  writeAll(gte, 7);
  gte.registers().FLAGS = 0xFFFFFFFFu;

  const GteRawState saved = gte.saveRaw();
  for (unsigned i = 0; i < 64; ++i) {
    CHECK_EQ(saved.reg[i], gte.registers().REG[i]);
  }
  CHECK_EQ(saved.flags, gte.registers().FLAGS);

  writeAll(gte, 99);
  gte.restoreRaw(saved);
  checkSame(gte.saveRaw(), saved);
}

void test_restore_after_instructions_is_passive() {
  psx::test::StubGameRuntime stubRuntime;
  auto game = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  psx::cpu::Gte &gte = game->core.gte();
  gte.writeControl(9, 0x00010000u);
  gte.writeControl(7, 0x00001000u);
  gte.execute(kRtps);
  gte.writeControl(21, 0x00000080u);
  gte.writeControl(22, 0x00000080u);
  gte.writeControl(23, 0x00000080u);
  gte.writeData(8, 0x00001000u);
  gte.writeData(6, 0x00204080u);
  gte.execute(kDpcs);
  gte.writeControl(0, 0x00001000u);
  gte.writeData(2, 0x00000010u);
  gte.writeData(3, 0x00000020u);
  gte.execute(kMvmva);

  const GteRawState saved = gte.saveRaw();
  gte.writeControl(0, 0x00002000u);
  gte.writeData(2, 0x00000030u);
  gte.execute(kMvmva);
  gte.execute(kRtps);
  gte.restoreRaw(saved);
  checkSame(gte.saveRaw(), saved);

  // The FIFO and flag registers read back what was restored, and reading them leaves the file intact.
  CHECK_EQ(gte.readData(12), saved.reg[12]);
  CHECK_EQ(gte.readData(13), saved.reg[13]);
  CHECK_EQ(gte.readData(14), saved.reg[14]);
  CHECK_EQ(gte.readControl(31), saved.reg[63]);
  checkSame(gte.saveRaw(), saved);
}

// Each Core's snapshot is its own file, whichever Core's GTE was bound last.
void test_snapshot_reads_its_own_core() {
  psx::test::StubGameRuntime stubRuntime;
  auto first = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  auto second = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  first->core.gte().writeControl(0, 0x11111111u);
  second->core.gte().writeControl(0, 0x22222222u);
  CHECK_EQ(first->core.gte().saveRaw().reg[32], 0x11111111u);
  CHECK_EQ(second->core.gte().saveRaw().reg[32], 0x22222222u);
}

// RTPS divides H by SZ3 through Beetle's reciprocal table; a Core needs no separate GTE setup call.
void test_a_new_core_projects_through_the_division_table() {
  psx::test::StubGameRuntime stubRuntime;
  auto game = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  psx::cpu::Gte &gte = game->core.gte();
  gte.writeControl(0, 0x00001000u); // R11
  gte.writeControl(2, 0x00001000u); // R22
  gte.writeControl(4, 0x00001000u); // R33
  gte.writeControl(26, 0x100u);     // H
  gte.writeData(0, 0x00000100u);    // VX0 = 0x100, VY0 = 0
  gte.writeData(1, 0x00000200u);    // VZ0 = 0x200
  gte.execute(kRtps);
  CHECK_EQ(gte.readData(14), 0x00000080u); // SX2 = H * VX / VZ
}

} // namespace

int main() {
  RUN(snapshot_is_the_register_file_and_restores_it);
  RUN(restore_after_instructions_is_passive);
  RUN(snapshot_reads_its_own_core);
  RUN(a_new_core_projects_through_the_division_table);
  return psx::test::summary();
}
