// GetlocP (command 0x11) reports the drive's real Sub-Q position. MMX4 polls its
// absolute position while XA music plays; an ack-only response reads as zeros and
// makes the game seek back to the start of the same three sectors forever.
#include "testutil.h"

#include <array>
#include <cstdint>

#include "cdc_command_phase.h"
#include "cdc_state.h"
#include "cdc_test_clock.h"
#include "disc.h"

namespace {

void select_bank(CdcState *cdc, uint8_t bank) {
  cdc_write(cdc, 0x1F801800u, bank);
}

uint8_t response_byte(CdcState *cdc) {
  return static_cast<uint8_t>(cdc_read(cdc, 0x1F801801u));
}

void issue_command(CdcState *cdc, uint8_t command) {
  select_bank(cdc, 0);
  cdc_write(cdc, 0x1F801801u, command);
}

uint8_t pending_irq_type(CdcState *cdc) {
  select_bank(cdc, 1);
  return static_cast<uint8_t>(cdc_read(cdc, 0x1F801803u) & 0x07u);
}

extern "C" int fixed_subq(struct DiscState *, uint32_t lba, uint8_t *out) {
  if (lba != 175'157u) {
    return 0;
  }
  const std::array<uint8_t, 8> expected = {0x01, 0x01, 0x38, 0x55, 0x32, 0x38, 0x57, 0x32};
  std::copy(expected.begin(), expected.end(), out);
  return 1;
}

void test_disc_subq_position_matches_beetle_chd_layout(void) {
  DiscState disc{};
  disc.track_count = 1;
  disc.tracks[0] = DiscTrackInfo{1, 0, 200'000, 150, 0, 0};

  std::array<uint8_t, 8> result{};
  CHECK(disc_get_subq_position(&disc, 175'157, result.data()));
  const std::array<uint8_t, 8> expected = {0x01, 0x01, 0x38, 0x55, 0x32, 0x38, 0x57, 0x32};
  CHECK(result == expected);
}

void test_disc_subq_position_selects_the_real_track(void) {
  DiscState disc{};
  disc.track_count = 2;
  disc.tracks[0] = DiscTrackInfo{1, 0, 10'000, 150, 0, 0};
  disc.tracks[1] = DiscTrackInfo{2, 10'150, 5'000, 150, 0, 0};

  std::array<uint8_t, 8> result{};
  CHECK(disc_get_subq_position(&disc, 10'175, result.data()));
  const std::array<uint8_t, 8> expected = {0x02, 0x01, 0x00, 0x00, 0x25, 0x02, 0x17, 0x50};
  CHECK(result == expected);
}

void test_chd_metadata1_layout_is_parsed_by_the_shipping_seam(void) {
  int32_t physicalLba = -150;
  DiscTrackInfo track{};
  CHECK(disc_parse_track_metadata("TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:200000", 0, &physicalLba, &track));
  CHECK_EQ(track.number, 1u);
  CHECK_EQ(track.lba, 0);
  CHECK_EQ(track.pregap, 150);
  CHECK_EQ(track.pregap_dv, 0);
  CHECK_EQ(track.sectors, 200000u);
  CHECK_EQ(physicalLba, 200000);
}

void test_chd_metadata2_virtual_pregap_accumulates_tracks(void) {
  int32_t physicalLba = -150;
  DiscTrackInfo first{};
  DiscTrackInfo second{};
  CHECK(disc_parse_track_metadata(
      "TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:10000 PREGAP:0 PGTYPE:N PGSUB:NONE POSTGAP:0",
      1,
      &physicalLba,
      &first));
  CHECK(
      disc_parse_track_metadata("TRACK:2 TYPE:AUDIO SUBTYPE:NONE FRAMES:5150 PREGAP:150 PGTYPE:V PGSUB:NONE POSTGAP:25",
                                1,
                                &physicalLba,
                                &second));
  CHECK_EQ(second.number, 2u);
  CHECK_EQ(second.lba, 10150);
  CHECK_EQ(second.pregap, 0);
  CHECK_EQ(second.pregap_dv, 150);
  CHECK_EQ(second.sectors, 5000u);
  CHECK_EQ(second.postgap, 25);
  CHECK_EQ(physicalLba, 15175);
}

void test_chd_metadata_parser_refuses_malformed_and_unsupported_records(void) {
  int32_t physicalLba = -150;
  DiscTrackInfo track{};
  CHECK(!disc_parse_track_metadata("TRACK:1 TYPE:MODE2_RAW", 0, &physicalLba, &track));
  CHECK(!disc_parse_track_metadata(
      "TRACK:1 TYPE:MODE1 SUBTYPE:NONE FRAMES:100 PREGAP:0 PGTYPE:N PGSUB:NONE POSTGAP:0", 1, &physicalLba, &track));
  CHECK_EQ(physicalLba, -150);
}

void test_getlocp_shipping_command_returns_all_eight_bytes(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc{};
  cdc.disc = &disc;
  cdc_state_init(&cdc);
  cdc.disc_get_subq_position_fn = fixed_subq;
  cdc.loc_lba = 175'157;
  cdc_test_bind(&cdc, &clock);

  issue_command(&cdc, 0x11);
  clock.ticks = cdc_command_ack_delay_cpu_ticks(0);
  cdc_drive_service(&cdc);
  CHECK_EQ(pending_irq_type(&cdc), 3);

  const std::array<uint8_t, 8> expected = {0x01, 0x01, 0x38, 0x55, 0x32, 0x38, 0x57, 0x32};
  for (uint8_t byte : expected) {
    CHECK_EQ(response_byte(&cdc), byte);
  }
  CHECK_EQ(response_byte(&cdc), 0);
}

void test_getlocp_disc_failure_is_not_a_successful_zero_position(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc{};
  cdc.disc = &disc;
  cdc_state_init(&cdc);
  cdc.disc_get_subq_position_fn = nullptr;
  cdc_test_bind(&cdc, &clock);

  issue_command(&cdc, 0x11);
  clock.ticks = cdc_command_ack_delay_cpu_ticks(0);
  cdc_drive_service(&cdc);
  CHECK_EQ(pending_irq_type(&cdc), 5);
  CHECK_EQ(response_byte(&cdc), 0x03);
  CHECK_EQ(response_byte(&cdc), 0x80);
}

} // namespace

int main() {
  RUN(disc_subq_position_matches_beetle_chd_layout);
  RUN(disc_subq_position_selects_the_real_track);
  RUN(chd_metadata1_layout_is_parsed_by_the_shipping_seam);
  RUN(chd_metadata2_virtual_pregap_accumulates_tracks);
  RUN(chd_metadata_parser_refuses_malformed_and_unsupported_records);
  RUN(getlocp_shipping_command_returns_all_eight_bytes);
  RUN(getlocp_disc_failure_is_not_a_successful_zero_position);
  return pt_summary();
}
