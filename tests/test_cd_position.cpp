#include "cd_position.h"
#include "testutil.h"

using psx::cd::commandCarriesPosition;
using psx::cd::msfToLba;

// THE CASE THIS FILE EXISTS FOR (issue 0115). Spyro asks for LBA 113,448 by handing the MSF to
// CdControl(CdlReadS, &loc, 0), with no Setloc of its own. Read the command byte alone and the
// position is lost; the XA cursor then starts at 0 and scans the disc inside one audio sample.
static void test_spyro_reads_position(void) {
  CHECK(commandCarriesPosition(0x1B));          // ReadS
  CHECK_EQ(msfToLba(0x25, 0x14, 0x48), 113448); // the exact MSF traced from the product
}

// The other answer: the commands that carry NO position must not be treated as if they did, or a
// Setmode/Setfilter parameter would be read as an MSF and move the head.
static void test_commands_without_a_position(void) {
  CHECK(!commandCarriesPosition(0x02)); // Setloc IS the position command, not a carrier of one
  CHECK(!commandCarriesPosition(0x0D)); // Setfilter — its parameter is file/channel
  CHECK(!commandCarriesPosition(0x0E)); // Setmode — its parameter is the mode byte
  CHECK(!commandCarriesPosition(0x09)); // Pause
  CHECK(!commandCarriesPosition(0x00));
  CHECK(!commandCarriesPosition(0xFF));
}

// The rest of libcd's table at Spyro 1's 0x80074DAC. Seeks carry a position and dropping it is the
// same defect as dropping ReadS's.
static void test_the_whole_carrier_set(void) {
  CHECK(commandCarriesPosition(0x03)); // SetlocL
  CHECK(commandCarriesPosition(0x06)); // ReadN
  CHECK(commandCarriesPosition(0x15)); // SeekL
  CHECK(commandCarriesPosition(0x16)); // SeekP
}

static void test_msf_is_bcd_with_a_lead_in(void) {
  CHECK_EQ(msfToLba(0x00, 0x02, 0x00), 0); // the first data sector
  CHECK_EQ(msfToLba(0x00, 0x02, 0x01), 1);
  CHECK_EQ(msfToLba(0x00, 0x03, 0x00), 75); // one second on
  CHECK_EQ(msfToLba(0x01, 0x02, 0x00), 4500);
  // BCD, not binary: 0x14 is 14, not 20. Reading it as binary would give a different sector.
  CHECK_EQ(msfToLba(0x00, 0x14, 0x00), (14 * 75) - 150);
  CHECK(msfToLba(0x00, 0x14, 0x00) != (0x14 * 75) - 150);
}

// Inside the lead-in there is no data sector, so it refuses rather than returning a negative LBA
// the disc layer would then index with.
static void test_before_the_first_sector_refuses(void) {
  CHECK_EQ(msfToLba(0x00, 0x00, 0x00), -1);
  CHECK_EQ(msfToLba(0x00, 0x01, 0x74), -1);
}

int main(void) {
  RUN(spyro_reads_position);
  RUN(commands_without_a_position);
  RUN(the_whole_carrier_set);
  RUN(msf_is_bcd_with_a_lead_in);
  RUN(before_the_first_sector_refuses);
  return pt_summary();
}
