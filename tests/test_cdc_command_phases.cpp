// test_cdc_command_phases.cpp — the native controller follows the oracle's pending-command
// write/argument/execution phases instead of preparing responses and side effects at MMIO write.
#include "testutil.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "cd_drive_timing.h"
#include "cdc_command_phase.h"
#include "cdc_state.h"
#include "cdc_test_clock.h"
#include "disc.h"

namespace {

constexpr uint32_t kSectorLba = 41;
constexpr size_t kRawSectorBytes = 2352;

void select_bank(CdcState *cdc, uint8_t bank) {
  cdc_write(cdc, 0x1F801800u, bank);
}

uint8_t status(CdcState *cdc) {
  return static_cast<uint8_t>(cdc_read(cdc, 0x1F801800u));
}

uint8_t pending_irq_type(CdcState *cdc) {
  select_bank(cdc, 1);
  return static_cast<uint8_t>(cdc_read(cdc, 0x1F801803u) & 0x07u);
}

uint8_t response_byte(CdcState *cdc) {
  return static_cast<uint8_t>(cdc_read(cdc, 0x1F801801u));
}

void acknowledge_irq(CdcState *cdc) {
  select_bank(cdc, 1);
  cdc_write(cdc, 0x1F801803u, 0x07u);
}

void push_parameter(CdcState *cdc, uint8_t value) {
  select_bank(cdc, 0);
  cdc_write(cdc, 0x1F801802u, value);
}

void issue_command(CdcState *cdc, uint8_t command) {
  select_bank(cdc, 0);
  cdc_write(cdc, 0x1F801801u, command);
}

// Fakes injected through CdcState's function pointers (see test_cdc_continuous_read.cpp).
extern "C" int test_fake_disc_read_raw(struct DiscState *, uint32_t lba, uint8_t *out, uint32_t count) {
  const uint8_t filler[12] = {0};
  uint32_t i = 0;
  for (; i + sizeof filler <= count; i += sizeof filler) {
    memcpy(out + i, filler, sizeof filler);
  }
  if (i < count) {
    memset(out + i, 0, count - i);
  }
  return 1;
}

extern "C" int test_fake_disc_read_sector(struct DiscState *, uint32_t, uint8_t *out) {
  memset(out, 0, 2048);
  return 1;
}

// A one-track data disc: track 1 from LBA 0 for 45'000 sectors, so the lead-out is at 10:02.
void install_one_track(DiscState *disc) {
  disc->track_count = 1;
  disc->tracks[0] = DiscTrackInfo{1, 0, 45'000, 150, 0, 0};
}

CdcState controller(DiscState *disc, CdcTestClock *clock) {
  install_one_track(disc);
  CdcState cdc{};
  cdc.disc = disc;
  cdc_state_init(&cdc);
  cdc.disc_read_raw_fn = test_fake_disc_read_raw;
  cdc.disc_read_sector_fn = test_fake_disc_read_sector;
  cdc_test_bind(&cdc, clock);
  return cdc;
}

void service_at(CdcState *cdc, CdcTestClock *clock, uint64_t ticks) {
  clock->ticks = ticks;
  cdc_drive_service(cdc);
}

void test_zero_argument_command_waits_for_execution_phase(void) {
  DiscState disc{};
  CdcTestClock clock{100};
  CdcState cdc = controller(&disc, &clock);

  issue_command(&cdc, 0x13); // GetTN
  CHECK((status(&cdc) & 0x80u) != 0);
  CHECK_EQ(pending_irq_type(&cdc), 0);
  CHECK_EQ(cdc.command_deadline_ticks, 100u + kCdcCommandWritePhaseCpuTicks);

  service_at(&cdc, &clock, 100u + cdc_command_ack_delay_cpu_ticks(0) - 1u);
  CHECK((status(&cdc) & 0x80u) != 0);
  CHECK_EQ(pending_irq_type(&cdc), 0);

  service_at(&cdc, &clock, 100u + cdc_command_ack_delay_cpu_ticks(0));
  CHECK_EQ(status(&cdc) & 0x80u, 0);
  CHECK_EQ(pending_irq_type(&cdc), 3);
  CHECK_EQ(response_byte(&cdc), 0x02);
  CHECK_EQ(response_byte(&cdc), 0x01);
  CHECK_EQ(response_byte(&cdc), 0x01);
}

void test_arguments_transfer_before_setloc_executes(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);
  cdc.loc_lba = 77;
  push_parameter(&cdc, 0x00);
  push_parameter(&cdc, 0x02);
  push_parameter(&cdc, 0x10); // 00:02:10 -> LBA 10
  issue_command(&cdc, 0x02);

  CHECK_EQ(cdc.loc_lba, 77u);
  CHECK_EQ(cdc.param_n, 3);
  service_at(&cdc, &clock, kCdcCommandWritePhaseCpuTicks);
  CHECK_EQ(cdc.loc_lba, 77u);
  CHECK_EQ(cdc.param_n, 2);
  service_at(&cdc, &clock, kCdcCommandWritePhaseCpuTicks + kCdcCommandArgumentPhaseCpuTicks * 3u);
  CHECK_EQ(cdc.loc_lba, 77u);
  CHECK_EQ(cdc.param_n, 0);
  CHECK((status(&cdc) & 0x08u) != 0); // argument FIFO is now empty, command remains busy

  service_at(&cdc, &clock, cdc_command_ack_delay_cpu_ticks(3));
  CHECK_EQ(cdc.loc_lba, 77u);
  CHECK_EQ(cdc.command_lba, 10u);
  CHECK_EQ(pending_irq_type(&cdc), 3);
}

void test_side_effects_remain_unchanged_before_execution(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);
  cdc.mode = 0x20;
  push_parameter(&cdc, 0xA0);
  issue_command(&cdc, 0x0E); // Setmode
  service_at(&cdc, &clock, cdc_command_ack_delay_cpu_ticks(1) - 1u);
  CHECK_EQ(cdc.mode, 0x20);
  CHECK_EQ(pending_irq_type(&cdc), 0);
  service_at(&cdc, &clock, cdc_command_ack_delay_cpu_ticks(1));
  CHECK_EQ(cdc.mode, 0xA0);
  CHECK_EQ(pending_irq_type(&cdc), 3);
}

void test_pause_ack_and_completion_are_separate_edges(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);
  cdc_set_mode(&cdc, 0xA0);
  cdc_begin_read(&cdc, kSectorLba);
  issue_command(&cdc, 0x09); // Pause

  service_at(&cdc, &clock, cdc_command_ack_delay_cpu_ticks(0));
  CHECK_EQ(pending_irq_type(&cdc), 3);
  CHECK_EQ(response_byte(&cdc), 0x22);
  CHECK_EQ(cdc.reading, 0);
  CHECK(cdc.command_event_armed);
  const uint64_t completion = cdc.command_deadline_ticks;

  service_at(&cdc, &clock, completion);
  CHECK_EQ(pending_irq_type(&cdc), 3); // completion cannot merge behind unacknowledged INT3
  acknowledge_irq(&cdc);
  CHECK_EQ(pending_irq_type(&cdc), 0);
  service_at(&cdc, &clock, completion);
  CHECK_EQ(pending_irq_type(&cdc), 2);
  CHECK_EQ(response_byte(&cdc), 0x02);
}

void test_new_command_replaces_pending_command(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);
  issue_command(&cdc, 0x13); // GetTN would return three bytes
  clock.ticks = 1'000;
  issue_command(&cdc, 0x01); // Nop/Getstat replaces it

  service_at(&cdc, &clock, 1'000u + cdc_command_ack_delay_cpu_ticks(0));
  CHECK_EQ(pending_irq_type(&cdc), 3);
  CHECK_EQ(response_byte(&cdc), 0x02);
  CHECK_EQ(response_byte(&cdc), 0); // exactly one result byte: no stale GetTN response
}

void test_argument_count_is_checked_after_transfer(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);
  push_parameter(&cdc, 0x55);
  issue_command(&cdc, 0x13); // GetTN accepts no arguments

  CHECK_EQ(pending_irq_type(&cdc), 0);
  service_at(&cdc, &clock, cdc_command_ack_delay_cpu_ticks(1));
  CHECK_EQ(cdc.param_n, 0);
  CHECK_EQ(pending_irq_type(&cdc), 5);
  CHECK_EQ(response_byte(&cdc), 0x03);
  CHECK_EQ(response_byte(&cdc), 0x20);
}

void test_drive_event_wins_an_exact_command_deadline_tie(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);
  cdc_set_mode(&cdc, 0xA0);
  cdc_begin_read(&cdc, kSectorLba);
  const uint64_t tie = cdc.drive_deadline_ticks;
  clock.ticks = tie - cdc_command_ack_delay_cpu_ticks(0);
  issue_command(&cdc, 0x13);

  service_at(&cdc, &clock, tie);
  CHECK_EQ(pending_irq_type(&cdc), 1);
  acknowledge_irq(&cdc);
  CHECK_EQ(pending_irq_type(&cdc), 3);
}

void test_command_queued_behind_current_irq_does_not_raise_an_early_edge(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);
  cdc_set_mode(&cdc, 0xA0);
  cdc_begin_read(&cdc, kSectorLba);
  service_at(&cdc, &clock, cdc.drive_deadline_ticks);
  CHECK_EQ(pending_irq_type(&cdc), 1);
  cdc.irq_edge = 0; // the MMIO dispatcher consumed the current INT1 edge

  issue_command(&cdc, 0x13);
  clock.ticks += cdc_command_ack_delay_cpu_ticks(0);
  CHECK_EQ(cdc_drive_service(&cdc), 0);
  CHECK_EQ(pending_irq_type(&cdc), 1);
  CHECK_EQ(cdc.irq_edge, 0);

  acknowledge_irq(&cdc);
  CHECK_EQ(pending_irq_type(&cdc), 3);
  CHECK_EQ(cdc.irq_edge, 1);
}

// A guest that masks the response types off (Spyro 3's disc check writes 0x18 to the interrupt-enable
// register, issues GetTN and polls the flag register) must get its response WITHOUT a CPU interrupt
// edge, or the title's libcd handler consumes the answer and records it against its own last command.
void test_masked_response_is_polled_without_an_interrupt_edge(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);
  select_bank(&cdc, 1);
  cdc_write(&cdc, 0x1F801802u, 0x18u); // interrupt enable: response types 1-7 masked

  issue_command(&cdc, 0x13);
  service_at(&cdc, &clock, cdc_command_ack_delay_cpu_ticks(0));
  CHECK_EQ(pending_irq_type(&cdc), 3); // the poll still sees the response
  CHECK_EQ(cdc.irq_edge, 0);           // but the CPU line never rose
  acknowledge_irq(&cdc);
  CHECK_EQ(pending_irq_type(&cdc), 0);
  CHECK_EQ(cdc.irq_edge, 0);

  // Unmasking while a response is current asserts the line, which is a fresh edge.
  issue_command(&cdc, 0x13);
  service_at(&cdc, &clock, clock.ticks + cdc_command_ack_delay_cpu_ticks(0));
  CHECK_EQ(cdc.irq_edge, 0);
  select_bank(&cdc, 1);
  cdc_write(&cdc, 0x1F801802u, 0x1Fu);
  CHECK_EQ(cdc.irq_edge, 1);
}

void test_responses_interrupt_with_the_bios_enable_mask(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);
  select_bank(&cdc, 1);
  CHECK_EQ(cdc_read(&cdc, 0x1F801803u) & 0x1Fu, 0); // bank 1: flag register, nothing pending
  select_bank(&cdc, 0);
  CHECK_EQ(cdc_read(&cdc, 0x1F801803u) & 0x1Fu, 0x1Fu); // bank 0: enable register as the BIOS left it

  issue_command(&cdc, 0x13);
  service_at(&cdc, &clock, cdc_command_ack_delay_cpu_ticks(0));
  CHECK_EQ(cdc.irq_edge, 1);
}

void execute(CdcState *cdc, CdcTestClock *clock, uint8_t command, int arguments) {
  issue_command(cdc, command);
  service_at(cdc, clock, clock->ticks + cdc_command_ack_delay_cpu_ticks(arguments));
}

// GetTN and GetTD answer from the disc's table of contents, not a fixed reply.
void test_track_queries_answer_from_the_disc_toc(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);

  execute(&cdc, &clock, 0x13, 0); // GetTN
  CHECK_EQ(pending_irq_type(&cdc), 3);
  CHECK_EQ(response_byte(&cdc), 0x02);
  CHECK_EQ(response_byte(&cdc), 0x01);
  CHECK_EQ(response_byte(&cdc), 0x01);
  acknowledge_irq(&cdc);

  push_parameter(&cdc, 0x00); // GetTD(0): the lead-out
  execute(&cdc, &clock, 0x14, 1);
  CHECK_EQ(pending_irq_type(&cdc), 3);
  CHECK_EQ(response_byte(&cdc), 0x02);
  CHECK_EQ(response_byte(&cdc), 0x10);
  CHECK_EQ(response_byte(&cdc), 0x02);
  acknowledge_irq(&cdc);

  push_parameter(&cdc, 0x02); // GetTD(2): the disc has no track 2
  execute(&cdc, &clock, 0x14, 1);
  CHECK_EQ(pending_irq_type(&cdc), 5);
  CHECK_EQ(response_byte(&cdc), 0x03);
  CHECK_EQ(response_byte(&cdc), 0x10);
}

// Acknowledging clears the interrupt flag, not the results: a guest that acknowledges first and
// reads the answer afterwards (Spyro 3's disc check) still reads it, until the next response
// replaces it.
void test_results_stay_readable_after_acknowledge(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);

  execute(&cdc, &clock, 0x13, 0);
  acknowledge_irq(&cdc);
  CHECK_EQ(pending_irq_type(&cdc), 0);
  CHECK((status(&cdc) & 0x20u) != 0); // RSLRRDY
  CHECK_EQ(response_byte(&cdc), 0x02);
  CHECK_EQ(response_byte(&cdc), 0x01);
  CHECK_EQ(response_byte(&cdc), 0x01);
  CHECK_EQ(status(&cdc) & 0x20u, 0);

  execute(&cdc, &clock, 0x01, 0); // the next response replaces the results
  acknowledge_irq(&cdc);
  CHECK_EQ(response_byte(&cdc), 0x02);
  CHECK_EQ(response_byte(&cdc), 0);
}

// Setloc 00:00:00 names LBA -150 in the lead-in; the seek is the short signed distance, not a
// wrap-around across the whole 32-bit range.
void test_seek_into_the_lead_in_is_a_short_signed_distance(void) {
  DiscState disc{};
  CdcTestClock clock{};
  CdcState cdc = controller(&disc, &clock);
  cdc.loc_lba = 0;
  push_parameter(&cdc, 0x00);
  push_parameter(&cdc, 0x00);
  push_parameter(&cdc, 0x00);
  execute(&cdc, &clock, 0x02, 3); // Setloc 00:00:00
  acknowledge_irq(&cdc);

  const uint64_t issued = clock.ticks;
  execute(&cdc, &clock, 0x15, 0); // SeekL
  CHECK_EQ(pending_irq_type(&cdc), 3);
  acknowledge_irq(&cdc);
  CHECK(cdc.command_deadline_ticks - issued < 33'868'800u); // well under one second
}

} // namespace

int main() {
  RUN(zero_argument_command_waits_for_execution_phase);
  RUN(arguments_transfer_before_setloc_executes);
  RUN(side_effects_remain_unchanged_before_execution);
  RUN(pause_ack_and_completion_are_separate_edges);
  RUN(new_command_replaces_pending_command);
  RUN(argument_count_is_checked_after_transfer);
  RUN(drive_event_wins_an_exact_command_deadline_tie);
  RUN(command_queued_behind_current_irq_does_not_raise_an_early_edge);
  RUN(masked_response_is_polled_without_an_interrupt_edge);
  RUN(track_queries_answer_from_the_disc_toc);
  RUN(results_stay_readable_after_acknowledge);
  RUN(seek_into_the_lead_in_is_a_short_signed_distance);
  RUN(responses_interrupt_with_the_bios_enable_mask);
  return pt_summary();
}
