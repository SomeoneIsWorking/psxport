#pragma once
// The display field rate as an exact rational, with milli-hertz for integer APIs. NTSC is
// 60000/1001 Hz; the standard is GP1(0x08) bit 3, decoded into DisplayArea::pal.
#include <cstdint>

namespace psx::frame {

struct DisplayFieldRate {
  uint32_t frequencyNumerator;
  uint32_t frequencyDenominator;

  constexpr bool operator==(const DisplayFieldRate &) const = default;
};

inline constexpr DisplayFieldRate DISPLAY_FIELD_RATE_NTSC{60000u, 1001u};
inline constexpr DisplayFieldRate DISPLAY_FIELD_RATE_PAL{50u, 1u};

constexpr unsigned fieldRateMilliHz(DisplayFieldRate rate) {
  return static_cast<unsigned>((static_cast<uint64_t>(rate.frequencyNumerator) * 1000u) / rate.frequencyDenominator);
}

inline constexpr unsigned FIELD_RATE_NTSC_MILLIHZ = fieldRateMilliHz(DISPLAY_FIELD_RATE_NTSC);
// Host screens (the picker) have no guest display mode and pace at the NTSC rate.
inline constexpr unsigned kHostScreenFieldRateMilliHz = FIELD_RATE_NTSC_MILLIHZ;

// Nominal non-interlaced lines per field; field parity is not modelled.
inline constexpr unsigned DISPLAY_LINES_NTSC = 263u;
inline constexpr unsigned DISPLAY_LINES_PAL = 314u;

inline constexpr DisplayFieldRate displayFieldRate(bool pal) {
  return pal ? DISPLAY_FIELD_RATE_PAL : DISPLAY_FIELD_RATE_NTSC;
}

inline unsigned fieldRateMilliHz(bool pal) {
  return fieldRateMilliHz(displayFieldRate(pal));
}

inline unsigned displayLinesPerField(bool pal) {
  return pal ? DISPLAY_LINES_PAL : DISPLAY_LINES_NTSC;
}

// `fields` display fields at `rateMilliHz`, split into `parts` equal intervals, in milliseconds.
constexpr double fieldIntervalMs(unsigned rateMilliHz, unsigned fields, unsigned parts) {
  return static_cast<double>(fields) * 1000000.0 / static_cast<double>(rateMilliHz) / static_cast<double>(parts);
}

} // namespace psx::frame
