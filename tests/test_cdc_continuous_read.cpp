// test_cdc_continuous_read.cpp — continuous ReadN data-ready events follow the programmed drive
// rate, independently of whether software drains the accepted sector's data FIFO.
//
// Crash Bash accepts a whole sector, DMA-reads its 12-byte header/subheader and 2048-byte payload,
// and deliberately leaves the 280-byte EDC/ECC tail unread. The old model tied the next sector to
// complete FIFO drainage, so it never raised another INT1. These cases drive the shipping
// cdc_begin_read/cdc_write/cdc_dma_read/cdc_drive_service path with an injected clock. They
// distinguish an elapsed-time event from the old immediate BFRD answer, prove Pause cancellation,
// and cover both Setmode rates without sleeping.
#include "testutil.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "cd_drive_timing.h"
#include "cdc_state.h"
#include "cdc_test_clock.h"
#include "disc.h"

namespace {

constexpr uint32_t kFirstLba = 16;
constexpr size_t kRawSectorBytes = 2352;
constexpr size_t kWholeSectorWords = 2340 / sizeof(uint32_t);

std::array<uint8_t, kRawSectorBytes> make_sector(uint32_t lba) {
  std::array<uint8_t, kRawSectorBytes> raw{};
  for (size_t offset = 0; offset < raw.size(); ++offset) {
    raw[offset] = static_cast<uint8_t>((lba * 37u + offset * 11u) & 0xFFu);
  }
  return raw;
}

const std::array<uint8_t, kRawSectorBytes> kFirstSector = make_sector(kFirstLba);
const std::array<uint8_t, kRawSectorBytes> kSecondSector = make_sector(kFirstLba + 1);

uint32_t read_le32(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u) |
         (static_cast<uint32_t>(bytes[2]) << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
}

void select_bank(CdcState *cdc, uint8_t bank) {
  cdc_write(cdc, 0x1F801800u, bank);
}

uint8_t pending_irq_type(CdcState *cdc) {
  select_bank(cdc, 1);
  return static_cast<uint8_t>(cdc_read(cdc, 0x1F801803u) & 0x07u);
}

uint8_t response_byte(CdcState *cdc) {
  select_bank(cdc, 1);
  return static_cast<uint8_t>(cdc_read(cdc, 0x1F801801u));
}

void acknowledge_irq(CdcState *cdc) {
  select_bank(cdc, 1);
  cdc_write(cdc, 0x1F801803u, 0x07);
}

void write_bfrd(CdcState *cdc, uint8_t value) {
  select_bank(cdc, 0);
  cdc_write(cdc, 0x1F801803u, value);
}

void issue_command(CdcState *cdc, uint8_t command) {
  select_bank(cdc, 0);
  cdc_write(cdc, 0x1F801801u, command);
}

CdcState begin_read(DiscState *disc, CdcTestClock *clock, uint8_t mode = 0xA0) {
  CdcState cdc{};
  cdc.disc = disc;
  cdc_state_init(&cdc);
  cdc_test_bind(&cdc, clock);
  cdc_set_mode(&cdc, mode);
  cdc_begin_read(&cdc, kFirstLba);
  return cdc;
}

bool acknowledge_current_sector(CdcState *cdc, CdcTestClock *clock) {
  if (cdc_test_service_deadline(cdc, clock) != 1) {
    return false;
  }
  if (pending_irq_type(cdc) != 1) {
    return false;
  }
  acknowledge_irq(cdc);
  return pending_irq_type(cdc) == 0;
}

void test_first_sector_waits_one_drive_period(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = begin_read(&disc, &clock);

  CHECK_EQ(pending_irq_type(&cdc), 0);
  CHECK_EQ(cdc.data_n, 0);
  CHECK_EQ(cdc.drive_deadline_ticks, cd_drive_sector_period_instruction_ticks(0xA0));

  clock.ticks = cdc.drive_deadline_ticks - 1;
  CHECK_EQ(cdc_drive_service(&cdc), 0);
  CHECK_EQ(pending_irq_type(&cdc), 0);
  CHECK_EQ(cdc.data_n, 0);

  clock.ticks++;
  CHECK_EQ(cdc_drive_service(&cdc), 1);
  CHECK_EQ(pending_irq_type(&cdc), 1);
  CHECK_EQ(response_byte(&cdc), 0x22); // CdlStatRead | CdlStatStandby
  CHECK_EQ(cdc.data_n, 2340);
  CHECK_EQ(cdc.loc_lba, kFirstLba);
}

void test_partial_fifo_does_not_block_following_sector_event(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = begin_read(&disc, &clock);
  std::array<uint32_t, 3> header{};
  std::array<uint32_t, 512> payload{};

  CHECK(acknowledge_current_sector(&cdc, &clock));
  write_bfrd(&cdc, 0x00);
  write_bfrd(&cdc, 0x80); // accept LBA 16; LBA 17 is not due until one drive period elapses
  CHECK_EQ(pending_irq_type(&cdc), 0);
  CHECK(cdc.drive_event_armed);
  CHECK_EQ(cdc.drive_deadline_ticks, clock.ticks + cd_drive_sector_period_instruction_ticks(0xA0));

  CHECK_EQ(cdc_dma_read(&cdc, header.data(), static_cast<int>(header.size())), header.size());
  write_bfrd(&cdc, 0x80); // repeated assertion between split DMA legs
  CHECK_EQ(cdc_dma_read(&cdc, payload.data(), static_cast<int>(payload.size())), payload.size());
  CHECK_EQ(cdc.loc_lba, kFirstLba);
  CHECK_EQ(cdc.data_rd, 2060);
  CHECK_EQ(cdc.data_n - cdc.data_rd, 280);
  CHECK_EQ(payload[0], read_le32(kFirstSector.data() + 24));

  // Neither FIFO traffic nor an early clock wake fabricates a data-ready event.
  clock.ticks = cdc.drive_deadline_ticks - 1;
  CHECK_EQ(cdc_drive_service(&cdc), 0);
  CHECK_EQ(pending_irq_type(&cdc), 0);

  clock.ticks = cdc.drive_deadline_ticks;
  CHECK_EQ(cdc_drive_service(&cdc), 1);
  CHECK_EQ(pending_irq_type(&cdc), 1);

  // Service the due sector in the guest's measured order: ACK, then BFRD 0 -> 1.
  acknowledge_irq(&cdc);
  write_bfrd(&cdc, 0x00);
  write_bfrd(&cdc, 0x80);
  CHECK_EQ(cdc.loc_lba, kFirstLba + 1);
  CHECK_EQ(cdc.data_rd, 0);
  CHECK_EQ(pending_irq_type(&cdc), 0); // LBA 18 has a new future deadline, not an immediate INT1
  CHECK_EQ(cdc.drive_deadline_ticks, clock.ticks + cd_drive_sector_period_instruction_ticks(0xA0));

  uint32_t second_word = 0;
  CHECK_EQ(cdc_dma_read(&cdc, &second_word, 1), 1);
  CHECK_EQ(second_word, read_le32(kSecondSector.data() + 12));
}

void test_stopped_controller_does_not_announce_sector(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = begin_read(&disc, &clock);

  CHECK(acknowledge_current_sector(&cdc, &clock));
  write_bfrd(&cdc, 0x80);
  const uint64_t cancelled_deadline = cdc.drive_deadline_ticks;
  CHECK(cdc.drive_event_armed);
  issue_command(&cdc, 0x09); // Pause: INT3 acknowledgement followed by INT2 completion
  CHECK_EQ(cdc.reading, 0);
  CHECK_EQ(cdc.following_sector_ready, 0);
  CHECK_EQ(cdc.drive_event_armed, 0);
  CHECK_EQ(pending_irq_type(&cdc), 3);
  CHECK_EQ(response_byte(&cdc), 0x22); // Pause ACK observes the still-reading status
  acknowledge_irq(&cdc);
  CHECK_EQ(pending_irq_type(&cdc), 2);
  CHECK_EQ(response_byte(&cdc), 0x02); // completion observes paused/not-reading status
  acknowledge_irq(&cdc);
  CHECK_EQ(pending_irq_type(&cdc), 0);

  write_bfrd(&cdc, 0x00);
  write_bfrd(&cdc, 0x80);
  clock.ticks = cancelled_deadline + 1;

  CHECK_EQ(cdc.reading, 0);
  CHECK_EQ(cdc_drive_service(&cdc), 0);
  CHECK_EQ(pending_irq_type(&cdc), 0);
  CHECK_EQ(cdc.loc_lba, kFirstLba);
}

void test_full_drain_rearms_bfrd_for_announced_sector(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = begin_read(&disc, &clock);
  std::array<uint32_t, kWholeSectorWords> whole_sector{};

  CHECK(acknowledge_current_sector(&cdc, &clock));
  write_bfrd(&cdc, 0x80);
  CHECK_EQ(pending_irq_type(&cdc), 0);
  CHECK_EQ(cdc_dma_read(&cdc, whole_sector.data(), static_cast<int>(whole_sector.size())), whole_sector.size());
  CHECK_EQ(cdc.loc_lba, kFirstLba);
  CHECK_EQ(cdc.data_n, 0);
  CHECK_EQ(cdc.data_rd, 0);

  clock.ticks = cdc.drive_deadline_ticks;
  CHECK_EQ(cdc_drive_service(&cdc), 1);
  CHECK_EQ(pending_irq_type(&cdc), 1);
  acknowledge_irq(&cdc);
  write_bfrd(&cdc, 0x80); // empty FIFO makes this a new service request without a prior 0 write
  CHECK_EQ(cdc.loc_lba, kFirstLba + 1);
  CHECK_EQ(pending_irq_type(&cdc), 0);
}

void test_setmode_selects_single_and_double_speed_deadlines(void) {
  DiscState disc{};
  CdcTestClock single_clock{};
  CdcTestClock double_clock{};
  CdcState single = begin_read(&disc, &single_clock, 0x20);
  CdcState twice = begin_read(&disc, &double_clock, 0xA0);

  CHECK_EQ(single.drive_deadline_ticks, cd_drive_sector_period_instruction_ticks(0x20));
  CHECK_EQ(twice.drive_deadline_ticks, cd_drive_sector_period_instruction_ticks(0xA0));
  CHECK_EQ(single.drive_deadline_ticks, 451'584u);
  CHECK_EQ(twice.drive_deadline_ticks, 225'792u);
}

} // namespace

extern "C" int disc_read_raw(DiscState *, uint32_t lba, uint8_t *out, uint32_t count) {
  const auto *sector = lba == kFirstLba ? &kFirstSector : lba == kFirstLba + 1 ? &kSecondSector : nullptr;
  if (sector == nullptr || count > sector->size()) {
    return 0;
  }
  std::copy_n(sector->data(), count, out);
  return 1;
}

extern "C" int disc_read_sector(DiscState *, uint32_t, uint8_t *) {
  return 0;
}

int main() {
  RUN(first_sector_waits_one_drive_period);
  RUN(partial_fifo_does_not_block_following_sector_event);
  RUN(stopped_controller_does_not_announce_sector);
  RUN(full_drain_rearms_bfrd_for_announced_sector);
  RUN(setmode_selects_single_and_double_speed_deadlines);
  return pt_summary();
}
