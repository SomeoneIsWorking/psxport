// The direct-runtime stock-libcd binding targets are the shipping synchronous read owners, not
// title-local copies. Exercise their guest ABI and state transitions without requiring a disc image.
#include "cd_control.h"
#include "game.h"
#include "invalidation.h"
#include "lightrec_executor.h"
#include "testutil.h"

#include <memory>

namespace {

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

constexpr uint32_t kBuffer = 0x80110000u;
constexpr uint32_t kResult = 0x80111000u;

// A sector source that fills every byte of the sector with its LBA, so a copied sector is identifiable.
int fakeSector(DiscState *, uint32_t lba, uint8_t *out, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = static_cast<uint8_t>(lba);
  }
  return 1;
}

} // namespace

static void test_stock_read_refuses_without_a_position() {
  auto game = std::make_unique<Game>();
  game->core.r[A0] = 1;
  game->core.r[A1] = kBuffer;
  game->core.r[A2] = 0;
  game->core.r[V0] = 0xDEADBEEFu;
  game->core.mem_w8(kBuffer, 0xA5u);

  cd_read_stock_sync(&game->core);

  CHECK_EQ(game->core.r[V0], 0u);
  CHECK_EQ(game->core.mem_r8(kBuffer), 0xA5u);
  CHECK_EQ(game->cd.setloc_lba, -1);
}

static void test_zero_sector_stock_read_completes_without_inventing_drive_work() {
  auto game = std::make_unique<Game>();
  game->cd.setloc_lba = 321;
  game->cd.sec_pos = 40;
  game->cd.sec_len = 2352;
  game->cd.sec_lba = 320;
  game->cd.stock_reading = 1;
  game->core.r[A0] = 0;
  game->core.r[A1] = kBuffer;
  game->core.r[A2] = 0;

  cd_read_stock_sync(&game->core);

  CHECK_EQ(game->core.r[V0], 1u);
  CHECK_EQ(game->cd.setloc_lba, 321);
  CHECK_EQ(game->cd.sec_pos, 0);
  CHECK_EQ(game->cd.sec_len, 0);
  CHECK_EQ(game->cd.sec_lba, -1);
  CHECK_EQ(game->cd.stock_reading, 0);
}

static void test_stock_readsync_reports_completed_and_zeros_result() {
  auto game = std::make_unique<Game>();
  for (uint32_t i = 0; i < 8; i++) {
    game->core.mem_w8(kResult + i, static_cast<uint8_t>(0x80u + i));
  }
  game->core.r[A0] = 0;
  game->core.r[A1] = kResult;
  game->core.r[V0] = 0xDEADBEEFu;

  cd_readsync_stock_sync(&game->core);

  CHECK_EQ(game->core.r[V0], 0u);
  for (uint32_t i = 0; i < 8; i++) {
    CHECK_EQ(game->core.mem_r8(kResult + i), 0u);
  }
}

static void test_stock_cdsync_reports_ready_and_zeros_result() {
  auto game = std::make_unique<Game>();
  for (uint32_t i = 0; i < 8; i++) {
    game->core.mem_w8(kResult + i, static_cast<uint8_t>(0x90u + i));
  }
  game->core.r[A0] = 0;
  game->core.r[A1] = kResult;
  game->core.r[V0] = 0xDEADBEEFu;

  cd_sync_stock_sync(&game->core);

  CHECK_EQ(game->core.r[V0], 2u);
  for (uint32_t i = 0; i < 8; i++) {
    CHECK_EQ(game->core.mem_r8(kResult + i), 0u);
  }
}

static void test_stock_command_uses_low_level_success_abi_and_applies_setloc() {
  auto game = std::make_unique<Game>();
  auto &core = game->core;
  core.mem_w8(kBuffer, 0x00u);
  core.mem_w8(kBuffer + 1, 0x03u);
  core.mem_w8(kBuffer + 2, 0x20u);
  core.mem_w8(kBuffer + 3, 0u);
  for (uint32_t i = 0; i < 8; ++i) {
    core.mem_w8(kResult + i, 0xA5u);
  }
  game->cd.sec_pos = 40;
  game->cd.sec_len = 2352;
  game->cd.sec_lba = 320;
  core.r[A0] = 0x02u; // Setloc 00:03:20 => LBA 95.
  core.r[A1] = kBuffer;
  core.r[A2] = kResult;
  core.r[V0] = 0xDEADBEEFu;

  cd_command_stock_sync(&core);

  CHECK_EQ(core.r[V0], 0u);
  CHECK_EQ(game->cd.setloc_lba, 95);
  CHECK_EQ(game->cd.sec_pos, 0);
  CHECK_EQ(game->cd.sec_len, 0);
  CHECK_EQ(game->cd.sec_lba, -1);
  for (uint32_t i = 0; i < 8; ++i) {
    CHECK_EQ(core.mem_r8(kResult + i), 0u);
  }

  // CdControl is a distinct public ABI over the same command effects: its success result is 1.
  core.r[V0] = 0xDEADBEEFu;
  cd_control_sync(&core);
  CHECK_EQ(core.r[V0], 1u);
  CHECK_EQ(game->cd.setloc_lba, 95);
}

static void test_stock_pause_stops_stream_and_accepts_null_output() {
  auto game = std::make_unique<Game>();
  game->cd.stock_reading = 1;
  game->cd.stream_active = 1;
  game->core.r[A0] = 0x09u;
  game->core.r[A1] = 0;
  game->core.r[A2] = 0;
  game->core.r[V0] = 0xDEADBEEFu;

  cd_command_stock_sync(&game->core);

  CHECK_EQ(game->core.r[V0], 0u);
  CHECK_EQ(game->cd.stock_reading, 0);
  CHECK_EQ(game->cd.stream_active, 0);
}

// ----------------------------------------------------------------------------
// The executable-write report on the CdRead route, and what it is worth without a disc.
//
// `cd_read_stock_sync` writes `sectors * bytes` from a guest-chosen `buf`, and the guest is free to aim
// that at CODE. Tomba! 1 does: `CdRead 7x2048 from LBA 103311 -> 0x800E7388`, after which the port's
// dispatch answered "claimed by none" for the callee — a module arriving on a route that never told the
// invalidation owner, so Lightrec could keep serving translated blocks for bytes that had just changed.
//
// THE POSITIVE CASE IS DISC-GATED, and this suite is hermetic, so it is NOT here: a nonzero `sectors`
// returns early when `disc_read_raw` fails, before any byte is written. What IS provable without media is
// the pair below, and the pair is what makes the zero meaningful — a counter that reads 0 because nothing
// fed it and a counter that reads 0 because the route correctly reported nothing are the same number
// unless the feeder is shown running first. That is the whole lesson of the nine dead taps in this
// workspace, so it is asserted rather than assumed.
static void test_the_invalidation_counter_is_live_and_a_zero_sector_read_does_not_move_it() {
  auto game = std::make_unique<Game>();
  const uint64_t atRest = game->core.lightrecExecutor().counters().invalidations;

  // POSITIVE: the owner is fed on purpose, and the counter moves. Without this line the zero below is
  // unreadable — it would be equally consistent with "the route reported nothing" and "nothing measures".
  psx::cpu::notifyExecutableWrite(game->core, {0x00100000u, 0x00100040u}, psx::cpu::ExecutableWriteSource::ModuleLoad);
  const uint64_t afterDeliberate = game->core.lightrecExecutor().counters().invalidations;
  CHECK(afterDeliberate > atRest);

  // NEGATIVE: a positioned read of ZERO sectors transfers nothing, so it must report nothing. A route
  // that reported an empty range unconditionally would invalidate on every poll.
  game->cd.setloc_lba = 321;
  game->core.r[A0] = 0;
  game->core.r[A1] = kBuffer;
  game->core.r[A2] = 0;
  cd_read_stock_sync(&game->core);
  CHECK_EQ(game->core.r[V0], 1u);
  CHECK_EQ(game->core.lightrecExecutor().counters().invalidations, afterDeliberate);
}

// The refusal path must be just as quiet: with no Setloc the read never starts, so there is no write and
// nothing to report. A refusal that still reported a range would invalidate on every unpositioned poll.
// The positive feeder, through the controller's sector-source binding: each sector costs ONE
// invalidation (its range, reported once), not one per byte copied.
static void test_a_stock_read_reports_one_invalidation_per_sector_not_per_byte() {
  auto game = std::make_unique<Game>();
  game->cdc.disc_read_raw_fn = fakeSector;
  game->cd.setloc_lba = 7;
  game->core.r[A0] = 3; // sectors
  game->core.r[A1] = kBuffer;
  game->core.r[A2] = 0; // 2048-byte payloads
  const auto before = game->core.lightrecExecutor().counters();

  cd_read_stock_sync(&game->core);

  const auto after = game->core.lightrecExecutor().counters();
  CHECK_EQ(game->core.r[V0], 1u);
  CHECK_EQ(game->core.mem_r8(kBuffer), 7u);
  CHECK_EQ(game->core.mem_r8(kBuffer + 3u * 2048u - 1u), 9u);
  CHECK_EQ(after.invalidations - before.invalidations, 3u);
  const auto source = static_cast<std::size_t>(psx::cpu::ExecutableWriteSource::ModuleLoad);
  CHECK_EQ(after.invalidationsBySource[source] - before.invalidationsBySource[source], 3u);
}

static void test_a_refused_read_reports_no_executable_write() {
  auto game = std::make_unique<Game>();
  psx::cpu::notifyExecutableWrite(game->core, {0x00100000u, 0x00100040u}, psx::cpu::ExecutableWriteSource::ModuleLoad);
  const uint64_t before = game->core.lightrecExecutor().counters().invalidations;
  game->core.r[A0] = 4;
  game->core.r[A1] = kBuffer;
  game->core.r[A2] = 0;
  cd_read_stock_sync(&game->core);
  CHECK_EQ(game->core.r[V0], 0u);
  CHECK_EQ(game->core.lightrecExecutor().counters().invalidations, before);
}

int main() {
  RUN(stock_command_uses_low_level_success_abi_and_applies_setloc);
  RUN(stock_pause_stops_stream_and_accepts_null_output);
  RUN(stock_read_refuses_without_a_position);
  RUN(zero_sector_stock_read_completes_without_inventing_drive_work);
  RUN(stock_readsync_reports_completed_and_zeros_result);
  RUN(stock_cdsync_reports_ready_and_zeros_result);
  RUN(the_invalidation_counter_is_live_and_a_zero_sector_read_does_not_move_it);
  RUN(a_stock_read_reports_one_invalidation_per_sector_not_per_byte);
  RUN(a_refused_read_reports_no_executable_write);
  return pt_summary();
}
