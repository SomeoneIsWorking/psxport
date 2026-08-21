// test_cdc_bfrd_split_dma.cpp — BFRD is a request latch, not a command that reloads a sector on
// every asserted write.
//
// Crash Bash exposes the distinction while reading the ISO9660 primary volume descriptor. It
// requests whole-sector LBA 16, DMA-reads the 12-byte header/subheader, writes the already-asserted
// BFRD bit again, then DMA-reads the 2048-byte PVD payload. Treating the second 0x80 write as a new
// request advances to LBA 17 and puts that sector's header where the PVD type and "CD001" must be.
//
// This is the shipping CDC path with a hermetic disc backend: cdc_begin_read() loads a synthetic raw
// sector through disc_read_raw(), cdc_write() receives the exact request-register sequence, and
// cdc_dma_read() performs both DMA legs. BFRD changes only FIFO access; the separate continuous-read
// test owns drive-time sector arrival.
#include "testutil.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "cdc_state.h"
#include "cdc_test_clock.h"
#include "disc.h"

namespace {

constexpr uint32_t kPvdLba = 16;
constexpr size_t kRawSectorBytes = 2352;
constexpr size_t kHeaderWords = 3;
constexpr size_t kPayloadWords = 512;

std::array<uint8_t, kRawSectorBytes> make_sector(uint32_t lba) {
  std::array<uint8_t, kRawSectorBytes> raw{};
  for (size_t offset = 0; offset < raw.size(); ++offset) {
    raw[offset] = static_cast<uint8_t>((lba * 29u + offset * 7u) & 0xFFu);
  }
  return raw;
}

const std::array<uint8_t, kRawSectorBytes> kPvdSector = make_sector(kPvdLba);
const std::array<uint8_t, kRawSectorBytes> kNextSector = make_sector(kPvdLba + 1);

uint32_t read_le32(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u) |
         (static_cast<uint32_t>(bytes[2]) << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
}

void write_bfrd(CdcState *cdc, uint8_t value) {
  cdc_write(cdc, 0x1F801800u, 0); // bank 0
  cdc_write(cdc, 0x1F801803u, value);
}

CdcState begin_whole_sector_read(DiscState *disc, CdcTestClock *clock) {
  CdcState cdc{};
  cdc.disc = disc;
  cdc_state_init(&cdc);
  cdc_test_bind(&cdc, clock);
  cdc_set_mode(&cdc, 0xA0); // double-speed, whole-sector FIFO
  cdc_begin_read(&cdc, kPvdLba);
  cdc_test_service_deadline(&cdc, clock);
  write_bfrd(&cdc, 0x00);
  write_bfrd(&cdc, 0x80);
  return cdc;
}

void test_repeated_assertion_preserves_split_dma_cursor(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = begin_whole_sector_read(&disc, &clock);
  std::array<uint32_t, kHeaderWords> header{};
  std::array<uint32_t, kPayloadWords> payload{};

  CHECK_EQ(cdc_dma_read(&cdc, header.data(), static_cast<int>(header.size())), header.size());
  CHECK_EQ(cdc.loc_lba, kPvdLba);
  CHECK_EQ(cdc.data_rd, kHeaderWords * sizeof(uint32_t));

  write_bfrd(&cdc, 0x80); // BFRD is still asserted: this is not a new request.

  CHECK_EQ(cdc.loc_lba, kPvdLba);
  CHECK_EQ(cdc.data_rd, kHeaderWords * sizeof(uint32_t));
  CHECK_EQ(cdc_dma_read(&cdc, payload.data(), static_cast<int>(payload.size())), payload.size());
  CHECK_EQ(cdc.loc_lba, kPvdLba);
  CHECK_EQ(cdc.data_rd, (kHeaderWords + kPayloadWords) * sizeof(uint32_t));

  for (size_t word = 0; word < header.size(); ++word) {
    CHECK_EQ(header[word], read_le32(kPvdSector.data() + 12 + word * sizeof(uint32_t)));
  }
  for (size_t word = 0; word < payload.size(); ++word) {
    CHECK_EQ(payload[word], read_le32(kPvdSector.data() + 24 + word * sizeof(uint32_t)));
  }
}

void test_deassert_then_assert_preserves_sector_until_drive_event(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = begin_whole_sector_read(&disc, &clock);
  std::array<uint32_t, kHeaderWords> header{};
  uint32_t next_word = 0;

  CHECK_EQ(cdc_dma_read(&cdc, header.data(), static_cast<int>(header.size())), header.size());
  write_bfrd(&cdc, 0x00);
  write_bfrd(&cdc, 0x80);

  CHECK_EQ(cdc.loc_lba, kPvdLba);
  CHECK_EQ(cdc.data_rd, kHeaderWords * sizeof(uint32_t));
  CHECK_EQ(cdc_dma_read(&cdc, &next_word, 1), 1);
  CHECK_EQ(next_word, read_le32(kPvdSector.data() + 24));
}

void test_deasserted_latch_blocks_fifo_access(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = begin_whole_sector_read(&disc, &clock);
  uint32_t word = 0;

  CHECK((cdc_read(&cdc, 0x1F801800u) & 0x40u) != 0); // DRQSTS while BFRD is asserted
  write_bfrd(&cdc, 0x00);
  CHECK_EQ(cdc_read(&cdc, 0x1F801800u) & 0x40u, 0);
  CHECK_EQ(cdc_dma_read(&cdc, &word, 1), 0);
  CHECK_EQ(cdc.data_rd, 0);

  write_bfrd(&cdc, 0x80);
  CHECK((cdc_read(&cdc, 0x1F801800u) & 0x40u) != 0);
  CHECK_EQ(cdc_dma_read(&cdc, &word, 1), 1);
  CHECK_EQ(word, read_le32(kPvdSector.data() + 12));
}

} // namespace

extern "C" int disc_read_raw(DiscState *, uint32_t lba, uint8_t *out, uint32_t count) {
  const auto *sector = lba == kPvdLba ? &kPvdSector : lba == kPvdLba + 1 ? &kNextSector : nullptr;
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
  RUN(repeated_assertion_preserves_split_dma_cursor);
  RUN(deassert_then_assert_preserves_sector_until_drive_event);
  RUN(deasserted_latch_blocks_fifo_access);
  return pt_summary();
}
