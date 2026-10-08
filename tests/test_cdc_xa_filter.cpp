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

std::array<uint8_t, 2352> xaSector(uint8_t file, uint8_t channel, uint8_t submode) {
  std::array<uint8_t, 2352> raw{};
  raw[15] = 2u;
  raw[16] = file;
  raw[17] = channel;
  raw[18] = submode;
  return raw;
}

void testFilterSelectsOnlyTheProgrammedStream() {
  CdcState cdc{};
  cdc_state_init(&cdc);
  cdc_set_mode(&cdc, 0xc8u); // double speed + XA + sector filter
  cdc_set_filter(&cdc, 1u, 5u);

  auto sector = xaSector(1u, 5u, kRtForm2Audio);
  require(cdc_sector_route(&cdc, sector.data()) == kCdcRouteXaAudio, "matching XA sector selected");

  sector[17] = 6u;
  require(cdc_sector_route(&cdc, sector.data()) == kCdcRouteXaDropped, "interleaved channel dropped");
  sector[17] = 5u;
  sector[18] = 0x44u;
  require(cdc_sector_route(&cdc, sector.data()) == kCdcRouteData, "non-form2 sector is data");
}

void testFilterBitOffAcceptsAllXAStreams() {
  CdcState cdc{};
  cdc_state_init(&cdc);
  cdc_set_mode(&cdc, 0xc0u); // double speed + XA, no SF
  cdc_set_filter(&cdc, 1u, 5u);
  const auto sector = xaSector(7u, 9u, kRtForm2Audio);
  require(cdc_sector_route(&cdc, sector.data()) == kCdcRouteXaAudio, "unfiltered XA sector selected");
}

// LBA 16 is XA on a rejected channel, 17 the selected one, 18 plain data.
extern "C" int xaFilterDiscReadRaw(struct DiscState *, uint32_t lba, uint8_t *out, uint32_t count) {
  const auto sector = lba == 16u   ? xaSector(1u, 0u, kRtForm2Audio)
                      : lba == 17u ? xaSector(1u, 13u, kRtForm2Audio)
                                   : xaSector(0u, 0u, kDataSubmode);
  std::copy_n(sector.data(), count, out);
  return 1;
}

extern "C" int xaFilterDiscReadSector(struct DiscState *, uint32_t, uint8_t *) {
  return 0;
}

void testRejectedXaSectorRaisesNoDataReady() {
  auto xa = std::make_unique<XaState>();
  xa_state_init(xa.get());
  CdcState cdc{};
  cdc_state_init(&cdc);
  cdc.xa = xa.get();
  cdc.disc_read_raw_fn = xaFilterDiscReadRaw;
  cdc.disc_read_sector_fn = xaFilterDiscReadSector;
  CdcTestClock clock;
  cdc_test_bind(&cdc, &clock);
  cdc_set_mode(&cdc, 0xe8u); // double speed + XA + whole sector + filter
  cdc_set_filter(&cdc, 1u, 13u);
  cdc_begin_read(&cdc, 16u);

  require(cdc_test_service_deadline(&cdc, &clock) == 0, "rejected XA sector raised INT1");
  require(cdc_test_service_deadline(&cdc, &clock) == 0, "selected XA sector raised INT1");
  require(cdc_test_service_deadline(&cdc, &clock) == 1, "data sector after the XA run raised no INT1");
}

} // namespace

int main() {
  testFilterSelectsOnlyTheProgrammedStream();
  testFilterBitOffAcceptsAllXAStreams();
  testRejectedXaSectorRaisesNoDataReady();
  std::cout << "cdc_xa_filter: PASS (MODE_SF file/channel routing)\n";
  return 0;
}
