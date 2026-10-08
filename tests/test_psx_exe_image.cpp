#include "game.h"
#include "lightrec_executor.h"
#include "psx_exe_image.h"
#include "testutil.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace {
using namespace psx::cpu;

void word(std::vector<uint8_t> &bytes, std::size_t offset, uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
  }
}

std::vector<uint8_t> executable(uint32_t value = 7) {
  std::vector<uint8_t> bytes(kPsxExeHeaderBytes + 12);
  std::memcpy(bytes.data(), "PS-X EXE", 8);
  word(bytes, 0x10, 0x80010000);
  word(bytes, 0x14, 0x80018000);
  word(bytes, 0x18, 0x80010000);
  word(bytes, 0x1c, 12);
  word(bytes, 0x30, 0x801ff000);
  word(bytes, 0x34, 0xff0);
  word(bytes, 0x800, 0x24020000 | value); // addiu v0,zero,value
  word(bytes, 0x804, 0x03e00008);         // jr ra
  return bytes;
}

void test_load_and_aliases() {
  for (uint32_t segment : {0u, 0x80000000u, 0xa0000000u}) {
    auto game = std::make_unique<Game>();
    auto &core = game->core;
    auto bytes = executable();
    word(bytes, 0x10, segment + 0x10000);
    word(bytes, 0x18, segment + 0x10000);
    core.r[31] = 0x80020000;
    core.hi = 123;
    const auto result = loadPsxExeImage(core, bytes, "synthetic");
    CHECK(result);
    CHECK_EQ(core.pc, segment + 0x10000);
    CHECK_EQ(core.r[28], 0x80018000u);
    CHECK_EQ(core.r[29], 0x801ffff0u);
    CHECK_EQ(core.r[30], core.r[29]);
    CHECK_EQ(core.r[31], 0x80020000u);
    CHECK_EQ(core.hi, 123u);
    CHECK(core.currentImageIdentity(0x80010000) == result.identity);
    CHECK_EQ(core.mem_r32(0x10000), 0x24020007u);
  }
}

void test_absent_stack_preserves_caller() {
  auto game = std::make_unique<Game>();
  auto bytes = executable();
  word(bytes, 0x14, 0);
  word(bytes, 0x30, 0);
  word(bytes, 0x34, 0);
  game->core.r[29] = 0x801fe000;
  game->core.r[30] = 0x801fe100;
  CHECK(loadPsxExeImage(game->core, bytes, "no-stack"));
  CHECK_EQ(game->core.r[28], 0u);
  CHECK_EQ(game->core.r[29], 0x801fe000u);
  CHECK_EQ(game->core.r[30], 0x801fe100u);
}

void test_top_level_registers_after_mapping() {
  for (uint32_t stack : {0u, 0x801ff000u}) {
    auto game = std::make_unique<Game>();
    auto bytes = executable();
    word(bytes, 0x30, stack);
    word(bytes, 0x34, stack ? 0xff0u : 0u);
    game->core.r[29] = 0x801fa000u;
    game->core.r[30] = 0x801fa100u;
    game->core.r[31] = 0x80020000u;
    const auto loaded = loadPsxExeImage(game->core, bytes, "top-level");
    CHECK(loaded);
    CHECK_EQ(game->core.r[29], stack ? 0x801ffff0u : 0x801fa000u);
    CHECK_EQ(game->core.r[30], stack ? 0x801ffff0u : 0x801fa100u);
    CHECK_EQ(game->core.r[31], 0x80020000u);
    applyPsxExeTopLevelRegisters(game->core, loaded.image);
    CHECK_EQ(game->core.r[29], 0x801ffff0u);
    CHECK_EQ(game->core.r[30], 0x801ffff0u);
    CHECK_EQ(game->core.r[31], 0xdead0000u);
  }
}

void test_refusals_preserve_core() {
  auto game = std::make_unique<Game>();
  auto &core = game->core;
  std::fill_n(core.ram, sizeof(core.ram), 0x5a);
  for (unsigned index = 0; index < 32; ++index) {
    core.r[index] = index * 1234;
  }
  core.pc = 0x80001000;
  const R3000 before = core;
  const auto old = core.imageCatalog().activate("existing", {0x10000, 0x1000c}, 1);
  const auto invalidations = core.lightrecExecutor().counters().invalidations;
  std::vector<std::vector<uint8_t>> bad;
  bad.emplace_back(7);
  bad.emplace_back(kPsxExeHeaderBytes - 1);
  bad.emplace_back(kPsxExeMaxBytes + 1);
  bad.push_back(executable());
  bad.back()[0] = 'X';
  bad.push_back(executable());
  bad.back().pop_back();
  for (auto [offset, value] : std::array<std::array<uint32_t, 2>, 13>{{
           {0x18, 0x20010000},
           {0x18, 0x80200000},
           {0x18, 0x801ffffc},
           {0x1c, 0xffffffff},
           {0x1c, 0},
           {0x10, 0x8001000c},
           {0x10, 0x80010001},
           {0x14, 0x1f801000},
           {0x30, 0xfffffff0},
           {0x34, 0xffffffff},
           {0x34, 0x20000000},
           {0x34, 1},
           {0x30, 0},
       }}) {
    bad.push_back(executable());
    word(bad.back(), offset, value);
  }
  for (const auto &bytes : bad) {
    const auto result = loadPsxExeImage(core, bytes, "bad");
    CHECK(!result);
    CHECK(!result.detail.empty());
    CHECK(std::memcmp(&before, static_cast<R3000 *>(&core), sizeof(before)) == 0);
    CHECK(std::all_of(std::begin(core.ram), std::end(core.ram), [](uint8_t byte) {
      return byte == 0x5a;
    }));
    CHECK_EQ(core.imageCatalog().activeCount(), 1u);
    CHECK(core.currentImageIdentity(0x80010000) == old);
    CHECK_EQ(core.lightrecExecutor().counters().invalidations, invalidations);
  }
}

void test_reload_invalidates_executed_code() {
  auto game = std::make_unique<Game>();
  auto &core = game->core;
  auto bytes = executable();
  const auto first = loadPsxExeImage(core, bytes, "same-name");
  CHECK(first);
  core.r[31] = 0x80010100;
  CHECK(core.lightrecExecutor().executeFunction(core.pc, core.r[31], ExecutionBudget::fromCycles(100)).returned());
  CHECK_EQ(core.r[2], 7u);
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0);
  const auto invalidations = core.lightrecExecutor().counters().invalidations;
  bytes = executable(19);
  const auto second = loadPsxExeImage(core, bytes, "same-name");
  CHECK(second);
  CHECK(first.identity != second.identity);
  CHECK(core.lightrecExecutor().counters().invalidations > invalidations);
  CHECK(core.lightrecExecutor().executeFunction(core.pc, core.r[31], ExecutionBudget::fromCycles(100)).returned());
  CHECK_EQ(core.r[2], 19u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

void test_file_startup_register_contract() {
  const std::filesystem::path path = "scratch/psx-exe-image/startup.exe";
  std::filesystem::create_directories(path.parent_path());
  for (uint32_t stack : {0u, 0x801fe000u, 0x801ff000u}) {
    auto bytes = executable();
    word(bytes, 0x30, stack);
    const uint32_t offset = stack == 0x801ff000u ? 0xff0u : 0u;
    word(bytes, 0x34, offset);
    {
      std::ofstream file(path, std::ios::binary);
      file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      CHECK(file.good());
    }
    auto game = std::make_unique<Game>();
    game->core.r[29] = 0x801fa000;
    load_exe(path.string().c_str(), &game->core);
    CHECK_EQ(game->core.r[28], 0x80018000u);
    CHECK_EQ(game->core.r[29], stack ? stack + offset : 0x801ffff0u);
    CHECK_EQ(game->core.pc, 0x80010000u);
    CHECK_EQ(game->core.r[30], game->core.r[29]);
    CHECK_EQ(game->core.r[31], 0xdead0000u);
    CHECK_EQ(game->core.mem_r32(0x10000), 0x24020007u);
    CHECK(game->core.currentImageIdentity(0x80010000));
  }
  CHECK(std::filesystem::remove(path));
}

void test_input_alias_is_refused_before_copy() {
  auto game = std::make_unique<Game>();
  auto bytes = executable();
  std::copy(bytes.begin(), bytes.end(), game->core.ram);
  const auto result = loadPsxExeImage(game->core, {game->core.ram, bytes.size()}, "aliased");
  CHECK(!result);
  CHECK(!result.detail.empty());
  CHECK_EQ(game->core.imageCatalog().activeCount(), 0u);
  CHECK(std::equal(bytes.begin(), bytes.end(), game->core.ram));
}
} // namespace

int main() {
  RUN(load_and_aliases);
  RUN(absent_stack_preserves_caller);
  RUN(top_level_registers_after_mapping);
  RUN(refusals_preserve_core);
  RUN(reload_invalidates_executed_code);
  RUN(input_alias_is_refused_before_copy);
  RUN(file_startup_register_contract);
  return pt_summary();
}
