#include "cdc_state.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *what) {
  if (!condition) {
    std::cerr << "cdc_xa_filter: " << what << '\n';
    std::exit(1);
  }
}

void testFilterSelectsOnlyTheProgrammedStream() {
  CdcState cdc{};
  cdc_state_init(&cdc);
  cdc_set_mode(&cdc, 0xc8u); // double speed + XA + sector filter
  cdc_set_filter(&cdc, 1u, 5u);

  uint8_t selected[2352]{};
  selected[15] = 2u;
  selected[16] = 1u;
  selected[17] = 5u;
  selected[18] = 0x64u;
  require(cdc_xa_sector_selected(&cdc, selected) != 0, "matching XA sector selected");

  selected[17] = 6u;
  require(cdc_xa_sector_selected(&cdc, selected) == 0, "interleaved channel rejected");
  selected[17] = 5u;
  selected[18] = 0x44u;
  require(cdc_xa_sector_selected(&cdc, selected) == 0, "non-form2 sector rejected");
}

void testFilterBitOffAcceptsAllXAStreams() {
  CdcState cdc{};
  cdc_state_init(&cdc);
  cdc_set_mode(&cdc, 0xc0u); // double speed + XA, no SF
  cdc_set_filter(&cdc, 1u, 5u);
  uint8_t sector[2352]{};
  sector[15] = 2u;
  sector[16] = 7u;
  sector[17] = 9u;
  sector[18] = 0x64u;
  require(cdc_xa_sector_selected(&cdc, sector) != 0, "unfiltered XA sector selected");
}

} // namespace

int main() {
  testFilterSelectsOnlyTheProgrammedStream();
  testFilterBitOffAcceptsAllXAStreams();
  std::cout << "cdc_xa_filter: PASS (MODE_SF file/channel routing)\n";
  return 0;
}
