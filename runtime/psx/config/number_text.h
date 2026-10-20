// number_text.h — strict whole-string number parsing for config values and debug commands.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace psx::config {

// The whole of `text` as an unsigned number in `base` (16 accepts a 0x prefix); nullopt otherwise.
std::optional<std::uint64_t> parseUnsigned64(std::string_view text, int base);
std::optional<std::uint32_t> parseUnsigned(std::string_view text, int base);
// The whole of `text` as a signed decimal number.
std::optional<long> parseSigned(std::string_view text);
// The whole of `text` as an int in decimal.
std::optional<int> parseInt(std::string_view text);
// The whole of `text` as a finite decimal float.
std::optional<float> parseFloat(std::string_view text);

struct NumberPair {
  std::uint32_t first;
  std::uint32_t second;
};
// How a pair is spelled: `<a><separator><b>`, both whole unsigned numbers in `base`.
struct PairFormat {
  char separator;
  int base;
};
inline constexpr PairFormat kHexRange{',', 16};
inline constexpr PairFormat kDecimalRange{'-', 10};
inline constexpr PairFormat kDimensions{'x', 10};

std::optional<NumberPair> parsePair(std::string_view text, PairFormat format);

} // namespace psx::config
