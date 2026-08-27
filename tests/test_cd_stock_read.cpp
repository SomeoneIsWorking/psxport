// The direct-runtime stock-libcd binding targets are the shipping synchronous read owners, not
// title-local copies. Exercise their guest ABI and state transitions without requiring a disc image.
#include "cd_control.h"
#include "game.h"
#include "testutil.h"

#include <memory>

namespace {

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

constexpr uint32_t kBuffer = 0x80110000u;
constexpr uint32_t kResult = 0x80111000u;

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

int main() {
  RUN(stock_read_refuses_without_a_position);
  RUN(zero_sector_stock_read_completes_without_inventing_drive_work);
  RUN(stock_readsync_reports_completed_and_zeros_result);
  return pt_summary();
}
