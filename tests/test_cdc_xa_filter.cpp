#include "cd_drive_timing.h"
#include "cdc_state.h"
#include "cdc_test_clock.h"
#include "xa_state.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

void require(bool condition, const char *what) {
  if (!condition) {
    std::cerr << "cdc_xa_filter: " << what << '\n';
    std::exit(1);
  }
}

constexpr uint8_t kRtForm2Audio = 0x64u;
constexpr uint8_t kDataSubmode = 0x08u;

// An XA subheader's file and channel numbers.
struct XaStream {
  uint8_t file = 0;
  uint8_t channel = 0;
};

std::array<uint8_t, 2352> xaSector(XaStream stream, uint8_t submode) {
  std::array<uint8_t, 2352> raw{};
  raw[15] = 2u;
  raw[16] = stream.file;
  raw[17] = stream.channel;
  raw[18] = submode;
  return raw;
}

void testFilterSelectsOnlyTheProgrammedStream() {
  psx::cd::CdController cdc{};
  cdc.reset();
  cdc.setMode(0xc8u); // double speed + XA + sector filter
  cdc.setFilter(1u, 5u);

  auto sector = xaSector({1u, 5u}, kRtForm2Audio);
  require(cdc.sectorRoute(sector.data()) == psx::cd::CdcSectorRoute::XaAudio, "matching XA sector selected");

  sector[17] = 6u;
  require(cdc.sectorRoute(sector.data()) == psx::cd::CdcSectorRoute::XaDropped, "interleaved channel dropped");
  sector[17] = 5u;
  sector[18] = 0x44u;
  require(cdc.sectorRoute(sector.data()) == psx::cd::CdcSectorRoute::Data, "non-form2 sector is data");
}

void testFilterBitOffAcceptsAllXAStreams() {
  psx::cd::CdController cdc{};
  cdc.reset();
  cdc.setMode(0xc0u); // double speed + XA, no SF
  cdc.setFilter(1u, 5u);
  const auto sector = xaSector({7u, 9u}, kRtForm2Audio);
  require(cdc.sectorRoute(sector.data()) == psx::cd::CdcSectorRoute::XaAudio, "unfiltered XA sector selected");
}

// LBA 16 is XA on a rejected channel, 17 the selected one, 18 plain data.
bool xaFilterDiscReadRaw(psx::cd::Disc *, uint32_t lba, uint8_t *out, uint32_t count) {
  const auto sector = lba == 16u   ? xaSector({1u, 0u}, kRtForm2Audio)
                      : lba == 17u ? xaSector({1u, 13u}, kRtForm2Audio)
                                   : xaSector({0u, 0u}, kDataSubmode);
  std::copy_n(sector.data(), count, out);
  return true;
}

bool xaFilterDiscReadSector(psx::cd::Disc *, uint32_t, uint8_t *) {
  return false;
}

void testRejectedXaSectorRaisesNoDataReady() {
  auto xa = std::make_unique<psx::audio::XaStream>();
  xa.get()->reset();
  psx::cd::CdController cdc{};
  cdc.reset();
  cdc.xa = xa.get();
  cdc.disc_read_raw_fn = xaFilterDiscReadRaw;
  cdc.disc_read_sector_fn = xaFilterDiscReadSector;
  CdcTestClock clock;
  psx::test::bindCdcClock(&cdc, &clock);
  cdc.setMode(0xe8u); // double speed + XA + whole sector + filter
  cdc.setFilter(1u, 13u);
  cdc.beginRead(16u);

  require(psx::test::serviceCdcDeadline(&cdc, &clock) == 0, "rejected XA sector raised INT1");
  require(psx::test::serviceCdcDeadline(&cdc, &clock) == 0, "selected XA sector raised INT1");
  require(psx::test::serviceCdcDeadline(&cdc, &clock) == 1, "data sector after the XA run raised no INT1");
}

// Every sector is the selected XA stream.
bool xaStreamDiscReadRaw(psx::cd::Disc *, uint32_t, uint8_t *out, uint32_t count) {
  const auto sector = xaSector({1u, 13u}, kRtForm2Audio);
  std::copy_n(sector.data(), count, out);
  return true;
}

// One NTSC field is 2.5 sector periods at double speed; a service that late owes every sector in it.
void testLateServicePassesEveryElapsedAudioSector() {
  auto xa = std::make_unique<psx::audio::XaStream>();
  xa.get()->reset();
  psx::cd::CdController cdc{};
  cdc.reset();
  cdc.xa = xa.get();
  cdc.disc_read_raw_fn = xaStreamDiscReadRaw;
  cdc.disc_read_sector_fn = xaFilterDiscReadSector;
  CdcTestClock clock;
  psx::test::bindCdcClock(&cdc, &clock);
  cdc.setMode(0xc8u);
  cdc.setFilter(1u, 13u);
  cdc.beginRead(16u);

  const uint64_t period = psx::cd::driveSectorPeriodCpuTicks(0xc8u);
  const uint64_t first = cdc.drive_deadline_ticks;
  clock.ticks = first + (period * 5u) / 2u;
  require(cdc.service() == 0, "an audio stream raised INT1");
  require(xa->sectors == 3u, "a service 2.5 periods late did not pass the 3 sectors due by then");
  require(cdc.drive_deadline_ticks == first + 3u * period, "the next sector was not due one period after the last");
}

} // namespace

int main() {
  testFilterSelectsOnlyTheProgrammedStream();
  testFilterBitOffAcceptsAllXAStreams();
  testRejectedXaSectorRaisesNoDataReady();
  testLateServicePassesEveryElapsedAudioSector();
  std::cout << "cdc_xa_filter: PASS (MODE_SF file/channel routing)\n";
  return 0;
}
