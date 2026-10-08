#include "oracle_shim.h"
#include "pc_observer.h"
#include "testutil.h"

#include <algorithm>
#include <array>
#include <vector>

namespace {

bool loadFixture() {
  const std::array<uint32_t, 12> code{0x3c088000u,
                                      0x35082000u,
                                      0x8d090000u,
                                      0x0c000408u,
                                      0x24040007u,
                                      0xad020004u,
                                      0x08000406u,
                                      0u,
                                      0x27bdfff0u,
                                      0x24820003u,
                                      0x03e00008u,
                                      0x27bd0010u};
  std::vector<uint8_t> image(0x1100u);
  for (size_t i = 0; i < code.size(); ++i) {
    for (unsigned lane = 0; lane < 4; ++lane) {
      image[i * 4u + lane] = (uint8_t)(code[i] >> (lane * 8u));
    }
  }
  image[0x1000] = 0x78;
  image[0x1001] = 0x56;
  image[0x1002] = 0x34;
  image[0x1003] = 0x12;
  return oracle_load_exe(image.data(), (uint32_t)image.size(), 0x80001000u, 0x80001000u, 0, 0x8001fff0u);
}

PSX_ObserverConfig config() {
  PSX_ObserverConfig out{};
  out.abi = PSX_OBSERVER_ABI;
  out.target_count = 2;
  out.range_count = 1;
  out.capacity = 8;
  out.targets[0] = {0x80001020u, 1};
  out.targets[1] = {0x8000100cu, 0};
  out.ranges[0] = {0x80002000u, 8};
  return out;
}

void test_real_cpu_observer_preserves_execution_and_active_pipeline() {
  CHECK(loadFixture());
  retro_psx_observer_disable();
  CHECK(oracle_run(100) == ORACLE_STOP_BUDGET);
  OracleState off{};
  oracle_capture(&off);
  const std::vector<uint8_t> ram(oracle_main_ram(), oracle_main_ram() + 0x200000u);
  CHECK_EQ(ram[0x2004], 10u);
  CHECK(loadFixture());
  auto options = config();
  options.range_count = 3;
  options.ranges[0] = {0x00002000u, 8};
  options.ranges[1] = {0x80002000u, 8};
  options.ranges[2] = {0xa0002000u, 8};
  CHECK(retro_psx_observer_configure(&options));
  retro_psx_observer_field(17);
  CHECK(oracle_run(100) == ORACLE_STOP_BUDGET);
  OracleState on{};
  oracle_capture(&on);
  CHECK(std::equal(std::begin(off.gpr), std::end(off.gpr), std::begin(on.gpr)));
  CHECK_EQ(on.pc, off.pc);
  CHECK_EQ(on.next_pc, off.next_pc);
  CHECK_EQ(on.timestamp, off.timestamp);
  CHECK_EQ(on.lo, off.lo);
  CHECK_EQ(on.hi, off.hi);
  CHECK_EQ(on.cp0_status, off.cp0_status);
  CHECK_EQ(on.cp0_cause, off.cp0_cause);
  CHECK_EQ(on.cp0_epc, off.cp0_epc);
  CHECK(std::equal(ram.begin(), ram.end(), oracle_main_ram()));
  PSX_ObserverStatus status{};
  retro_psx_observer_status(&status);
  CHECK(status.scanned > 12u);
  CHECK_EQ(status.matched, 3u);
  CHECK_EQ(status.complete, 1u);
  CHECK_EQ(status.entries[0], 1u);
  CHECK_EQ(status.returns[0], 1u);
  std::array<PSX_ObserverRecord, 8> records{};
  CHECK_EQ(retro_psx_observer_drain(records.data(), records.size()), 3u);
  CHECK_EQ(records[0].pc, 0x8000100cu);
  CHECK_EQ(records[0].load_register, 9u);
  CHECK_EQ(records[0].load_value, 0x12345678u);
  CHECK_EQ(records[0].gpr[9], 0u);
  CHECK_EQ(records[1].pc, 0x80001020u);
  CHECK_EQ(records[1].gpr[31], 0x80001014u);
  CHECK_EQ(records[1].gpr[4], 7u);
  CHECK_EQ(records[2].kind, 1u);
  CHECK_EQ(records[2].pc, 0x80001014u);
  CHECK_EQ(records[2].gpr[29], 0x8001fff0u);
  CHECK_EQ(records[2].gpr[2], 10u);
  CHECK_EQ(records[2].field, 17u);
  CHECK_EQ(records[2].ram_bytes, 24u);
  CHECK(std::equal(records[2].ram, records[2].ram + 8, records[2].ram + 8));
  CHECK(std::equal(records[2].ram, records[2].ram + 8, records[2].ram + 16));
  CHECK_EQ(records[2].ram[0], 0x78u);
  CHECK_EQ(records[2].ram[4], 0u); // The observed return precedes the caller's store.
  CHECK_EQ(oracle_main_ram()[0x2004], 10u);
  retro_psx_observer_status(&status);
  CHECK_EQ(status.queued, 0u);
  CHECK_EQ(status.retained, 3u);
}

void test_unreturned_reentry_is_incomplete() {
  CHECK(loadFixture());
  auto options = config();
  options.target_count = 1;
  options.targets[0] = {0x80001018u, 1}; // Reentered loop never reaches its saved RA.
  CHECK(retro_psx_observer_configure(&options));
  CHECK(oracle_run(100) == ORACLE_STOP_BUDGET);
  PSX_ObserverStatus status{};
  retro_psx_observer_status(&status);
  CHECK(status.entries[0] > 1u);
  CHECK_EQ(status.returns[0], 0u);
  CHECK_EQ(status.pending, 1u);
  CHECK_EQ(status.pairing_errors, status.entries[0] - 1u);
  CHECK_EQ(status.complete, 0u);
  retro_psx_observer_disable();
}

void test_unreachable_overflow_and_invalid_configuration_are_explicit() {
  CHECK(loadFixture());
  auto options = config();
  options.target_count = 1;
  options.targets[0] = {0x80001800u, 0}; // Inside RAM, outside this program's executed path.
  CHECK(retro_psx_observer_configure(&options));
  CHECK(oracle_run(100) == ORACLE_STOP_BUDGET);
  PSX_ObserverStatus unreachable{};
  retro_psx_observer_status(&unreachable);
  CHECK(unreachable.scanned > 0);
  CHECK_EQ(unreachable.matched, 0u);
  CHECK_EQ(unreachable.complete, 0u);
  CHECK(loadFixture());
  options = config();
  options.capacity = 1;
  CHECK(retro_psx_observer_configure(&options));
  CHECK(oracle_run(100) == ORACLE_STOP_BUDGET);
  PSX_ObserverStatus overflow{};
  retro_psx_observer_status(&overflow);
  CHECK_EQ(overflow.matched, 3u);
  CHECK_EQ(overflow.retained, 1u);
  CHECK_EQ(overflow.dropped, 2u);
  CHECK_EQ(overflow.complete, 0u);
  for (unsigned fault = 0; fault < 5; ++fault) {
    auto invalid = options;
    switch (fault) {
    case 0:
      invalid.capacity = PSX_OBSERVER_RECORDS + 1;
      break;
    case 1:
      invalid.targets[0].pc |= 1u;
      break;
    case 2:
      invalid.targets[1].pc = invalid.targets[0].pc;
      break;
    case 3:
      invalid.ranges[0] = {0x801fffffu, 2};
      break;
    default:
      invalid.ranges[0].bytes = PSX_OBSERVER_BYTES + 1;
      break;
    }
    CHECK(!retro_psx_observer_configure(&invalid));
    PSX_ObserverStatus preserved{};
    retro_psx_observer_status(&preserved);
    CHECK_EQ(preserved.scanned, overflow.scanned);
    CHECK_EQ(preserved.dropped, overflow.dropped);
  }
  for (const uint32_t address : {0x20010000u, 0x40010000u, 0x60010000u, 0xc0010000u, 0xe0010000u}) {
    auto invalid = options;
    invalid.ranges[0] = {address, 8};
    CHECK(!retro_psx_observer_configure(&invalid));
    PSX_ObserverStatus preserved{};
    retro_psx_observer_status(&preserved);
    CHECK_EQ(preserved.scanned, overflow.scanned);
    CHECK_EQ(preserved.matched, overflow.matched);
    CHECK_EQ(preserved.dropped, overflow.dropped);
    CHECK_EQ(preserved.queued, overflow.queued);
    CHECK_EQ(preserved.enabled, overflow.enabled);
    CHECK_EQ(preserved.entries[0], overflow.entries[0]);
    CHECK_EQ(preserved.returns[0], overflow.returns[0]);
  }
  PSX_ObserverRecord retained{};
  CHECK_EQ(retro_psx_observer_drain(&retained, 1), 1u);
  CHECK_EQ(retained.ram_bytes, 8u);
  CHECK_EQ(retained.ram[0], 0x78u);
  retro_psx_observer_disable();
}

} // namespace

int main() {
  if (!oracle_init()) {
    return 1;
  }
  RUN(real_cpu_observer_preserves_execution_and_active_pipeline);
  RUN(unreturned_reentry_is_incomplete);
  RUN(unreachable_overflow_and_invalid_configuration_are_explicit);
  retro_psx_observer_disable();
  oracle_teardown();
  return pt_summary();
}
