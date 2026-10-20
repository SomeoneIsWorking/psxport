// number_text.h — strict whole-string number parsing for config values and debug commands.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace psx::config {

// The whole of `text` as an unsigned number in `base`; 16 accepts a 0x prefix, and 0 takes the base
// from the prefix as strtoul does (0x hex, leading 0 octal, else decimal). Nullopt otherwise.
std::optional<std::uint64_t> parseUnsigned64(std::string_view text, int base);
std::optional<std::uint32_t> parseUnsigned(std::string_view text, int base);
// The whole of `text` as a signed number, an optional '-' before an unsigned spelling in `base`.
std::optional<long> parseSigned(std::string_view text, int base = 10);
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

// The comma and whitespace a config list may separate its entries with.
inline constexpr std::string_view kListSeparators = ", \t";

// The non-empty runs of `text` between any of `separators`.
std::vector<std::string_view> splitList(std::string_view text, std::string_view separators);
// Every entry of a kListSeparators list as an unsigned number in `base`; nullopt if any entry is not one.
std::optional<std::vector<std::uint32_t>> parseUnsignedList(std::string_view text, int base);

} // namespace psx::config
