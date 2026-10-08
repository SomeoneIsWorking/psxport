// The disc's table of contents as the CD controller answers it: GetTN's track range and GetTD's
// track start positions, in BCD, from the parsed CHD tracks. One owner for both the register-level
// controller (cdc_native.cpp) and the stock libcd command HLE (stock_cd_response.cpp).
#pragma once

#include <cstdint>

struct DiscState;

namespace psx::disc_toc {

// Two BCD bytes of a GetTN/GetTD answer, after the status byte.
struct BcdPair {
  uint8_t first;
  uint8_t second;
};

// GetTN: the first and last track numbers. Opens the disc if its TOC is not loaded yet; false when
// it cannot be opened or the TOC is malformed.
bool track_range(DiscState &disc, BcdPair &out);

// GetTD: the absolute minute and second where the BCD-numbered track starts; track 0 names the
// lead-out. False for an unreadable or malformed TOC, a non-BCD number, or a track the disc lacks.
bool track_start(DiscState &disc, uint8_t track_bcd, BcdPair &out);

} // namespace psx::disc_toc
